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
#       [--clean] [--skip-deps] [--prod] [--gc [gc-args...]]
#       [--set KEY=VALUE ...]
#   --prod adds stage 8, the release package under platform/MacOS/prod/. A
#   DEVELOPMENT build -- the default -- produces no package and copies no
#   assets: it runs, and is tested, from the git root. A FINAL build passes
#   --prod, is tested from the package with tests/run_test.sh --package, and
#   is what ships. The procedure is docs/testing.md.
#   --config debug|release   optimisation (aliases: --debug, --release)
#   --logs on|off            debug logging compiled in (MT_DEBUG_LOGS); on by
#                            default, OFF by default under --prod
#   --symbols on|off         debug symbols in the shipped binary
#                            (MT_RELEASE_SYMBOLS); on by default, OFF under --prod
#   --tier dev|commercial    licence tier (MT_COMMERCIAL_BUILD); dev by default
#   An explicit switch beats what --prod implies: `--prod --logs on` is a
#   diagnostic package. --symbols on with --tier commercial is refused.
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
        # The old default was to package on every build and --no-prod opted
        # out. Rejected rather than accepted as a no-op: a script still
        # passing it must fail loudly, not quietly build something else.
        --no-prod)   echo "ERROR: --no-prod is gone. A build makes no package unless you pass --prod (see docs/testing.md)." >&2; exit 2 ;;
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

MT_OUT=""; MT_CAPS_LIBS_DIR=""; MT_CAPS_RESOLVED=""; MT_FFMPEG_BUILD_MODE=""; MT_RELEASE_SYMBOLS=""; MT_DEBUG_LOGS=""
for s in "${MT_SETTINGS[@]}"; do
    case "$s" in
        MT_CAPS_OUT=*)          MT_OUT="${s#MT_CAPS_OUT=}" ;;
        MT_CAPS_LIBS_DIR=*)     MT_CAPS_LIBS_DIR="${s#MT_CAPS_LIBS_DIR=}" ;;
        MT_CAPS_RESOLVED=*)     MT_CAPS_RESOLVED="${s#MT_CAPS_RESOLVED=}" ;;
        MT_FFMPEG_BUILD_MODE=*) MT_FFMPEG_BUILD_MODE="${s#MT_FFMPEG_BUILD_MODE=}" ;;
        MT_RELEASE_SYMBOLS=*)   MT_RELEASE_SYMBOLS="${s#MT_RELEASE_SYMBOLS=}" ;;
        MT_DEBUG_LOGS=*)        MT_DEBUG_LOGS="${s#MT_DEBUG_LOGS=}" ;;
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
# tier and ffmpeg mode are different axes; printing the tier as "mode" beside
# the real mode made two names for one label and hid the disagreement.
echo "  tier    : $([ "$COMMERCIAL" = 1 ] && echo commercial || echo non-commercial), ffmpeg=$MT_FFMPEG_BUILD_MODE, symbols=$MT_RELEASE_SYMBOLS, logs=${MT_DEBUG_LOGS:-1}"

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

# ---------------------------------------------------------------------------
# 8. release package (Stage 7 of the unification plan). Windows has had this
#    since the driver landed; macOS and Linux never implemented it, so the
#    contract's "LICENCES.txt ships ALWAYS" was true on one platform out of
#    three -- the document exists in $MT_OUT on every platform, and nobody
#    copied it.
#
#    BESIDE THE BUNDLE, NOT INSIDE IT. Adding a file to Contents/Resources
#    after the build invalidates the ad-hoc signature the strip branch above
#    applies, and would need a re-sign in one branch and not the other. The
#    Windows package puts the document next to the executable; this does the
#    same next to the bundle.
# ---------------------------------------------------------------------------
if [[ "$MAKE_PROD" == "true" ]]; then
    PROD_DIR="$APP_DIR/platform/MacOS/prod/$(uname -m)"
    # The tier in the artifact name, as on Windows (-nc for non-commercial).
    # A reader with two builds in front of them can tell which is which.
    PROD_NAME="$MT_MACOS_SCHEME$([[ "${COMMERCIAL:-0}" == "1" ]] || echo -n "-nc").app"

    echo ""
    echo "=== Deploying release package ($(uname -m) $CONFIGURATION) ==="
    rm -rf "$PROD_DIR"
    mkdir -p "$PROD_DIR"
    cp -R "$APP_BUNDLE" "$PROD_DIR/$PROD_NAME"

    LICENCES_SRC="$MT_OUT/LICENSES.txt"
    [[ -f "$LICENCES_SRC" ]] || { echo "ERROR: mtcaps wrote no LICENSES.txt at $LICENCES_SRC" >&2; exit 1; }
    cp "$LICENCES_SRC" "$PROD_DIR/LICENSES.txt"

    ASSETS_NAME="${MT_MACOS_ASSETS:-assets}"
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

    # Belt for the symbols contract, the counterpart of the Windows *.pdb
    # sweep: a dSYM is the macOS debug companion and must not ride a package.
    find "$PROD_DIR" -name '*.dSYM' -maxdepth 3 -exec rm -rf {} + 2>/dev/null || true

    # The check the contract needs. "Always" that nothing verifies is a
    # comment; this is the one place that can fail a build over it.
    [[ -f "$PROD_DIR/LICENSES.txt" ]] || { echo "ERROR: release package has no LICENSES.txt: $PROD_DIR" >&2; exit 1; }
    echo "Deployed: $PROD_DIR/$PROD_NAME"
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
