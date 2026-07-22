#pragma once
#include <chrono>
#include <thread>
#include <vector>
#include "compute_node/algorithm/position_score.hpp"
#include "inspection.grpc.pb.h"
#include "membox/membox.hpp"

namespace inspection {

// Serves ProcessPosition (⑥): reads every die's frame zero-copy from the
// memory box, scores the position, and optionally pads with compute_ms to
// emulate realistic algorithm cost.
class NodeService final : public v1::ComputeNode::Service {
 public:
  NodeService(Membox& box, int compute_ms) : box_(box), compute_ms_(compute_ms) {}

  grpc::Status ProcessPosition(grpc::ServerContext*, const v1::PositionRequest* req,
                               v1::PositionValue* reply) override {
    try {
      std::vector<std::span<const std::byte>> frames;
      frames.reserve(req->slots_size());
      for (auto slot : req->slots()) frames.push_back(box_.read_frame(slot));
      double value = position_score(frames);
      if (compute_ms_ > 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(compute_ms_));
      reply->set_value(value);
      return grpc::Status::OK;
    } catch (const std::exception& e) {
      return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
  }

 private:
  Membox& box_;
  int compute_ms_;
};

// Announces this node's dial-back endpoint to the controller (Ⅶ). wait_for_ready
// lets the node start before the controller: the call blocks until the controller
// is reachable or the deadline expires, so launch order never matters but an
// unreachable controller still fails loudly within the deadline.
inline grpc::Status register_with_controller(const std::string& controller,
                                             const std::string& self_endpoint,
                                             std::chrono::milliseconds deadline) {
  auto stub = v1::Controller::NewStub(
      grpc::CreateChannel(controller, grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  ctx.set_wait_for_ready(true);
  ctx.set_deadline(std::chrono::system_clock::now() + deadline);
  v1::NodeEndpoint ep;
  ep.set_endpoint(self_endpoint);
  v1::Nothing nothing;
  return stub->RegisterNode(&ctx, ep, &nothing);
}

}  // namespace inspection
