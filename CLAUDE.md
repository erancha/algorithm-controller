# algorithm-controller

A design, with a working C++20 implementation, for the controller of a wafer-inspection tool:
five gRPC processes on one host, pixels in POSIX shared memory. `docs/architecture.md` maps the
parts and the code layout.

## Operations scripts (scripts/)

- `VCPKG_ROOT` must point at a vcpkg checkout; every script that builds stops without it. The
  first configure compiles gRPC and Protobuf from source and takes several minutes.
- On a Windows mount (`/mnt/*`) the build directory is `~/algorithm-controller-build`, not
  `build/`; `ALGCTL_BUILD` overrides it. Look for binaries, fixtures and benchmark logs there.
- Each link of the static gRPC stack takes a few GB of memory, so the build runs at most one job
  per core; an unbounded `-j` runs out of memory.
- The services listen on fixed localhost ports (50050-50052, nodes from 50061). `start.sh`
  refuses to start while another pipeline holds them.
- A large `--frame-dim` needs room in `/dev/shm` for a whole slice; `start.sh` refuses a size it
  cannot hold before generating fixtures.

Scripts:

- `start.sh`: build when sources changed, then run the whole pipeline, single services, a
  driver-only run, or fixture generation.
- `test.sh`: the quality gate: script syntax, build, then the unit, service and e2e layers.
- `benchmark.sh`: one pipeline run per frame size, reporting wall time and per-node CPU time.
- `common.sh`: sourced by the others; build directory, build, and the help printer.
