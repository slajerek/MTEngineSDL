#!/usr/bin/env bash
# app-build-linux.sh -- the Linux APP-BUILD DRIVER (unification plan, Phase 3).
# Same seven-stage contract as app-build-macos.sh; see its header. The app
# stub owns only the engine clone / MTENGINE_REF verification and then execs
# this script.
#
# Usage: tools/appbuild/app-build-linux.sh --app-dir <dir> [--debug|--release]
#        [--clean] [--skip-deps] [--prod] [--gc [gc-args...]]
#        [--set KEY=VALUE ...]
# --prod adds the release package under platform/Linux/prod/. A DEVELOPMENT
# build -- the default -- produces no package and copies no assets: it runs,
# and is tested, from the git root. A FINAL build passes --prod and is tested
# from the package with tests/run_test.sh --package. See docs/testing.md.
# --config debug|release   optimisation (aliases: --debug, --release)
# --logs on|off            debug logging compiled in (MT_DEBUG_LOGS); on by
#                          default, OFF by default under --prod
# --symbols on|off         debug symbols in the shipped binary
#                          (MT_RELEASE_SYMBOLS); on by default, OFF under --prod
# --tier dev|commercial    licence tier (MT_COMMERCIAL_BUILD); dev by default
# An explicit switch beats what --prod implies: `--prod --logs on` is a
# diagnostic package. --symbols on with --tier commercial is refused.
# --set forwards a capability override to mtcaps (rung 1): it is how a store
# build is made -- --set MT_COMMERCIAL_BUILD=1 --set MT_PRIVATE_BUILD=0 --
# without editing the app's tracked licence manifest. The override changes the
# caps hash, so such a build gets its own out root and deps bucket.
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
MAKE_PROD=false
LOGS=""
SYMBOLS=""
TIER=""
CAPS_SETS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app-dir)   APP_DIR="${2:-}"; [[ -n "$APP_DIR" ]] || { echo "ERROR: --app-dir needs a value" >&2; exit 2; }; shift ;;
        --debug)     CONFIGURATION="Debug" ;;
        --release)   CONFIGURATION="Release" ;;
        --config)    v="$(echo "${2:-}" | tr 'A-Z' 'a-z')"; case "$v" in debug) CONFIGURATION="Debug" ;; release) CONFIGURATION="Release" ;; *) echo "ERROR: --config needs debug or release" >&2; exit 2 ;; esac; shift ;;
        --logs)      case "${2:-}" in on|off) LOGS="$2" ;; *) echo "ERROR: --logs needs on or off" >&2; exit 2 ;; esac; shift ;;
        --symbols)   case "${2:-}" in on|off) SYMBOLS="$2" ;; *) echo "ERROR: --symbols needs on or off" >&2; exit 2 ;; esac; shift ;;
        --tier)      case "${2:-}" in dev|commercial) TIER="$2" ;; *) echo "ERROR: --tier needs dev or commercial" >&2; exit 2 ;; esac; shift ;;
        --clean)     CLEAN=true; SKIP_DEPS=true ;;
        --gc)        shift; exec python3 "$ENGINE_DIR/tools/appbuild/mtengine-gc.py" "$@" ;;
        --skip-deps) SKIP_DEPS=true ;;
        --prod)      MAKE_PROD=true ;;
        --no-prod)   echo "ERROR: --no-prod is gone. A build makes no package unless you pass --prod (see docs/testing.md)." >&2; exit 2 ;;
        --set)       [[ -n "${2:-}" ]] || { echo "ERROR: --set needs KEY=VALUE" >&2; exit 2; }; CAPS_SETS+=("--set" "$2"); shift ;;
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

# ---------------------------------------------------------------------------
# THE FOUR SWITCHES, the same on every platform (maintainer, 2026-09-05):
#   --config debug|release   optimisation            default release
#   --logs on|off            MT_DEBUG_LOGS           default on;  --prod: off
#   --symbols on|off         MT_RELEASE_SYMBOLS      default on;  --prod: off
#   --tier dev|commercial    MT_COMMERCIAL_BUILD     default dev
# An explicit switch always beats what --prod implies. A diagnostic package
# for an issue that appears only when deployed is `--prod --logs on`.
# ---------------------------------------------------------------------------
if [[ -z "$LOGS" ]];    then LOGS=$([[ "$MAKE_PROD" == "true" ]] && echo off || echo on); fi
if [[ -z "$SYMBOLS" ]]; then SYMBOLS=$([[ "$MAKE_PROD" == "true" ]] && echo off || echo on); SYMBOLS_EXPLICIT=false; else SYMBOLS_EXPLICIT=true; fi
[[ -n "$TIER" ]] || TIER=dev
# A store build is stripped, full stop. resolve's commercial forcing yields to
# an explicit --set, so the refusal has to happen here.
if [[ "$TIER" == "commercial" && "$SYMBOLS_EXPLICIT" == "true" && "$SYMBOLS" == "on" ]]; then
    echo "ERROR: --symbols on cannot be combined with --tier commercial: a store build never ships symbols." >&2; exit 2
fi
CAPS_SETS+=("--set" "MT_DEBUG_LOGS=$([[ "$LOGS" == "on" ]] && echo 1 || echo 0)")
if [[ "$SYMBOLS" == "off" ]]; then CAPS_SETS+=("--set" "MT_RELEASE_SYMBOLS=0"); fi
if [[ "$TIER" == "commercial" ]]; then CAPS_SETS+=("--set" "MT_COMMERCIAL_BUILD=1" "--set" "MT_PRIVATE_BUILD=0"); fi
echo "Build: config=$(echo "$CONFIGURATION" | tr 'A-Z' 'a-z') logs=$LOGS symbols=$SYMBOLS tier=$TIER prod=$([[ "$MAKE_PROD" == "true" ]] && echo yes || echo no)"

PYTHON3="$(command -v python3 || true)"
[[ -n "$PYTHON3" ]] || { echo "ERROR: python3 not found (tools/mtcaps needs it)." >&2; exit 2; }
case "$(uname -m)" in
    aarch64|arm64) MT_ENGINE_OPTION="MT_GGML_NATIVE=OFF" ;;
    *)             MT_ENGINE_OPTION="MT_GGML_NATIVE=ON" ;;
esac
MTCAPS_OUTPUT="$("$PYTHON3" -B "$ENGINE_DIR/tools/mtcaps/mtcaps.py" resolve \
    --manifest "$MANIFEST" --app "$MT_APP_NAME" \
    --platform linux --arch "$(uname -m)" --config "$CONFIGURATION" \
    --engine-dir "$ENGINE_DIR" --engine-option "$MT_ENGINE_OPTION" \
    ${CAPS_SETS[@]+"${CAPS_SETS[@]}"})" || {
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
MT_DEBUG_LOGS="$(sed -n 's/^set(MT_DEBUG_LOGS \([01]\))$/\1/p' "$MT_OUT/MTEngineCapabilities.cmake")"

echo "Capabilities: $MT_APP_NAME"
echo "  out     : $MT_OUT"
echo "  deps    : $MT_CAPS_LIBS_DIR"
# tier and ffmpeg mode are different axes; see app-build-macos.sh.
echo "  tier    : $([ "$COMMERCIAL" = 1 ] && echo commercial || echo non-commercial), ffmpeg=$MT_FFMPEG_BUILD_MODE, symbols=${MT_RELEASE_SYMBOLS:-1}, logs=${MT_DEBUG_LOGS:-1}"

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
# --config finally reaches CMake (it was parsed and ignored until 2026-09-05).
# release -> RelWithDebInfo: debug info is ALWAYS generated and the symbols
# step below decides what the shipped binary carries.
MT_BUILD_TYPE=$([[ "$CONFIGURATION" == "Debug" ]] && echo Debug || echo RelWithDebInfo)
export MT_BUILD_TYPE
cmake ../ -DCMAKE_BUILD_TYPE="$MT_BUILD_TYPE" "${MT_CAPS_ARGS[@]}" ${CMAKE_EXTRA_ARGS:-}
# PARALLELISM: every core unless MT_BUILD_JOBS says otherwise. The default is
# unchanged -- nproc is what this has always used. The knob exists because -j
# scales with core count and not with available RAM: on a box with little
# memory per core the OOM killer takes the compiler, and make reports that as a
# plain build failure with no hint that memory was the cause. Same variable,
# same meaning, as the Windows driver.
MT_JOBS="${MT_BUILD_JOBS:-$(nproc)}"
case "$MT_JOBS" in
    ""|*[!0-9]*|0) echo "ERROR: MT_BUILD_JOBS must be a positive integer, got '${MT_BUILD_JOBS}'" >&2; exit 1 ;;
esac
if [[ -n "${MT_BUILD_JOBS:-}" ]]; then
    echo "Build parallelism: -j$MT_JOBS (MT_BUILD_JOBS)"
fi
export CMAKE_BUILD_PARALLEL_LEVEL="$MT_JOBS"
make -j"$MT_JOBS" "$MT_CMAKE_TARGET"
APP_BINARY="$APP_DIR/build/$MT_CMAKE_TARGET"
[[ -f "$APP_BINARY" ]] || { echo "ERROR: build reported success but no binary at $APP_BINARY" >&2; exit 1; }

# ---------------------------------------------------------------------------
# licence gate
# ---------------------------------------------------------------------------
# Keyed on the FLAG, not the capability -- see the note in app-build-macos.sh.
# MT_ENABLE_FFMPEG is already in scope here: this driver has always read the
# emitted fragment through mt_caps_read_flags.
if [[ "${MT_ENABLE_FFMPEG:-1}" == "1" ]]; then
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
fi

# The POSITIVE half -- see app-build-macos.sh for why it exists and why it runs
# in both directions on every build. UNVERIFIED ON LINUX: written from the macOS
# version, where both directions were proven. The scanner uses ldd and
# `nm -D --undefined-only` here rather than otool/nm -gu.
if [[ "${MT_ENABLE_FFMPEG:-1}" == "1" ]]; then
    bash "$SCRIPT_DIR/scan-no-ffmpeg.sh" --binary "$APP_BINARY" --expect present
else
    bash "$SCRIPT_DIR/scan-no-ffmpeg.sh" --binary "$APP_BINARY" --expect absent
fi


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

# ---------------------------------------------------------------------------
# Release package (Stage 7). Same shape as the Windows and macOS drivers: a
# directory cleared on every run, holding the artifact, the assets and the
# licence document mtcaps writes into $MT_OUT on every resolve. Until now
# only Windows implemented this, so a contract reading "LICENSES.txt ships
# ALWAYS" held on one platform in three.
# ---------------------------------------------------------------------------
if [[ "$MAKE_PROD" == "true" ]]; then
    PROD_DIR="$APP_DIR/platform/Linux/prod/$(uname -m)"
    # The tier in the artifact name, as on Windows and macOS.
    PROD_NAME="$MT_CMAKE_TARGET$([[ "${COMMERCIAL:-0}" == "1" ]] || echo -n "-nc")"

    echo ""
    echo "=== Deploying release package ($(uname -m) $CONFIGURATION) ==="
    rm -rf "$PROD_DIR"
    mkdir -p "$PROD_DIR"
    cp "$APP_BINARY" "$PROD_DIR/$PROD_NAME"

    LICENCES_SRC="$MT_OUT/LICENSES.txt"
    [[ -f "$LICENCES_SRC" ]] || { echo "ERROR: mtcaps wrote no LICENSES.txt at $LICENCES_SRC" >&2; exit 1; }
    cp "$LICENCES_SRC" "$PROD_DIR/LICENSES.txt"

    ASSETS_NAME="${MT_LINUX_ASSETS:-assets}"
    [[ -d "$APP_DIR/$ASSETS_NAME" ]] && cp -R "$APP_DIR/$ASSETS_NAME" "$PROD_DIR/assets"

    # MT_APP_PAYLOAD -- further directories the app needs AT RUNTIME, copied in
    # under their own names. One `assets` directory was not enough: the package
    # is the working directory a test or a user runs from, so anything the app
    # opens by a relative path has to be in it. One host application reads a
    # config file under data/ during init and SYS_FatalExit's without it, which
    # is exactly what the first run of its package did (2026-09-02).
    # Space separated, repo-relative, same key on all three platforms.
    for _mt_payload in ${MT_APP_PAYLOAD:-}; do
        if [[ -d "$APP_DIR/$_mt_payload" ]]; then
            cp -R "$APP_DIR/$_mt_payload" "$PROD_DIR/$(basename "$_mt_payload")"
        else
            echo "WARNING: MT_APP_PAYLOAD names '$_mt_payload', which does not exist in $APP_DIR" >&2
        fi
    done

    # FFMPEG IS SHIPPED, NOT BORROWED.
    #
    # This package staged no shared library at all, while macOS copies
    # Contents/Frameworks and Windows copies the DLLs. The binary carries
    # DT_NEEDED libavcodec.so.61, so without these files it resolves against
    # whatever libavcodec the distribution installed -- and a distro build has
    # aac, hevc, eac3 and the whole WMA family compiled in: precisely the set
    # decoders_withheld exists to keep out of a distributed build. Every gate
    # in this tree (the nm scan, scan-forbidden-symbols) inspects the libraries
    # we STAGE; not one of them inspects the library actually loaded.
    #
    # MT_CAPS_LIBS_DIR is exported above and already names this tree for the
    # link line further up. The lib*.so.* glob shape is the one the app's own
    # CMakeLists already uses to put these beside the DEV binary, which is why
    # the defect was package-only and invisible in development.
    if [[ "${MT_ENABLE_FFMPEG:-1}" != "0" ]]; then
        _ff_lib_dir="$MT_CAPS_LIBS_DIR/ffmpeg/lib"
        shopt -s nullglob
        _ff_libs=("$_ff_lib_dir"/lib{avcodec,avformat,avutil,swscale,swresample}.so.*)
        shopt -u nullglob
        if [[ ${#_ff_libs[@]} -eq 0 ]]; then
            echo "ERROR: FFmpeg is enabled but $_ff_lib_dir holds no libav*.so.* to stage." >&2
            echo "       Refusing to package a binary that would load the distribution's" >&2
            echo "       libavcodec, which carries every decoder this build withholds." >&2
            exit 1
        fi
        cp -a "${_ff_libs[@]}" "$PROD_DIR/"
    fi

    # Belt for the symbols contract: the separated debug info lives in
    # $MT_OUT/symbols and never rides the package.
    find "$PROD_DIR" -name '*.debug' -delete 2>/dev/null || true

    # THE GATE, not the rpath, is what proves the above. This driver serves all
    # five apps and must not assume every one of them sets $ORIGIN; if a libav*
    # still resolves outside the package, the staging did not take and the
    # package must not ship.
    if [[ "${MT_ENABLE_FFMPEG:-1}" != "0" ]] && command -v ldd >/dev/null 2>&1; then
        _ff_stray="$(ldd "$PROD_DIR/$PROD_NAME" 2>/dev/null \
                     | grep -E 'libav(codec|format|util)|libsw(scale|resample)' \
                     | grep -v "$PROD_DIR" || true)"
        if [[ -n "$_ff_stray" ]]; then
            echo "ERROR: packaged binary resolves FFmpeg outside the package:" >&2
            echo "$_ff_stray" >&2
            exit 1
        fi
    fi

    [[ -f "$PROD_DIR/LICENSES.txt" ]] || { echo "ERROR: release package has no LICENSES.txt: $PROD_DIR" >&2; exit 1; }
    echo "Deployed: $PROD_DIR/$PROD_NAME"
fi

echo ""
echo "$MT_APP_NAME built successfully."
echo "$APP_BINARY"
