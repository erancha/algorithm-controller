#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include "controller/controller_service.hpp"
#include "membox/manager_service.hpp"
#include "membox/membox.hpp"

using namespace inspection;

// A camera whose "slice" is scripted: streams the given frames, then finishes
// with final_status (OK by default; a non-OK status models a camera failure
// after some frames were already streamed).
class FakeCamera final : public v1::Camera::Service {
 public:
  explicit FakeCamera(std::vector<v1::FrameStored> frames,
                      grpc::Status final_status = grpc::Status::OK)
      : frames_(std::move(frames)), final_status_(std::move(final_status)) {}
  grpc::Status ImageSlice(grpc::ServerContext*, const v1::SliceRequest*,
                          grpc::ServerWriter<v1::FrameStored>* w) override {
    for (const auto& f : frames_) w->Write(f);
    return final_status_;
  }

 private:
  std::vector<v1::FrameStored> frames_;
  grpc::Status final_status_;
};

// A node whose reply encodes both slot identity and die order, so the test
// can verify the exact slot list (not just its size) dispatched per position.
// served counts invocations, and a nonzero hold keeps each invocation in
// flight long enough for distribution tests to force overlapping dispatch.
class FakeNode final : public v1::ComputeNode::Service {
 public:
  explicit FakeNode(std::chrono::milliseconds hold = {}) : hold_(hold) {}
  grpc::Status ProcessPosition(grpc::ServerContext*, const v1::PositionRequest* req,
                               v1::PositionValue* reply) override {
    ++served;
    if (hold_.count() > 0) std::this_thread::sleep_for(hold_);
    double v = 10000.0 * req->frame_in_die();
    for (int d = 0; d < req->slots_size(); ++d) v += double(req->slots(d)) * (d + 1);
    reply->set_value(v);
    return grpc::Status::OK;
  }
  std::atomic<int> served{0};

 private:
  std::chrono::milliseconds hold_;
};

// A node that always fails, used to verify that a node failure aborts the
// slice and still releases the slots the camera already handed out.
class FailingNode final : public v1::ComputeNode::Service {
 public:
  grpc::Status ProcessPosition(grpc::ServerContext*, const v1::PositionRequest*,
                               v1::PositionValue*) override {
    return {grpc::StatusCode::INTERNAL, "boom"};
  }
};

// A node that stalls instead of answering — the hung-peer case, distinct from
// FailingNode's crashed-peer case. Polling IsCancelled lets the handler exit
// as soon as the caller's deadline fires, so server shutdown never blocks on it.
class HangingNode final : public v1::ComputeNode::Service {
 public:
  grpc::Status ProcessPosition(grpc::ServerContext* ctx, const v1::PositionRequest*,
                               v1::PositionValue*) override {
    while (!ctx->IsCancelled())
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return {grpc::StatusCode::CANCELLED, "hung until cancelled"};
  }
};

namespace {
struct Served {
  std::unique_ptr<grpc::Server> server;
  std::shared_ptr<grpc::Channel> channel;
  std::string endpoint;
};

template <typename Service>
Served serve(Service& s) {
  grpc::ServerBuilder b;
  int port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
  b.RegisterService(&s);
  auto server = b.BuildAndStart();
  std::string endpoint = "localhost:" + std::to_string(port);
  auto channel = grpc::CreateChannel(endpoint, grpc::InsecureChannelCredentials());
  return {std::move(server), channel, endpoint};
}

// Registers a node endpoint the way node_main does, through the Ⅶ RPC handler.
void register_node(ControllerService& controller, const std::string& endpoint) {
  v1::NodeEndpoint ep;
  ep.set_endpoint(endpoint);
  v1::Nothing nothing;
  ASSERT_TRUE(controller.RegisterNode(nullptr, &ep, &nothing).ok());
}

v1::FrameStored fs(std::uint32_t die, std::uint32_t frame, std::uint64_t slot) {
  v1::FrameStored m;
  m.set_die(die);
  m.set_frame_in_die(frame);
  m.set_slot(slot);
  return m;
}
}  // namespace

TEST(ControllerService, GathersOneValuePerPositionAndReleasesSlots) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(8, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test", 8 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test", true), frame_bytes);

  // Claim 6 slots from the manager so the controller's release is legal.
  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 6; ++i) slots.push_back(box.write_frame(px));

  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1]), fs(1, 0, slots[2]),
                     fs(1, 1, slots[3]), fs(2, 0, slots[4]), fs(2, 1, slots[5])});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FakeNode node_a, node_b;
  auto [na_server, na_channel, na_ep] = serve(node_a);
  auto [nb_server, nb_channel, nb_ep] = serve(node_b);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  register_node(controller, nb_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  ASSERT_TRUE(stub->ProcessSlice(&ctx, req, &results).ok());
  ASSERT_EQ(results.values_size(), 2);  // 2 frame-in-die positions

  // Position 0's slot list is (die0,pos0), (die1,pos0), (die2,pos0) in die order.
  EXPECT_DOUBLE_EQ(results.values(0),
                   double(slots[0]) * 1 + double(slots[2]) * 2 + double(slots[4]) * 3);
  // Position 1's slot list is (die0,pos1), (die1,pos1), (die2,pos1) in die order.
  EXPECT_DOUBLE_EQ(results.values(1), 10000.0 * 1 + double(slots[1]) * 1 +
                                          double(slots[3]) * 2 + double(slots[5]) * 3);

  // All six slots were released — the manager's pool is full again.
  for (int i = 0; i < 6; ++i) EXPECT_NO_THROW(box.write_frame(px));

  ctrl_server->Shutdown(); nb_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, DistributesPositionsAcrossFreeNodes) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(8, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-dist", 8 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-dist", true), frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<v1::FrameStored> frames;
  for (std::uint32_t p = 0; p < 8; ++p) frames.push_back(fs(0, p, box.write_frame(px)));
  FakeCamera camera(frames);
  auto [cam_server, cam_channel, cam_ep] = serve(camera);

  // Each invocation is held in flight, so consecutive positions must check
  // out different nodes — dispatch frozen on one node would leave the other
  // at zero served.
  FakeNode node_a(std::chrono::milliseconds(50)), node_b(std::chrono::milliseconds(50));
  auto [na_server, na_channel, na_ep] = serve(node_a);
  auto [nb_server, nb_channel, nb_ep] = serve(node_b);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  register_node(controller, nb_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  ASSERT_TRUE(stub->ProcessSlice(&ctx, req, &results).ok());
  ASSERT_EQ(results.values_size(), 8);
  EXPECT_GT(node_a.served.load(), 0);
  EXPECT_GT(node_b.served.load(), 0);

  ctrl_server->Shutdown(); nb_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, CameraErrorPropagates) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(2, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-cam-err", 2 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-cam-err", true),
            frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 2; ++i) slots.push_back(box.write_frame(px));

  // Streams 2 frames, then fails the slice — as if the camera died mid-stream.
  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1])},
                    {grpc::StatusCode::UNAVAILABLE, "camera died"});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FakeNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::UNAVAILABLE);

  // Both streamed slots were released — the full pool is available again.
  for (int i = 0; i < 2; ++i) EXPECT_NO_THROW(box.write_frame(px));

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, EmptyStreamFailsPrecondition) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(1, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-empty", 1 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-empty", true),
            frame_bytes);

  FakeCamera camera({});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FakeNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, IncompleteSliceFailsPrecondition) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(3, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-incomplete", 3 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-incomplete", true),
            frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 3; ++i) slots.push_back(box.write_frame(px));

  // 2 dies x 2 positions expected, but (die1,pos1) is missing.
  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1]), fs(1, 0, slots[2])});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FakeNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  // All three streamed slots were released — the full pool is available again.
  for (int i = 0; i < 3; ++i) EXPECT_NO_THROW(box.write_frame(px));

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, NodeFailureAbortsAndReleases) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(6, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-node-fail", 6 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-node-fail", true),
            frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 6; ++i) slots.push_back(box.write_frame(px));

  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1]), fs(1, 0, slots[2]),
                     fs(1, 1, slots[3]), fs(2, 0, slots[4]), fs(2, 1, slots[5])});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FailingNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  ControllerService controller(cam_channel, box);
  register_node(controller, na_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::INTERNAL);
  // The failing node was evicted — the next slice will not dispatch to it.
  EXPECT_EQ(controller.registered_node_count(), 0u);

  // All six slice slots were released — the manager's pool is full again.
  for (int i = 0; i < 6; ++i) EXPECT_NO_THROW(box.write_frame(px));

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, NoNodesFailsPrecondition) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(1, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-no-nodes", 1 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-no-nodes", true),
            frame_bytes);

  FakeCamera camera({});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);

  // Short first-node wait: the barrier expiring, not a 10 s default, is under test.
  ControllerService controller(cam_channel, box,
                               {.first_node_wait = std::chrono::milliseconds(50)});
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::FAILED_PRECONDITION);

  ctrl_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, SliceWaitsForFirstRegistration) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(2, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-wait-reg", 2 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-wait-reg", true),
            frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 2; ++i) slots.push_back(box.write_frame(px));

  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1])});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  FakeNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  ControllerService controller(cam_channel, box,
                               {.first_node_wait = std::chrono::seconds(5)});
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  // The slice arrives while the pool is empty; it must block on the startup
  // barrier and complete once the first node registers.
  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st;
  std::thread slice([&] { st = stub->ProcessSlice(&ctx, req, &results); });
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  register_node(controller, na_ep);
  slice.join();
  ASSERT_TRUE(st.ok()) << st.error_message();
  EXPECT_EQ(results.values_size(), 2);

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, HungNodeTimesOutAbortsAndEvicts) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(2, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-hang", 2 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-hang", true),
            frame_bytes);

  std::vector<std::byte> px(frame_bytes, std::byte{0});
  std::vector<std::uint64_t> slots;
  for (int i = 0; i < 2; ++i) slots.push_back(box.write_frame(px));

  FakeCamera camera({fs(0, 0, slots[0]), fs(0, 1, slots[1])});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);
  HangingNode node_a;
  auto [na_server, na_channel, na_ep] = serve(node_a);

  // Tight position deadline: the timeout firing, not the minute-long default,
  // is under test.
  ControllerService controller(cam_channel, box,
                               {.position_deadline = std::chrono::milliseconds(200)});
  register_node(controller, na_ep);
  auto [ctrl_server, ctrl_channel, ctrl_ep] = serve(controller);
  auto stub = v1::Controller::NewStub(ctrl_channel);

  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  v1::SliceResults results;
  grpc::Status st = stub->ProcessSlice(&ctx, req, &results);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  // The stalled node was evicted, and the slice's slots were still released.
  EXPECT_EQ(controller.registered_node_count(), 0u);
  for (int i = 0; i < 2; ++i) EXPECT_NO_THROW(box.write_frame(px));

  ctrl_server->Shutdown(); na_server->Shutdown();
  cam_server->Shutdown(); mgr_server->Shutdown();
}

TEST(ControllerService, ReRegistrationReplacesInsteadOfDuplicating) {
  const std::size_t frame_bytes = 64;
  ManagerService manager(1, frame_bytes);
  auto segment = ShmSegment::create("/algctl-ctrl-test-rereg", 1 * frame_bytes);
  auto [mgr_server, mgr_channel, mgr_ep] = serve(manager);
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-ctrl-test-rereg", true),
            frame_bytes);

  FakeCamera camera({});
  auto [cam_server, cam_channel, cam_ep] = serve(camera);

  ControllerService controller(cam_channel, box);
  register_node(controller, "localhost:59991");
  register_node(controller, "localhost:59991");  // restarted node, same endpoint
  register_node(controller, "localhost:59992");
  EXPECT_EQ(controller.registered_node_count(), 2u);

  v1::NodeEndpoint empty;
  v1::Nothing nothing;
  EXPECT_EQ(controller.RegisterNode(nullptr, &empty, &nothing).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);

  cam_server->Shutdown(); mgr_server->Shutdown();
}
