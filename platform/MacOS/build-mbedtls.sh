#!/usr/bin/env bash
set -euo pipefail

# Build mbedTLS static libs via CMake, then package into a single libmbedtls_bundle.a.
# Used by the game app/MTEngineSDL to enable HTTPS downloads (cpp-httplib + mbedTLS).

export PATH="/opt/local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own. A script phase inherits
# ~600 Xcode build settings, and this script's own BUILD_DIR is one of the names
# in that namespace -- so the strip has to happen while those values are still
# purely inherited. (Measured the other way round: called after the assignments,
# it deleted the script's BUILD_DIR and the build died on an unbound variable.)
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env
MBEDTLS_SRC_DIR="$ROOT_DIR/other/lib/mbedtls"
# OUTSIDE the checkout -- see mt_caps_lib_dir in ../caps-lib.sh for why this
# is a correctness change and not tidiness.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). mbedTLS follows MT_CAP_HTTPS and nothing else.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_MBEDTLS:-}" mbedtls
OUT_LIB="$OUT_LIB_DIR/libmbedtls_bundle.a"
STAMP_FILE="$OUT_LIB_DIR/libmbedtls_bundle.stamp"

mkdir -p "$OUT_LIB_DIR"

SCRIPT_SHA="unknown"
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

if [[ "${MT_ENABLE_MBEDTLS:-1}" == "0" ]]; then
  # Create a stub archive so projects that always link this lib still build.
  STAMP_VALUE="disabled:${SCRIPT_SHA}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
    if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
      exit 0
    fi
  fi

  TMP_DIR="$OUT_LIB_DIR/mbedtls_stub"
  rm -rf "$TMP_DIR"
  mkdir -p "$TMP_DIR"

  cat > "$TMP_DIR/mbedtls_stub.c" <<'EOF'
int mbedtls_bundle_disabled_stub = 0;
EOF

  cc -c "$TMP_DIR/mbedtls_stub.c" -o "$TMP_DIR/mbedtls_stub.o"
  rm -f "$OUT_LIB"
  libtool -static -o "$OUT_LIB" "$TMP_DIR/mbedtls_stub.o"
  ranlib "$OUT_LIB" || true

  echo -n "$STAMP_VALUE" > "$STAMP_FILE"
  rm -rf "$TMP_DIR"
  exit 0
fi

if [[ ! -d "$MBEDTLS_SRC_DIR" ]]; then
  echo "ERROR: mbedtls submodule not found at $MBEDTLS_SRC_DIR" >&2
  echo "Run: git submodule update --init --recursive" >&2
  exit 1
fi

MBEDTLS_GIT_SHA="unknown"
if command -v git >/dev/null 2>&1; then
  if git -C "$MBEDTLS_SRC_DIR" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    MBEDTLS_GIT_SHA="$(git -C "$MBEDTLS_SRC_DIR" rev-parse --short HEAD)"
  fi
fi

STAMP_VALUE="${MBEDTLS_GIT_SHA}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

# Phase 5: the build tree lives OUTSIDE the checkout, in the shared work root.
BUILD_DIR="$(mt_caps_work_dir mbedtls)/build-macos"

mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$MBEDTLS_SRC_DIR"
cmake -S "$MBEDTLS_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" \
  -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_TESTING=OFF \
  -DENABLE_PROGRAMS=OFF

cmake --build "$BUILD_DIR" --config Release --target mbedcrypto mbedx509 mbedtls -j 8

LIBS=(
  "$BUILD_DIR/library/libmbedcrypto.a"
  "$BUILD_DIR/library/libmbedx509.a"
  "$BUILD_DIR/library/libmbedtls.a"
)

for p in "${LIBS[@]}"; do
  if [[ ! -f "$p" ]]; then
    echo "ERROR: expected mbedTLS library not found: $p" >&2
    (ls -la "$BUILD_DIR/library" 2>/dev/null || true) >&2
    exit 1
  fi
done

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${LIBS[@]}"
ranlib "$OUT_LIB" || true

echo -n "$STAMP_VALUE" > "$STAMP_FILE"
