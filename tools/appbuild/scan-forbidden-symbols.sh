#!/usr/bin/env bash
# Scan built FFmpeg codec libraries for the patent-encumbered decoders a
# restricted build must not carry (unification plan Phase 2, 2026-08-31).
#
# GENERIC AND ENGINE-OWNED: the decoder list comes from the vocabulary
# (forbidden_decoders_commercial on MT_CAP_VIDEO_PLAYBACK / _PHOTO_CODECS),
# never from a list typed in an app repo -- the photo app's build-macos.sh scan
# was the model, generalized. No app names appear here.
#
# Usage:
#   scan-forbidden-symbols.sh --mode <full|commercial> --libs <dir>
#
#   --mode  the RESOLVED FFmpeg build mode (MT_FFMPEG_BUILD_MODE). `full` is
#           the private, never-distributed tier: the scan SKIPS, the decoders
#           are allowed there. Anything else must scan clean.
#   --libs  the keyed deps dir holding the staged libav* dylibs / .so files.
#
# Exit 0 = clean (or full mode); exit 1 = a forbidden decoder is present;
# exit 2 = usage / cannot scan.
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VOCAB="$SCRIPT_DIR/../mtcaps/vocabulary.json"

MODE=""
LIBS=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --mode) MODE="$2"; shift ;;
        --libs) LIBS="$2"; shift ;;
        *) echo "ERROR: unknown argument '$1'" >&2; exit 2 ;;
    esac
    shift
done

[[ -n "$MODE" && -n "$LIBS" ]] || { echo "ERROR: --mode and --libs are required" >&2; exit 2; }

if [[ "$MODE" == "full" ]]; then
    echo "scan-forbidden-symbols: mode=full (private build) -- scan not applicable"
    exit 0
fi

if [[ ! -d "$LIBS" ]]; then
    echo "ERROR: no libs dir at $LIBS" >&2
    exit 2
fi

# The deny-list, from the single source of truth. python3 is already a hard
# requirement of the capability system (mtcaps), so this adds no dependency.
DECODERS="$(python3 -B -c "
import json, sys
with open('$VOCAB', encoding='utf-8') as f:
    data = json.load(f)
names = []
for cap in data['capabilities'].values():
    names += cap.get('commercial', {}).get('forbidden_decoders_commercial', [])
print(' '.join(sorted(set(names))))
")" || { echo 'ERROR: could not read the vocabulary' >&2; exit 2; }

# THE PROBE (respecified 2026-08-31 after review): nm is blind here -- the
# staged dylibs/DLLs export ~600 symbols and ff_*_decoder entries are
# internal on every platform; and bare codec-NAME strings false-positive on
# parsers (a commercial build legitimately keeps the hevc/aac PARSERS).
# What is precise on all three platforms is the EMBEDDED CONFIGURE STRING:
# the engine's codec builds always pass explicit --enable-decoder= lists,
# and avcodec_configuration() embeds the full configure line in the image.
# A forbidden decoder inside any --enable-decoder= clause is definitive.
FOUND=0
SCANNED=0
shopt -s nullglob
for lib in "$LIBS"/libavcodec*.dylib "$LIBS"/libavcodec*.so* "$LIBS"/ffmpeg/lib/libavcodec* "$LIBS"/avcodec*.dll "$LIBS"/ffmpeg/bin/avcodec*.dll; do
    [[ -L "$lib" ]] && continue
    [[ -f "$lib" ]] || continue
    SCANNED=$((SCANNED + 1))
    HITS="$(python3 -B - "$lib" $DECODERS << 'PYSCAN'
import re, sys
lib = sys.argv[1]
forbidden = set(sys.argv[2:])
data = open(lib, "rb").read()
enabled = set()
for m in re.finditer(rb"--enable-decoder=['\"]?([a-z0-9_,]+)", data):
    enabled |= set(m.group(1).decode().split(","))
if not enabled:
    print("NO_CONFIG")
    sys.exit(0)
bad = sorted(enabled & forbidden)
if bad:
    print(" ".join(bad))
PYSCAN
)" || { echo "ERROR: scan failed on $lib" >&2; exit 2; }
    if [[ "$HITS" == "NO_CONFIG" ]]; then
        echo "ERROR: no --enable-decoder clause found in $lib -- not one of the" >&2
        echo "       engine's own codec builds (they always pass explicit lists)," >&2
        echo "       so this probe cannot prove anything about it." >&2
        exit 2
    elif [[ -n "$HITS" ]]; then
        echo "FORBIDDEN: decoder(s) [$HITS] enabled in $lib" >&2
        FOUND=1
    fi
done

if [[ "$SCANNED" -eq 0 ]]; then
    echo "ERROR: no libavcodec image found under $LIBS -- nothing was scanned, which proves nothing" >&2
    exit 2
fi

if [[ "$FOUND" -ne 0 ]]; then
    echo "FAIL: a '$MODE' build carries patent-encumbered decoders. The FFmpeg" >&2
    echo "      prefix is stale for this mode; rebuild the codecs (the stamped" >&2
    echo "      rebuild corrects it) and re-run." >&2
    exit 1
fi

echo "scan-forbidden-symbols: $SCANNED libavcodec image(s) clean for mode=$MODE ($(echo $DECODERS | wc -w | tr -d ' ') decoders checked)"
exit 0
