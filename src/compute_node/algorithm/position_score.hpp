#pragma once
#include <cstddef>
#include <span>

namespace inspection {

// Cross-die comparison at one frame-in-die position: reference = pixel-wise
// median across dies, per-die score = mean absolute deviation from it,
// returned value = max per-die score. Position-agnostic — it never interprets
// circuit content, only cross-die disagreement.
double position_score(std::span<const std::span<const std::byte>> frames);

}  // namespace inspection
