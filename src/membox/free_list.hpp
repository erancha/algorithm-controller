#pragma once
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <vector>

namespace inspection {

// Owns which slots of the memory box are in use. Offsets are byte offsets,
// always multiples of frame_bytes.
class FreeList {
 public:
  FreeList(std::size_t slot_count, std::size_t frame_bytes)
      : frame_bytes_(frame_bytes), in_use_(slot_count, false) {
    for (std::size_t i = slot_count; i-- > 0;) free_.push_back(i * frame_bytes);
  }

  std::optional<std::uint64_t> alloc() {
    if (free_.empty()) return std::nullopt;
    std::uint64_t off = free_.back();
    free_.pop_back();
    in_use_[off / frame_bytes_] = true;
    return off;
  }

  void release(std::uint64_t offset) {
    if (offset % frame_bytes_ != 0) throw std::invalid_argument("misaligned offset");
    std::size_t idx = offset / frame_bytes_;
    if (idx >= in_use_.size()) throw std::invalid_argument("offset out of range");
    if (!in_use_[idx]) throw std::invalid_argument("offset not allocated");
    in_use_[idx] = false;
    free_.push_back(offset);
  }

  std::size_t free_count() const { return free_.size(); }

 private:
  std::size_t frame_bytes_;
  std::vector<std::uint64_t> free_;
  std::vector<bool> in_use_;
};

}  // namespace inspection
