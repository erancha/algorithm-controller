#include <gtest/gtest.h>
#include <cstdio>
#include <filesystem>
#include "common/pgm.hpp"

using namespace inspection;

TEST(Pgm, RoundTrips) {
  std::vector<std::uint8_t> px(6 * 4);
  for (std::size_t i = 0; i < px.size(); ++i) px[i] = static_cast<std::uint8_t>(i * 10);
  auto path = std::filesystem::temp_directory_path() / "pgm_test.pgm";
  write_pgm(path, 6, 4, px);
  Image img = read_pgm(path);
  EXPECT_EQ(img.width, 6u);
  EXPECT_EQ(img.height, 4u);
  EXPECT_EQ(img.pixels, px);
}

TEST(Pgm, ReadMissingFileThrows) {
  EXPECT_THROW(read_pgm("/nonexistent/nope.pgm"), std::runtime_error);
}

TEST(Pgm, TruncatedAfterMaxvalThrows) {
  auto path = std::filesystem::temp_directory_path() / "pgm_truncated.pgm";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fputs("P5\n2 2\n255", f);  // header ends at maxval — no separator, no pixels
  std::fclose(f);
  EXPECT_THROW(read_pgm(path), std::runtime_error);
}

TEST(Pgm, MissingHeaderSeparatorThrows) {
  auto path = std::filesystem::temp_directory_path() / "pgm_bad_sep.pgm";
  std::FILE* f = std::fopen(path.c_str(), "wb");
  ASSERT_NE(f, nullptr);
  std::fputs("P5\n2 2\n255", f);  // no whitespace after maxval...
  std::fputc('X', f);             // ...a non-whitespace byte instead
  std::fwrite("abcd", 1, 4, f);   // followed by a full 2x2 pixel payload
  std::fclose(f);
  EXPECT_THROW(read_pgm(path), std::runtime_error);
}
