#pragma once
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <cstdint>
#include <span>
#include "inspection.grpc.pb.h"
#include "membox/shm_segment.hpp"

namespace inspection {

using SlotOffset = std::uint64_t;

// Client view of the memory box: pixels through the mapped segment, slot
// bookkeeping through the manager. read_frame returns a span aliasing the
// mapping — the caller must not use it after the slot is released.
class Membox {
 public:
  // rpc_deadline bounds each bookkeeping call — the calls move a few slot
  // numbers, so a hung manager surfaces as a timeout error, never a stall.
  Membox(std::shared_ptr<grpc::Channel> manager_channel, ShmSegment segment,
         std::size_t frame_bytes,
         std::chrono::milliseconds rpc_deadline = std::chrono::seconds(10));

  SlotOffset write_frame(std::span<const std::byte> pixels);
  std::span<const std::byte> read_frame(SlotOffset slot) const;
  void release_slots(std::span<const SlotOffset> slots);

 private:
  std::unique_ptr<v1::MemboxManager::Stub> stub_;
  ShmSegment segment_;
  std::size_t frame_bytes_;
  std::chrono::milliseconds rpc_deadline_;
};

}  // namespace inspection
