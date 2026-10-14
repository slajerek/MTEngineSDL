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
#   2. take THIS APP's build lock (tools/mtcaps/build-lock.sh) -- not a
#      checkout-wide one: a CLI build collides with nothing an Xcode build of a
#      DIFFERENT app touches, so the two now run together
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

# A final build tells the dependency scripts so: build-jpeg.sh then REFUSES an
# x86_64 slice without SIMD (no nasm) instead of warning. Exported before the
# deps and compile stages; the Xcode script phases inherit it from xcodebuild.
# The driver OWNS the variable (like the Windows driver): a value inherited from
# the calling shell never turns a development build into a final one.
if [[ "$MAKE_PROD" == "true" ]]; then export MT_PROD_BUILD=1; else unset MT_PROD_BUILD; fi
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

# ---------------------------------------------------------------------------
# THE MT_ENABLE_* FLAGS, from the fragment of the SAME resolve.
#
# This driver used to know only the six keys parsed above. The flags were
# passed straight through to xcodebuild and never bound to a shell variable, so
# `${MT_ENABLE_FFMPEG:-1}` in this file read the DEFAULT, always, on every app
# -- including c64d, which resolves MT_CAP_VIDEO_PLAYBACK=0 and links no FFmpeg
# at all. Any gate written against that would have passed by accident today and
# broken the moment the flag could differ, which it now can (Plan A Task 1).
#
# Linux has done this since it was written (app-build-linux.sh), which is why
# `${MT_ENABLE_FFMPEG:-1}` already means what it says there. Same call, same
# fragment, one mechanism on both platforms -- rather than adding a seventh key
# to the parse loop above, which would bind the one flag somebody remembered
# and leave the rest still lying.
# shellcheck source=/dev/null
source "$ENGINE_DIR/platform/caps-lib.sh"
mt_caps_read_flags "$MT_OUT/MTEngineCapabilities.cmake"

# ---------------------------------------------------------------------------
# WHAT XCODE IS TOLD THE LIBS DIR IS -- a stable path, not the keyed one.
#
# The engine's Xcode project declares every capability archive as a script
# phase OUTPUT under $(MT_CAPS_LIBS_DIR):
#
#     outputPaths = ( "$(MT_CAPS_LIBS_DIR)/libmbedtls_bundle.a", ... )
#
# and that directory is content-addressed by the caps hash. So each distinct
# capability set gives Xcode a DIFFERENT set of declared outputs, XCBuild
# records them in build.db for stale-file removal, and on the next build with
# another caps set the previous ones are "no longer produced" -- stale. It then
# tries to delete them, finds them under $MTENGINE_BUILD_ROOT rather than under
# SRCROOT or DerivedData, and reports
#
#     warning: Stale file '<...>/libs/libmbedtls_bundle.a' is located outside
#              of the allowed root paths.
#
# The entry can never be retired, because the mechanism that would retire it is
# the one refusing to act -- so the warnings accumulate, one per archive per
# caps set ever built, for the life of the DerivedData. Measured before this
# change: 17 distinct buckets recorded in MTEngineSDLDummyApp's build.db, and 8
# of an app's 9 already pointing at directories that no longer exist.
#
# So Xcode gets ONE path that never varies, per app and configuration, and that
# path is a symlink to whichever keyed directory this build resolved to. The
# archives are still built into, and shared from, the keyed store -- the script
# phases write through the link -- so nothing about the dependency cache
# changes. Only what Xcode records changes, and it now records the same path
# every time.
#
# The link name carries the app and configuration because two apps, or the same
# app in Debug and Release, resolve to different keyed directories and must not
# fight over one link.
MT_CAPS_LIBS_REAL="$MT_CAPS_LIBS_DIR"
MT_CAPS_LIBS_LINK="$(dirname "$(dirname "$MT_CAPS_LIBS_REAL")")/_xcode-${MT_APP_NAME}-${CONFIGURATION}"
if ln -sfn "$MT_CAPS_LIBS_REAL" "$MT_CAPS_LIBS_LINK" 2>/dev/null; then
    for i in "${!MT_SETTINGS[@]}"; do
        case "${MT_SETTINGS[$i]}" in
            MT_CAPS_LIBS_DIR=*) MT_SETTINGS[$i]="MT_CAPS_LIBS_DIR=$MT_CAPS_LIBS_LINK" ;;
        esac
    done
else
    # A filesystem that cannot symlink is not a reason to fail the build; it
    # only means the warnings come back.
    echo "WARNING: could not create $MT_CAPS_LIBS_LINK -- Xcode will record the" >&2
    echo "         keyed deps path and emit stale-file warnings after a caps change." >&2
fi
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
    # Pin the build products to the app repo's own build-macos tree. Without
    # this xcodebuild uses the global DerivedData directory, whose path is
    # machine-specific and invisible to the surrounding tooling -- the release
    # script stages "build-macos/Build/Products/<config>" and an IDE-side
    # assumption that the bundle lives beside the checkout is dead code. The
    # query below (-showBuildSettings) follows the same setting, so every
    # consumer of BUILT_PRODUCTS_DIR agrees on one place. Matches the old
    # app-owned build-macos.sh, which passed -derivedDataPath for the same
    # reason before the driver took the flow over.
    -derivedDataPath "$APP_DIR/build-macos"
    CODE_SIGN_IDENTITY="-" CODE_SIGNING_REQUIRED=NO CODE_SIGNING_ALLOWED=NO
)
if [[ "$CLEAN" == "true" ]]; then
    xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 clean
    echo "Clean complete."
    exit 0
fi
# Ask the build where the product is -- never search DerivedData. Asked BEFORE
# the build, not after, because the embedded-dylib guard below needs the path
# while the bundle still holds the PREVIOUS build's copies. It costs nothing
# extra: this query used to run here anyway, just a few lines later.
BUILT_PRODUCTS_DIR=$(xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 \
    -showBuildSettings 2>/dev/null \
    | awk -F' = ' '/ BUILT_PRODUCTS_DIR = /{gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}')
APP_BUNDLE="$BUILT_PRODUCTS_DIR/$MT_MACOS_SCHEME.app"

# ---------------------------------------------------------------------------
# EMBEDDED DYLIBS ARE REPLACED, NEVER OVERWRITTEN IN PLACE.
#
# Xcode's copy-files phase writes over the dylib already in
# Contents/Frameworks, which REUSES ITS INODE. macOS caches a code signature
# against the vnode, so when the bytes change but the inode does not, dyld
# refuses the library at the next launch:
#
#   Library not loaded: @rpath/libavutil.59.dylib
#   ... fcntl(fd, F_ADDFILESIGS_RETURN) failed with errno=37   [EALREADY]
#
# The app then dies at exec, before main, before the crash reporter exists --
# so it leaves no crash report, and nothing on screen points at the cause.
#
# It bites whenever two builds embed DIFFERENT bytes at the same path, which
# is exactly what alternating licence tiers does: --tier commercial rebuilds
# FFmpeg in commercial mode, and the next development build copies the full
# dylibs back over those same inodes. Measured here three times, once by a
# maintainer who simply ran the app after a prod build and saw it die.
#
# So when the dependency set changes, the whole directory goes and the copy
# phase recreates it with fresh inodes. Keyed on MT_CAPS_LIBS_DIR because that
# path carries the caps hash, which is what changes when the tier or any
# capability does.
FW_STAMP="$BUILT_PRODUCTS_DIR/.mt-embedded-libs-$MT_MACOS_SCHEME"
if [[ -d "$APP_BUNDLE/Contents/Frameworks" ]] \
   && [[ "$(cat "$FW_STAMP" 2>/dev/null)" != "$MT_CAPS_LIBS_DIR" ]]; then
    echo "Embedded libraries: dependency set changed, replacing them (stale-inode signature guard)"
    rm -rf "$APP_BUNDLE/Contents/Frameworks"
fi

# shellcheck disable=SC2086
xcodebuild "${XCODE_ARGS[@]}" "${MT_SETTINGS[@]}" MT_CAPS_WRAPPER=1 ${MTENGINE_XCODE_ARGS:-} build

mkdir -p "$BUILT_PRODUCTS_DIR" 2>/dev/null || true
printf '%s\n' "$MT_CAPS_LIBS_DIR" > "$FW_STAMP" 2>/dev/null || true
APP_BINARY="$APP_BUNDLE/Contents/MacOS/$MT_MACOS_SCHEME"
[[ -f "$APP_BINARY" ]] || { echo "ERROR: build reported success but no binary at $APP_BINARY" >&2; exit 1; }

# ---------------------------------------------------------------------------
# 6. licence gate -- generic, engine-owned, runs for every app.
# ---------------------------------------------------------------------------
# KEYED ON THE FLAG, NOT THE CAPABILITY.
#
# This used to read `case $MT_CAPS_RESOLVED in *MT_CAP_VIDEO_PLAYBACK=1*)`, and
# MT_CAPS_RESOLVED is canonical_form, which structurally cannot carry an
# MT_ENABLE_*. So it could not see an FFmpeg-less build at all: it would demand
# a .ffmpeg-build-mode marker from a build that deliberately has no FFmpeg and
# exit 1. Video-on/FFmpeg-off is the whole point of the programme, so the gate
# has to ask about FFmpeg. The flag is in scope because this driver now reads
# the emitted fragment (engine 29212704); the :-1 is a belt-and-braces default
# matching "absent means on" everywhere else, not the working path.
FFMPEG_ON="${MT_ENABLE_FFMPEG:-1}"
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
# 6b. THE POSITIVE HALF of the licence gate.
#
# Everything above checks WHICH DECODERS are inside a libavcodec that is
# present. Nothing checked the claim this programme actually makes: that a
# distributed build contains no FFmpeg at all. scan-capability-symbols.sh's
# `forbidden_when_off` cannot -- it runs only when MT_CAP_VIDEO_PLAYBACK is 0,
# and a store build has it at 1.
#
# RUN IN BOTH DIRECTIONS, ON EVERY BUILD, and that is deliberate. A gate that
# only ever executes in the rare configuration is a gate nobody has tested; this
# one asserts the ordinary truth on every ordinary build, so it is exercised
# constantly and its failure mode is known. It was proven to fail before it was
# trusted: on an FFmpeg-on bundle `--expect absent` reports 19 matches including
# nine staged dylibs.
# ---------------------------------------------------------------------------
if [[ "$FFMPEG_ON" == "1" ]]; then
    bash "$SCRIPT_DIR/scan-no-ffmpeg.sh" --binary "$APP_BINARY" --bundle "$APP_BUNDLE" --expect present
else
    bash "$SCRIPT_DIR/scan-no-ffmpeg.sh" --binary "$APP_BINARY" --bundle "$APP_BUNDLE" --expect absent
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
    # RE-SIGN THE EXECUTABLE, AND FAIL IF IT DOES NOT WORK.
    #
    # strip invalidates the ad-hoc signature Xcode applied, and on Apple
    # silicon an invalid signature is not a warning: the kernel SIGKILLs the
    # process at exec, before main, with no output at all -- the caller sees
    # "Killed: 9" and an empty log, with nothing pointing at the signature. So
    # this step is not cosmetic, and its failure must not be swallowed, which
    # it was by `2>/dev/null || true` for as long as this driver has existed.
    #
    # SIGNED OUTSIDE THE BUNDLE, THEN COPIED BACK. That looks roundabout and is
    # the only arrangement that works; the two obvious ones both fail:
    #
    #   - `codesign` on ANY path inside a bundle operates on the BUNDLE, not on
    #     the file named. Pointed at Contents/MacOS/<app> it exits 1 with "code
    #     object is not signed at all / In subcomponent:
    #     .../Frameworks/libswresample.<v>.dylib", because a bundle cannot be
    #     sealed while its nested code is unsigned -- and leaves the app
    #     stripped and unsigned. That is the original bug.
    #
    #   - Signing those nested dylibs first fixes THIS build and breaks the
    #     NEXT one. Measured: Xcode's copy-files phase does not re-copy a dylib
    #     whose timestamp and size are unchanged, so the signatures survive
    #     into a later development build and dyld then refuses them --
    #     "Library not loaded: @rpath/libavutil.59.dylib ... fcntl(fd,
    #     F_ADDFILESIGS_RETURN) failed with errno=37". A whole suite run died
    #     that way. Never sign an artefact a later incremental build will reuse.
    #
    # A lone Mach-O outside any bundle has no nested code to seal, so signing
    # it succeeds; the signature is embedded in the file and travels with the
    # bytes when it is copied back. strip touched exactly one file, and exactly
    # one file is re-signed.
    # SIGNED AND VERIFIED WHILE IT IS STILL A LONE MACH-O, then copied back.
    #
    # Both halves have to happen outside the bundle, and for the same reason:
    # codesign given ANY path inside a bundle acts on the BUNDLE. Signing there
    # fails on the unsigned nested dylibs; verifying there fails on whatever
    # else the bundle root holds -- one app stages assets/, data/ and
    # cache/ beside Contents/, and codesign calls that "unsealed contents
    # present in the bundle root" and exits 1 even when the executable is
    # perfectly signed. Neither complaint is about the thing strip touched.
    #
    # A lone Mach-O has no bundle, no nested code and no seal, so both
    # operations mean exactly what they say. The signature is embedded in the
    # file and travels with the bytes when it is copied back.
    SIGN_TMP_DIR="$(mktemp -d)"
    SIGN_TMP="$SIGN_TMP_DIR/$(basename "$APP_BINARY")"
    if ! cp "$APP_BINARY" "$SIGN_TMP" \
       || ! codesign -f -s - "$SIGN_TMP" \
       || ! codesign --verify "$SIGN_TMP"; then
        rm -rf "$SIGN_TMP_DIR"
        echo "ERROR: the stripped executable could not be re-signed and verified." >&2
        echo "       On Apple silicon it would be killed at exec with no output." >&2
        echo "       Not shipping it." >&2
        exit 1
    fi
    cp "$SIGN_TMP" "$APP_BINARY"
    rm -rf "$SIGN_TMP_DIR"
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

# THE BINARY, BY ABSOLUTE PATH, ONCE, AT THE END. Printed after the strip and
# re-sign above (both rewrite the file, so its mtime is final only here). A
# runner that picks a binary by searching can then be checked against this
# line by eye: tests/run_test.sh prints "=== Using binary: <abs> (<mtime>)"
# in the same shape, and the app itself prints its own path at startup. A
# stale binary run from somewhere else -- the DerivedData product of an older
# Xcode build, for instance -- shows up as two different paths.
_mt_mtime() { stat -f '%Sm' -t '%Y-%m-%d %H:%M:%S' "$1" 2>/dev/null || echo "mtime unknown"; }
_mt_abs()   { (cd "$(dirname "$1")" && printf '%s/%s\n' "$(pwd -P)" "$(basename "$1")"); }
echo ""
echo "=== Built binary: $(_mt_abs "$APP_BINARY") ($CONFIGURATION, $(_mt_mtime "$APP_BINARY")) ==="
if [[ "$MAKE_PROD" == "true" ]]; then
    PROD_BINARY="$PROD_DIR/$PROD_NAME/Contents/MacOS/$MT_MACOS_SCHEME"
    echo "=== Deployed binary: $(_mt_abs "$PROD_BINARY") ($CONFIGURATION, $(_mt_mtime "$PROD_BINARY")) ==="
fi
echo "$MT_APP_NAME built successfully."
echo "$APP_BUNDLE"
