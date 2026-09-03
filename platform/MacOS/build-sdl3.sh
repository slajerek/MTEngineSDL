#!/usr/bin/env bash
set -euo pipefail

# Build a static libSDL3.a from the vendored SDL3 source.
#
# WHY VENDORED AND STATIC (do not "simplify" this):
#  - CLAUDE.md's third-party rule: we build our OWN copy of every library on
#    every platform. A Homebrew/MacPorts path in `otool -L` is a release
#    blocker for the App Store / Microsoft Store builds.
#  - Homebrew's "sdl2" formula is nowadays an alias for sdl2-compat (a dynamic
#    shim over SDL3) and ships no static archive at all, which is what broke
#    the libtool step here and caused build-sdl2.sh to be written in the first
#    place. The same trap is waiting for SDL3.
#  - SDL3 links INTO libMTEngineSDL.a, so no app ever ships an SDL runtime
#    library. See specs/superpowers/plans/2026-08-17-s2-sdl-3.4.14-upgrade.md.
#
# SDL_CAMERA=OFF is deliberate, and it is a PRODUCT decision, not a build tidy-up.
# SDL3 gained a camera subsystem that SDL2 never had. We do not use it -- the
# engine has had its own AVFoundation capture (MT_CAMERA_CAPTURE_ENABLED,
# CCameraMacOS) for years -- and leaving it on has two real costs:
#   1. It drags AVCaptureDevice/AVCaptureSession symbols into every app, so
#      every app must link AVFoundation whether or not it has a camera feature.
#      That is how this was discovered: the photo app failed to link.
#   2. These are STORE-SHIPPED apps. A camera-capable binary invites
#      NSCameraUsageDescription and a privacy declaration for a capability the
#      product does not have. Shipping an unused camera surface in a photo
#      culling tool is the wrong default.
# If an app ever wants SDL's camera, turn this back on and link AVFoundation
# there -- deliberately, with the privacy strings that go with it.
#
# THE TRAP THIS SCRIPT EXISTS TO AVOID: SDL3's CMake defaults to building the
# SHARED library ONLY. When SDL_SHARED_DEFAULT and SDL_STATIC_DEFAULT are both
# ON and BUILD_SHARED_LIBS is UNDEFINED, SDL3's CMakeLists.txt (3.4.14, ~line
# 220) sets SDL_STATIC_DEFAULT OFF with the comment "Default to just building
# the shared library". A plain `cmake ..` therefore produces no static archive,
# and the first symptom is a link error -- or worse, a successful link against
# a dylib that then has to be shipped. -DSDL_STATIC=ON -DSDL_SHARED=OFF are
# NOT optional here.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own. A script phase inherits
# ~600 Xcode build settings, and this script's own BUILD_DIR is one of the names
# in that namespace -- so the strip has to happen while those values are still
# purely inherited. (Measured the other way round: called after the assignments,
# it deleted the script's BUILD_DIR and the build died on an unbound variable.)
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env
SDL3_SRC_DIR="$ROOT_DIR/other/lib/SDL-release-3.4.14-static"
# OUTSIDE the checkout -- see mt_caps_lib_dir in ../caps-lib.sh for why this
# is a correctness change and not tidiness.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"
mkdir -p "$OUT_LIB_DIR"

# Store and view (L16): SDL3 reads no capability at all, so its store key is
# empty and it has ONE bucket for the life of the machine -- where before, any
# capability flip moved the whole libs directory and rebuilt it to produce the
# same 248 objects. With no store in the environment (a standalone run) the
# store IS the view and the sync is a no-op.
mt_caps_use_store "${MT_STORE_SDL3:-}" sdl3

OUT_LIB="$OUT_LIB_DIR/libSDL3.a"
STAMP_FILE="$OUT_LIB_DIR/libSDL3.stamp"

if [[ ! -d "$SDL3_SRC_DIR" ]]; then
  echo "ERROR: vendored SDL3 source not found at $SDL3_SRC_DIR" >&2
  exit 1
fi

# Stamp-based caching: skip rebuild if source and script haven't changed.
# SDL3 is vendored directly (not a submodule), so hash a version-bearing file
# instead of a git SHA.
SDL3_SRC_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SDL3_SRC_SHA="$(shasum -a 256 "$SDL3_SRC_DIR/include/SDL3/SDL_version.h" | cut -d ' ' -f 1)"
fi

SCRIPT_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

STAMP_VALUE="${SDL3_SRC_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    # The exit trap installed by mt_caps_use_store copies the store into the
    # view; a stamp hit means the store is good, not that the view has it.
    exit 0
  fi
fi

# Phase 5: the build tree lives OUTSIDE the checkout, in the shared work root.
BUILD_DIR="$(mt_caps_work_dir sdl3)/build-macos"

mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$SDL3_SRC_DIR"
cmake -S "$SDL3_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  -DSDL_STATIC=ON \
  -DSDL_SHARED=OFF \
  -DSDL_TEST_LIBRARY=OFF \
  -DBUILD_SHARED_LIBS=OFF \
  -DSDL_CAMERA=OFF

cmake --build "$BUILD_DIR" --config Release --target SDL3-static -j 8

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

# Assert we really got a static archive and not something else. A shared build
# leaves a .dylib here instead, and the failure would otherwise surface much
# later as a missing symbol or a shipped runtime library.
if [[ -n "$(find "$BUILD_DIR" -maxdepth 1 -name 'libSDL3*.dylib' -print -quit)" ]]; then
  echo "ERROR: SDL3 produced a dylib -- the static-only flags did not take." >&2
  exit 1
fi

rm -f "$OUT_LIB"
cp -f "$found" "$OUT_LIB"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "SDL3 built: $OUT_LIB"
