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
  --frame-dim N    frame edge in pixels: frames are N×N single-byte pixels,
                   and fixtures, --frame-bytes, and the membox slot count are
                   all derived from this one value (default 256 = 64 KiB
                   frames; 4096 = the 16 MiB frames of a production-class
                   inspection camera)
  --slice N        slice id the driver runs (default 0)
  --frame-rate N   camera pacing in frames/s, 0 = unpaced (default 200)
  --compute-ms N   per-invocation compute cost on a node (default 200)
  --node-count N   compute-node replicas on consecutive ports (50061,
                   50062, ...); each registers itself with the controller,
                   which needs no node configuration (default 3)
  --duration N     repeat the slice for N seconds, printing each run's wall
                   time and a process-watch command for observing every
                   service's CPU and memory (default: run the slice once)
  --collect [MAX]  run scripts/collect.sh over frame dimensions doubling from
                   256 up to MAX — default 2048; e.g. 4096 extends the ladder
                   to the 16 MiB production frame. One sequential slice per
                   dimension, reporting wall time, detection ranking, and the
                   per-node CPU-time distribution, then exit
  --help           show this help
EOF
  usage_environment
}

SLICE=0; FRAME_RATE=200; COMPUTE_MS=200; NODE_COUNT=3; GENERATE=0; DURATION=0
FRAME_DIM=256; COLLECT=0; COLLECT_MAX=2048
SERVICES=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --frame-dim) FRAME_DIM=${2:?missing value for --frame-dim}; shift 2 ;;
    # The value is optional, so it is consumed only when the next word is
    # numeric — anything else (an action, another flag) stays unparsed.
    --collect) COLLECT=1
      if [[ $# -ge 2 && "$2" =~ ^[0-9]+$ ]]; then COLLECT_MAX=$2; shift 2; else shift; fi ;;
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

if [[ $COLLECT -eq 1 ]]; then
  DIMS=(); d=256
  while (( d <= COLLECT_MAX )); do DIMS+=("$d"); d=$((d * 2)); done
  # A max that is not on the doubling ladder would silently truncate to the
  # rung below it; refuse instead.
  if [[ ${#DIMS[@]} -eq 0 || ${DIMS[-1]} -ne $COLLECT_MAX ]]; then
    echo "--collect max must be 256 doubled some number of times (256, 512, ..., 2048, 4096, ...)" >&2
    exit 2
  fi
  exec "$REPO_DIR/scripts/collect.sh" collect "${DIMS[@]}"
fi

# No services named = all of them.
want() { [[ ${#SERVICES[@]} -eq 0 ]] || printf '%s\n' "${SERVICES[@]}" | grep -qx "$1"; }

build_project

# One knob sizes the whole data path. Frames are FRAME_DIM×FRAME_DIM
# single-byte pixels; the slot count must cover every frame of the largest
# slice because the controller releases a slice's slots only after all its
# positions are scored — fewer slots would fail the camera stream mid-slice.
[[ "$FRAME_DIM" =~ ^[0-9]+$ ]] || { echo "--frame-dim must be a number" >&2; exit 2; }
FB=$((FRAME_DIM * FRAME_DIM))
DIE_COUNTS="20,12"; FRAMES_PER_DIE=24
MAX_DIES=0
IFS=, read -ra DIES <<<"$DIE_COUNTS"
for d in "${DIES[@]}"; do if (( d > MAX_DIES )); then MAX_DIES=$d; fi; done
SLOTS=$((MAX_DIES * FRAMES_PER_DIE))
# Deadlines scale with the same knob. Scoring one position walks MAX_DIES
# frames on one node — assume a conservative 2 MB/s with every node sharing
# the host's cores; the camera stream reads SLOTS frames from cold disk at a
# conservative 25 MB/s plus any pacing delay. Floors preserve the small-frame
# defaults, and the driver's bound must stay above the controller's internal
# ones so those fire first with better diagnoses.
POS_DEADLINE=$((MAX_DIES * FB / 2000000))
if ((POS_DEADLINE < 60)); then POS_DEADLINE=60; fi
PACING=$(awk -v s=$SLOTS -v r="$FRAME_RATE" 'BEGIN{printf "%d", (r > 0 ? s / r : 0)}')
STREAM_DEADLINE=$((SLOTS * FB / 25000000 + PACING))
if ((STREAM_DEADLINE < 300)); then STREAM_DEADLINE=300; fi
DRIVER_DEADLINE=$((STREAM_DEADLINE + FRAMES_PER_DIE * POS_DEADLINE / NODE_COUNT + 60))
# Fixtures are cached per dimension, so switching sizes never reuses stale
# frames and never regenerates ones already on disk.
FIXTURES="$BUILD/fixtures-$FRAME_DIM"

generate_fixtures() {
  "$BUILD/camera" --generate --fixtures "$FIXTURES" --die-counts "$DIE_COUNTS" \
    --frames-per-die $FRAMES_PER_DIE --frame-dim $FRAME_DIM
  # Disk footprint of every cached dimension, this dimension's slices, and one
  # frame — each cache is safe to rm -rf and regenerates on the next run.
  du -sh "$BUILD"/fixtures-*
  du -sh "$FIXTURES"/slice_*
  ls -lh "$FIXTURES"/slice_0/die_0/frame_0.pgm
}
if [[ $GENERATE -eq 1 ]]; then generate_fixtures; exit 0; fi

# The segment is created sparse, so an oversized box would not fail at
# creation — it would SIGBUS mid-slice when tmpfs fills. Refuse upfront,
# before an infeasible dimension spends minutes generating fixtures.
if want membox; then
  SHM_AVAIL=$(df --output=avail -B1 /dev/shm | tail -1)
  if (( SLOTS * FB > SHM_AVAIL )); then
    echo "membox needs $((SLOTS * FB / 1048576)) MiB of /dev/shm but only \
$((SHM_AVAIL / 1048576)) MiB is free — free space there, raise the WSL memory \
limit in .wslconfig, or pass a smaller --frame-dim" >&2
    exit 1
  fi
fi
[[ -d "$FIXTURES" ]] || generate_fixtures

PIDS=()
# Waits after the kill: the manager shuts down gracefully to unlink its shm
# segment, so returning before the children exit would leave ports briefly
# occupied and make an immediately following run refuse to start.
cleanup() {
  if [[ ${#PIDS[@]} -gt 0 ]]; then
    kill "${PIDS[@]}" 2>/dev/null || true
    wait "${PIDS[@]}" 2>/dev/null || true
  fi
}
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

# Startup readiness is polled, not slept over: a fixed delay races service
# binding whenever the machine is busy (e.g. gigabytes of freshly generated
# fixtures still flushing to disk), and a lost race surfaces as a confusing
# connection-refused mid-pipeline.
wait_for_port() {
  for _ in $(seq 1 300); do
    if ss -ltn 2>/dev/null | grep -q ":$1 "; then return 0; fi
    sleep 0.1
  done
  echo "service on port $1 did not start listening within 30 s" >&2
  exit 1
}

if want membox; then
  port_free_or_die 50050
  "$BUILD/membox-manager" --slots $SLOTS --frame-bytes $FB & PIDS+=($!)
  wait_for_port 50050
fi
if want camera; then
  port_free_or_die 50051
  "$BUILD/camera" --simulate --fixtures "$FIXTURES" --frame-rate "$FRAME_RATE" \
    --frame-bytes $FB & PIDS+=($!)
fi
if want node; then
  for ((i = 0; i < NODE_COUNT; i++)); do
    port_free_or_die $((50061 + i))
    "$BUILD/compute-node" --listen localhost:$((50061 + i)) --frame-bytes $FB \
      --compute-ms "$COMPUTE_MS" & PIDS+=($!)
  done
fi
if want camera; then wait_for_port 50051; fi
if want node; then
  for ((i = 0; i < NODE_COUNT; i++)); do wait_for_port $((50061 + i)); done
fi
if want controller; then
  port_free_or_die 50052
  "$BUILD/controller" --frame-bytes $FB --camera-stream-deadline-s $STREAM_DEADLINE \
    --position-deadline-s $POS_DEADLINE & PIDS+=($!)
  wait_for_port 50052
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
      if ! "$BUILD/tool-driver" --slice "$SLICE" --deadline-s $DRIVER_DEADLINE \
          >/dev/null 2>"$BUILD/driver-err.log"; then
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
    "$BUILD/tool-driver" --slice "$SLICE" --deadline-s $DRIVER_DEADLINE
    awk -v ns=$(( $(date +%s%N) - t0 )) \
      'BEGIN{printf "slice wall time: %.1f s (request sent to results received)\n", ns/1e9}' >&2
  fi
else
  echo "services up: ${SERVICES[*]} (node count: $NODE_COUNT) — Ctrl-C to stop"
  wait
fi
