#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include <cstring>
#include "membox/manager_service.hpp"
#include "membox/membox.hpp"
#include "membox/shm_segment.hpp"

using namespace inspection;

class MemboxFixture : public ::testing::Test {
 protected:
  void SetUp() override {
    service_ = std::make_unique<ManagerService>(/*slot_count=*/4, /*frame_bytes=*/256);
    grpc::ServerBuilder b;
    int port = 0;
    b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(service_.get());
    server_ = b.BuildAndStart();
    channel_ = grpc::CreateChannel("localhost:" + std::to_string(port),
                                   grpc::InsecureChannelCredentials());
    segment_ = std::make_unique<ShmSegment>(ShmSegment::create("/algctl-membox-test", 4 * 256));
  }
  void TearDown() override { server_->Shutdown(); }

  std::unique_ptr<ManagerService> service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<ShmSegment> segment_;
};

TEST_F(MemboxFixture, WriteThenReadAliasesSameBytes) {
  Membox box(channel_, ShmSegment::open_existing("/algctl-membox-test", true), 256);
  std::vector<std::byte> px(256, std::byte{0xAB});
  SlotOffset slot = box.write_frame(px);
  auto view = box.read_frame(slot);
  ASSERT_EQ(view.size(), 256u);
  EXPECT_EQ(view[0], std::byte{0xAB});
  // Zero-copy: box's view and the fixture's independent mapping of the same shm
  // object see identical bytes at the slot offset, rather than view holding a
  // private copy. The two mappings are separate, concurrently-live VMAs, so their
  // base addresses cannot be compared directly — only content aliasing can.
  EXPECT_EQ(std::memcmp(view.data(), segment_->data() + slot, view.size()), 0);
  // Mutating through the fixture's own mapping must be visible through the
  // earlier view — content equality alone cannot distinguish a copy.
  segment_->data()[slot] = std::byte{0x5C};
  EXPECT_EQ(view[0], std::byte{0x5C});
}

TEST_F(MemboxFixture, ReleaseReturnsSlotsToTheManager) {
  Membox box(channel_, ShmSegment::open_existing("/algctl-membox-test", true), 256);
  std::vector<std::byte> px(256, std::byte{1});
  std::vector<SlotOffset> slots;
  for (int i = 0; i < 4; ++i) slots.push_back(box.write_frame(px));
  box.release_slots(slots);
  EXPECT_NO_THROW(box.write_frame(px));  // pool refilled
}

TEST_F(MemboxFixture, ExhaustionAndBadOffsetFailFast) {
  Membox box(channel_, ShmSegment::open_existing("/algctl-membox-test", true), 256);
  std::vector<std::byte> px(256, std::byte{1});
  for (int i = 0; i < 4; ++i) box.write_frame(px);
  EXPECT_THROW(box.write_frame(px), std::runtime_error);   // no free slot
  EXPECT_THROW(box.read_frame(13), std::out_of_range);     // misaligned
  EXPECT_THROW(box.read_frame(4 * 256), std::out_of_range);// past the end
}

// Manager stand-in that always hands back a misaligned slot, exercising the
// same offset validation on the write path that a compromised or buggy
// manager would otherwise bypass.
class BadOffsetManagerService final : public v1::MemboxManager::Service {
 public:
  grpc::Status AllocSlot(grpc::ServerContext*, const v1::AllocRequest*,
                         v1::SlotReply* reply) override {
    reply->set_slot(13);
    return grpc::Status::OK;
  }
};

TEST(MemboxBadManager, WriteFrameRejectsManagerSuppliedMisalignedSlot) {
  BadOffsetManagerService service;
  grpc::ServerBuilder b;
  int port = 0;
  b.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &port);
  b.RegisterService(&service);
  auto server = b.BuildAndStart();

  auto channel = grpc::CreateChannel("localhost:" + std::to_string(port),
                                     grpc::InsecureChannelCredentials());
  Membox box(channel, ShmSegment::create("/algctl-membox-badoffset-test", 4 * 256), 256);
  std::vector<std::byte> px(256, std::byte{1});
  EXPECT_THROW(box.write_frame(px), std::out_of_range);

  server->Shutdown();
}
