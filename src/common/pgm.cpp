#include "common/pgm.hpp"
#include <cctype>
#include <cstdio>
#include <memory>
#include <stdexcept>

namespace inspection {

namespace {
using FilePtr = std::unique_ptr<std::FILE, decltype(&std::fclose)>;

FilePtr open_or_throw(const std::filesystem::path& path, const char* mode) {
  std::FILE* f = std::fopen(path.c_str(), mode);
  if (!f) throw std::runtime_error("cannot open " + path.string());
  return {f, &std::fclose};
}
}  // namespace

void write_pgm(const std::filesystem::path& path, std::uint32_t width, std::uint32_t height,
               std::span<const std::uint8_t> pixels) {
  if (pixels.size() != std::size_t{width} * height)
    throw std::runtime_error("pixel count does not match dimensions: " + path.string());
  auto f = open_or_throw(path, "wb");
  if (std::fprintf(f.get(), "P5\n%u %u\n255\n", width, height) < 0)
    throw std::runtime_error("header write failed: " + path.string());
  if (std::fwrite(pixels.data(), 1, pixels.size(), f.get()) != pixels.size())
    throw std::runtime_error("short write: " + path.string());
}

Image read_pgm(const std::filesystem::path& path) {
  auto f = open_or_throw(path, "rb");
  Image img;
  unsigned maxval = 0;
  if (std::fscanf(f.get(), "P5 %u %u %u", &img.width, &img.height, &maxval) != 3 ||
      maxval != 255)
    throw std::runtime_error("not an 8-bit P5 PGM: " + path.string());
  int sep = std::fgetc(f.get());
  if (sep == EOF || !std::isspace(static_cast<unsigned char>(sep)))
    throw std::runtime_error("malformed PGM header: " + path.string());
  img.pixels.resize(std::size_t{img.width} * img.height);
  if (std::fread(img.pixels.data(), 1, img.pixels.size(), f.get()) != img.pixels.size())
    throw std::runtime_error("short read: " + path.string());
  return img;
}

}  // namespace inspection
