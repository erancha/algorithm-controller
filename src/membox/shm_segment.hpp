#pragma once
#include <cstddef>
#include <string>

namespace inspection {

// One mapped POSIX shared-memory region. The creating instance owns the name
// and unlinks it on destruction; open_existing instances only unmap.
class ShmSegment {
 public:
  static ShmSegment create(const std::string& name, std::size_t bytes);
  static ShmSegment open_existing(const std::string& name, bool writable);

  ShmSegment(ShmSegment&&) noexcept;
  ShmSegment& operator=(ShmSegment&&) noexcept;
  ShmSegment(const ShmSegment&) = delete;
  ShmSegment& operator=(const ShmSegment&) = delete;
  ~ShmSegment();

  std::byte* data() { return base_; }
  const std::byte* data() const { return base_; }
  std::size_t size() const { return size_; }

 private:
  ShmSegment(std::string name, std::byte* base, std::size_t size, bool owner)
      : name_(std::move(name)), base_(base), size_(size), owner_(owner) {}
  std::string name_;
  std::byte* base_ = nullptr;
  std::size_t size_ = 0;
  bool owner_ = false;
};

}  // namespace inspection
