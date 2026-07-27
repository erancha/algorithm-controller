#!/usr/bin/env bash
# Builds and runs the test suite — all layers, or only the ones named on the command line.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

usage() {
  cat <<'EOF'
Usage: test.sh [--help] [layer ...]

Builds when sources changed, then runs the requested test layers.

Layers:
  unit         in-process tests of a single component
  service      gRPC service tests with in-process servers
  e2e          full five-process pipeline test
  (none)       all layers
EOF
  usage_environment
}

LAYERS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    --help) usage; exit 0 ;;
    unit|service|e2e) LAYERS+=("$1"); shift ;;
    *) echo "unknown argument $1 (see test.sh --help)" >&2; exit 2 ;;
  esac
done

build_project
CTEST_ARGS=(--test-dir "$BUILD" --output-on-failure)
# ctest -L takes a regex over test labels; anchor it so a layer name can never
# partially match another.
if [[ ${#LAYERS[@]} -gt 0 ]]; then
  CTEST_ARGS+=(-L "$(IFS='|'; echo "^(${LAYERS[*]})$")")
fi
ctest "${CTEST_ARGS[@]}"
