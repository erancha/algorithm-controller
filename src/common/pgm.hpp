#pragma once
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace inspection {

struct Image {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;  // row-major, width*height bytes
};

void write_pgm(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> pixels);
Image read_pgm(const std::filesystem::path& path);

}  // namespace inspection
