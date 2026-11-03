#!/usr/bin/env bash
# Runs the whole pipeline once per frame size and reports how each run scaled — the evidence for
# whether position dispatch actually spreads work over the compute nodes. benchmark.sh --help
# lists the options.

# No -e: a failed run is reported and the next frame size still runs.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

# Camera pacing off and a near-zero compute cost, so the wall time measures the pipeline itself
# rather than start.sh's simulated camera and compute delays.
RUN_ARGS=(--frame-rate 0 --compute-ms 5)

usage() {
  cat <<EOF
Usage: benchmark.sh [--max-dim N] [--help]

Runs the whole pipeline once per frame dimension N, doubling from 256 up to
the maximum. Each run is exactly:
  start.sh --frame-dim N ${RUN_ARGS[*]}
so any single row can be reproduced by hand with that command. Plain start.sh
is the 256 run with its simulated timing left on, so its wall time is longer.
While each run is up, it samples every compute node's cumulative
CPU time every 2 s; the last sample before teardown is that run's work
distribution across the node pool. For each dimension it prints:
  - the start.sh exit code and the slice's wall time
  - the detection ranking (the highest-scoring positions)
  - the final CPU time of each compute node
Each run's full output goes to <build>/benchmark-<dim>.log.

Options:
  --max-dim N  largest frame dimension; must be 256 doubled some number of
               times (default 2048; 4096 adds the 16 MiB frame of a
               production-class inspection camera)
  --help       show this help
EOF
  usage_environment
}

MAX_DIM=2048
while [[ $# -gt 0 ]]; do
  case "$1" in
    --max-dim) MAX_DIM=${2:?missing value for --max-dim}; shift 2 ;;
    --help) usage; exit 0 ;;
    *) echo "unknown argument $1 (see benchmark.sh --help)" >&2; exit 2 ;;
  esac
done

[[ "$MAX_DIM" =~ ^[0-9]+$ ]] || { echo "--max-dim must be a number" >&2; exit 2; }
DIMS=(); d=256
while (( d <= MAX_DIM )); do DIMS+=("$d"); d=$((d * 2)); done
# A max that is not on the doubling ladder would silently truncate to the rung below it.
if [[ ${#DIMS[@]} -eq 0 || ${DIMS[-1]} -ne $MAX_DIM ]]; then
  echo "--max-dim must be 256 doubled some number of times (256, 512, ..., 2048, 4096, ...)" >&2
  exit 2
fi

for DIM in "${DIMS[@]}"; do
  echo "=== dim $DIM $(date +%T) ==="
  LOG="$BUILD/benchmark-$DIM.log"
  "$REPO_DIR/scripts/start.sh" --frame-dim "$DIM" "${RUN_ARGS[@]}" \
    > "$LOG" 2>&1 &
  RUN=$!
  DIST=""
  while kill -0 $RUN 2>/dev/null; do
    S=$(ps -o pid,time --no-headers -C compute-node 2>/dev/null | awk '{printf "%s=%s ", $1, $2}')
    [[ -n "$S" ]] && DIST="$S"
    sleep 2
  done
  wait $RUN
  echo "rc=$?"
  grep -E "highest|wall time|failed|error|in use" "$LOG"
  echo "final node cpu-times: $DIST"
done
