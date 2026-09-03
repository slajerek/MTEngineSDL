#!/usr/bin/env bash
set -euo pipefail

# Build the uSockets static library for macOS.
# Output: the caps-keyed view given by mt_caps_lib_dir; the build happens in
# this unit's own store (L16).
#
# uSockets is VENDORED in this repository -- not a submodule, not a sibling
# checkout, not a download. The committed copy carries a local ARM64 fix
# (`listenAddr = NULL` in src/bsd.c); pristine upstream leaves that pointer
# uninitialised, so an archive built from upstream is silently broken on ARM.
#
# WHY THIS FILE EXISTS AT ALL. Until L16 the macOS uSockets build was ~15 lines
# inline in build-macos.sh, and it was the only dependency on any platform
# without a producer script of its own. That cost two things. It had no stamp,
# so it re-ran `make` on every single build-macos.sh invocation -- unnoticed
# because a rebuilt uSockets.a is byte-identical, so nothing broke except time.
# And the registry test that checks a unit's store key against the capabilities
# its script reads could not be applied to it: scanning build-macos.sh attributes
# that whole script's reads to uSockets, which reads nothing.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USOCKETS_DIR="$ROOT_DIR/other/lib/uSockets"

if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). uSockets reads no capability, so its store key is empty
# and it has ONE bucket per machine, shared by every app. With no store in the
# environment (a standalone run) the store IS the view and the sync is a no-op.
mt_caps_use_store "${MT_STORE_USOCKETS:-}" usockets
OUT_LIB="$OUT_LIB_DIR/uSockets.a"
STAMP_FILE="$OUT_LIB_DIR/uSockets.stamp"

mkdir -p "$OUT_LIB_DIR"

# The Makefile alone is not proof of a whole tree, so check a patched source file
# too: a half-restored checkout builds and produces the broken archive.
if [[ ! -f "$USOCKETS_DIR/Makefile" || ! -f "$USOCKETS_DIR/src/bsd.c" ]]; then
  echo "Restoring the vendored uSockets from this repository's git"
  if ! git -C "$ROOT_DIR" checkout HEAD -- other/lib/uSockets; then
    echo "ERROR: could not restore other/lib/uSockets from git." >&2
    echo "       It is VENDORED here and carries a local ARM64 fix; do NOT clone" >&2
    echo "       uSockets from upstream over it -- the result builds and is" >&2
    echo "       silently broken on ARM." >&2
    exit 1
  fi
fi

# Vendored, so there is no independent git SHA to stamp against the way mbedTLS
# has one -- the content lives inside THIS repo's history. A content hash of the
# vendored tree is the substitute: it changes exactly when a commit touches the
# vendored source, and not otherwise. The script's own hash joins it so that
# editing the build here invalidates the stamp too.
#
# No hasher, no stamp: writing one that cannot distinguish two trees would turn
# every later build into a false hit. Rebuilding is the safe direction.
sha_of() { shasum -a 256 "$@" 2>/dev/null | cut -d ' ' -f 1; }
STAMP_VALUE=""
if command -v shasum >/dev/null 2>&1; then
  USOCKETS_SRC_SHA="$(find "$USOCKETS_DIR" -type f \( -name '*.c' -o -name '*.h' -o -name 'Makefile' \) -print0 \
      | sort -z | xargs -0 shasum -a 256 | shasum -a 256 | cut -d ' ' -f 1)"
  STAMP_VALUE="${USOCKETS_SRC_SHA}:$(sha_of "${BASH_SOURCE[0]}")"
fi
if [[ -n "$STAMP_VALUE" && -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    echo "uSockets: up to date (stamp match), skipping build"
    exit 0
  fi
fi

echo "Building uSockets"
# Build in a disposable copy under the work root: the in-tree `make` was the
# last build write inside this checkout on macOS. The vendored tree stays
# pristine and the stamp still hashes IT, not the copy.
USOCKETS_WORK="$(mt_caps_work_dir uSockets)/src-macos"
rm -rf "$USOCKETS_WORK"
mkdir -p "$USOCKETS_WORK"
cp -R "$USOCKETS_DIR/." "$USOCKETS_WORK/"
make -C "$USOCKETS_WORK" -j"$(sysctl -n hw.ncpu)"

cp -f "$USOCKETS_WORK/uSockets.a" "$OUT_LIB"
[[ -n "$STAMP_VALUE" ]] && printf '%s' "$STAMP_VALUE" > "$STAMP_FILE"

echo "uSockets built: $OUT_LIB"
