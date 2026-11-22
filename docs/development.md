# Development & deployment

How to build, run and test this repo locally. Back to the [README](../README.md); how the running
processes fit together is in [architecture](architecture.md#running-on-one-host).

**Contents:** [Working copy location](#working-copy-location) · [Configuration](#configuration) ·
[Tests](#tests) · [Scripts (`scripts/`)](#scripts-scripts) ·
[Simulated acquisition](#simulated-acquisition) · [Measure](#measure)

## Working copy location

Everything runs on one local machine as five ordinary OS processes, with no Docker or other
container runtime, and nothing is deployed elsewhere. It needs a Linux shell, CMake 3.25 or
later, a C++20 compiler, and a [vcpkg](https://github.com/microsoft/vcpkg) checkout. gRPC,
Protobuf and GoogleTest come from its manifest, `vcpkg.json`.

Keep the working copy on the Linux filesystem (under WSL2,
`~/projects/cpp/algorithm-controller`) rather than on a Windows drive (`/mnt/*`). There the
scripts build in the repo's own `build/` folder. A working copy on a Windows drive still works,
but its scripts build under `~/algorithm-controller-build` instead: WSL2 reaches Windows drives
through a file-sharing protocol that makes every file access a round trip to the Windows side,
and linking the large debug binaries through it is about ten times slower.

## Configuration

The repo has no settings files. The scripts read two environment variables, which each
script's `--help` lists: `VCPKG_ROOT`, the vcpkg checkout (required), and `ALGCTL_BUILD`, which
overrides the build directory. Every run option, such as the frame size or the node count, is a
command-line option of `start.sh`.

## Tests

`./scripts/test.sh` checks every script's syntax, then builds and runs the test suite: every
layer, or only the ones named as arguments.

- `unit`: in-process tests of a single component.
- `service`: gRPC service tests with in-process servers.
- `e2e`: `tests/e2e.sh` runs the five processes on a 20-die and then a 12-die slice and checks
  that the two frame-in-die positions carrying seeded defects score highest in both.

CI runs `scripts/test.sh` on every push to `main` and every pull request
(`.github/workflows/ci.yml`).

## Scripts (`scripts/`)

Each script's header is its full documentation, and `--help` prints it.

- [`start.sh`](../scripts/start.sh): build when sources changed, then run the whole pipeline,
  single services (each in its own terminal), a driver-only run against running services, or
  fixture generation. The README's [Getting started](../README.md#getting-started) uses it.
- [`test.sh`](../scripts/test.sh): the quality gate described under [Tests](#tests).
- [`benchmark.sh`](../scripts/benchmark.sh): one pipeline run per frame size, described under
  [Measure](#measure).
- [`common.sh`](../scripts/common.sh): sourced by the others; resolves the build directory,
  builds the project, and prints a script's header as its help.

## Simulated acquisition

There is no camera hardware here. The camera serves the fixture images that `start.sh generate`
renders as if they were live sensor frames; hardware acquisition is not implemented, as the [camera
README](../src/camera/README.md) describes. Two `start.sh` options keep the simulation's timing
realistic:

- `--frame-rate` paces the camera, modeling the rate a real camera link sustains.
- `--compute-ms` makes each compute-node invocation take that long, modeling real algorithm cost.

With the default pacing, photographing the standard 20-die slice takes about 2.4 s, and only
then does the controller start handing work to the compute nodes. With both options at 0, the
whole pipeline (start all five processes, run one slice, tear down) finishes in about 4 s.

## Measure

`./scripts/benchmark.sh` runs the whole pipeline once per frame size N, doubling from 256 up
to 2048 pixels per side (`--max-dim 4096` adds the 16 MiB production frame). Each run switches
off the simulated camera pacing and compute cost, so the wall time measures the pipeline itself,
and each row of its report opens with the exact `start.sh` command, so any row can be reproduced
by hand. That is also why plain `./scripts/start.sh` is not the benchmark's first row: it runs
at 256 too, but with the simulated timing on, so it takes longer and only its detection ranking
matches.

For each size the benchmark reports the wall time, the detection ranking, and the CPU time each
compute node used, which shows whether the work spread evenly across the nodes.

### Memory footprint

Resident memory per process at dev scale, measured with `ps -o rss=` on a running 20-die slice
(512 slots, 64 KB frames, 3 compute nodes):

| process                   | resident    | why |
|----------------------------|-------------|-----|
| `membox-manager`            | ~27 MB      | gRPC runtime floor; the free list holds slot numbers only |
| `controller`                 | ~28 MB      | gRPC runtime floor; the frame index holds slot numbers only |
| `camera`                     | ~58 MB      | gRPC floor plus PGM file buffers while streaming a slice |
| `compute-node` (×3)           | ~37 MB each | gRPC floor; frames are read zero-copy from the mapped segment |
| shm segment (512 × 64 KB)     | 32 MB, once | mapped by every process; the OS accounts it once, not per-mapper |

Two figures do not grow with frame size:

- **Controller memory.** The frame index stores only `uint64_t` slot numbers, so larger frames
  leave it unchanged. Only more dies or positions grow it, by a few bytes per entry.
- **Compute-node memory.** `read_frame` (Ⅴ) returns a view into the shared mapping, so the pixels
  a node reads live once, in the mapping, and never in the node's own heap.
