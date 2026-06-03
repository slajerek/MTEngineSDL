#!/usr/bin/env bash
set -euo pipefail

# Build FTXUI static libs via CMake, then package into a single libftxui.a.
# FTXUI has 3 components: screen, dom, component — merged into one archive.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FTXUI_SRC_DIR="$ROOT_DIR/other/lib/ftxui"
OUT_LIB_DIR="$ROOT_DIR/platform/MacOS/libs"
OUT_LIB="$OUT_LIB_DIR/libftxui.a"

mkdir -p "$OUT_LIB_DIR"

if [[ ! -d "$FTXUI_SRC_DIR" ]]; then
  echo "ERROR: ftxui submodule not found at $FTXUI_SRC_DIR" >&2
  echo "Run: git submodule update --init --recursive" >&2
  exit 1
fi

# Stamp-based caching: skip rebuild if source and script haven't changed.
FTXUI_GIT_SHA="unknown"
if command -v git >/dev/null 2>&1; then
  if git -C "$FTXUI_SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    FTXUI_GIT_SHA="$(git -C "$FTXUI_SRC_DIR" rev-parse --short HEAD)"
  fi
fi

SCRIPT_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

STAMP_FILE="$OUT_LIB_DIR/libftxui.stamp"
STAMP_VALUE="${FTXUI_GIT_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

BUILD_DIR="$FTXUI_SRC_DIR/build-macos"

cmake -S "$FTXUI_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DBUILD_SHARED_LIBS=OFF \
  -DFTXUI_BUILD_DOCS=OFF \
  -DFTXUI_BUILD_EXAMPLES=OFF \
  -DFTXUI_BUILD_MODULES=OFF \
  -DFTXUI_BUILD_TESTS=OFF \
  -DFTXUI_BUILD_TESTS_FUZZER=OFF \
  -DFTXUI_ENABLE_INSTALL=OFF \
  -DFTXUI_QUIET=ON

cmake --build "$BUILD_DIR" --config Release --target screen dom component -j 8

# Collect built static libraries.
LIBS=()
for f in libftxui-screen.a libftxui-dom.a libftxui-component.a; do
  found=""
  for d in "$BUILD_DIR" "$BUILD_DIR/src" "$BUILD_DIR/src/ftxui"; do
    if [[ -f "$d/$f" ]]; then
      found="$d/$f"
      break
    fi
  done
  if [[ -z "$found" ]]; then
    # Fallback: search recursively
    found="$(find "$BUILD_DIR" -name "$f" -type f 2>/dev/null | head -1 || true)"
  fi
  if [[ -n "$found" ]]; then
    LIBS+=("$found")
  else
    echo "WARNING: $f not found in build dir, skipping" >&2
  fi
done

if [[ ${#LIBS[@]} -lt 3 ]]; then
  echo "ERROR: expected 3 FTXUI static libs, found ${#LIBS[@]} in build dir: $BUILD_DIR" >&2
  find "$BUILD_DIR" -name "*.a" -type f >&2 || true
  exit 1
fi

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${LIBS[@]}"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "FTXUI built: $OUT_LIB (${#LIBS[@]} components merged)"
