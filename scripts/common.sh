# Sourced by start.sh and test.sh: resolves REPO_DIR and BUILD.
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
