#pragma once
#include <mutex>
#include "inspection.grpc.pb.h"
#include "membox/free_list.hpp"

namespace inspection {

class ManagerService final : public v1::MemboxManager::Service {
 public:
  ManagerService(std::size_t slot_count, std::size_t frame_bytes)
      : free_list_(slot_count, frame_bytes) {}

  grpc::Status AllocSlot(grpc::ServerContext*, const v1::AllocRequest*,
                         v1::SlotReply* reply) override {
    std::lock_guard lock(mu_);
    auto off = free_list_.alloc();
    if (!off) return {grpc::StatusCode::RESOURCE_EXHAUSTED, "no free slot"};
    reply->set_slot(*off);
    return grpc::Status::OK;
  }

  grpc::Status ReleaseSlots(grpc::ServerContext*, const v1::ReleaseRequest* req,
                            v1::Nothing*) override {
    std::lock_guard lock(mu_);
    try {
      for (auto s : req->slots()) free_list_.release(s);
    } catch (const std::invalid_argument& e) {
      return {grpc::StatusCode::INVALID_ARGUMENT, e.what()};
    }
    return grpc::Status::OK;
  }

 private:
  std::mutex mu_;
  FreeList free_list_;
};

}  // namespace inspection
