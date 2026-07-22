#!/usr/bin/env bash
# Sequentially runs one slice per frame dimension, sampling every compute
# node's cumulative CPU time every 2 s while the pipeline is up; the last
# sample before teardown is the slice's final work distribution across the
# node pool. Reports each dimension's wall time, detection ranking, and that
# distribution — the evidence for whether position dispatch actually spreads
# work over the nodes.
set -uo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

LABEL=${1:?usage: collect.sh <label> <frame-dim ...>}
shift
[[ $# -gt 0 ]] || { echo "collect.sh: no frame dimensions given" >&2; exit 2; }

for DIM in "$@"; do
  echo "=== dim $DIM $(date +%T) ==="
  LOG="$BUILD/collect-$LABEL-$DIM.log"
  "$REPO_DIR/scripts/start.sh" --frame-dim "$DIM" --frame-rate 0 --compute-ms 5 \
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
