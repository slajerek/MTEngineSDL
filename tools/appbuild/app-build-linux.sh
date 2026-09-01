#!/usr/bin/env bash
# app-build-linux.sh -- the Linux APP-BUILD DRIVER (unification plan, Phase 3).
# Same seven-stage contract as app-build-macos.sh; see its header. The app
# stub owns only the engine clone / MTENGINE_REF verification and then execs
# this script.
#
# Usage: tools/appbuild/app-build-linux.sh --app-dir <dir> [--debug|--release]
#        [--clean] [--skip-deps] [--gc [gc-args...]]
# --clean cleans and STOPS (no build; same contract on all three platforms).
# --gc is a shortcut to tools/appbuild/mtengine-gc.py; everything after it
# goes to the GC verbatim (--gc = report, --gc --prune, ...).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

APP_DIR=""
CONFIGURATION="Release"
CLEAN=false
SKIP_DEPS=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app-dir)   APP_DIR="${2:-}"; [[ -n "$APP_DIR" ]] || { echo "ERROR: --app-dir needs a value" >&2; exit 2; }; shift ;;
        --debug)     CONFIGURATION="Debug" ;;
        --release)   CONFIGURATION="Release" ;;
        --clean)     CLEAN=true; SKIP_DEPS=true ;;
        --gc)        shift; exec python3 "$ENGINE_DIR/tools/appbuild/mtengine-gc.py" "$@" ;;
        --skip-deps) SKIP_DEPS=true ;;
        --incremental) : ;;  # compatibility no-op: incremental IS the default
        -h|--help)   sed -n '2,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "ERROR: unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

[[ -n "$APP_DIR" ]] || { echo "ERROR: --app-dir is required (the stub passes it)." >&2; exit 2; }
APP_DIR="$(cd "$APP_DIR" && pwd)"

# ---------------------------------------------------------------------------
# 0. engine-ref verification + stub drift check (stub-shrink, 2026-08-31):
#    both moved here FROM the stubs -- verification is read-only, so the
#    driver can own it, and the stub keeps only clone-when-absent.
# ---------------------------------------------------------------------------
# shellcheck source=appbuild-lib.sh
source "$SCRIPT_DIR/appbuild-lib.sh"
mt_appbuild_verify_ref "$APP_DIR" "$ENGINE_DIR"
mt_appbuild_check_stub "$APP_DIR" "$ENGINE_DIR" "build-linux.sh"

CONF="$APP_DIR/mtengine-app.conf"
[[ -f "$CONF" ]] || { echo "ERROR: no $CONF" >&2; exit 2; }
# shellcheck source=/dev/null
source "$CONF"
for var in MT_APP_NAME MT_CMAKE_TARGET; do
    [[ -n "${!var:-}" ]] || { echo "ERROR: $CONF does not set $var" >&2; exit 2; }
done
MANIFEST="$APP_DIR/mtengine.caps"
[[ -f "$MANIFEST" ]] || { echo "ERROR: no manifest at $MANIFEST" >&2; exit 2; }

export MT_BUILD_LOCK_PID=$$
export MT_BUILD_LOCK_KIND=cli
"$ENGINE_DIR/tools/mtcaps/build-lock.sh" acquire "$MT_APP_NAME"
trap '"$ENGINE_DIR/tools/mtcaps/build-lock.sh" release "$MT_APP_NAME" || true' EXIT

# ---------------------------------------------------------------------------
# resolve ONCE, directly. <backend> is the driver's to resolve here for the
# same reason it was the wrapper's: MT_GGML_NATIVE follows `uname -m`.
# ---------------------------------------------------------------------------
PYTHON3="$(command -v python3 || true)"
[[ -n "$PYTHON3" ]] || { echo "ERROR: python3 not found (tools/mtcaps needs it)." >&2; exit 2; }
case "$(uname -m)" in
    aarch64|arm64) MT_ENGINE_OPTION="MT_GGML_NATIVE=OFF" ;;
    *)             MT_ENGINE_OPTION="MT_GGML_NATIVE=ON" ;;
esac
MTCAPS_OUTPUT="$("$PYTHON3" -B "$ENGINE_DIR/tools/mtcaps/mtcaps.py" resolve \
    --manifest "$MANIFEST" --app "$MT_APP_NAME" \
    --platform linux --arch "$(uname -m)" --config Release \
    --engine-dir "$ENGINE_DIR" --engine-option "$MT_ENGINE_OPTION")" || {
    echo "ERROR: mtcaps resolve failed for $MANIFEST" >&2; exit 2; }

MT_OUT="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^out_dir=//p')"
MT_CAPS_LIBS_DIR="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^deps_dir=//p')"
MT_CAPS_RESOLVED="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^resolved=//p')"
MT_FFMPEG_BUILD_MODE="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^ffmpeg_mode=//p')"
MT_BUILD_DIR="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^build_dir=//p')"
[[ -n "$MT_BUILD_DIR" ]] || MT_BUILD_DIR="$MT_OUT"
for v in MT_OUT MT_CAPS_LIBS_DIR MT_CAPS_RESOLVED; do
    [[ -n "${!v}" ]] || { echo "ERROR: resolve carried no $v" >&2; exit 2; }
done
export MT_CAPS_LIBS_DIR MT_FFMPEG_BUILD_MODE
case "$MT_CAPS_RESOLVED" in
    *MT_COMMERCIAL_BUILD=1*) export COMMERCIAL=1 ;;
    *)                       export COMMERCIAL=0 ;;
esac
# Flags + the symbols value, from the fragment of the SAME resolve.
# shellcheck source=/dev/null
source "$ENGINE_DIR/platform/caps-lib.sh"
mt_caps_read_flags "$MT_OUT/MTEngineCapabilities.cmake"
MT_RELEASE_SYMBOLS="$(sed -n 's/^set(MT_RELEASE_SYMBOLS \([01]\))$/\1/p' "$MT_OUT/MTEngineCapabilities.cmake")"

echo "Capabilities: $MT_APP_NAME"
echo "  out     : $MT_OUT"
echo "  deps    : $MT_CAPS_LIBS_DIR"
echo "  mode    : $([ "$COMMERCIAL" = 1 ] && echo commercial || echo full), ffmpeg=$MT_FFMPEG_BUILD_MODE, symbols=${MT_RELEASE_SYMBOLS:-1}"

# ---------------------------------------------------------------------------
# deps + engine, PRE-RESOLVED (submodule init inside -- the only init path).
# ---------------------------------------------------------------------------
if [[ "$SKIP_DEPS" != "true" ]]; then
    MTENGINE_PRERESOLVED_OUT="$MT_OUT" bash "$ENGINE_DIR/build-linux.sh" \
        --manifest "$MANIFEST" --app "$MT_APP_NAME"
fi

# ---------------------------------------------------------------------------
# app build
# ---------------------------------------------------------------------------
MT_CAPS_ARGS=(
    -DMT_CAPS_FRAGMENT="$MT_OUT/MTEngineCapabilities.cmake"
    -DMT_CAPS_LIBS_DIR="$MT_CAPS_LIBS_DIR"
    -DMT_ENGINE_BUILD_DIR="$MT_BUILD_DIR/engine-build"
)
if [[ "$CLEAN" == "true" ]]; then
    rm -rf "$APP_DIR/build" "$MT_BUILD_DIR/engine-build"
    echo "Clean complete."
    exit 0
fi
mkdir -p "$APP_DIR/build"
cd "$APP_DIR/build"
# shellcheck disable=SC2086
cmake ../ "${MT_CAPS_ARGS[@]}" ${CMAKE_EXTRA_ARGS:-}
make -j"$(nproc)" "$MT_CMAKE_TARGET"
APP_BINARY="$APP_DIR/build/$MT_CMAKE_TARGET"
[[ -f "$APP_BINARY" ]] || { echo "ERROR: build reported success but no binary at $APP_BINARY" >&2; exit 1; }

# ---------------------------------------------------------------------------
# licence gate
# ---------------------------------------------------------------------------
case "$MT_CAPS_RESOLVED" in *MT_CAP_VIDEO_PLAYBACK=1*)
    MARKER="$MT_CAPS_LIBS_DIR/ffmpeg/.ffmpeg-build-mode"
    MARKER_MODE="$(cat "$MARKER" 2>/dev/null || echo missing)"
    if [[ "$COMMERCIAL" == "1" && "$MARKER_MODE" != "commercial" ]]; then
        echo "ERROR: commercial app build against a '$MARKER_MODE' FFmpeg install ($MARKER)." >&2
        exit 1
    elif [[ -n "$MT_FFMPEG_BUILD_MODE" && "$MARKER_MODE" != "$MT_FFMPEG_BUILD_MODE" ]]; then
        echo "WARNING: FFmpeg install is '$MARKER_MODE' but this resolve expects '$MT_FFMPEG_BUILD_MODE'." >&2
    fi
    if [[ "${MT_FFMPEG_BUILD_MODE:-full}" != "full" ]]; then
        bash "$SCRIPT_DIR/scan-forbidden-symbols.sh" --mode "$MT_FFMPEG_BUILD_MODE" \
            --libs "$MT_CAPS_LIBS_DIR/ffmpeg/lib"
    fi
;; esac


# ---------------------------------------------------------------------------
# app post-build hook (optional): the ONE place app-specific verification
# lives -- the photo app's sentinel/Homebrew scans are the model. The hook runs
# with the resolved environment exported; a nonzero exit fails the build.
# ---------------------------------------------------------------------------
if [[ -n "${MT_HOOK_POST_BUILD:-}" ]]; then
    HOOK="$APP_DIR/$MT_HOOK_POST_BUILD"
    [[ -f "$HOOK" ]] || { echo "ERROR: MT_HOOK_POST_BUILD names a missing file: $HOOK" >&2; exit 1; }
    echo "Running post-build hook: $MT_HOOK_POST_BUILD"
    APP_DIR="$APP_DIR" APP_BINARY="$APP_BINARY" APP_BUNDLE="${APP_BUNDLE:-}" \
    CONFIGURATION="$CONFIGURATION" MT_OUT="$MT_OUT" \
        bash "$HOOK"
fi

# ---------------------------------------------------------------------------
# symbols contract: split debug info always kept, binary stripped at 0.
# ---------------------------------------------------------------------------
SYMBOLS_DIR="$MT_OUT/symbols"
mkdir -p "$SYMBOLS_DIR"
if command -v objcopy >/dev/null 2>&1; then
    objcopy --only-keep-debug "$APP_BINARY" "$SYMBOLS_DIR/$MT_CMAKE_TARGET.debug" \
        || echo "WARNING: objcopy produced no debug file" >&2
    if [[ "${MT_RELEASE_SYMBOLS:-1}" == "0" ]]; then
        strip --strip-debug "$APP_BINARY"
        objcopy --add-gnu-debuglink="$SYMBOLS_DIR/$MT_CMAKE_TARGET.debug" "$APP_BINARY" || true
        echo "Symbols: STRIPPED for distribution; .debug retained at $SYMBOLS_DIR"
    else
        echo "Symbols: kept in the binary; .debug at $SYMBOLS_DIR"
    fi
fi


# Cache-growth hint (cheap: a dir count, never a du): rev-keyed out dirs
# accumulate per engine commit and per dirty edit; the GC owns cleanup.
MT_ROOT="${MTENGINE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/mtengine}"
if [[ -d "$MT_ROOT/$MT_APP_NAME" ]]; then
    REV_COUNT=$(find "$MT_ROOT/$MT_APP_NAME" -mindepth 1 -maxdepth 1 -type d ! -name '_build' | wc -l | tr -d ' ')
    if [[ "$REV_COUNT" -gt 12 ]]; then
        echo "NOTE: $REV_COUNT rev-keyed build dirs under $MT_ROOT/$MT_APP_NAME -- consider: python3 \"$ENGINE_DIR/tools/appbuild/mtengine-gc.py\" --prune"
    fi
fi

echo ""
echo "$MT_APP_NAME built successfully."
echo "$APP_BINARY"
