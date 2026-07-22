#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <string>
#include <vector>
#include "compute_node/node_service.hpp"
#include "membox/manager_service.hpp"
#include "membox/membox.hpp"

using namespace inspection;

TEST(NodeService, ScoresFramesFromTheMemoryBox) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(8, frame_bytes);
  auto segment = ShmSegment::create("/algctl-node-test", 8 * frame_bytes);
  grpc::ServerBuilder mb;
  int mgr_port = 0;
  mb.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &mgr_port);
  mb.RegisterService(&manager);
  auto mgr_server = mb.BuildAndStart();
  auto mgr_channel = grpc::CreateChannel("localhost:" + std::to_string(mgr_port),
                                         grpc::InsecureChannelCredentials());

  Membox writer_box(mgr_channel, ShmSegment::open_existing("/algctl-node-test", true),
                    frame_bytes);
  std::vector<std::byte> clean(frame_bytes, std::byte{50});
  std::vector<std::byte> defective(frame_bytes, std::byte{50});
  for (int i = 0; i < 8; ++i) defective[i] = std::byte{250};
  std::vector<std::uint64_t> slots = {writer_box.write_frame(clean),
                                      writer_box.write_frame(clean),
                                      writer_box.write_frame(defective)};

  Membox node_box(mgr_channel, ShmSegment::open_existing("/algctl-node-test", false),
                  frame_bytes);
  NodeService node(node_box, /*compute_ms=*/0);
  grpc::ServerBuilder nb;
  int node_port = 0;
  nb.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &node_port);
  nb.RegisterService(&node);
  auto node_server = nb.BuildAndStart();
  auto stub = v1::ComputeNode::NewStub(grpc::CreateChannel(
      "localhost:" + std::to_string(node_port), grpc::InsecureChannelCredentials()));

  grpc::ClientContext ctx;
  v1::PositionRequest req;
  req.set_slice_id(0);
  req.set_frame_in_die(1);
  for (auto s : slots) req.add_slots(s);
  v1::PositionValue reply;
  ASSERT_TRUE(stub->ProcessPosition(&ctx, req, &reply).ok());
  // Median = 50 everywhere; the defective die deviates 200 on 8/64 pixels = 25.
  EXPECT_NEAR(reply.value(), 25.0, 0.01);

  grpc::ClientContext ctx2;
  v1::PositionRequest bad;
  bad.add_slots(13);  // misaligned offset
  EXPECT_EQ(stub->ProcessPosition(&ctx2, bad, &reply).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  node_server->Shutdown();
  mgr_server->Shutdown();
}

// Accepts registrations and records them, standing in for the controller.
class RecordingController final : public v1::Controller::Service {
 public:
  grpc::Status RegisterNode(grpc::ServerContext*, const v1::NodeEndpoint* req,
                            v1::Nothing*) override {
    endpoints.push_back(req->endpoint());
    return grpc::Status::OK;
  }
  std::vector<std::string> endpoints;
};

TEST(NodeRegistration, AnnouncesEndpointToTheController) {
  RecordingController controller;
  grpc::ServerBuilder b;
  int port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
  b.RegisterService(&controller);
  auto server = b.BuildAndStart();

  grpc::Status st = register_with_controller("localhost:" + std::to_string(port),
                                             "localhost:50061", std::chrono::seconds(5));
  ASSERT_TRUE(st.ok()) << st.error_message();
  ASSERT_EQ(controller.endpoints.size(), 1u);
  EXPECT_EQ(controller.endpoints[0], "localhost:50061");

  server->Shutdown();
}

TEST(NodeRegistration, UnreachableControllerFailsWithinDeadline) {
  // Nothing listens on this port; wait_for_ready must give up at the deadline
  // instead of hanging.
  grpc::Status st = register_with_controller("localhost:59999", "localhost:50061",
                                             std::chrono::milliseconds(300));
  EXPECT_EQ(st.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
}
