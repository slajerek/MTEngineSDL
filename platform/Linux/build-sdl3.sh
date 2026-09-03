#!/usr/bin/env bash
set -euo pipefail

# Build a static libSDL3.a from the vendored SDL3 source, for Linux.
#
# WHY THIS SCRIPT EXISTS AT ALL -- it is a FIX, not just a build step.
#
# CLAUDE.md's rule is that we build our OWN copy of every third-party library
# on every platform, because a package-manager path in a shipped artifact is a
# release blocker for the store builds. Linux violated that for SDL from the
# beginning: `find_package(SDL2 REQUIRED)` plus `apt-get install libsdl2-dev`,
# carried on the CM tracker for months as "pre-existing".
#
# The SDL3 port is the moment it stops being pre-existing, because the
# dependency is being replaced anyway. Adding find_package(SDL3 REQUIRED) would
# knowingly re-commit the violation in new code.
#
# There was no platform/Linux/build-sdl2.sh to copy -- Linux had NEVER vendored
# SDL. The model is platform/MacOS/build-sdl3.sh; SDL builds with CMake
# identically on both.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDL3_SRC_DIR="$ROOT_DIR/other/lib/SDL-release-3.4.14-static"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). SDL3 reads no capability at all: one bucket for the life of the machine.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_SDL3:-}" sdl3
OUT_LIB="$OUT_LIB_DIR/libSDL3.a"
STAMP_FILE="$OUT_LIB_DIR/libSDL3.stamp"

mkdir -p "$OUT_LIB_DIR"

if [[ ! -d "$SDL3_SRC_DIR" ]]; then
  echo "ERROR: vendored SDL3 source not found at $SDL3_SRC_DIR" >&2
  exit 1
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  elif command -v shasum >/dev/null 2>&1; then
    shasum -a 256 "$1" | cut -d ' ' -f 1
  else
    echo unknown
  fi
}

SDL3_SRC_SHA="$(sha256_of "$SDL3_SRC_DIR/include/SDL3/SDL_version.h")"
SCRIPT_SHA="$(sha256_of "${BASH_SOURCE[0]}")"
STAMP_VALUE="${SDL3_SRC_SHA}:${SCRIPT_SHA}"

if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

# Phase 5: the build tree lives OUTSIDE the checkout, in the shared work root.
BUILD_DIR="$(mt_caps_work_dir sdl3)/build-linux"

# -DSDL_STATIC=ON -DSDL_SHARED=OFF are NOT optional: SDL3's CMake defaults to
# building the shared library ONLY when BUILD_SHARED_LIBS is undefined
# (CMakeLists.txt ~line 220, "Default to just building the shared library"), so
# a plain `cmake ..` produces no static archive at all.
#
# SDL_DEPS_SHARED is deliberately LEFT AT ITS DEFAULT (ON). It makes SDL dlopen
# X11/Wayland/PulseAudio at RUNTIME, which is exactly what lets one binary run
# across distros. Forcing it off would hard-link them and make portability
# WORSE, not better. Static SDL and dynamically probed system compositors are
# not in conflict -- do not "fix" this.
#
# SDL_CAMERA=OFF matches the macOS script: we have our own capture path and
# these are store-shipped apps, so an unused camera surface is the wrong
# default. See platform/MacOS/build-sdl3.sh for the full reasoning.
mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$SDL3_SRC_DIR"
cmake -S "$SDL3_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DSDL_STATIC=ON \
  -DSDL_SHARED=OFF \
  -DSDL_TEST_LIBRARY=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_CAMERA=OFF

cmake --build "$BUILD_DIR" --config Release --target SDL3-static -j "$(nproc)"

found=""
for f in libSDL3.a libSDL3-static.a; do
  for d in "$BUILD_DIR" "$BUILD_DIR/src"; do
    if [[ -f "$d/$f" ]]; then
      found="$d/$f"
      break 2
    fi
  done
done
if [[ -z "$found" ]]; then
  found="$(find "$BUILD_DIR" -name "libSDL3*.a" -type f 2>/dev/null | head -1 || true)"
fi
if [[ -z "$found" ]]; then
  echo "ERROR: built SDL3 static lib not found in $BUILD_DIR" >&2
  find "$BUILD_DIR" -name "*.a" -type f >&2 || true
  exit 1
fi

# Assert we really got a static archive. A shared build leaves a .so here
# instead, and the failure would otherwise surface much later -- as a missing
# symbol, or worse as a runtime library that gets shipped.
if [[ -n "$(find "$BUILD_DIR" -maxdepth 1 -name 'libSDL3*.so*' -print -quit)" ]]; then
  echo "ERROR: SDL3 produced a shared object -- the static-only flags did not take." >&2
  exit 1
fi

rm -f "$OUT_LIB"
cp -f "$found" "$OUT_LIB"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "SDL3 built: $OUT_LIB"
