#include <gtest/gtest.h>
#include <grpcpp/grpcpp.h>
#include "inspection.grpc.pb.h"

TEST(Proto, MessagesAndStubsExist) {
  inspection::v1::FrameStored fs;
  fs.set_die(3);
  fs.set_frame_in_die(7);
  fs.set_slot(65536);
  EXPECT_EQ(fs.die(), 3u);
  inspection::v1::PositionRequest pr;
  pr.add_slots(0);
  pr.add_slots(65536);
  EXPECT_EQ(pr.slots_size(), 2);
  // Stub types must exist and be constructible from a channel.
  auto ch = grpc::CreateChannel("localhost:1", grpc::InsecureChannelCredentials());
  auto stub = inspection::v1::MemboxManager::NewStub(ch);
  EXPECT_NE(stub, nullptr);
}
