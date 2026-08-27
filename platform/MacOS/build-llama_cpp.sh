#!/usr/bin/env bash
set -euo pipefail

# Build llama.cpp static libs via CMake, then package into a single libllama_cpp.a.
# This is used by LightHeroes/MTEngineSDL macOS builds.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own. A script phase inherits
# ~600 Xcode build settings, and this script's own BUILD_DIR is one of the names
# in that namespace -- so the strip has to happen while those values are still
# purely inherited. (Measured the other way round: called after the assignments,
# it deleted the script's BUILD_DIR and the build died on an unbound variable.)
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env
LLAMA_SRC_DIR="$ROOT_DIR/other/lib/llama.cpp"
# OUTSIDE the checkout -- see mt_caps_lib_dir in ../caps-lib.sh for why this
# is a correctness change and not tidiness.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"
OUT_LIB="$OUT_LIB_DIR/libllama_cpp.a"

mkdir -p "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning llama.cpp build artifacts..."
  rm -f "$OUT_LIB" "$STAMP_FILE"
  rm -rf "$LLAMA_SRC_DIR/build-macos"
  echo "Done. Run without 'clean' to rebuild."
  exit 0
fi

# ---------------------------------------------------------------------------
# Capability gate.
#
# The flag arrives as a build setting (an Xcode script phase sees settings as
# environment variables) or as an exported variable from the wrapper. Default 1,
# so a standalone engine build with no manifest behaves exactly as before.
#
# It emits a STUB ARCHIVE rather than skipping, following the precedent
# build-mbedtls.sh already set: a PBXBuildFile cannot be conditioned on a build
# setting, so simply not producing the archive fails the LINK on a missing .a,
# in four different app-side linking idioms. A stub carries no library symbols,
# so the `nm` proof still distinguishes on from off.
#
# It comes BEFORE the submodule check on purpose. With the capability off the
# submodule is not fetched at all -- that is the point -- so a missing-submodule
# error here would turn the saving into a build failure.
# ---------------------------------------------------------------------------
source "$ROOT_DIR/platform/caps-lib.sh"

if [[ "${MT_ENABLE_LLAMA_CPP:-1}" == "0" ]]; then
  STUB_STAMP="disabled:$(shasum -a 256 "${BASH_SOURCE[0]}" 2>/dev/null | cut -d ' ' -f 1)"
  if [[ -f "$OUT_LIB" && -f "$OUT_LIB_DIR/libllama_cpp.stamp" && "$(cat "$OUT_LIB_DIR/libllama_cpp.stamp" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "llama_cpp"
  echo -n "$STUB_STAMP" > "$OUT_LIB_DIR/libllama_cpp.stamp"
  exit 0
fi

if [[ ! -d "$LLAMA_SRC_DIR" ]]; then
  echo "ERROR: llama.cpp submodule not found at $LLAMA_SRC_DIR" >&2
  echo "Run: git submodule update --init --recursive" >&2
  exit 1
fi

LLAMA_GIT_SHA="unknown"
if command -v git >/dev/null 2>&1; then
  if git -C "$LLAMA_SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    LLAMA_GIT_SHA="$(git -C "$LLAMA_SRC_DIR" rev-parse --short HEAD)"
  fi
fi

SCRIPT_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

STAMP_FILE="$OUT_LIB_DIR/libllama_cpp.stamp"
STAMP_VALUE="${LLAMA_GIT_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

BUILD_DIR="$LLAMA_SRC_DIR/build-macos"

cmake -S "$LLAMA_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DBUILD_SHARED_LIBS=OFF \
  -DLLAMA_BUILD_COMMON=OFF \
  -DLLAMA_BUILD_TESTS=OFF \
  -DLLAMA_BUILD_TOOLS=OFF \
  -DLLAMA_BUILD_EXAMPLES=OFF \
  -DLLAMA_BUILD_SERVER=OFF \
  -DLLAMA_OPENSSL=OFF \
  -DLLAMA_BUILD_HTML=OFF \
  -DGGML_METAL=ON \
  -DGGML_METAL_EMBED_LIBRARY=ON \
  -DGGML_CUDA=OFF \
  -DGGML_VULKAN=OFF

# Build only the libraries we need.
cmake --build "$BUILD_DIR" --config Release --target llama ggml ggml-base ggml-cpu ggml-blas ggml-metal -j 8

LIBS=()
SEARCH_DIRS=(
  "$BUILD_DIR/src"
  "$BUILD_DIR/ggml/src"
  "$BUILD_DIR/ggml/src/ggml-blas"
  "$BUILD_DIR/ggml/src/ggml-metal"
)

for f in libllama.a libggml.a libggml-base.a libggml-cpu.a libggml-blas.a libggml-metal.a; do
  for d in "${SEARCH_DIRS[@]}"; do
    if [[ -f "$d/$f" ]]; then
      LIBS+=("$d/$f")
      break
    fi
  done
done

if [[ ${#LIBS[@]} -lt 3 ]]; then
  echo "ERROR: expected llama.cpp static libs not found in build dir: $BUILD_DIR" >&2
  echo "Found libs:" >&2
  (ls -la "$BUILD_DIR/src" 2>/dev/null || true) >&2
  (ls -la "$BUILD_DIR/ggml/src" 2>/dev/null || true) >&2
  exit 1
fi

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${LIBS[@]}"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

# Generate version header from git tag (e.g. "b8235")
LLAMA_VERSION_TAG="unknown"
if command -v git >/dev/null 2>&1; then
  TAG="$(git -C "$LLAMA_SRC_DIR" describe --tags --abbrev=0 2>/dev/null || true)"
  if [[ -n "$TAG" ]]; then
    LLAMA_VERSION_TAG="$TAG"
  fi
fi

# NOT src/Engine/Sci/Llama/llama_cpp_version.h, which is TRACKED.
#
# This was the strongest form of the invariant violation: an app build writing a
# tracked file inside the engine checkout, on every build with MT_CAP_LLM=1. It
# also fed back into the output-root key -- engine_rev() appends
# `-dirty-<sha(status+diff)>`, so the moment `git describe` yielded anything but
# the committed tag, every subsequent resolve landed in a DIFFERENT $MT_OUT and
# the keyed cache was thrown away.
#
# It goes under the generated include dir instead, which is already on every
# build system's include path. mtcaps writes a placeholder there first, so the
# header exists even when this script never runs (MT_CAP_LLM=0 exits at the stub
# above) -- the file is included unconditionally by CGuiViewLlamaModelLoader.cpp.
MT_GEN_INCLUDE="${MT_CAPS_OUT:-${MTENGINE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/mtengine}/_standalone}/include"
VERSION_HEADER="$MT_GEN_INCLUDE/Sci/Llama/llama_cpp_version.h"
mkdir -p "$(dirname "$VERSION_HEADER")"
cat > "$VERSION_HEADER" <<HEADER_EOF
// Auto-generated by build-llama_cpp.sh — do not edit, do not commit
#pragma once
#define MT_LLAMA_CPP_VERSION "$LLAMA_VERSION_TAG"
HEADER_EOF
