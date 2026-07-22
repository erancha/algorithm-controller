#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace inspection {

struct Geometry {
  std::uint32_t frames_per_die = 24;
  std::uint32_t frame_width = 256;
  std::uint32_t frame_height = 256;
};

struct Defect {
  std::uint32_t die, frame_in_die, x, y, radius;
  int delta;
};

std::vector<std::uint8_t> circuit_pattern(std::uint32_t frame_in_die, const Geometry& g);

void generate_fixtures(const std::filesystem::path& root, const Geometry& g,
                       std::span<const std::uint32_t> dies_per_slice,
                       std::span<const Defect> defects);

}  // namespace inspection
