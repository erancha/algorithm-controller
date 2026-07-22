#!/usr/bin/env bash
# Builds and runs the whole test suite: unit tests plus the end-to-end pipeline test.
set -euo pipefail
source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
[[ -d "$BUILD" ]] || cmake -B "$BUILD" -S "$REPO_DIR" \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:?set VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
cmake --build "$BUILD" -j
ctest --test-dir "$BUILD" --output-on-failure
