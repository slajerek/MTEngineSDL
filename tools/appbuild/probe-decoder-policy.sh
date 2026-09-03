#!/usr/bin/env bash
# Does the built FFmpeg match the decoder policy the resolve asked for?
#
# THE GATE FOR THE TIER-VS-MODE DEFECT (2026-09-02). Nine branches in the three
# codec scripts chose their decoder set from the licence TIER ($COMMERCIAL)
# while the rest of the system had already resolved a MODE
# (MT_FFMPEG_BUILD_MODE, `full` only for MT_PRIVATE_BUILD=1). The public/free
# tier is COMMERCIAL=0 with mode `commercial`, so it took the full branch.
#
# WHY THIS IS A BUILD PROBE AND NOT A UNIT TEST. mtcaps already asserts the
# resolve at tools/mtcaps/tests/test_mtcaps.py
# (test_ffmpeg_mode_line_derives_from_private_not_commercial) and that test
# passed throughout: the resolve was never wrong. The defect was scripts
# ignoring a correct answer, and only a build shows that.
#
# THE THIRD CASE IS THE ONE NOBODY WOULD THINK TO WRITE. On the macOS IDE path
# NEITHER variable is set -- xcode-preaction.sh publishes only `= 0|1`
# settings, so the string-valued mode never reached it and the script fell
# back to `full`. A probe that sets the mode cannot see that; case 3 unsets
# both and asserts the fallback is no longer how the mode is decided.
#
# Usage: probe-decoder-policy.sh [--app-dir DIR]
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
APP_DIR="${1:-}"
[[ "$APP_DIR" == "--app-dir" ]] && APP_DIR="${2:-}"
APP_DIR="${APP_DIR:-$(cd "$ENGINE_DIR/../MTEngineSDLDummyApp" && pwd)}"
[[ -f "$APP_DIR/mtengine.caps" ]] || { echo "ERROR: no manifest at $APP_DIR/mtengine.caps" >&2; exit 2; }

PYTHON3="$(command -v python3)"
MTCAPS="$ENGINE_DIR/tools/mtcaps/mtcaps.py"
PLATFORM="$([[ "$(uname -s)" == "Darwin" ]] && echo macos || echo linux)"
ARCH="$(uname -m)"; [[ "$ARCH" == "arm64" && "$PLATFORM" == "linux" ]] && ARCH="aarch64"

# THE SAME ENGINE OPTION THE WRAPPER PASSES. The backend is part of the deps
# key, so a resolve without it names a bucket no build ever fills -- the first
# run of this probe reported "no FFmpeg built" for two cases that were in fact
# built, under a different backend segment.
case "$ARCH" in
    arm64|aarch64) ENGINE_OPTION=(--engine-option MT_GGML_NATIVE=OFF) ;;
    *)             ENGINE_OPTION=(--engine-option MT_GGML_NATIVE=ON) ;;
esac

fail=0
note() { printf '  %s\n' "$*"; }

# The withheld set, from the vocabulary rather than a copy of it.
WITHHELD="$("$PYTHON3" - "$ENGINE_DIR" <<'PY'
import json, os, sys
v = json.load(open(os.path.join(sys.argv[1], "tools/mtcaps/vocabulary.json")))
# The withheld list lives in the ffmpeg policy block since L4 (2026-09-02);
# it was commercial.forbidden_decoders_commercial before that.
print(" ".join(v["capabilities"]["MT_CAP_VIDEO_PLAYBACK"]["ffmpeg"]["decoders_withheld"]))
PY
)"

# Which decoders a built libavcodec actually carries, read the way the licence
# scanner reads it: the configure string FFmpeg bakes into the library. nm is
# blind here -- a stripped dylib reports nothing and the scan would pass.
enabled_decoders() {
    local libdir="$1" lib
    lib="$(ls "$libdir"/libavcodec.*.dylib "$libdir"/libavcodec.so.* 2>/dev/null | head -1)"
    [[ -n "$lib" ]] || return 1
    strings -a "$lib" 2>/dev/null | tr ' ' '\n' | sed -n 's/^--enable-decoder=//p' | tr ',' '\n' | sort -u
}

probe() {
    local label="$1" want_mode="$2"; shift 2
    echo "== $label"
    local deps
    deps="$("$PYTHON3" -B "$MTCAPS" resolve --manifest "$APP_DIR/mtengine.caps" \
                --app "$(basename "$APP_DIR")" --platform "$PLATFORM" --arch "$ARCH" \
                --config Release --engine-dir "$ENGINE_DIR" "${ENGINE_OPTION[@]}" "$@" --print deps-dir)"
    # The mode IS the resolve's answer: `full` exactly when MT_PRIVATE_BUILD=1.
    local resolved mode
    resolved="$("$PYTHON3" -B "$MTCAPS" resolve --manifest "$APP_DIR/mtengine.caps" \
                    --app "$(basename "$APP_DIR")" --platform "$PLATFORM" --arch "$ARCH" \
                    --config Release --engine-dir "$ENGINE_DIR" "${ENGINE_OPTION[@]}" "$@" | sed -n 's/^resolved=//p')"
    if [[ "$resolved" == *MT_PRIVATE_BUILD=1* ]]; then mode="full"; else mode="commercial"; fi
    note "resolved mode: $mode (expected $want_mode)"
    [[ "$mode" == "$want_mode" ]] || { note "FAIL: resolve disagrees"; fail=1; return; }

    local marker="$deps/.ffmpeg-build-mode"
    [[ -f "$marker" ]] || marker="$deps/ffmpeg/.ffmpeg-build-mode"
    if [[ ! -f "$marker" ]]; then
        note "SKIP: no FFmpeg built in $deps yet -- run the driver for this case first"
        return
    fi
    local marked; marked="$(cat "$marker")"
    note "marker: $marked"
    [[ "$marked" == "$mode" ]] || { note "FAIL: marker says '$marked', resolve says '$mode'"; fail=1; }

    local libdir="$deps/ffmpeg/lib"; [[ -d "$libdir" ]] || libdir="$deps"
    local have; have="$(enabled_decoders "$libdir" || true)"
    [[ -n "$have" ]] || { note "SKIP: no libavcodec to read in $libdir"; return; }
    local found=""
    for d in $WITHHELD; do
        grep -qx "$d" <<<"$have" && found="$found $d"
    done
    if [[ "$mode" == "commercial" && -n "$found" ]]; then
        note "FAIL: commercial mode carries withheld decoders:$found"; fail=1
    elif [[ "$mode" == "full" && -z "$found" ]]; then
        note "FAIL: full mode carries none of the withheld decoders -- the mode did not reach configure"; fail=1
    else
        note "decoders agree with the mode"
    fi
}

echo "Decoder-policy probe -- $APP_DIR on $PLATFORM/$ARCH"
echo

# 1. public/free: tier non-commercial, mode commercial. The combination the
#    nine branches got wrong.
probe "public/free tier (--set MT_PRIVATE_BUILD=0)" commercial --set MT_PRIVATE_BUILD=0

# 2. the app's own manifest, whatever it says.
probe "manifest as written" \
      "$(grep -qE '^MT_PRIVATE_BUILD=1' "$APP_DIR/mtengine.caps" && echo full || echo commercial)"

# 3. the IDE path's real state: neither COMMERCIAL nor MT_FFMPEG_BUILD_MODE in
#    the environment. Everything the scripts decide must come from the
#    published settings, not from a fallback.
echo "== IDE-path environment (both variables unset)"
if env -u COMMERCIAL -u MT_FFMPEG_BUILD_MODE bash -c \
     'grep -q "MT_FFMPEG_BUILD_MODE" "$1"' _ "$APP_DIR/../.mtengine-ide/$(basename "$APP_DIR")/caps.xcconfig" 2>/dev/null; then
    note "the pre-action publishes MT_FFMPEG_BUILD_MODE -- the IDE path can see the mode"
else
    note "FAIL: no MT_FFMPEG_BUILD_MODE in the IDE xcconfig; an IDE build would fall back to 'full'"
    note "      (run an Xcode build once so the pre-action writes it, then re-run this probe)"
    fail=1
fi

echo
if [[ "$fail" == 0 ]]; then echo "decoder policy: OK"; else echo "decoder policy: FAILURES ABOVE" >&2; fi
exit "$fail"
