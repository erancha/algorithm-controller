#include "membox/membox.hpp"
#include <cstring>
#include <stdexcept>

namespace inspection {

Membox::Membox(std::shared_ptr<grpc::Channel> manager_channel, ShmSegment segment,
               std::size_t frame_bytes, std::chrono::milliseconds rpc_deadline)
    : control_(std::move(manager_channel), rpc_deadline),
      segment_(std::move(segment)), frame_bytes_(frame_bytes) {}

SlotOffset Membox::write_frame(std::span<const std::byte> pixels) {
  if (pixels.size() != frame_bytes_) throw std::invalid_argument("wrong frame size");
  SlotOffset slot = control_.alloc_slot();
  if (slot % frame_bytes_ != 0 || slot + frame_bytes_ > segment_.size())
    throw std::out_of_range("manager returned invalid slot offset");
  std::memcpy(segment_.data() + slot, pixels.data(), frame_bytes_);
  return slot;
}

std::span<const std::byte> Membox::read_frame(SlotOffset slot) const {
  if (slot % frame_bytes_ != 0 || slot + frame_bytes_ > segment_.size())
    throw std::out_of_range("invalid slot offset");
  return {segment_.data() + slot, frame_bytes_};
}

void Membox::release_slots(std::span<const SlotOffset> slots) { control_.release_slots(slots); }

}  // namespace inspection
