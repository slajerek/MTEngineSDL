#!/usr/bin/env bash
set -euo pipefail

# Build a static libSDL2.a from the vendored SDL2 source, so the macOS build
# no longer depends on Homebrew's "sdl2" formula -- it is nowadays an alias
# for sdl2-compat (a dynamic shim over SDL3) and ships no static archive,
# which broke the libtool step here ("could not find 'libSDL2.a'").

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
SDL2_SRC_DIR="$ROOT_DIR/other/lib/SDL-release-2.32.10-static"
OUT_LIB_DIR="$ROOT_DIR/platform/MacOS/libs"
OUT_LIB="$OUT_LIB_DIR/libSDL2.a"
STAMP_FILE="$OUT_LIB_DIR/libSDL2.stamp"

mkdir -p "$OUT_LIB_DIR"

if [[ ! -d "$SDL2_SRC_DIR" ]]; then
  echo "ERROR: vendored SDL2 source not found at $SDL2_SRC_DIR" >&2
  exit 1
fi

# Stamp-based caching: skip rebuild if source and script haven't changed.
# SDL2 is vendored directly (not a submodule), so hash a version-bearing
# file instead of a git SHA.
SDL2_SRC_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SDL2_SRC_SHA="$(shasum -a 256 "$SDL2_SRC_DIR/include/SDL_version.h" | cut -d ' ' -f 1)"
fi

SCRIPT_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

STAMP_VALUE="${SDL2_SRC_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

BUILD_DIR="$SDL2_SRC_DIR/build-macos"

cmake -S "$SDL2_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15 \
  -DSDL_STATIC=ON \
  -DSDL_SHARED=OFF \
  -DSDL_TEST=OFF

cmake --build "$BUILD_DIR" --config Release --target SDL2-static -j 8

found=""
for f in libSDL2.a libSDL2-static.a; do
  for d in "$BUILD_DIR" "$BUILD_DIR/src"; do
    if [[ -f "$d/$f" ]]; then
      found="$d/$f"
      break 2
    fi
  done
done
if [[ -z "$found" ]]; then
  found="$(find "$BUILD_DIR" -name "libSDL2*.a" -type f 2>/dev/null | head -1 || true)"
fi
if [[ -z "$found" ]]; then
  echo "ERROR: built SDL2 static lib not found in $BUILD_DIR" >&2
  find "$BUILD_DIR" -name "*.a" -type f >&2 || true
  exit 1
fi

rm -f "$OUT_LIB"
cp -f "$found" "$OUT_LIB"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "SDL2 built: $OUT_LIB"
