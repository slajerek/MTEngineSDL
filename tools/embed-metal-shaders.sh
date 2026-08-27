#!/bin/bash
#
# Compile every MSL shader to a .metallib and embed it in a committed header.
#
# WHY EMBED. c64d must be a single executable -- it embeds its fonts, icons and
# ROMs into src/Embedded/*.h precisely to get that -- so a .metallib sitting
# next to the binary is not an option. Precompiling also moves shader syntax
# errors from a user's first launch to our build, and takes MSL compilation off
# startup entirely.
#
# Runtime source compilation stays as the DEVELOPMENT path: edit a .metal file,
# relaunch, see it. Regenerate before committing.
#
# Usage:
#   ./tools/embed-metal-shaders.sh          regenerate the headers
#   ./tools/embed-metal-shaders.sh --check  fail if any header is stale (CI/build)

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
SHADER_DIR="$REPO/platform/MacOS/shaders"
OUT_DIR="$REPO/platform/MacOS/src.MacOS/Render/Generated"
CHECK_ONLY=false

if [ "${1:-}" = "--check" ]; then
    CHECK_ONLY=true
fi

if ! command -v xcrun >/dev/null 2>&1; then
    echo "embed-metal-shaders: xcrun not found; this script is macOS-only" >&2
    exit 1
fi

mkdir -p "$OUT_DIR"
TMP_DIR="$(mktemp -d)"
trap 'rm -rf "$TMP_DIR"' EXIT

status=0
found=false

for src in "$SHADER_DIR"/*.metal; do
    [ -e "$src" ] || continue
    found=true
    name="$(basename "$src" .metal)"
    out="$OUT_DIR/${name}Metallib.h"

    if [ "$CHECK_ONLY" = true ]; then
        # Compare the source's hash against the one recorded in the header.
        # A HASH, not a timestamp: git does not preserve mtimes, so a fresh
        # clone or a branch switch would fail or pass depending on checkout
        # order, and "regenerate because the mtime moved" teaches people to
        # regenerate without looking at what changed.
        if [ ! -f "$out" ]; then
            echo "STALE: $out does not exist" >&2
            status=1
            continue
        fi
        want="$(shasum -a 256 "$src" | cut -d' ' -f1)"
        have="$(grep -o 'k[A-Za-z0-9_]*SourceSha256 = "[0-9a-f]*"' "$out" | grep -o '"[0-9a-f]*"' | tr -d '"')"
        if [ "$want" != "$have" ]; then
            echo "STALE: $out was generated from a different $name.metal" >&2
            echo "       run ./tools/embed-metal-shaders.sh and commit the result" >&2
            status=1
        fi
        continue
    fi

    air="$TMP_DIR/$name.air"
    lib="$TMP_DIR/$name.metallib"

    # -ffast-math is NOT passed. The Metal ports are judged against their GLSL
    # originals pixel for pixel, and relaxed floating point is exactly the kind
    # of difference that shows up as a small, hard-to-attribute colour shift.
    xcrun -sdk macosx metal -std=macos-metal2.0 -c "$src" -o "$air"
    xcrun -sdk macosx metallib "$air" -o "$lib"

    python3 "$REPO/tools/bin2header.py" "$name" "$src" "$lib" "$out"
done

if [ "$found" = false ]; then
    echo "embed-metal-shaders: no .metal files in $SHADER_DIR" >&2
    exit 1
fi

if [ "$CHECK_ONLY" = true ] && [ $status -eq 0 ]; then
    echo "embed-metal-shaders: all generated headers are up to date"
fi

exit $status
