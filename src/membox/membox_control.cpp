#include "membox/membox_control.hpp"
#include <stdexcept>

namespace inspection {

MemboxControl::MemboxControl(std::shared_ptr<grpc::Channel> manager_channel,
                             std::chrono::milliseconds rpc_deadline)
    : stub_(v1::MemboxManager::NewStub(std::move(manager_channel))),
      rpc_deadline_(rpc_deadline) {}

SlotOffset MemboxControl::alloc_slot() {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + rpc_deadline_);
  v1::AllocRequest req;
  v1::SlotReply reply;
  grpc::Status st = stub_->AllocSlot(&ctx, req, &reply);
  if (!st.ok()) throw std::runtime_error("AllocSlot: " + st.error_message());
  return reply.slot();
}

void MemboxControl::release_slots(std::span<const SlotOffset> slots) {
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + rpc_deadline_);
  v1::ReleaseRequest req;
  for (auto s : slots) req.add_slots(s);
  v1::Nothing nothing;
  grpc::Status st = stub_->ReleaseSlots(&ctx, req, &nothing);
  if (!st.ok()) throw std::runtime_error("ReleaseSlots: " + st.error_message());
}

}  // namespace inspection
