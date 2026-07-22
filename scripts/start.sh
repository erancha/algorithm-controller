#!/usr/bin/env bash
# Builds when sources changed, then runs the pipeline — the whole stack, selected services, or
# fixture generation. start.sh --help lists actions and options.

set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() {
  cat <<'EOF'
Usage: start.sh [options] [action ...]

Builds when sources changed, then runs the requested actions.

Actions:
  generate     build and render the fixture images, then exit (also runs
               automatically before the first start)
  membox | camera | node | controller
               start only the named services and stay up until Ctrl-C, so
               each can run in its own terminal
  driver       run one slice against already-running services, then exit
  (none)       full pipeline: start every service, run one slice through
               tool-driver, tear down

Options:
  --slice N        slice id the driver runs (default 0)
  --frame-rate N   camera pacing in frames/s, 0 = unpaced (default 200)
  --compute-ms N   per-invocation compute cost on a node (default 200)
  --node-count N   compute-node replicas on consecutive ports (50061,
                   50062, ...); each registers itself with the controller,
                   which needs no node configuration (default 3)
  --duration N     repeat the slice for N seconds, printing each run's wall
                   time and a process-watch command for observing every
                   service's CPU and memory (default: run the slice once)
  --help           show this help

Environment:
  ALGCTL_BUILD     build directory; defaults to ~/algorithm-controller-build
                   when the repo lives on a Windows mount (/mnt/*), else
                   <repo>/build
  VCPKG_ROOT       vcpkg checkout providing the toolchain (required)
EOF
}

SLICE=0; FRAME_RATE=200; COMPUTE_MS=200; NODE_COUNT=3; GENERATE=0; DURATION=0
SERVICES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --slice) SLICE=${2:?missing value for --slice}; shift 2 ;;
    --duration) DURATION=${2:?missing value for --duration}; shift 2 ;;
    --frame-rate) FRAME_RATE=${2:?missing value for --frame-rate}; shift 2 ;;
    --compute-ms) COMPUTE_MS=${2:?missing value for --compute-ms}; shift 2 ;;
    --node-count) NODE_COUNT=${2:?missing value for --node-count}; shift 2 ;;
    --help) usage; exit 0 ;;
    generate) GENERATE=1; shift ;;
    membox|camera|node|controller|driver) SERVICES+=("$1"); shift ;;
    *) echo "unknown argument $1 (see start.sh --help)" >&2; exit 2 ;;
  esac
done

# No services named = all of them.
want() { [[ ${#SERVICES[@]} -eq 0 ]] || printf '%s\n' "${SERVICES[@]}" | grep -qx "$1"; }

[[ -d "$BUILD" ]] || cmake -B "$BUILD" -S "$REPO_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:?set VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
cmake --build "$BUILD" -j

FB=65536
generate_fixtures() {
  "$BUILD/camera" --generate --fixtures "$BUILD/fixtures" --die-counts 20,12
}
if [[ $GENERATE -eq 1 ]]; then generate_fixtures; exit 0; fi
[[ -d "$BUILD/fixtures" ]] || generate_fixtures

PIDS=()
cleanup() { [[ ${#PIDS[@]} -gt 0 ]] && kill "${PIDS[@]}" 2>/dev/null || true; }
trap cleanup EXIT
# An untrapped fatal signal kills bash without running the EXIT trap, orphaning
# the services (e.g. under `timeout` or Ctrl-C); converting to exit keeps
# teardown on every path.
trap 'exit 143' TERM
trap 'exit 130' INT

# gRPC listens with SO_REUSEPORT, so a second stack on the same ports would
# start silently and cross-wire connections with the first; refuse instead.
port_free_or_die() {
  if ss -ltn 2>/dev/null | grep -q ":$1 "; then
    echo "port $1 is already in use — another pipeline instance is running?" >&2
    exit 1
  fi
}

if want membox; then
  port_free_or_die 50050
  "$BUILD/membox-manager" --slots 512 --frame-bytes $FB & PIDS+=($!)
  sleep 0.3
fi
if want camera; then
  port_free_or_die 50051
  "$BUILD/camera" --simulate --fixtures "$BUILD/fixtures" --frame-rate "$FRAME_RATE" \
    --frame-bytes $FB & PIDS+=($!)
fi
if want node; then
  for ((i = 0; i < NODE_COUNT; i++)); do
    port_free_or_die $((50061 + i))
    "$BUILD/compute-node" --listen localhost:$((50061 + i)) --frame-bytes $FB \
      --compute-ms "$COMPUTE_MS" & PIDS+=($!)
  done
fi
sleep 0.3
if want controller; then
  port_free_or_die 50052
  "$BUILD/controller" --frame-bytes $FB & PIDS+=($!)
  sleep 0.3
fi

if [[ ${#SERVICES[@]} -eq 0 ]] || want driver; then
  if [[ $DURATION -gt 0 ]]; then
    echo "repeating slice $SLICE for ${DURATION}s — watch CPU/memory from another terminal with:" >&2
    echo "  watch -n 1 'ps -o pid,comm,pcpu,rss,etime -C membox-manager,camera,compute-node,controller,tool-driver'" >&2
    runs=0; t0=$(date +%s%N); end=$(( t0 + DURATION * 1000000000 ))
    while (( $(date +%s%N) < end )); do
      r0=$(date +%s%N)
      # Per-slice values repeat identically run to run, so they are dropped;
      # a failing run surfaces its captured diagnostics and stops the loop.
      if ! "$BUILD/tool-driver" --slice "$SLICE" >/dev/null 2>"$BUILD/driver-err.log"; then
        cat "$BUILD/driver-err.log" >&2
        exit 1
      fi
      runs=$((runs + 1))
      awk -v ns=$(( $(date +%s%N) - r0 )) -v i=$runs \
        'BEGIN{printf "run %d: %.1f s\n", i, ns/1e9}' >&2
    done
    awk -v ns=$(( $(date +%s%N) - t0 )) -v n=$runs \
      'BEGIN{printf "ran %d slices in %.1f s (%.1f s per slice)\n", n, ns/1e9, ns/1e9/n}' >&2
  else
    t0=$(date +%s%N)
    "$BUILD/tool-driver" --slice "$SLICE"
    awk -v ns=$(( $(date +%s%N) - t0 )) \
      'BEGIN{printf "slice wall time: %.1f s (request sent to results received)\n", ns/1e9}' >&2
  fi
else
  echo "services up: ${SERVICES[*]} (node count: $NODE_COUNT) — Ctrl-C to stop"
  wait
fi
