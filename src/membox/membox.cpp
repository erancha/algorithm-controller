#include "membox/membox.hpp"
#include <cstring>
#include <stdexcept>
#include "inspection.grpc.pb.h"

namespace inspection {

Membox::Membox(std::shared_ptr<grpc::Channel> manager_channel, ShmSegment segment,
               std::size_t frame_bytes, std::chrono::milliseconds rpc_deadline)
    : stub_(v1::MemboxManager::NewStub(std::move(manager_channel))),
      segment_(std::move(segment)), frame_bytes_(frame_bytes), rpc_deadline_(rpc_deadline) {}

SlotOffset Membox::write_frame(std::span<const std::byte> pixels) {
  if (pixels.size() != frame_bytes_) throw std::invalid_argument("wrong frame size");
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + rpc_deadline_);
  v1::AllocRequest req;
  v1::SlotReply reply;
  grpc::Status st = stub_->AllocSlot(&ctx, req, &reply);
  if (!st.ok()) throw std::runtime_error("AllocSlot: " + st.error_message());
  if (reply.slot() % frame_bytes_ != 0 || reply.slot() + frame_bytes_ > segment_.size())
    throw std::out_of_range("manager returned invalid slot offset");
  std::memcpy(segment_.data() + reply.slot(), pixels.data(), frame_bytes_);
  return reply.slot();
}

std::span<const std::byte> Membox::read_frame(SlotOffset slot) const {
  if (slot % frame_bytes_ != 0 || slot + frame_bytes_ > segment_.size())
    throw std::out_of_range("invalid slot offset");
  return {segment_.data() + slot, frame_bytes_};
}

void Membox::release_slots(std::span<const SlotOffset> slots) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + rpc_deadline_);
  v1::ReleaseRequest req;
  for (auto s : slots) req.add_slots(s);
  v1::Nothing nothing;
  grpc::Status st = stub_->ReleaseSlots(&ctx, req, &nothing);
  if (!st.ok()) throw std::runtime_error("ReleaseSlots: " + st.error_message());
}

}  // namespace inspection
