#include "compute_node/algorithm/position_score.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <vector>

namespace inspection {

double position_score(std::span<const std::span<const std::byte>> frames) {
  if (frames.empty()) throw std::invalid_argument("no frames");
  const std::size_t pixels = frames[0].size();
  for (const auto& f : frames)
    if (f.size() != pixels) throw std::invalid_argument("frame size mismatch");
  if (pixels == 0) throw std::invalid_argument("zero-length frames");
  const std::size_t n = frames.size();

  std::vector<double> abs_dev(n, 0.0);
  std::vector<std::uint8_t> column(n);
  std::vector<std::uint8_t> sorted(n);
  for (std::size_t p = 0; p < pixels; ++p) {
    for (std::size_t d = 0; d < n; ++d) column[d] = std::to_integer<std::uint8_t>(frames[d][p]);
    sorted = column;
    std::nth_element(sorted.begin(), sorted.begin() + n / 2, sorted.end());
    const int median = sorted[n / 2];
    for (std::size_t d = 0; d < n; ++d) abs_dev[d] += std::abs(int(column[d]) - median);
  }
  double max_score = 0.0;
  for (double s : abs_dev) max_score = std::max(max_score, s / double(pixels));
  return max_score;
}

}  // namespace inspection
