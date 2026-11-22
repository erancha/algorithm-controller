#!/usr/bin/env bash
# Sourced by every script in scripts/: resolves ROOT and BUILD, builds the project, and prints a
# script's header as its --help text.
#
# BUILD honors ALGCTL_BUILD when set. Otherwise, a repo on a Windows mount (/mnt/*) builds under
# $HOME: WSL2's bridge to Windows drives makes linking the ~450 MB debug binaries there roughly an
# order of magnitude slower than the Linux filesystem. A repo already on the Linux filesystem
# builds in-tree.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${ALGCTL_BUILD:-}" ]]; then
  BUILD="$ALGCTL_BUILD"
elif [[ "$ROOT" == /mnt/* ]]; then
  BUILD="$HOME/algorithm-controller-build"
else
  BUILD="$ROOT/build"
fi

# Configures CMake on the first run, then builds. Parallelism is capped at the
# core count: every executable links the static gRPC stack at a few GB of
# memory per link, and make's unbounded -j runs all of those links at once —
# enough to exhaust a 16 GB machine (it OOM-kills CI runners).
build_project() {
  [[ -d "$BUILD" ]] || cmake -B "$BUILD" -S "$ROOT" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:?set VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  cmake --build "$BUILD" -j "$(nproc)"
}

# Prints the calling script's header (the comment block after the shebang, without its "# "
# prefix), then the environment every script reads.
print_help() {
  awk 'NR == 1 { next } /^#/ { sub(/^# ?/, ""); print; next } { exit }' "$0"
  cat <<'EOF'

Environment:
  ALGCTL_BUILD     build directory; defaults to ~/algorithm-controller-build
                   when the repo lives on a Windows mount (/mnt/*), else
                   <repo>/build
  VCPKG_ROOT       vcpkg checkout providing the toolchain (required)
EOF
}

# For an argument the script does not know: names it, prints the help to stderr, exits 2.
die_usage() {
  echo "unknown argument: $1" >&2
  print_help >&2
  exit 2
}
