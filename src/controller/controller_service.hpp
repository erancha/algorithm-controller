#pragma once
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "inspection.grpc.pb.h"
#include "membox/membox.hpp"

namespace inspection {

// Serves ProcessSlice (①): images the slice (②), builds the frame index from
// the ④ stream, dispatches one invocation per position (⑥) across the node
// pool, gathers replies at their position index, and releases the slice's
// slots (⑦). Die count and position count are derived from the stream, never
// configured. The node pool is filled by RegisterNode (Ⅶ): each compute node
// announces its own endpoint at startup, so the controller is never
// configured with node addresses. A node whose invocation fails or times out
// is evicted from the pool — the slice aborts, but later slices no longer
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
      // never grows the pool.
      nodes_.insert_or_assign(req->endpoint(), std::move(stub));
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
    // Snapshot the pool for this slice; shared_ptr copies keep each stub alive
    // even if its endpoint re-registers or is evicted mid-slice.
    std::vector<std::pair<std::string, std::shared_ptr<v1::ComputeNode::Stub>>> nodes;
    {
      std::unique_lock lock(nodes_mu_);
      nodes_cv_.wait_for(lock, timing_.first_node_wait, [this] { return !nodes_.empty(); });
      if (nodes_.empty())
        return {grpc::StatusCode::FAILED_PRECONDITION, "no compute nodes registered"};
      for (const auto& [endpoint, stub] : nodes_) nodes.emplace_back(endpoint, stub);
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

    // ⑥ — one worker per node, each keeping one invocation in flight.
    std::vector<double> values(positions, 0.0);
    std::mutex mu;
    std::uint32_t next_pos = 0;
    grpc::Status first_error = grpc::Status::OK;
    std::vector<std::thread> workers;
    for (auto& node : nodes) {
      workers.emplace_back([&, &endpoint = node.first, &stub = node.second] {
        for (;;) {
          std::uint32_t pos;
          {
            std::lock_guard lock(mu);
            if (next_pos >= positions || !first_error.ok()) return;
            pos = next_pos++;
          }
          v1::PositionRequest preq;
          preq.set_slice_id(req->slice_id());
          preq.set_frame_in_die(pos);
          for (auto s : slot_lists[pos]) preq.add_slots(s);
          grpc::ClientContext ctx;
          ctx.set_deadline(std::chrono::system_clock::now() + timing_.position_deadline);
          v1::PositionValue reply;
          grpc::Status st = stub->ProcessPosition(&ctx, preq, &reply);
          if (!st.ok()) {
            evict(endpoint, stub, st);
            std::lock_guard lock(mu);
            if (first_error.ok()) first_error = st;
            return;
          }
          std::lock_guard lock(mu);
          values[pos] = reply.value();
        }
      });
    }
    for (auto& w : workers) w.join();
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
  // Removes a failed node from the pool so later slices stop dispatching to
  // it. Skipped when the endpoint re-registered since this slice's snapshot —
  // the pool then holds a fresh connection the failure says nothing about.
  void evict(const std::string& endpoint, const std::shared_ptr<v1::ComputeNode::Stub>& stub,
             const grpc::Status& why) {
    std::lock_guard lock(nodes_mu_);
    auto it = nodes_.find(endpoint);
    if (it == nodes_.end() || it->second != stub) return;
    nodes_.erase(it);
    std::cerr << "evicting node " << endpoint << ": " << why.error_message() << std::endl;
  }

  std::unique_ptr<v1::Camera::Stub> camera_;
  Membox& box_;
  Timing timing_;
  std::mutex nodes_mu_;
  std::condition_variable nodes_cv_;
  // Key: node endpoint as it registered. Value: stub dialing that endpoint.
  std::map<std::string, std::shared_ptr<v1::ComputeNode::Stub>> nodes_;
};

}  // namespace inspection
