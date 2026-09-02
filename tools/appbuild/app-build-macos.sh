#!/usr/bin/env bash
# app-build-macos.sh -- the macOS APP-BUILD DRIVER (unification plan, Phase 3).
#
# The engine owns the build flow; an app owns parameters. An app repo keeps a
# thin stub (its build-macos.sh) whose only jobs are the chicken-and-egg ones:
# clone the engine when absent, verify MTENGINE_REF, and exec THIS script from
# the verified checkout. Everything else happens here, identically for every
# app:
#
#   1. read <app-dir>/mtengine-app.conf (the app's parameters)
#   2. take the build lock (tools/mtcaps/build-lock.sh)
#   3. resolve the capability set ONCE (via the engine wrapper's
#      --print-settings; every later engine call runs pre-resolved)
#   4. acquire dependencies (engine wrapper --deps-only, pre-resolved:
#      caps-gated submodule init -- the ONLY init path, decision 0.6a --
#      and the vendored uSockets build)
#   5. build the app scheme (which builds the engine as a dependency; the
#      caps settings ride the whole invocation, MT_CAPS_WRAPPER=1 always)
#   6. licence gate: keyed .ffmpeg-build-mode marker guard + the
#      vocabulary-driven forbidden-decoder scan for restricted modes
#   7. symbols contract (decision 0.5): dSYM always extracted to
#      $MT_OUT/symbols/; the binary stripped when MT_RELEASE_SYMBOLS=0
#
# Usage (from the app stub):
#   tools/appbuild/app-build-macos.sh --app-dir <dir> [--debug|--release]
#       [--clean] [--skip-deps] [--gc [gc-args...]] [--set KEY=VALUE ...]
#   --set forwards a capability override to mtcaps (rung 1, persisted to
#   overrides.caps so `check` re-resolves identically). It is how a store
#   build is made -- `--set MT_COMMERCIAL_BUILD=1 --set MT_PRIVATE_BUILD=0` --
#   WITHOUT editing the app's tracked licence manifest. The override changes
#   the caps hash, so such a build gets its own out root and deps bucket and
#   cannot be confused with a dev one.
#   --clean cleans and STOPS (no build; same contract on all three platforms).
#   --gc is a shortcut to tools/appbuild/mtengine-gc.py; everything after it
#   goes to the GC verbatim (--gc = report, --gc --prune, --gc --prune
#   --dry-run, ...).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

APP_DIR=""
CONFIGURATION="Release"
CLEAN=false
SKIP_DEPS=false
CAPS_SETS=()

while [[ $# -gt 0 ]]; do
    case "$1" in
        --app-dir)   APP_DIR="${2:-}"; [[ -n "$APP_DIR" ]] || { echo "ERROR: --app-dir needs a value" >&2; exit 2; }; shift ;;
        --debug)     CONFIGURATION="Debug" ;;
        --release)   CONFIGURATION="Release" ;;
        --clean)     CLEAN=true; SKIP_DEPS=true ;;
        --gc)        shift; exec python3 "$ENGINE_DIR/tools/appbuild/mtengine-gc.py" "$@" ;;
        --skip-deps) SKIP_DEPS=true ;;
        --set)       [[ -n "${2:-}" ]] || { echo "ERROR: --set needs KEY=VALUE" >&2; exit 2; }; CAPS_SETS+=("--set" "$2"); shift ;;
        --incremental) : ;;  # compatibility no-op: incremental IS the default
        -h|--help)   sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
mt_appbuild_check_stub "$APP_DIR" "$ENGINE_DIR" "build-macos.sh"

# ---------------------------------------------------------------------------
# 1. the app's parameters -- one file, three platforms
# ---------------------------------------------------------------------------
CONF="$APP_DIR/mtengine-app.conf"
[[ -f "$CONF" ]] || { echo "ERROR: no $CONF -- the app must declare its parameters (see the DummyApp's)." >&2; exit 2; }
# Same constraints as mtengine.caps: KEY=VALUE, shell-sourceable, no inline
# comments -- PowerShell reads this file too.
# shellcheck source=/dev/null
source "$CONF"
for var in MT_APP_NAME MT_MACOS_PROJECT MT_MACOS_SCHEME; do
    [[ -n "${!var:-}" ]] || { echo "ERROR: $CONF does not set $var" >&2; exit 2; }
done
MANIFEST="$APP_DIR/mtengine.caps"
[[ -f "$MANIFEST" ]] || { echo "ERROR: no manifest at $MANIFEST" >&2; exit 2; }

# ---------------------------------------------------------------------------
# 2. the build lock -- ALWAYS, for every app. C2-class bugs (an app that took
#    no lock while dep builds still collide inside the shared checkout) cannot
#    be re-introduced by an app forgetting a step it no longer owns.
# ---------------------------------------------------------------------------
export MT_BUILD_LOCK_PID=$$
export MT_BUILD_LOCK_KIND=cli
"$ENGINE_DIR/tools/mtcaps/build-lock.sh" acquire "$MT_APP_NAME"
trap '"$ENGINE_DIR/tools/mtcaps/build-lock.sh" release "$MT_APP_NAME" || true' EXIT

MT_CONFIG_FLAG=$([[ "$CONFIGURATION" == "Debug" ]] && echo "--debug" || echo "--release")

# ---------------------------------------------------------------------------
# 3. resolve ONCE. --print-settings runs the single mtcaps resolve and prints
#    the xcodebuild settings; every value this driver needs is parsed from
#    that one answer, and every later engine call runs MTENGINE_PRERESOLVED.
# ---------------------------------------------------------------------------
MT_SETTINGS=()
while IFS= read -r line; do
    MT_SETTINGS+=("$line")
done < <(bash "$ENGINE_DIR/build-macos.sh" $MT_CONFIG_FLAG \
             --manifest "$MANIFEST" --app "$MT_APP_NAME" --print-settings \
             ${CAPS_SETS[@]+"${CAPS_SETS[@]}"})
[[ ${#MT_SETTINGS[@]} -gt 0 ]] || { echo "ERROR: the engine wrapper returned no capability settings." >&2; exit 1; }

MT_OUT=""; MT_CAPS_LIBS_DIR=""; MT_CAPS_RESOLVED=""; MT_FFMPEG_BUILD_MODE=""; MT_RELEASE_SYMBOLS=""
for s in "${MT_SETTINGS[@]}"; do
    case "$s" in
        MT_CAPS_OUT=*)          MT_OUT="${s#MT_CAPS_OUT=}" ;;
        MT_CAPS_LIBS_DIR=*)     MT_CAPS_LIBS_DIR="${s#MT_CAPS_LIBS_DIR=}" ;;
        MT_CAPS_RESOLVED=*)     MT_CAPS_RESOLVED="${s#MT_CAPS_RESOLVED=}" ;;
        MT_FFMPEG_BUILD_MODE=*) MT_FFMPEG_BUILD_MODE="${s#MT_FFMPEG_BUILD_MODE=}" ;;
        MT_RELEASE_SYMBOLS=*)   MT_RELEASE_SYMBOLS="${s#MT_RELEASE_SYMBOLS=}" ;;
    esac
done
for v in MT_OUT MT_CAPS_LIBS_DIR MT_CAPS_RESOLVED; do
    [[ -n "${!v}" ]] || { echo "ERROR: --print-settings carried no $v" >&2; exit 1; }
done
export MT_CAPS_LIBS_DIR
export MT_FFMPEG_BUILD_MODE
case "$MT_CAPS_RESOLVED" in
    *MT_COMMERCIAL_BUILD=1*) export COMMERCIAL=1 ;;
    *)                       export COMMERCIAL=0 ;;
esac

echo "Capabilities: $MT_APP_NAME ($CONFIGURATION)"
echo "  out     : $MT_OUT"
echo "  deps    : $MT_CAPS_LIBS_DIR"
echo "  mode    : $([ "$COMMERCIAL" = 1 ] && echo commercial || echo full), ffmpeg=$MT_FFMPEG_BUILD_MODE, symbols=$MT_RELEASE_SYMBOLS"

# ---------------------------------------------------------------------------
# 4. dependencies: submodule init (the ONLY init path -- decision 0.6a) and
#    the vendored uSockets, both inside the engine wrapper, PRE-RESOLVED.
# ---------------------------------------------------------------------------
if [[ "$SKIP_DEPS" != "true" ]]; then
    MTENGINE_PRERESOLVED_OUT="$MT_OUT" bash "$ENGINE_DIR/build-macos.sh" \
        --deps-only $MT_CONFIG_FLAG --manifest "$MANIFEST" --app "$MT_APP_NAME" \
        ${CAPS_SETS[@]+"${CAPS_SETS[@]}"}
fi

# ---------------------------------------------------------------------------
# 5. build the app scheme. The engine builds as a cross-project dependency of
#    the scheme, with the same settings; MT_CAPS_WRAPPER=1 makes the Xcode
#    pre-action skip its own resolve AND never touch the shared IDE channel.
# ---------------------------------------------------------------------------
XCODE_ARGS=(
    -project "$APP_DIR/$MT_MACOS_PROJECT"
    -scheme "$MT_MACOS_SCHEME"
    -configuration "$CONFIGURATION"
    CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO
)
if [[ "$CLEAN" == "true" ]]; then
    xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 clean
    echo "Clean complete."
    exit 0
fi
# shellcheck disable=SC2086
xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 ${MTENGINE_XCODE_ARGS:-} build

# Ask the build where the product is -- never search DerivedData.
BUILT_PRODUCTS_DIR=$(xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 \
    -showBuildSettings 2>/dev/null \
    | awk -F' = ' '/ BUILT_PRODUCTS_DIR = /{gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}')
APP_BUNDLE="$BUILT_PRODUCTS_DIR/$MT_MACOS_SCHEME.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/$MT_MACOS_SCHEME"
[[ -f "$APP_BINARY" ]] || { echo "ERROR: build reported success but no binary at $APP_BINARY" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 6. licence gate -- generic, engine-owned, runs for every app.
# ---------------------------------------------------------------------------
FFMPEG_ON=0
case "$MT_CAPS_RESOLVED" in *MT_CAP_VIDEO_PLAYBACK=1*) FFMPEG_ON=1 ;; esac
if [[ "$FFMPEG_ON" == "1" ]]; then
    MARKER="$MT_CAPS_LIBS_DIR/.ffmpeg-build-mode"
    MARKER_MODE="$(cat "$MARKER" 2>/dev/null || echo missing)"
    if [[ "$COMMERCIAL" == "1" && "$MARKER_MODE" != "commercial" ]]; then
        echo "ERROR: commercial app build against a '$MARKER_MODE' FFmpeg install ($MARKER)." >&2
        exit 1
    elif [[ -n "$MT_FFMPEG_BUILD_MODE" && "$MARKER_MODE" != "$MT_FFMPEG_BUILD_MODE" ]]; then
        echo "WARNING: FFmpeg install is '$MARKER_MODE' but this resolve expects '$MT_FFMPEG_BUILD_MODE' -- stale prefix; the stamped rebuild corrects it." >&2
    fi
    if [[ "${MT_FFMPEG_BUILD_MODE:-full}" != "full" ]]; then
        SCAN_DIR="$MT_CAPS_LIBS_DIR"
        [[ -f "$APP_BUNDLE/Contents/Frameworks/libavcodec.61.dylib" || -f "$APP_BUNDLE/Contents/Frameworks/libavcodec.dylib" ]] \
            && SCAN_DIR="$APP_BUNDLE/Contents/Frameworks"
        bash "$SCRIPT_DIR/scan-forbidden-symbols.sh" --mode "$MT_FFMPEG_BUILD_MODE" --libs "$SCAN_DIR"
    fi
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
# 7. symbols contract (decision 0.5). The split debug info ALWAYS lands in
#    $MT_OUT/symbols/ -- store-crash symbolication and the stripped-build
#    probe surface -- and the BINARY carries symbols only when the resolved
#    MT_RELEASE_SYMBOLS says so.
# ---------------------------------------------------------------------------
SYMBOLS_DIR="$MT_OUT/symbols"
mkdir -p "$SYMBOLS_DIR"
dsymutil "$APP_BINARY" -o "$SYMBOLS_DIR/$MT_MACOS_SCHEME.dSYM" 2>/dev/null \
    || echo "WARNING: dsymutil produced no dSYM (no debug info in the objects?)" >&2
if [[ "${MT_RELEASE_SYMBOLS:-1}" == "0" ]]; then
    BEFORE=$(nm "$APP_BINARY" 2>/dev/null | wc -l | tr -d ' ')
    # -Sx, not -S: on macOS -S removes only debug entries (DWARF lives in the
    # objects anyway; the dSYM above is the retained copy), so the symbol
    # table survived untouched -- measured, 50980 -> 50980 nm lines. -x drops
    # the local symbols too, which is what a distributed artifact sheds.
    strip -Sx "$APP_BINARY"
    AFTER=$(nm "$APP_BINARY" 2>/dev/null | wc -l | tr -d ' ')
    codesign -f -s - "$APP_BUNDLE" 2>/dev/null || true
    echo "Symbols: STRIPPED for distribution ($BEFORE -> $AFTER nm lines); dSYM retained at $SYMBOLS_DIR"
else
    echo "Symbols: kept in the binary (MT_RELEASE_SYMBOLS=$MT_RELEASE_SYMBOLS); dSYM at $SYMBOLS_DIR"
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
echo "$APP_BUNDLE"
