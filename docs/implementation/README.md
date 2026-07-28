# Implementation

The design's two-machine architecture collapses onto one host: `start.sh`
launches each binary on its own localhost port, and the memory box is a POSIX shared-memory
segment that every process maps. gRPC serves every control-plane call (Ⅰ–Ⅳ, and the
bookkeeping half of Ⅵ), shared memory serves the pixel plane (Ⅴ, and the data half of Ⅵ),
so pixels never cross gRPC.

## Usage

### Getting started

Everything runs on one local machine as five ordinary OS processes — no Docker or other
container runtime involved. Requires a Linux shell, CMake ≥ 3.25, a C++20 compiler, and a
[vcpkg](https://github.com/microsoft/vcpkg) checkout (gRPC, Protobuf, and GoogleTest come from
its manifest). From the repo root:

```bash
export VCPKG_ROOT=~/vcpkg     # your vcpkg checkout; the first configure compiles gRPC and
                              # protobuf from source (several minutes, once)
./scripts/start.sh generate   # one-time: build and render the fixture images
./scripts/start.sh            # start the whole stack, run one slice, tear down
```

`./scripts/start.sh --help` lists the granular actions (individual services, driver-only runs)
and options (pacing, compute cost, node replicas). `./scripts/test.sh` builds and runs the test
suite — all of it, or only the layers named as arguments (`unit`, `service`, `e2e`).

Every run simulates acquisition: the camera serves the fixture images generated above as if they
were live sensor frames (`--simulate` — the only mode `start.sh` uses; hardware acquisition is
not implemented). Two knobs keep the simulation's timing realistic: `--frame-rate` paces the
camera at N frames per second (default 200, the rate a real multi-GB/s camera link sustains;
0 disables pacing), and `--compute-ms` makes each compute-node call take that long (default
200 ms, modeling real algorithm cost; 0 disables it).

With the defaults, photographing the standard slice — 20 dies × 24 frames = 480 frames — takes
about 2.4 s, and only then does the controller start handing work to the compute nodes, because
dispatch deliberately waits for the complete slice. With both knobs at 0, the whole pipeline —
start all five processes, run one slice, tear down — finishes in about 4 s. The end-to-end test
runs a 20-die and a 12-die slice and checks that the two frames where the generator planted
defects (positions 7 and 19) score highest in both.

## Process topology

Five binaries, each a separate OS process — names, roles, and step numbers match the
[architecture diagram](../../README.md#architecture):

| binary           | role (diagram)          | serves (gRPC)                     | calls |
|------------------|--------------------------|-------------------------------------|-------|
| `tool-driver`    | Tool driver              | —                                    | ① `ProcessSlice` |
| `controller`     | Controller + frame index | `ProcessSlice` (Ⅰ), `RegisterNode` (Ⅶ) | ② `ImageSlice`, ⑥ `ProcessPosition`, ⑦ `ReleaseSlots` |
| `camera`         | Camera                   | `ImageSlice` (Ⅱ, streams Ⅲ back)     | ③ `write_frame` (libmembox) |
| `compute-node`   | Compute node (×N)        | `ProcessPosition` (Ⅳ)                | Ⅴ `read_frame` (libmembox), Ⅶ `RegisterNode` |
| `membox-manager` | memory box bookkeeping   | `AllocSlot`, `ReleaseSlots` (Ⅵ)      | — |

## Memory footprint (dev scale)

Measured with `ps -o rss=` on a running 20-die slice (512 slots, 64 KB frames, 3 compute nodes):

| process                   | RSS         | why |
|----------------------------|-------------|-----|
| `membox-manager`            | ~27 MB      | gRPC runtime floor; the free list holds slot numbers only |
| `controller`                 | ~28 MB      | gRPC runtime floor; the frame index holds slot numbers only |
| `camera`                     | ~58 MB      | gRPC floor plus PGM file buffers while streaming a slice |
| `compute-node` (×3)           | ~37 MB each | gRPC floor; frames are read zero-copy from the mapped segment |
| shm segment (512 × 64 KB)     | 32 MB, once | mapped by every process; the OS accounts it once, not per-mapper |

Two figures do not scale with the obvious inputs:

- **Controller RAM is pixel-independent.** The frame index is a bookkeeping table of
  (die, frame-in-die) → slot built from the ④ stream; it stores only `uint64_t` slot numbers, so a
  larger frame (more pixels) leaves controller RAM unchanged — only more dies or positions grow
  it, and only by a few bytes per entry.
- **Nodes read zero-copy.** `read_frame` (Ⅴ) returns a span aliasing the shm mapping directly — a
  compute node's own RSS does not grow with `frame_bytes`; the pixel bytes it reads live once, in
  the shared mapping, never duplicated into a node's own heap.

## Sub-component documentation

- [`src/membox/README.md`](../../src/membox/README.md) — the two-plane split, the backend
  abstraction, and the release-invalidates-views rule.
- [`src/camera/README.md`](../../src/camera/README.md) — the three acquisition modes, fixture
  layout, and pacing.
- [`src/controller/README.md`](../../src/controller/README.md) — frame-index construction, derived
  geometry, and per-node dispatch.
- [`src/compute_node/algorithm/README.md`](../../src/compute_node/algorithm/README.md) — the
  median-reference algorithm with a worked example.
