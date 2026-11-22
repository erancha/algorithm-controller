#!/usr/bin/env bash
# Checks every script's syntax, builds when sources changed, then runs the test suite: every
# layer, or only the named ones.
#
# Usage: test.sh [-h|--help] [layer ...]
#
# Layers:
#   unit         in-process tests of a single component
#   service      gRPC service tests with in-process servers
#   e2e          full five-process pipeline test
#   (none)       all layers
#
# Examples:
#   ./scripts/test.sh
#   ./scripts/test.sh unit service
set -euo pipefail
# shellcheck source=common.sh
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

LAYERS=()
while [[ $# -gt 0 ]]; do
  case "$1" in
    -h|--help) print_help; exit 0 ;;
    unit|service|e2e) LAYERS+=("$1"); shift ;;
    *) die_usage "$1" ;;
  esac
done

for script in "$ROOT"/scripts/*.sh "$ROOT"/tests/*.sh; do bash -n "$script"; done
build_project
CTEST_ARGS=(--test-dir "$BUILD" --output-on-failure)
# ctest -L takes a regex over test labels; anchor it so a layer name can never
# partially match another.
if [[ ${#LAYERS[@]} -gt 0 ]]; then
  CTEST_ARGS+=(-L "$(IFS='|'; echo "^(${LAYERS[*]})$")")
fi
ctest "${CTEST_ARGS[@]}"
