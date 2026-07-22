#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <filesystem>
#include "camera/camera_service.hpp"
#include "camera/fixtures.hpp"
#include "membox/manager_service.hpp"
#include "membox/membox.hpp"

using namespace inspection;
namespace fs = std::filesystem;

TEST(CameraService, StreamsEveryFrameWithPlacements) {
  Geometry g{.frames_per_die = 3, .frame_width = 16, .frame_height = 16};
  fs::path root = fs::temp_directory_path() / "algctl-camera-test";
  fs::remove_all(root);
  std::uint32_t dies[] = {2};
  generate_fixtures(root, g, dies, {});

  const std::size_t frame_bytes = 16 * 16;
  ManagerService manager(/*slot_count=*/8, frame_bytes);
  auto segment = ShmSegment::create("/algctl-camera-test", 8 * frame_bytes);

  grpc::ServerBuilder b;
  int mgr_port = 0, cam_port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &mgr_port);
  b.RegisterService(&manager);
  auto mgr_server = b.BuildAndStart();

  auto mgr_channel = grpc::CreateChannel("localhost:" + std::to_string(mgr_port),
                                         grpc::InsecureChannelCredentials());
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-camera-test", true), frame_bytes);
  CameraService camera(root, box, /*frame_rate=*/0);

  grpc::ServerBuilder cb;
  cb.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &cam_port);
  cb.RegisterService(&camera);
  auto cam_server = cb.BuildAndStart();

  auto stub = v1::Camera::NewStub(grpc::CreateChannel(
      "localhost:" + std::to_string(cam_port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  auto reader = stub->ImageSlice(&ctx, req);
  int count = 0;
  v1::FrameStored fs_msg;
  while (reader->Read(&fs_msg)) {
    EXPECT_LT(fs_msg.die(), 2u);
    EXPECT_LT(fs_msg.frame_in_die(), 3u);
    ++count;
  }
  EXPECT_TRUE(reader->Finish().ok());
  EXPECT_EQ(count, 2 * 3);  // every (die, frame) reported exactly once

  grpc::ClientContext ctx2;
  v1::SliceRequest missing;
  missing.set_slice_id(99);
  auto r2 = stub->ImageSlice(&ctx2, missing);
  v1::FrameStored unused;
  while (r2->Read(&unused)) {}
  EXPECT_EQ(r2->Finish().error_code(), grpc::StatusCode::NOT_FOUND);

  cam_server->Shutdown();
  mgr_server->Shutdown();
}

TEST(CameraService, ClientGoneMidStreamReleasesTheUnreportedSlot) {
  Geometry g{.frames_per_die = 3, .frame_width = 16, .frame_height = 16};
  fs::path root = fs::temp_directory_path() / "algctl-camera-cancel-test";
  fs::remove_all(root);
  std::uint32_t dies[] = {1};
  generate_fixtures(root, g, dies, {});

  const std::size_t frame_bytes = 16 * 16;
  ManagerService manager(/*slot_count=*/8, frame_bytes);
  auto segment = ShmSegment::create("/algctl-camera-cancel-test", 8 * frame_bytes);

  grpc::ServerBuilder b;
  int mgr_port = 0, cam_port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &mgr_port);
  b.RegisterService(&manager);
  auto mgr_server = b.BuildAndStart();

  auto mgr_channel = grpc::CreateChannel("localhost:" + std::to_string(mgr_port),
                                         grpc::InsecureChannelCredentials());
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-camera-cancel-test", true),
            frame_bytes);
  // Slow pacing gives the cancellation time to land while the camera sleeps
  // between frame 0 and frame 1.
  CameraService camera(root, box, /*frame_rate=*/3);

  grpc::ServerBuilder cb;
  cb.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &cam_port);
  cb.RegisterService(&camera);
  auto cam_server = cb.BuildAndStart();

  auto stub = v1::Camera::NewStub(grpc::CreateChannel(
      "localhost:" + std::to_string(cam_port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  auto reader = stub->ImageSlice(&ctx, req);
  v1::FrameStored first;
  ASSERT_TRUE(reader->Read(&first));
  ctx.TryCancel();
  v1::FrameStored rest;
  while (reader->Read(&rest)) {}
  EXPECT_EQ(reader->Finish().error_code(), grpc::StatusCode::CANCELLED);

  // Shutdown blocks until the handler finishes writing frame 1, failing its
  // report, and releasing that slot — after it, the pool state is settled.
  cam_server->Shutdown();

  // 8 slots: frame 0's stays allocated (it was reported — releasing it is the
  // reader's job), frame 1's was written but unreported and must be back in
  // the pool. Exactly 7 allocations succeed.
  std::vector<std::byte> px(frame_bytes, std::byte{1});
  for (int i = 0; i < 7; ++i) EXPECT_NO_THROW(box.write_frame(px));
  EXPECT_THROW(box.write_frame(px), std::runtime_error);

  mgr_server->Shutdown();
}

TEST(CameraService, ImageSliceReturnsInternalWithDiagnosableMessageOnSlotExhaustion) {
  Geometry g{.frames_per_die = 3, .frame_width = 16, .frame_height = 16};
  fs::path root = fs::temp_directory_path() / "algctl-camera-exhaustion-test";
  fs::remove_all(root);
  std::uint32_t dies[] = {2};
  generate_fixtures(root, g, dies, {});

  const std::size_t frame_bytes = 16 * 16;
  // Fewer slots than the slice has frames (2 dies x 3 frames = 6), so
  // write_frame is guaranteed to throw partway through the slice.
  ManagerService manager(/*slot_count=*/2, frame_bytes);
  auto segment = ShmSegment::create("/algctl-camera-exhaustion-test", 2 * frame_bytes);

  grpc::ServerBuilder b;
  int mgr_port = 0, cam_port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &mgr_port);
  b.RegisterService(&manager);
  auto mgr_server = b.BuildAndStart();

  auto mgr_channel = grpc::CreateChannel("localhost:" + std::to_string(mgr_port),
                                         grpc::InsecureChannelCredentials());
  Membox box(mgr_channel, ShmSegment::open_existing("/algctl-camera-exhaustion-test", true),
            frame_bytes);
  CameraService camera(root, box, /*frame_rate=*/0);

  grpc::ServerBuilder cb;
  cb.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &cam_port);
  cb.RegisterService(&camera);
  auto cam_server = cb.BuildAndStart();

  auto stub = v1::Camera::NewStub(grpc::CreateChannel(
      "localhost:" + std::to_string(cam_port), grpc::InsecureChannelCredentials()));
  grpc::ClientContext ctx;
  v1::SliceRequest req;
  req.set_slice_id(0);
  auto reader = stub->ImageSlice(&ctx, req);
  v1::FrameStored fs_msg;
  while (reader->Read(&fs_msg)) {}
  grpc::Status status = reader->Finish();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_FALSE(status.error_message().empty());
  EXPECT_NE(status.error_message().find("AllocSlot"), std::string::npos);

  cam_server->Shutdown();
  mgr_server->Shutdown();
}
