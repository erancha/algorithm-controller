#pragma once
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdint>
#include <memory>
#include <span>
#include "inspection.grpc.pb.h"

namespace inspection {

using SlotOffset = std::uint64_t;

// Slot bookkeeping on the memory box, over gRPC to membox-manager only: allocates and releases
// slots, never touches pixels and maps no shared memory. A process that only decides which slots
// to free (the controller) uses this alone, so it can run on a machine without the segment.
class MemboxControl {
 public:
  // rpc_deadline bounds each call — the calls move a few slot numbers, so a hung manager
  // surfaces as a timeout error, never a stall.
  explicit MemboxControl(std::shared_ptr<grpc::Channel> manager_channel,
                         std::chrono::milliseconds rpc_deadline = std::chrono::seconds(10));

  SlotOffset alloc_slot();
  void release_slots(std::span<const SlotOffset> slots);

 private:
  std::unique_ptr<v1::MemboxManager::Stub> stub_;
  std::chrono::milliseconds rpc_deadline_;
};

}  // namespace inspection
