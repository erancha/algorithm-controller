#include <gtest/gtest.h>
#include <set>
#include "membox/free_list.hpp"

using namespace inspection;

TEST(FreeList, AllocatesDistinctAlignedOffsetsUntilExhausted) {
  FreeList fl(3, 100);
  std::set<std::uint64_t> got;
  for (int i = 0; i < 3; ++i) {
    auto off = fl.alloc();
    ASSERT_TRUE(off.has_value());
    EXPECT_EQ(*off % 100, 0u);
    got.insert(*off);
  }
  EXPECT_EQ(got.size(), 3u);
  EXPECT_FALSE(fl.alloc().has_value());
}

TEST(FreeList, ReleaseMakesSlotReusable) {
  FreeList fl(1, 64);
  auto off = fl.alloc();
  ASSERT_TRUE(off.has_value());
  fl.release(*off);
  EXPECT_EQ(fl.free_count(), 1u);
  EXPECT_TRUE(fl.alloc().has_value());
}

TEST(FreeList, InvalidReleaseThrows) {
  FreeList fl(2, 64);
  EXPECT_THROW(fl.release(64), std::invalid_argument);   // never allocated
  EXPECT_THROW(fl.release(13), std::invalid_argument);   // misaligned
  EXPECT_THROW(fl.release(1024), std::invalid_argument); // out of range
}
