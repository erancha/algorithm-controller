# compute_node/algorithm

`position_score` (`src/compute_node/algorithm/position_score.cpp`) is the algorithm block Ⅳ of the
[main design](../../../README.md#component-apis) runs per invocation: input is N frames imaging
the same frame-in-die position, one per die; the return value is a single `double`, near zero when
the dies agree and large when one carries a defect.

## Method: cross-die comparison against a computed reference

1. **Reference** — for every pixel, the median value across the N dies' frames at that pixel.
2. **Per-die score** — for every die, the mean absolute deviation of its frame from the reference,
   averaged over all pixels in the frame.
3. **Returned value** — the maximum per-die score: near zero when every die matches the reference,
   large when any single die disagrees.

## Worked example

`tests/position_score_test.cpp`, `OneDefectiveDieDominatesTheScore`: 7 dies, each a flat frame of
100 pixels at value 50. One die gets a 200-value blob on 10 of its 100 pixels (a delta of +150 on
10% of the frame). Six of seven dies still agree pixel-for-pixel, so the median stays 50 at every
pixel — the defective die cannot drag its own reference off-center. That die's mean absolute
deviation is `(10 pixels × 150 + 90 pixels × 0) / 100 = 15`; every clean die scores 0. The returned
value — the max — is `15.0`, exactly what the test asserts.

## Why the median needs no golden die

The median is the value more than half the dies agree on, at each pixel independently. As long as
fewer than half the dies are defective at a given pixel, the median tracks the clean value with no
input telling it in advance which die is trustworthy — robustness comes from the aggregate, not
from any one die's identity. `MedianIsRobustToTheDefectiveDie` in the same test file pins this: an
entirely wrong die (all pixels at 255 against a 50 baseline) still leaves the median — and every
clean die's score — untouched; only the wrong die itself scores high (205).

## Two rejected alternatives

- **Die-to-die voting** — comparing every pair of dies directly is O(N²) and still needs a rule
  for turning pairwise disagreements into one baseline; a defective die's votes still count,
  unlike a median that a minority of outliers cannot move.
- **Designated golden die** — a fixed reference die is a single point of failure: if that die is
  itself defective, every other die reads as wrong, and keeping a golden die correct requires
  separate calibration and maintenance the median-of-all-dies approach never needs.

## Even-die-count median: a deliberate integer-domain choice

`position_score` computes the median with `std::nth_element(sorted.begin(), sorted.begin() + n /
2, sorted.end())`, then reads `sorted[n / 2]`. For an odd die count this is the exact middle
element. For an even die count (e.g. `n = 8`, index `4`), it is the *upper*-middle element — the
5th-smallest of 8 — not the average of the two middle values. Pixel values are `std::uint8_t`;
averaging the two middle values would add a floating-point (or rounding) step that integer
`nth_element` selection avoids, at the cost of an even-N median not being the textbook statistical
median. The design accepts that trade: the reference only needs to be a value the majority of dies
agree on, not a mathematically exact midpoint.

See the root README's [Component APIs](../../../README.md#component-apis), block Ⅳ, for how this
fits the rest of the pipeline.
