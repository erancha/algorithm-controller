# Sourced by start.sh and test.sh: resolves REPO_DIR and BUILD, and provides
# the shared build entry point and --help boilerplate.
#
# BUILD honors ALGCTL_BUILD when set. Otherwise, a repo on a Windows mount
# (/mnt/*) builds under $HOME — WSL2's bridge to Windows drives makes linking
# the ~450 MB debug binaries there roughly an order of magnitude slower than
# the Linux filesystem. A repo already on the Linux filesystem builds in-tree.
REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
if [[ -n "${ALGCTL_BUILD:-}" ]]; then
  BUILD="$ALGCTL_BUILD"
elif [[ "$REPO_DIR" == /mnt/* ]]; then
  BUILD="$HOME/algorithm-controller-build"
else
  BUILD="$REPO_DIR/build"
fi

# Configures CMake on the first run, then builds. Parallelism is capped at the
# core count: every executable links the static gRPC stack at a few GB of
# memory per link, and make's unbounded -j runs all of those links at once —
# enough to exhaust a 16 GB machine (it OOM-kills CI runners).
build_project() {
  [[ -d "$BUILD" ]] || cmake -B "$BUILD" -S "$REPO_DIR" \
    -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:?set VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
  cmake --build "$BUILD" -j "$(nproc)"
}

# Environment section shared by every script's --help text.
usage_environment() {
  cat <<'EOF'

Environment:
  ALGCTL_BUILD     build directory; defaults to ~/algorithm-controller-build
                   when the repo lives on a Windows mount (/mnt/*), else
                   <repo>/build
  VCPKG_ROOT       vcpkg checkout providing the toolchain (required)
EOF
}
