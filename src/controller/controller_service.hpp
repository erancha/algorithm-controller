#pragma once
#include <grpcpp/grpcpp.h>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "inspection.grpc.pb.h"
#include "membox/membox.hpp"

namespace inspection {

// Serves ProcessSlice (①): images the slice (②), builds the frame index from
// the ④ stream, dispatches one invocation per position (⑥) to whichever node
// is free, gathers replies at their position index, and releases the slice's
// slots (⑦). Die count and position count are derived from the stream, never
// configured. The node pool is filled by RegisterNode (Ⅶ): each compute node
// announces its own endpoint at startup, so the controller is never
// configured with node addresses. Every checkout reads the live pool, so a
// node that registers while a slice is already dispatching starts receiving
// positions immediately. A node whose invocation fails or times out is
// evicted from the pool — the slice aborts, but later slices no longer
// dispatch to it; re-registering (a restarted node) restores it.
class ControllerService final : public v1::Controller::Service {
 public:
  // Every cross-process wait ProcessSlice performs is bounded, so a hung peer
  // fails the slice instead of wedging it. Tests shrink these to keep
  // deadline behavior fast to exercise.
  struct Timing {
    // Startup barrier: nodes and controller launch concurrently, so an
    // arriving slice briefly waiting for the first registration is legal.
    std::chrono::milliseconds first_node_wait = std::chrono::seconds(10);
    // Bounds the whole ②–⑤ stream; imaging a paced slice legitimately takes
    // seconds, so this is generous rather than tight.
    std::chrono::milliseconds camera_stream_deadline = std::chrono::minutes(5);
    // Bounds one ⑥ invocation on one node.
    std::chrono::milliseconds position_deadline = std::chrono::minutes(1);
  };

  ControllerService(std::shared_ptr<grpc::Channel> camera, Membox& box)
      : ControllerService(std::move(camera), box, Timing()) {}

  ControllerService(std::shared_ptr<grpc::Channel> camera, Membox& box, Timing timing)
      : camera_(v1::Camera::NewStub(std::move(camera))), box_(box), timing_(timing) {}

  grpc::Status RegisterNode(grpc::ServerContext*, const v1::NodeEndpoint* req,
                            v1::Nothing*) override {
    if (req->endpoint().empty())
      return {grpc::StatusCode::INVALID_ARGUMENT, "empty node endpoint"};
    auto stub = std::shared_ptr<v1::ComputeNode::Stub>(v1::ComputeNode::NewStub(
        grpc::CreateChannel(req->endpoint(), grpc::InsecureChannelCredentials())));
    {
      std::lock_guard lock(nodes_mu_);
      // Re-registering an endpoint replaces its connection (node restart),
      // never grows the pool. The fresh entry starts free even if the old
      // one was mid-call — that call belongs to a connection this entry no
      // longer represents.
      nodes_.insert_or_assign(req->endpoint(), NodeEntry{std::move(stub), false});
    }
    nodes_cv_.notify_all();
    std::cout << "node registered: " << req->endpoint() << std::endl;
    return grpc::Status::OK;
  }

  std::size_t registered_node_count() {
    std::lock_guard lock(nodes_mu_);
    return nodes_.size();
  }

  grpc::Status ProcessSlice(grpc::ServerContext*, const v1::SliceRequest* req,
                            v1::SliceResults* results) override {
    // Startup barrier only — dispatch below always reads the live pool, so
    // nodes registering after this point still join the slice.
    {
      std::unique_lock lock(nodes_mu_);
      nodes_cv_.wait_for(lock, timing_.first_node_wait, [this] { return !nodes_.empty(); });
      if (nodes_.empty())
        return {grpc::StatusCode::FAILED_PRECONDITION, "no compute nodes registered"};
    }

    // ② + ④ + ⑤ — one streaming call fills the frame index.
    std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint64_t> frame_index;
    std::uint32_t max_die = 0, max_pos = 0;

    // Releases every slot currently in frame_index; used on every return once the
    // stream may have claimed slots, so a failure at any later stage cannot leak them.
    auto release_slice_slots = [&] {
      std::vector<SlotOffset> all_slots;
      for (const auto& [key, slot] : frame_index) all_slots.push_back(slot);
      box_.release_slots(all_slots);
    };
    // On an error return the release is best-effort logging only — the root cause
    // that triggered the abort must win over a secondary release failure.
    auto release_and_keep_error = [&](const grpc::Status& primary) {
      try {
        release_slice_slots();
      } catch (const std::exception& e) {
        std::cerr << "release_slots failed while aborting: " << e.what() << std::endl;
      }
      return primary;
    };

    {
      grpc::ClientContext ctx;
      ctx.set_deadline(std::chrono::system_clock::now() + timing_.camera_stream_deadline);
      auto reader = camera_->ImageSlice(&ctx, *req);
      v1::FrameStored msg;
      while (reader->Read(&msg)) {
        frame_index[{msg.die(), msg.frame_in_die()}] = msg.slot();
        max_die = std::max(max_die, msg.die());
        max_pos = std::max(max_pos, msg.frame_in_die());
      }
      grpc::Status st = reader->Finish();
      if (!st.ok()) return release_and_keep_error(st);
    }
    if (frame_index.empty())
      return {grpc::StatusCode::FAILED_PRECONDITION, "camera reported no frames"};
    const std::uint32_t dies = max_die + 1, positions = max_pos + 1;
    if (frame_index.size() != std::size_t{dies} * positions)
      return release_and_keep_error({grpc::StatusCode::FAILED_PRECONDITION, "incomplete slice"});

    // Ⅳa — compose each position's slot list, in die order.
    std::vector<std::vector<std::uint64_t>> slot_lists(positions);
    for (std::uint32_t pos = 0; pos < positions; ++pos)
      for (std::uint32_t die = 0; die < dies; ++die)
        slot_lists[pos].push_back(frame_index.at({die, pos}));

    // ⑥ — dispatch each position to whichever node is free. Checkout blocks
    // while every node is busy, which bounds the in-flight invocations to the
    // pool size while letting mid-slice registrations add concurrency.
    std::vector<double> values(positions, 0.0);
    std::mutex mu;
    grpc::Status first_error = grpc::Status::OK;
    std::vector<std::thread> in_flight;
    for (std::uint32_t pos = 0; pos < positions; ++pos) {
      {
        std::lock_guard lock(mu);
        if (!first_error.ok()) break;
      }
      std::optional<CheckedOutNode> node = checkout_node();
      if (!node) {
        std::lock_guard lock(mu);
        if (first_error.ok())
          first_error = {grpc::StatusCode::UNAVAILABLE, "no compute node became available"};
        break;
      }
      in_flight.emplace_back([&, pos, node = std::move(*node)] {
        v1::PositionRequest preq;
        preq.set_slice_id(req->slice_id());
        preq.set_frame_in_die(pos);
        for (auto s : slot_lists[pos]) preq.add_slots(s);
        grpc::ClientContext ctx;
        ctx.set_deadline(std::chrono::system_clock::now() + timing_.position_deadline);
        v1::PositionValue reply;
        grpc::Status st = node.stub->ProcessPosition(&ctx, preq, &reply);
        if (st.ok()) {
          std::lock_guard lock(mu);
          values[pos] = reply.value();
        } else {
          // Recorded before the eviction wakes blocked checkouts, so the
          // dispatcher observes this root cause instead of stamping its own
          // pool-exhausted error over it.
          {
            std::lock_guard lock(mu);
            if (first_error.ok()) first_error = st;
          }
          evict(node.endpoint, node.stub, st);
        }
        checkin(node.endpoint, node.stub);
      });
    }
    for (auto& t : in_flight) t.join();
    if (!first_error.ok()) return release_and_keep_error(first_error);

    // ⑦ — the slice is done; its slots may hold the next slice's frames.
    try {
      release_slice_slots();
    } catch (const std::exception& e) {
      return {grpc::StatusCode::INTERNAL, e.what()};
    }

    for (double v : values) results->add_values(v);
    return grpc::Status::OK;
  }

 private:
  struct NodeEntry {
    std::shared_ptr<v1::ComputeNode::Stub> stub;
    // True while one invocation is in flight on this connection — a node
    // serves at most one position at a time.
    bool busy;
  };

  // A checked-out pool entry; the shared_ptr keeps the stub alive even if the
  // endpoint is evicted or re-registered while the invocation is in flight.
  struct CheckedOutNode {
    std::string endpoint;
    std::shared_ptr<v1::ComputeNode::Stub> stub;
  };

  // Blocks until some node in the live pool is free and marks it busy.
  // Returns nullopt when the pool empties (every node evicted mid-slice) or
  // when no node frees within position_deadline plus slack — every in-flight
  // call is bounded by position_deadline, and the slack lets such a call hit
  // its own deadline first, so its status (the root cause) wins over this
  // pool-exhausted fallback.
  std::optional<CheckedOutNode> checkout_node() {
    std::unique_lock lock(nodes_mu_);
    auto free_node = [this] {
      return std::find_if(nodes_.begin(), nodes_.end(),
                          [](const auto& kv) { return !kv.second.busy; });
    };
    bool woke = nodes_cv_.wait_for(lock, timing_.position_deadline + std::chrono::seconds(5),
                                   [&] { return nodes_.empty() || free_node() != nodes_.end(); });
    if (!woke || nodes_.empty()) return std::nullopt;
    auto it = free_node();
    it->second.busy = true;
    return CheckedOutNode{it->first, it->second.stub};
  }

  // Returns a checked-out node to the pool. Skipped when the endpoint was
  // evicted or re-registered since checkout — the current entry then belongs
  // to a different connection whose free/busy state is not ours to change.
  void checkin(const std::string& endpoint, const std::shared_ptr<v1::ComputeNode::Stub>& stub) {
    {
      std::lock_guard lock(nodes_mu_);
      auto it = nodes_.find(endpoint);
      if (it == nodes_.end() || it->second.stub != stub) return;
      it->second.busy = false;
    }
    nodes_cv_.notify_all();
  }

  // Removes a failed node from the pool so later slices stop dispatching to
  // it. Skipped when the endpoint re-registered since checkout — the pool
  // then holds a fresh connection the failure says nothing about. Waiting
  // checkouts are woken so they can observe an emptied pool.
  void evict(const std::string& endpoint, const std::shared_ptr<v1::ComputeNode::Stub>& stub,
             const grpc::Status& why) {
    {
      std::lock_guard lock(nodes_mu_);
      auto it = nodes_.find(endpoint);
      if (it == nodes_.end() || it->second.stub != stub) return;
      nodes_.erase(it);
    }
    nodes_cv_.notify_all();
    std::cerr << "evicting node " << endpoint << ": " << why.error_message() << std::endl;
  }

  std::unique_ptr<v1::Camera::Stub> camera_;
  Membox& box_;
  Timing timing_;
  std::mutex nodes_mu_;
  std::condition_variable nodes_cv_;
  // Key: node endpoint as it registered. Value: that endpoint's connection
  // and in-flight state.
  std::map<std::string, NodeEntry> nodes_;
};

}  // namespace inspection
