#!/usr/bin/env bash
set -euo pipefail

# Build uSockets static library for Linux.
# Output: platform/Linux/libs/uSockets.a
#
# uSockets is VENDORED in this repository -- not a submodule, not a sibling
# checkout, not a download. The committed copy carries a local ARM64 fix
# (`listenAddr = NULL` in src/bsd.c:484, :602); pristine upstream leaves that
# pointer uninitialised, so an archive built from upstream is silently broken on
# ARM.
#
# This script used to `git clone` upstream INTO the vendored path whenever the
# directory was absent -- which is why two apps carried an `rm -rf` plus
# `git checkout HEAD -- other/lib/uSockets` workaround: they were defending
# against the engine's own build script overwriting the engine's own patched
# source. That is an app repairing its dependency's checkout, which is a bug in
# the dependency. Restoring the vendored copy is THIS repository's job, so it is
# done here, from git, and never from upstream.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USOCKETS_DIR="$ROOT_DIR/other/lib/uSockets"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"
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

# uSockets is vendored, not a submodule, so there is no independent git SHA to
# stamp against the way mbedTLS's is -- the content lives inside THIS repo's own
# history. A content hash of the vendored tree is the substitute: it changes
# exactly when a future commit touches the vendored source, and not otherwise.
# Every sibling acquisition script in this directory self-skips on a stamp; this
# one used to `make` unconditionally on every invocation of build-linux.sh,
# never caught by the "no thrash between apps" measurement only because
# uSockets carries no capability-off stub to overwrite -- it was rebuilding a
# byte-identical archive, not a wrong one, so nothing broke, only time did.
USOCKETS_SRC_SHA="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  USOCKETS_SRC_SHA="$(find "$USOCKETS_DIR" -type f \( -name '*.c' -o -name '*.h' -o -name 'Makefile' \) -print0 \
      | sort -z | xargs -0 sha256sum | sha256sum | cut -d ' ' -f 1)"
fi
SCRIPT_SHA="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  SCRIPT_SHA="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi
STAMP_VALUE="${USOCKETS_SRC_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    echo "uSockets: up to date (stamp match), skipping build"
    exit 0
  fi
fi

echo "Building uSockets"
# Phase 5: `make` in the vendored tree was the last in-checkout build write.
# Build in a disposable copy under the work root instead; the vendored tree
# stays pristine and the stamp still hashes IT, not the copy.
USOCKETS_WORK="$(mt_caps_work_dir uSockets)/src-linux"
rm -rf "$USOCKETS_WORK"
mkdir -p "$USOCKETS_WORK"
cp -R "$USOCKETS_DIR/." "$USOCKETS_WORK/"
make -C "$USOCKETS_WORK" -j"$(nproc)"

cp -f "$USOCKETS_WORK/uSockets.a" "$OUT_LIB"
echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "uSockets built: $OUT_LIB"
