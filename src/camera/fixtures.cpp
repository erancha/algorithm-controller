#include "camera/fixtures.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include "common/pgm.hpp"

namespace inspection {

namespace {
// Deterministic per-(die, frame) noise so every regeneration is identical.
std::uint32_t xorshift(std::uint32_t& s) {
  s ^= s << 13; s ^= s >> 17; s ^= s << 5;
  return s;
}
}  // namespace

std::vector<std::uint8_t> circuit_pattern(std::uint32_t frame_in_die, const Geometry& g) {
  std::vector<std::uint8_t> px(std::size_t{g.frame_width} * g.frame_height);
  for (std::uint32_t y = 0; y < g.frame_height; ++y)
    for (std::uint32_t x = 0; x < g.frame_width; ++x)
      px[std::size_t{y} * g.frame_width + x] =
          static_cast<std::uint8_t>(32 + ((x / 8 + y / 8 + frame_in_die * 7) % 6) * 32);
  return px;
}

void generate_fixtures(const std::filesystem::path& root, const Geometry& g,
                       std::span<const std::uint32_t> dies_per_slice,
                       std::span<const Defect> defects) {
  std::filesystem::create_directories(root);
  for (std::uint32_t slice = 0; slice < dies_per_slice.size(); ++slice) {
    for (std::uint32_t die = 0; die < dies_per_slice[slice]; ++die) {
      auto dir = root / ("slice_" + std::to_string(slice)) / ("die_" + std::to_string(die));
      std::filesystem::create_directories(dir);
      for (std::uint32_t frame = 0; frame < g.frames_per_die; ++frame) {
        auto px = circuit_pattern(frame, g);
        std::uint32_t seed = die * 1000003u + frame * 101u + 7u;
        for (auto& v : px) {
          int noisy = int(v) + int(xorshift(seed) % 5) - 2;
          v = static_cast<std::uint8_t>(std::clamp(noisy, 0, 255));
        }
        for (const auto& d : defects) {
          if (d.die != die || d.frame_in_die != frame) continue;
          for (std::uint32_t y = 0; y < g.frame_height; ++y)
            for (std::uint32_t x = 0; x < g.frame_width; ++x) {
              long dx = long(x) - d.x, dy = long(y) - d.y;
              if (dx * dx + dy * dy > long(d.radius) * d.radius) continue;
              auto& v = px[std::size_t{y} * g.frame_width + x];
              v = static_cast<std::uint8_t>(std::clamp(int(v) + d.delta, 0, 255));
            }
        }
        write_pgm(dir / ("frame_" + std::to_string(frame) + ".pgm"), g.frame_width,
                  g.frame_height, px);
      }
    }
  }
  std::FILE* m = std::fopen((root / "manifest.txt").c_str(), "w");
  if (!m) throw std::runtime_error("cannot write manifest");
  for (std::uint32_t slice = 0; slice < dies_per_slice.size(); ++slice)
    for (const auto& d : defects)
      if (d.die < dies_per_slice[slice])
        std::fprintf(m, "defect %u %u %u %u %u %u %d\n", slice, d.die, d.frame_in_die, d.x,
                     d.y, d.radius, d.delta);
  std::fclose(m);
}

}  // namespace inspection
