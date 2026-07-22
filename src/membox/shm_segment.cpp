#include "membox/shm_segment.hpp"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdexcept>
#include <utility>

namespace inspection {

namespace {
std::byte* map_fd(int fd, std::size_t bytes, bool writable) {
  int prot = PROT_READ | (writable ? PROT_WRITE : 0);
  void* p = ::mmap(nullptr, bytes, prot, MAP_SHARED, fd, 0);
  ::close(fd);
  if (p == MAP_FAILED) throw std::runtime_error("mmap failed");
  return static_cast<std::byte*>(p);
}
}  // namespace

ShmSegment ShmSegment::create(const std::string& name, std::size_t bytes) {
  ::shm_unlink(name.c_str());  // a fresh run must not inherit a stale segment
  int fd = ::shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0) throw std::runtime_error("shm_open(create) failed: " + name);
  if (::ftruncate(fd, static_cast<off_t>(bytes)) != 0) {
    ::close(fd);
    ::shm_unlink(name.c_str());
    throw std::runtime_error("ftruncate failed: " + name);
  }
  try {
    return ShmSegment(name, map_fd(fd, bytes, true), bytes, /*owner=*/true);
  } catch (...) {
    ::shm_unlink(name.c_str());
    throw;
  }
}

ShmSegment ShmSegment::open_existing(const std::string& name, bool writable) {
  int fd = ::shm_open(name.c_str(), writable ? O_RDWR : O_RDONLY, 0);
  if (fd < 0) throw std::runtime_error("shm_open failed: " + name);
  off_t len = ::lseek(fd, 0, SEEK_END);
  if (len <= 0) {
    ::close(fd);
    throw std::runtime_error("empty shm segment: " + name);
  }
  return ShmSegment(name, map_fd(fd, static_cast<std::size_t>(len), writable),
                    static_cast<std::size_t>(len), /*owner=*/false);
}

ShmSegment::ShmSegment(ShmSegment&& o) noexcept
    : name_(std::move(o.name_)), base_(std::exchange(o.base_, nullptr)),
      size_(std::exchange(o.size_, 0)), owner_(std::exchange(o.owner_, false)) {}

ShmSegment& ShmSegment::operator=(ShmSegment&& o) noexcept {
  if (this != &o) {
    this->~ShmSegment();
    new (this) ShmSegment(std::move(o));
  }
  return *this;
}

ShmSegment::~ShmSegment() {
  if (base_) ::munmap(base_, size_);
  if (owner_) ::shm_unlink(name_.c_str());
}

}  // namespace inspection
