#!/usr/bin/env bash
# Positive licence gate: prove a build that said it carries no FFmpeg carries none.
#
# WHY A SECOND SCANNER, when two already exist.
#
#   scan-capability-symbols.sh  checks `forbidden_when_off`, which by design runs
#                               only when the CAPABILITY is off. A store build has
#                               MT_CAP_VIDEO_PLAYBACK=1 and MT_ENABLE_FFMPEG=0, so
#                               it never fires.
#   scan-forbidden-symbols.sh   checks WHICH DECODERS are inside a libavcodec
#                               image, and exits 2 when there is no image to
#                               inspect -- "nothing was scanned, which proves
#                               nothing".
#
# Neither can express "there is no libavcodec at all, and that is the point".
# Without this, the central claim of the media-codecs programme -- that a
# distributed build ships no LGPL FFmpeg -- is enforced by nothing.
#
# TWO METHOD CHOICES THAT LOOK LIKE DETAILS AND ARE NOT:
#
# 1. `nm -gU` CANNOT SEE DYNAMICALLY LINKED FFmpeg. -U is --defined-only, and an
#    app that links FFmpeg as a dylib has those symbols UNDEFINED, not defined.
#    Measured on a real FFmpeg-on binary: `nm -gU | grep av_` returns nothing at
#    all while `otool -L` shows five libav dylibs. A gate built on -U would have
#    passed a bundle full of FFmpeg. This uses the LINK TABLE as the primary
#    signal and `nm -gu` (undefined) as the secondary.
#
# 2. THE BUNDLE IS THE SUBJECT, NOT THE EXECUTABLE. A build can stop LINKING
#    FFmpeg while an embed phase still COPIES the dylibs into
#    Contents/Frameworks -- measured 2026-09-10: 9 libav files in a bundle whose
#    binary referenced none, left by the previous build. Shipping those is
#    shipping LGPL, whether or not anything calls them.
#
# The pattern deliberately EXCLUDES vpx_ and opus_. libvpx and libopus are BSD,
# royalty-free, and SHIP in a store build carrying the WebM lane; reusing
# `forbidden_when_off` (which lists them) would fail on the code we intend to keep.
set -euo pipefail

BIN=""; BUNDLE=""; EXPECT="absent"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --binary) BIN="$2"; shift 2 ;;
        --bundle) BUNDLE="$2"; shift 2 ;;   # optional: a .app whose Frameworks dir is also scanned
        --expect) EXPECT="$2"; shift 2 ;;   # absent | present
        *) echo "scan-no-ffmpeg: unknown argument '$1'" >&2; exit 2 ;;
    esac
done

[[ -n "$BIN" && -f "$BIN" ]] || { echo "ERROR: scan-no-ffmpeg: no binary at '${BIN:-}'" >&2; exit 2; }
case "$EXPECT" in absent|present) ;; *) echo "ERROR: --expect must be absent|present" >&2; exit 2 ;; esac

# FFmpeg's own libraries. NOT vpx/opus -- see the note above.
SYM_PATTERN='av_|avcodec_|avformat_|avutil_|swscale_|swresample_'
LIB_PATTERN='libav|libsw(scale|resample)'

LINKED=""; UNDEF=""; STAGED=""
case "$(uname -s)" in
    Darwin)
        LINKED="$(otool -L "$BIN" 2>/dev/null || true)"
        UNDEF="$(nm -gu "$BIN" 2>/dev/null || true)"
        ;;
    *)
        LINKED="$(ldd "$BIN" 2>/dev/null || true)"
        UNDEF="$(nm -D --undefined-only "$BIN" 2>/dev/null || true)"
        ;;
esac
if [[ -n "$BUNDLE" && -d "$BUNDLE" ]]; then
    STAGED="$(find "$BUNDLE" -maxdepth 3 \( -name 'libav*' -o -name 'libsw*' \) 2>/dev/null || true)"
fi

# A SCAN THAT SCANNED NOTHING PROVES NOTHING, and this is the check that says so
# out loud rather than passing quietly. Both halves must have produced output:
# on macOS otool -L always prints at least the binary's own id line, so an empty
# LINKED means the tool failed, not that the binary is clean. Requiring BOTH to
# be empty (an earlier draft) made this guard dead code on macOS -- the exact
# tracker-0.2 shape it exists to prevent.
if [[ -z "$LINKED" ]]; then
    echo "ERROR: scan-no-ffmpeg read no link table from '$BIN'; this proves nothing." >&2
    exit 2
fi

HITS="$( { grep -oE "$LIB_PATTERN[a-z0-9.]*" <<< "$LINKED" || true
           grep -oE "$SYM_PATTERN" <<< "$UNDEF" || true
           printf '%s\n' "$STAGED" | grep -oE '[^/]*lib(av|sw)[a-z]*[^/]*$' || true
         } | sed '/^$/d' | sort -u )"

if [[ "$EXPECT" == "absent" ]]; then
    if [[ -n "$HITS" ]]; then
        echo "FAIL: this build declared MT_ENABLE_FFMPEG=0 but carries FFmpeg:" >&2
        sed 's/^/    /' <<< "$HITS" >&2
        [[ -n "$STAGED" ]] && { echo "  staged inside the bundle:" >&2; sed 's/^/    /' <<< "$STAGED" >&2; }
        echo "  A distributed build shipping these is shipping LGPL-2.1 code the" >&2
        echo "  store's own terms make it impossible to comply with." >&2
        exit 1
    fi
    echo "ok: no FFmpeg in $(basename "$BIN") -- link table clean, no undefined av_* imports, nothing staged"
else
    if [[ -z "$HITS" ]]; then
        echo "FAIL: expected FFmpeg to be present and found none." >&2
        echo "  Either this build lost FFmpeg unintentionally, or the scanner is" >&2
        echo "  looking at the wrong file. '$BIN'" >&2
        exit 1
    fi
    echo "ok: FFmpeg present as expected ($(wc -l <<< "$HITS" | tr -d ' ') distinct matches)"
fi
