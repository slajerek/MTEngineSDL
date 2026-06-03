#!/usr/bin/env bash
set -euo pipefail

# Build uSockets static library for Linux.
# Downloads uSockets source into MTEngineSDL/other/lib/uSockets if not present.
# Output: platform/Linux/libs/uSockets.a

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
USOCKETS_DIR="$ROOT_DIR/other/lib/uSockets"
OUT_LIB_DIR="$ROOT_DIR/platform/Linux/libs"
OUT_LIB="$OUT_LIB_DIR/uSockets.a"

mkdir -p "$OUT_LIB_DIR"

if [[ ! -d "$USOCKETS_DIR" ]]; then
  echo "Cloning uSockets into $USOCKETS_DIR"
  git clone https://github.com/uNetworking/uSockets.git "$USOCKETS_DIR"
fi

echo "Building uSockets"
cd "$USOCKETS_DIR"
make -j"$(nproc)"

cp -f "$USOCKETS_DIR/uSockets.a" "$OUT_LIB"

echo "uSockets built: $OUT_LIB"
