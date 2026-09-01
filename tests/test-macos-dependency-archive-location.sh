#!/usr/bin/env bash
#
# THE RULE: an app's build never writes inside this checkout. The six macOS
# dependency archives used to violate it outright -- every acquisition script
# staged into platform/MacOS/libs/. They now stage to $MT_CAPS_LIBS_DIR, which
# both build channels supply and which lives outside every checkout.
#
# Two things are asserted, and the second is the one that will actually rot:
#
#   1. Nothing names the old in-checkout path any more.
#   2. Every acquisition script strips the HOST build environment BEFORE it
#      defines anything of its own. A script phase inherits ~600 Xcode build
#      settings; `make` imports every environment variable as a make variable;
#      and libvpx's build/make/Makefile does `BUILD_ROOT?=.` followed by
#      `CFLAGS+=-I$(BUILD_PFX)$(BUILD_ROOT)`. Xcode's BUILD_ROOT therefore
#      replaced the libvpx build directory in the include path and every
#      `#include "vpx_config.h"` failed. That bug sat undetected for as long as
#      the archives lived in the checkout, because the stamp was always current
#      and the phase always self-skipped.
#
#      The ORDER matters as much as the call: these scripts use BUILD_DIR
#      themselves, and stripping after the assignment deletes their own value.
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MACOS_DIR="$ROOT_DIR/platform/MacOS"
ENGINE_PROJECT="$MACOS_DIR/MTEngineSDL.xcodeproj/project.pbxproj"

SCRIPTS=(build-sdl3.sh build-ftxui.sh build-mbedtls.sh
         build-image_codecs.sh build-video_codecs.sh build-llama_cpp.sh)

fail() { echo "$*" >&2; exit 1; }

for s in "${SCRIPTS[@]}"; do
    f="$MACOS_DIR/$s"
    [[ -f "$f" ]] || fail "missing acquisition script: $f"

    grep -Fq 'OUT_LIB_DIR="$(mt_caps_lib_dir)"' "$f" \
        || fail "$s does not resolve its output directory through mt_caps_lib_dir"

    if grep -Fq 'platform/MacOS/libs' "$f"; then
        fail "$s still names the in-checkout archive directory"
    fi

    strip_line="$(grep -n '^mt_caps_strip_host_build_env$' "$f" | head -1 | cut -d: -f1)"
    [[ -n "$strip_line" ]] || fail "$s does not call mt_caps_strip_host_build_env"

    # BEFORE the script's own BUILD_DIR, or the strip removes it.
    build_dir_line="$(grep -n '^BUILD_DIR=' "$f" | head -1 | cut -d: -f1)"
    if [[ -n "$build_dir_line" && "$strip_line" -gt "$build_dir_line" ]]; then
        fail "$s strips the host build environment (line $strip_line) AFTER assigning BUILD_DIR (line $build_dir_line)"
    fi
done

# BUILD_ROOT is the measured culprit; if it ever leaves the list, say so here
# rather than in a twenty-minute build that fails inside libvpx.
grep -Fq 'BUILD_ROOT' "$ROOT_DIR/platform/caps-lib.sh" \
    || fail "caps-lib.sh no longer strips BUILD_ROOT -- libvpx will fail under Xcode"

# MT_CAPS_LIBS_DIR wins when supplied, and the fallback is outside every checkout.
# shellcheck source=../platform/caps-lib.sh
source "$ROOT_DIR/platform/caps-lib.sh"
supplied="$(MT_CAPS_LIBS_DIR=/tmp/mt-caps-probe bash -c 'source "'"$ROOT_DIR"'/platform/caps-lib.sh"; mt_caps_lib_dir')"
[[ "$supplied" == "/tmp/mt-caps-probe" ]] \
    || fail "mt_caps_lib_dir ignored a supplied MT_CAPS_LIBS_DIR (got: $supplied)"

fallback="$(MT_CAPS_LIBS_DIR= bash -c 'source "'"$ROOT_DIR"'/platform/caps-lib.sh"; mt_caps_lib_dir')"
case "$fallback" in
    "$ROOT_DIR"/*) fail "mt_caps_lib_dir falls back INSIDE the engine checkout: $fallback" ;;
esac

# The project links through the search path, never a path into the checkout.
grep -Fq '$(MT_CAPS_LIBS_DIR)' "$ENGINE_PROJECT" \
    || fail "engine project does not use \$(MT_CAPS_LIBS_DIR)"
if grep -Fq '$(PROJECT_DIR)/libs' "$ENGINE_PROJECT"; then
    fail "engine project still stages into the checkout's libs directory"
fi

# uSockets is the one archive without a lib* name, so -l cannot reach it and the
# first pass over these projects missed it entirely: it stayed a PBXFileReference
# with `path = libs/uSockets.a` and the build died on "Build input file cannot be
# found". It is now merged into the static library by absolute path instead.
grep -Fq '$(MT_CAPS_LIBS_DIR)/uSockets.a' "$ENGINE_PROJECT" \
    || fail "engine project does not link uSockets.a from \$(MT_CAPS_LIBS_DIR)"
if grep -Fq 'path = libs/uSockets.a' "$ENGINE_PROJECT"; then
    fail "engine project still references uSockets.a inside the checkout"
fi

# FFmpeg is not folded into libmt_video_codecs.a (LGPL dynamic linking), so the
# five dylibs are a second half of that script's output and have to reach the
# same directory. Staging only the archive left an app linking FFmpeg with
# nothing to embed, which is how CVideoSourceFFmpeg came to fail at link with
# "_av_bsf_alloc" undefined.
VIDEO_SCRIPT="$MACOS_DIR/build-video_codecs.sh"
grep -Fq 'Staging FFmpeg dylibs to $OUT_LIB_DIR' "$VIDEO_SCRIPT" \
    || fail "build-video_codecs.sh no longer stages the FFmpeg dylibs beside the archives"
grep -Fq '$OUT_LIB_DIR/libavcodec.dylib' "$VIDEO_SCRIPT" \
    || fail "build-video_codecs.sh's freshness check ignores the staged dylibs, so a fresh key would skip staging them"

echo "OK: dependency archives resolve outside the checkout, and the host build environment is stripped first"
