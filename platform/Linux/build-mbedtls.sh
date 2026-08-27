#!/usr/bin/env bash
set -euo pipefail

# Build mbedTLS static libs via CMake, then package into a single libmbedtls_bundle.a.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MBEDTLS_SRC_DIR="$ROOT_DIR/other/lib/mbedtls"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"
OUT_LIB="$OUT_LIB_DIR/libmbedtls_bundle.a"
STAMP_FILE="$OUT_LIB_DIR/libmbedtls_bundle.stamp"

mkdir -p "$OUT_LIB_DIR"

SCRIPT_SHA="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  SCRIPT_SHA="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
elif command -v shasum >/dev/null 2>&1; then
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
  ar rcs "$OUT_LIB" "$TMP_DIR/mbedtls_stub.o"
  ranlib "$OUT_LIB" 2>/dev/null || true

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

BUILD_DIR="$ROOT_DIR/other/lib/mbedtls.linux"

cmake -S "$MBEDTLS_SRC_DIR" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
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

TMP_DIR="$BUILD_DIR/mbedtls_bundle_tmp"
rm -rf "$TMP_DIR"
mkdir -p "$TMP_DIR"

(cd "$TMP_DIR" && ar x "${LIBS[0]}" && ar x "${LIBS[1]}" && ar x "${LIBS[2]}")
rm -f "$OUT_LIB"
ar rcs "$OUT_LIB" "$TMP_DIR"/*.o
ranlib "$OUT_LIB" 2>/dev/null || true

rm -rf "$TMP_DIR"

echo -n "$STAMP_VALUE" > "$STAMP_FILE"
