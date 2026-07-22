#include <gtest/gtest.h>
#include <cstring>
#include "membox/shm_segment.hpp"

using namespace inspection;

TEST(ShmSegment, WriterAndReaderShareBytes) {
  const std::string name = "/algctl-shm-test";
  auto owner = ShmSegment::create(name, 4096);
  ASSERT_EQ(owner.size(), 4096u);
  std::memcpy(owner.data(), "hello", 5);

  auto reader = ShmSegment::open_existing(name, /*writable=*/false);
  EXPECT_EQ(std::memcmp(reader.data(), "hello", 5), 0);
}

TEST(ShmSegment, OpenMissingThrows) {
  EXPECT_THROW(ShmSegment::open_existing("/algctl-shm-never-created", false),
               std::runtime_error);
}

TEST(ShmSegment, CreateFailureUnlinksTheName) {
  const std::string name = "/algctl-shm-mapfail-test";
  EXPECT_THROW(ShmSegment::create(name, 0), std::runtime_error);  // mmap of length 0 fails
  EXPECT_THROW(ShmSegment::open_existing(name, false), std::runtime_error);
}
