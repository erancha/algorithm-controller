#!/usr/bin/env bash
# Launches all five processes, runs one slice, and checks that the seeded
# defect positions (frames 7 and 19, from the generator's defect list) score
# strictly higher than every clean position. Second run exercises a
# variable-die-count slice.
set -euo pipefail
BUILD=${1:?usage: e2e.sh <build-dir>}
WORK=$(mktemp -d)
PIDS=()
cleanup() { kill "${PIDS[@]}" 2>/dev/null || true; rm -rf "$WORK"; }
trap cleanup EXIT

FB=65536
"$BUILD/camera" --generate --fixtures "$WORK/fixtures" --die-counts 20,12

"$BUILD/membox-manager" --listen localhost:50050 --slots 512 --frame-bytes $FB \
  --shm /algctl-e2e & PIDS+=($!)
sleep 0.3
"$BUILD/camera" --simulate --fixtures "$WORK/fixtures" --listen localhost:50051 \
  --frame-rate 0 --manager localhost:50050 --shm /algctl-e2e --frame-bytes $FB & PIDS+=($!)
# Nodes start before the controller exists: registration blocks on wait_for_ready
# until the controller comes up, proving launch order does not matter.
for p in 50061 50062 50063; do
  "$BUILD/compute-node" --listen localhost:$p --controller localhost:50052 \
    --manager localhost:50050 --shm /algctl-e2e --frame-bytes $FB --compute-ms 0 & PIDS+=($!)
done
sleep 0.3
"$BUILD/controller" --listen localhost:50052 --camera localhost:50051 \
  --manager localhost:50050 --shm /algctl-e2e --frame-bytes $FB & PIDS+=($!)
sleep 0.3

check_slice() {
  local slice=$1
  local out; out=$("$BUILD/tool-driver" --controller localhost:50052 --slice "$slice")
  [ "$(echo "$out" | wc -l)" -eq 24 ] || { echo "expected 24 values"; exit 1; }
  # The two defect positions must be the top-2 scores.
  local top2; top2=$(echo "$out" | sort -k2 -gr | head -2 | awk '{print $1}' | sort -n)
  [ "$top2" = "$(printf '7\n19')" ] || { echo "slice $slice: top-2 = $top2, want 7 19"; exit 1; }
  echo "slice $slice OK"
}

check_slice 0   # 20 dies
check_slice 1   # 12 dies — nothing assumed a fixed die count
echo "e2e PASS"
