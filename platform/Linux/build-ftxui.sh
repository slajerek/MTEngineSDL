#!/usr/bin/env bash
set -euo pipefail

# Build FTXUI static library for Linux.
# Output: platform/Linux/libs/libftxui.a (3 components merged)

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
FTXUI_SRC_DIR="$ROOT_DIR/other/lib/ftxui"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). FTXUI follows MT_CAP_FTXUI and nothing else.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_FTXUI:-}" ftxui
OUT_LIB="$OUT_LIB_DIR/libftxui.a"

mkdir -p "$OUT_LIB_DIR"

if [[ ! -d "$FTXUI_SRC_DIR" ]]; then
  echo "ERROR: ftxui submodule not found at $FTXUI_SRC_DIR" >&2
  echo "Run: git submodule update --init --recursive" >&2
  exit 1
fi

# Stamp-based caching
FTXUI_GIT_SHA="unknown"
if command -v git >/dev/null 2>&1; then
  if git -C "$FTXUI_SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    FTXUI_GIT_SHA="$(git -C "$FTXUI_SRC_DIR" rev-parse --short HEAD)"
  fi
fi

SCRIPT_SHA="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  SCRIPT_SHA="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
elif command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

# THE CAPABILITY GATE. The macOS script has had one since the programme started;
# this one did not, so MT_CAP_FTXUI=0 still built FTXUI on Linux. build-linux.sh
# already exports the flags via mt_caps_read_flags, so the gate only had to be
# written -- unlike Windows, where the publishing half was missing too.
#
# Absent means ON, so a bare engine build still builds everything.
if [[ "${MT_ENABLE_FTXUI:-1}" == "0" ]]; then
  STUB_STAMP="disabled:${SCRIPT_SHA}"
  if [[ -f "$OUT_LIB" && -f "$OUT_LIB_DIR/libftxui.stamp" && "$(cat "$OUT_LIB_DIR/libftxui.stamp" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "ftxui"
  echo -n "$STUB_STAMP" > "$OUT_LIB_DIR/libftxui.stamp"
  exit 0
fi

STAMP_FILE="$OUT_LIB_DIR/libftxui.stamp"
STAMP_VALUE="${FTXUI_GIT_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    echo "FTXUI: up to date (stamp match), skipping build"
    exit 0
  fi
fi

# Phase 5: the build tree lives OUTSIDE the checkout, in the shared work root.
BUILD_DIR="$(mt_caps_work_dir ftxui)/build-linux"

echo "Configuring FTXUI in $BUILD_DIR"
mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$FTXUI_SRC_DIR"
cmake -S "$FTXUI_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF \
  -DFTXUI_BUILD_DOCS=OFF \
  -DFTXUI_BUILD_EXAMPLES=OFF \
  -DFTXUI_BUILD_MODULES=OFF \
  -DFTXUI_BUILD_TESTS=OFF \
  -DFTXUI_BUILD_TESTS_FUZZER=OFF \
  -DFTXUI_ENABLE_INSTALL=OFF \
  -DFTXUI_QUIET=ON

echo "Building FTXUI"
cmake --build "$BUILD_DIR" --config Release --target screen dom component -j "$(nproc)"

# Collect and merge
LIBS=()
for f in libftxui-screen.a libftxui-dom.a libftxui-component.a; do
  found="$(find "$BUILD_DIR" -name "$f" -type f 2>/dev/null | head -1 || true)"
  if [[ -n "$found" ]]; then
    LIBS+=("$found")
  else
    echo "ERROR: $f not found in $BUILD_DIR" >&2
    exit 1
  fi
done

# Use ar MRI script to merge
rm -f "$OUT_LIB"
MRI_FILE="$OUT_LIB_DIR/ftxui_merge.mri"
{
  echo "CREATE $OUT_LIB"
  for lib in "${LIBS[@]}"; do
    echo "ADDLIB $lib"
  done
  echo "SAVE"
  echo "END"
} > "$MRI_FILE"

ar -M < "$MRI_FILE"
ranlib "$OUT_LIB" || true
rm -f "$MRI_FILE"

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "FTXUI built: $OUT_LIB (${#LIBS[@]} components merged)"
