# Development

How to build, run and test this repo locally. Back to the [README](../README.md); how the running
processes fit together is in [architecture](architecture.md#running-on-one-host).

## Prerequisites

Everything runs on one local machine as five ordinary OS processes, with no Docker or other
container runtime. It needs a Linux shell, CMake 3.25 or later, a C++20 compiler, and a
[vcpkg](https://github.com/microsoft/vcpkg) checkout. gRPC, Protobuf and GoogleTest come from its
manifest, `vcpkg.json`.

## Run

The README's [Getting started](../README.md#getting-started) builds the fixtures and runs one slice
through the whole stack. `./scripts/start.sh --help` lists the other actions (single services,
driver-only runs) and every option with its default.

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

## Test

`./scripts/test.sh` builds and runs the test suite: every layer, or only the ones named as
arguments.

- `unit`: in-process tests of a single component.
- `service`: gRPC service tests with in-process servers.
- `e2e`: `tests/e2e.sh` runs the five processes on a 20-die and then a 12-die slice and checks
  that the two frame-in-die positions carrying seeded defects score highest in both.

CI runs `scripts/test.sh` on every push to `main` and every pull request
(`.github/workflows/ci.yml`).

## Measure

`./scripts/benchmark.sh` runs the whole pipeline once per frame size N, doubling from 256 up
to 2048 pixels per side (`--max-dim 4096` adds the 16 MiB production frame). Each run is exactly

```bash
./scripts/start.sh --frame-dim N --frame-rate 0 --compute-ms 5
```

so any row of its report can be reproduced by hand. The two options switch off the simulated
camera pacing and compute cost, so the wall time measures the pipeline itself. That is also why
plain `./scripts/start.sh` is not the benchmark's first row: it runs at 256 too, but with the
simulated timing on, so it takes longer and only its detection ranking matches.

For each size the benchmark reports the wall time, the detection ranking, and the CPU time each
compute node used, which shows whether the work spread evenly across the nodes.
