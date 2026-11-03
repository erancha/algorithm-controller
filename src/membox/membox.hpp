#pragma once
#include <grpcpp/grpcpp.h>
#include <chrono>
#include <span>
#include "membox/membox_control.hpp"
#include "membox/shm_segment.hpp"

namespace inspection {

// Client view of the memory box for processes that move pixels (camera, compute nodes): pixels
// through the mapped segment, slot bookkeeping through MemboxControl. read_frame returns a span
// aliasing the mapping — the caller must not use it after the slot is released.
class Membox {
 public:
  Membox(std::shared_ptr<grpc::Channel> manager_channel, ShmSegment segment,
         std::size_t frame_bytes,
         std::chrono::milliseconds rpc_deadline = std::chrono::seconds(10));

  SlotOffset write_frame(std::span<const std::byte> pixels);
  std::span<const std::byte> read_frame(SlotOffset slot) const;
  void release_slots(std::span<const SlotOffset> slots);

 private:
  MemboxControl control_;
  ShmSegment segment_;
  std::size_t frame_bytes_;
};

}  // namespace inspection
