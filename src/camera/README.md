# camera

Serves the acquisition side of the [main design](../../README.md#component-apis): images a slice
(②) by streaming every frame through `write_frame` (③) and reporting each placement (④) before
signaling completion (⑤) — see `src/camera/camera_service.hpp`.

## Three modes

One binary, `camera`, selected by flags:

- `--generate` — one-time: renders the fixture tree and `manifest.txt`, then exits. Does not open
  a gRPC channel or touch the memory box.
- `--simulate` — serves `ImageSlice` by streaming the generated fixture files through
  `write_frame`, exactly as a hardware path would stream sensor frames; requires `--fixtures` to
  already hold a tree written by a prior `--generate` run.
- hardware mode — OPEN, not implemented. Same `ImageSlice` service surface, frames sourced from a
  sensor SDK instead of fixture files.

## Fixture tree layout

`generate_fixtures` (`src/camera/fixtures.cpp`) writes, under the `--fixtures` root:

```
fixtures/slice_<id>/die_<d>/frame_<f>.pgm
fixtures/manifest.txt
```

Each frame file is binary PGM (P5) — trivial to write and read with no image library, and
viewable in any standard tool (`src/common/pgm.hpp`). `--die-counts` (comma-separated, e.g.
`20,12` for two slices — the default is a single 20-die slice) gives each generated slice its die
count; `--frames-per-die` (default 24) and the frame dimensions (256×256, 8-bit) are geometry
shared with `membox-manager`'s `--frame-bytes` sizing (256×256 = 65536 bytes = the 64 KB default).

## `manifest.txt` line format

One line per seeded defect, per slice it appears in:

```
defect <slice> <die> <frame_in_die> <x> <y> <radius> <delta>
```

`delta` is signed: a positive value brightens the blob, negative darkens it. The default generator
seeds two defects — `die 3, frame_in_die 7` (`delta +90`) and `die 11, frame_in_die 19`
(`delta -70`) — which is why `tests/e2e.sh` and `scripts/start.sh` both expect frame-in-die
positions 7 and 19 to score highest.

## Pacing

`--frame-rate` (default 200 frames/s) sleeps between successive `write_frame` calls inside
`ImageSlice`, modeling acquisition as the physical rate limiter — the default 480-frame slice (20
dies × 24 frames/die) then takes about 2.4 s to image. `--frame-rate 0` disables pacing, the
setting `tests/e2e.sh` uses so unit and integration runs stay fast.

## Slot ownership on a broken stream

A frame's slot changes owner the moment its ④ report is delivered: reported slots are the
controller's to release (its abort path does), and a slot whose report failed — the client
disconnected mid-stream — is released by the camera itself, since nobody else ever learned of
it. Either way a broken stream leaks no memory-box capacity.

See the root README's [Component APIs](../../README.md#component-apis) — blocks Ⅱ and Ⅲ are this
service's `ImageSlice` contract; block Ⅵ's `write_frame` half is where frames actually land.
