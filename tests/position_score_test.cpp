#include <gtest/gtest.h>
#include <vector>
#include "compute_node/algorithm/position_score.hpp"

using namespace inspection;

namespace {
std::vector<std::byte> flat(std::size_t n, std::uint8_t v) {
  return std::vector<std::byte>(n, std::byte{v});
}
std::vector<std::span<const std::byte>> views(const std::vector<std::vector<std::byte>>& fs) {
  return {fs.begin(), fs.end()};
}
}  // namespace

TEST(PositionScore, IdenticalFramesScoreZero) {
  std::vector<std::vector<std::byte>> frames(5, flat(100, 50));
  auto v = views(frames);
  EXPECT_DOUBLE_EQ(position_score(v), 0.0);
}

TEST(PositionScore, OneDefectiveDieDominatesTheScore) {
  std::vector<std::vector<std::byte>> frames(7, flat(100, 50));
  for (int i = 0; i < 10; ++i) frames[3][i] = std::byte{200};  // defect blob in die 3
  auto v = views(frames);
  // Median stays 50 (6 of 7 dies agree), die 3 deviates by 150 on 10% of pixels.
  EXPECT_NEAR(position_score(v), 15.0, 0.01);
}

TEST(PositionScore, MedianIsRobustToTheDefectiveDie) {
  std::vector<std::vector<std::byte>> frames(7, flat(100, 50));
  for (auto& b : frames[2]) b = std::byte{255};  // an entirely wrong die
  auto v = views(frames);
  // The clean dies still score 0 against the median; only die 2 deviates (by 205).
  EXPECT_NEAR(position_score(v), 205.0, 0.01);
}

TEST(PositionScore, InvalidInputThrows) {
  std::vector<std::vector<std::byte>> none;
  auto v0 = views(none);
  EXPECT_THROW(position_score(v0), std::invalid_argument);
  std::vector<std::vector<std::byte>> uneven = {flat(4, 1), flat(5, 1)};
  auto v1 = views(uneven);
  EXPECT_THROW(position_score(v1), std::invalid_argument);
  std::vector<std::vector<std::byte>> empties(3);  // dies agree on zero-length frames
  auto v2 = views(empties);
  EXPECT_THROW(position_score(v2), std::invalid_argument);
}
