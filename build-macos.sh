#!/usr/bin/env bash
set -euo pipefail

# Master build script for MTEngineSDL on macOS -- the counterpart to
# build-linux.sh, which had no macOS equivalent until now.
#
# Two audiences, one script:
#   * development -- `./build-macos.sh --debug` and keep using Xcode.app; the
#     build shares Xcode's default DerivedData, so the IDE sees the same
#     products and incremental builds are not thrown away.
#   * deployment  -- `./build-macos.sh --release --derived-data <dir>` gives a
#     deterministic, self-contained output location for CI and release jobs,
#     and prints the resolved archive path on the last line.
#
# Usage:
#   ./build-macos.sh                     # Release, active arch, shared DerivedData
#   ./build-macos.sh --debug             # Debug
#   ./build-macos.sh --clean             # clean before building
#   ./build-macos.sh --deps-only         # dependencies + uSockets, no engine build
#   ./build-macos.sh --arch x86_64       # override the architecture
#   ./build-macos.sh --derived-data DIR  # deterministic products location
#
# CAPABILITIES
#   ./build-macos.sh --manifest ../MyApp/mtengine.caps --app MyApp
#
#   With --manifest and --app the wrapper runs tools/mtcaps, writes the generated
#   fragments to $MT_OUT (outside every checkout), and passes the resolved set to
#   xcodebuild as SETTINGS. Without them it builds in STANDALONE mode: the engine
#   compiles with its own defaults from platform/MacOS/MTEngineSDL.xcconfig, which
#   is what "clone the engine, open it, build it" must keep doing.
#
#   ./build-macos.sh --manifest M --app A --print-settings
#       resolve only, and print one `KEY=value` xcodebuild setting per line. This
#       is how an app's own build-macos.sh gets the settings for ITS xcodebuild
#       invocation -- a command-line setting reaches every target in an
#       invocation, so the app has to pass the same list its engine build used.
#
# Extra xcodebuild settings can be passed through the environment:
#   MTENGINE_XCODE_ARGS='MT_COMMERCIAL_BUILD=1' ./build-macos.sh --release
#
# Everything else -- SDL3, image/video codecs, llama.cpp, mbedTLS, FTXUI -- is
# built by the engine target's own Xcode script phases, so there is nothing to
# pre-build here. uSockets is the one exception; see Step 3.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
XCODE_PROJECT="$SCRIPT_DIR/platform/MacOS/MTEngineSDL.xcodeproj"
# The staging destination is resolved LATER, once the manifest has been read --
# it depends on $MT_CAPS_OUT. See mt_caps_lib_dir in platform/caps-lib.sh.
LIBS_DIR=""

# uSockets is VENDORED inside this repository -- not a submodule and not a
# sibling checkout. The vendored copy carries a local ARM64 fix
# (`listenAddr = NULL` in other/lib/uSockets/src/bsd.c); pristine upstream
# leaves that pointer uninitialised, so cloning upstream over it silently
# produces a broken archive. Build this copy and only this copy.
USOCKETS_DIR="$SCRIPT_DIR/other/lib/uSockets"

CONFIGURATION="Release"
ARCH=""
DERIVED_DATA=""
CLEAN=false
DEPS_ONLY=false
MANIFEST=""
APP_NAME=""
PRINT_SETTINGS=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)        CONFIGURATION="Debug" ;;
        --release)      CONFIGURATION="Release" ;;
        --clean)        CLEAN=true ;;
        --deps-only)    DEPS_ONLY=true ;;
        --arch)         ARCH="${2:-}"; [[ -n "$ARCH" ]] || { echo "ERROR: --arch needs a value" >&2; exit 2; }; shift ;;
        --derived-data) DERIVED_DATA="${2:-}"; [[ -n "$DERIVED_DATA" ]] || { echo "ERROR: --derived-data needs a value" >&2; exit 2; }; shift ;;
        --manifest)     MANIFEST="${2:-}"; [[ -n "$MANIFEST" ]] || { echo "ERROR: --manifest needs a value" >&2; exit 2; }; shift ;;
        --app)          APP_NAME="${2:-}"; [[ -n "$APP_NAME" ]] || { echo "ERROR: --app needs a value" >&2; exit 2; }; shift ;;
        --print-settings) PRINT_SETTINGS=true ;;
        -h|--help)
            sed -n '4,46p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
            exit 0 ;;
        *)  echo "ERROR: unknown argument '$1' (try --help)" >&2; exit 2 ;;
    esac
    shift
done

if ! command -v xcodebuild >/dev/null 2>&1; then
    echo "ERROR: xcodebuild not found." >&2
    echo "       Install Xcode and run: sudo xcode-select --switch /Applications/Xcode.app" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 0: resolve the capability set
#
# THE WRAPPER IS THE GENERATOR, AND THE ONLY ONE. It runs tools/mtcaps, writes
# the fragments to $MT_OUT -- outside every checkout -- and passes the resolved
# values to xcodebuild as SETTINGS. Nothing is written inside this repository.
#
# Measured (spike G, claude/spikes/xcconfig/README.md in the DummyApp repo):
#
#   * a command-line setting reaches a CROSS-PROJECT dependency's compile line,
#     which is what makes one invocation configure both the engine and the app;
#   * a *tracked* xcconfig line `$(inherited) $(MT_CAPS_DEFINES)` expands a
#     command-line setting -- the mechanism, and it had never been shown;
#   * a BARE `GCC_PREPROCESSOR_DEFINITIONS=` replaces the list wholesale, which
#     would delete MACOS and every target's private defines. So this script never
#     passes one. It passes MT_CAPS_DEFINES and lets each target's tracked
#     xcconfig consume it;
#   * a command-line setting cannot be scoped per-target and does not need to be:
#     the split is made by which settings each xcconfig references.
# ---------------------------------------------------------------------------
MTCAPS_SETTINGS=()
MT_OUT=""

if [[ -n "$MANIFEST" || -n "$APP_NAME" ]]; then
    if [[ -z "$MANIFEST" || -z "$APP_NAME" ]]; then
        echo "ERROR: --manifest and --app go together." >&2
        echo "       Omit BOTH for a standalone engine build." >&2
        exit 2
    fi
    if [[ ! -f "$MANIFEST" ]]; then
        echo "ERROR: manifest not found: $MANIFEST" >&2
        exit 2
    fi

    # A RESOLVED interpreter, never a bare `python`/`python3` on PATH beyond this
    # lookup: a missing interpreter is an error naming the install command, not a
    # downgrade.
    PYTHON3="$(command -v python3 || true)"
    if [[ -z "$PYTHON3" ]]; then
        echo "ERROR: python3 not found, and tools/mtcaps needs it." >&2
        echo "       Install it with: brew install python3" >&2
        exit 2
    fi

    RESOLVE_ARCH="${ARCH:-$(uname -m)}"

    # <backend> is the WRAPPER's to resolve, not the app's. MT_GGML_NATIVE's
    # effective value is decided from `uname -m` inside the engine's own scripts,
    # and mtcaps runs in the app's tree before those are invoked -- an app passing
    # it would be replicating the engine's ARM rule in four places.
    ENGINE_OPTIONS=()
    case "$RESOLVE_ARCH" in
        arm64|aarch64) ENGINE_OPTIONS+=(--engine-option MT_GGML_NATIVE=OFF) ;;
        *)             ENGINE_OPTIONS+=(--engine-option MT_GGML_NATIVE=ON) ;;
    esac

    MTCAPS_OUTPUT="$("$PYTHON3" -B "$SCRIPT_DIR/tools/mtcaps/mtcaps.py" resolve \
        --manifest "$MANIFEST" --app "$APP_NAME" \
        --platform macos --arch "$RESOLVE_ARCH" --config "$CONFIGURATION" \
        --engine-dir "$SCRIPT_DIR" "${ENGINE_OPTIONS[@]}")" || {
        echo "ERROR: mtcaps resolve failed for $MANIFEST" >&2
        exit 2
    }

    MT_CAPS_RESOLVED="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^resolved=//p')"
    MT_OUT="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^out_dir=//p')"
    if [[ -z "$MT_OUT" || -z "$MT_CAPS_RESOLVED" ]]; then
        echo "ERROR: mtcaps resolve produced no out_dir/resolved line" >&2
        exit 2
    fi

    # The dependency-archive directory, read from the SAME resolve that produced
    # $MT_OUT and then handed to every consumer: the six acquisition script
    # phases, the linker's search path, and this shell's own uSockets staging.
    # It comes OUT OF mtcaps rather than being recomputed here -- one resolve,
    # one <backend>, so the fragments and the archives cannot land under
    # different keys. See resolve.deps_dir for why it is not $MT_OUT.
    if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
        # shellcheck source=platform/caps-lib.sh
        source "$SCRIPT_DIR/platform/caps-lib.sh"
    fi
    MT_CAPS_LIBS_DIR="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^deps_dir=//p')"
    if [[ -z "$MT_CAPS_LIBS_DIR" ]]; then
        echo "ERROR: mtcaps resolve produced no deps_dir line" >&2
        exit 2
    fi
    export MT_CAPS_LIBS_DIR

    GEN_XCCONFIG="$MT_OUT/MTEngineCaps.xcconfig"
    caps_value() { sed -n "s/^$1 = //p" "$GEN_XCCONFIG"; }

    # Each MT_ENABLE_*/MT_CAP_* also rides as a STANDALONE setting, not only
    # inside the MT_CAPS_DEFINES blob. A script phase sees build settings as
    # environment variables, so this is what lets the engine's six acquisition
    # phases read `$MT_ENABLE_LLAMA_CPP` directly. Re-parsing a define blob in
    # six shell scripts would make the check its own source of bugs.
    MTCAPS_FLAG_SETTINGS=()
    while IFS= read -r flagline; do
        MTCAPS_FLAG_SETTINGS+=("$flagline")
    done < <(grep -E '^(MT_ENABLE_|MT_CAP_|MT_CAMERA_CAPTURE_ENABLED)[A-Z0-9_]* = [01]$' \
                  "$GEN_XCCONFIG" | sed 's/ = /=/')

    MTCAPS_SETTINGS=(
        "MT_CAPS_SUPPLIED=YES"
        # WHICH APP, on the command line and not only in the shared xcconfig.
        # The backstop compares this against OBJROOT to catch a build handed
        # another app's set. A wrapper build does not write the shared file at
        # all, so without this line MT_CAPS_APP would be read from whatever a
        # PREVIOUS build left there -- and the check would fire on a build that
        # is perfectly correct. (It did, first time this was tried.)
        "MT_CAPS_APP=$APP_NAME"
        "MT_CAPS_DEFINES=$(caps_value MT_CAPS_DEFINES)"
        "MT_CAPS_DEFINES_ENGINE=$(caps_value MT_CAPS_DEFINES_ENGINE)"
        "HEADER_SEARCH_PATHS=\$(inherited) \"$MT_OUT/include\""
        "MT_CAPS_OUT=$MT_OUT"
        "MT_CAPS_LIBS_DIR=$MT_CAPS_LIBS_DIR"
        "MT_CAPS_RESOLVED=$MT_CAPS_RESOLVED"
    )
    if [[ ${#MTCAPS_FLAG_SETTINGS[@]} -gt 0 ]]; then
        MTCAPS_SETTINGS+=("${MTCAPS_FLAG_SETTINGS[@]}")
    fi
    # HEADER_SEARCH_PATHS carries $(inherited) and is the one setting passed
    # directly: measured, a bare assignment replaces the engine's own list --
    # which is substantial -- so getting it wrong breaks every include rather than
    # just the capability header.

    if [[ "$PRINT_SETTINGS" == "true" ]]; then
        printf '%s\n' "${MTCAPS_SETTINGS[@]}"
        exit 0
    fi

    # Read every MT_ENABLE_*/MT_CAP_* out of the generated fragment and EXPORT
    # them, so the acquisition scripts and the Xcode script phases can read a
    # flag directly instead of re-parsing a blob. The generator emits each as a
    # standalone assignment precisely so this is possible.
    # shellcheck source=platform/caps-lib.sh
    source "$SCRIPT_DIR/platform/caps-lib.sh"
    mt_caps_read_flags "$GEN_XCCONFIG"

    # The licence mode reaches the acquisition scripts as a PARAMETER, which is
    # what closes the "second acquisition-time policy input" the two retired
    # BUILD_MODE_DEFAULT files used to be. One switch, one channel.
    case "$MT_CAPS_RESOLVED" in
        *MT_COMMERCIAL_BUILD=1*) export COMMERCIAL=1 ;;
        *)                       export COMMERCIAL=0 ;;
    esac

    # EXPORT it, do not merely pass it to xcodebuild. $MT_OUT reaches the Xcode
    # script phases as a build setting (MTCAPS_SETTINGS above), which is why the
    # acquisition scripts they run stage correctly. But this SHELL also stages a
    # dependency -- uSockets, Step 3 -- through mt_caps_lib_dir, and that helper
    # reads $MT_CAPS_OUT from the environment. Without this export it fell back
    # to the _standalone root, so uSockets.a landed outside the keyed tree the
    # app then searched (measured: everything else keyed correctly, uSockets did
    # not).
    export MT_CAPS_OUT="$MT_OUT"

    echo "Capabilities: $APP_NAME"
    echo "  manifest : $MANIFEST"
    echo "  out      : $MT_OUT"
    echo "  mode     : $([ "$COMMERCIAL" = 1 ] && echo commercial || echo full)"
elif [[ "$PRINT_SETTINGS" == "true" ]]; then
    echo "ERROR: --print-settings needs --manifest and --app." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# Step 1: submodules
#
# Guarded, not unconditional: only fetch when a submodule is genuinely absent.
# An unconditional `git submodule update --init --recursive` re-fetches every
# submodule on every build, which is both slow and the thing that makes
# selective dependency acquisition impossible to observe.
# ---------------------------------------------------------------------------
# SELECTIVE, not blanket. This used to fetch all three gated submodules -- 1.25
# GB of mbedTLS, llama.cpp and FTXUI -- whenever any one was missing. Now it
# fetches only what the resolved set asks for, which for c64d is none of them.
#
# With no manifest (a standalone build) mt_caps_submodules defaults every flag to
# 1 and this behaves exactly as before.
if ! declare -f mt_caps_init_submodules >/dev/null 2>&1; then
    # shellcheck source=platform/caps-lib.sh
    source "$SCRIPT_DIR/platform/caps-lib.sh"
fi
mt_caps_init_submodules "$SCRIPT_DIR"

# ---------------------------------------------------------------------------
# Step 2: sanity-check the vendored uSockets
# ---------------------------------------------------------------------------
if [[ ! -f "$USOCKETS_DIR/Makefile" ]]; then
    echo "ERROR: vendored uSockets not found at $USOCKETS_DIR" >&2
    echo "       It ships inside this repository. Restore it with:" >&2
    echo "         git checkout HEAD -- other/lib/uSockets" >&2
    echo "       Do NOT clone uSockets from upstream -- the vendored copy is patched." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Step 3: build and stage uSockets.a
#
# This lives here rather than in each app's build script. uSockets is the
# engine's vendored dependency, so restoring and building it is the engine's
# job; an app that has to repair its dependency's checkout is a bug in the
# dependency. Apps consume $MT_CAPS_OUT/libs/uSockets.a and nothing else.
#
# The engine target is a static library, so it does not link uSockets itself --
# the apps do. Staging it here is what lets an app build without knowing the
# archive's provenance.
# ---------------------------------------------------------------------------
# THE BUILD LOCK, taken here because THIS script is what writes inside the
# checkout: `make` runs in the vendored uSockets source tree below, which all
# four apps share. (The staged result no longer lands in the checkout -- it goes
# to $MT_CAPS_OUT/libs -- but the source-tree `make` still needs it.) Running this
# script directly is documented usage, so it cannot rely on an app's wrapper
# having taken the lock.
#
# When an app's build-macos.sh IS the caller, it already holds the lock and this
# passes straight through -- the passthrough is keyed on process ancestry, and
# this script is that script's child.
#
# Not taken for --print-settings: that path exits before any of this and is a
# pure query. It is also called from inside an app's build, where blocking on a
# lock to answer a question would be a deadlock waiting to happen.
MT_LOCK="$SCRIPT_DIR/tools/mtcaps/build-lock.sh"
if [[ -x "$MT_LOCK" ]]; then
    export MT_BUILD_LOCK_PID=$$
    MT_BUILD_LOCK_KIND=engine "$MT_LOCK" acquire "${APP_NAME:-MTEngineSDL}" || exit 1
    trap '"$MT_LOCK" release "${APP_NAME:-MTEngineSDL}" >/dev/null 2>&1 || true' EXIT
fi

echo "Building uSockets"
( cd "$USOCKETS_DIR" && make -j"$(sysctl -n hw.ncpu)" )
# OUTSIDE the checkout. This `cp` and the `make` above it were the last two
# writes an app's build made inside the engine repository, and the reason the
# build lock had to cover the whole dependency step rather than one file.
. "$SCRIPT_DIR/platform/caps-lib.sh"
LIBS_DIR="$(mt_caps_lib_dir)"
mkdir -p "$LIBS_DIR"
cp -f "$USOCKETS_DIR/uSockets.a" "$LIBS_DIR/"

if [[ "$DEPS_ONLY" == "true" ]]; then
    echo ""
    echo "Dependencies staged. Skipping the engine build (--deps-only)."
    exit 0
fi

# ---------------------------------------------------------------------------
# Step 4: build MTEngineSDL
#
# ALWAYS BUILD VIA -scheme, NEVER -target or a bare -project.
#
# The target sets ONLY_ACTIVE_ARCH=YES with ARCHS=$(ARCHS_STANDARD). With
# `-scheme`, xcodebuild resolves an active architecture and builds one slice.
# Without it there is no active architecture, so ONLY_ACTIVE_ARCH degrades to
# "build everything in ARCHS" and the build takes twice as long -- and any
# consumer expecting a single-slice archive gets a surprise.
#
# `-scheme` is also the only invocation form that runs scheme pre-actions,
# which matters for anything wired into the scheme later.
# ---------------------------------------------------------------------------
XCODE_ARGS=(
    -project "$XCODE_PROJECT"
    -scheme MTEngineSDL
    -configuration "$CONFIGURATION"
)
[[ -n "$ARCH" ]]         && XCODE_ARGS+=(-arch "$ARCH")
[[ -n "$DERIVED_DATA" ]] && XCODE_ARGS+=(-derivedDataPath "$DERIVED_DATA")

# The capability settings ride every invocation, including -showBuildSettings
# below -- otherwise the product path would be resolved under a different
# configuration than the one that was built.
if [[ ${#MTCAPS_SETTINGS[@]} -gt 0 ]]; then
    XCODE_ARGS+=("${MTCAPS_SETTINGS[@]}")
fi

if [[ "$CLEAN" == "true" ]]; then
    echo "Cleaning MTEngineSDL ($CONFIGURATION)"
    xcodebuild "${XCODE_ARGS[@]}" clean
fi

echo "Building MTEngineSDL ($CONFIGURATION${ARCH:+, $ARCH}) via xcodebuild..."

# ${MTENGINE_XCODE_ARGS:-}, not a bare expansion. This script runs under
# `set -u`, so a bare ${MTENGINE_XCODE_ARGS} aborts the build with "unbound
# variable" for everyone who did not export it -- which is everyone following
# the documented usage. Making the caller responsible for a variable this
# script invented is the wrong way round.
# shellcheck disable=SC2086
xcodebuild "${XCODE_ARGS[@]}" ${MTENGINE_XCODE_ARGS:-} build

# ---------------------------------------------------------------------------
# Step 5: locate and verify the product
#
# ASK xcodebuild where it put things -- do NOT search DerivedData.
#
# Searching is what the first version of this script did, and it reported the
# wrong file on its very first run. Every app that builds the engine as a
# cross-project dependency leaves its own libMTEngineSDL.a under its own
# <DerivedData>/<app>-*/Build/Products/<Config>/, so a `find` across the shared
# DerivedData root matches several archives and `head -n 1` picks one
# arbitrarily: it printed c64d's copy immediately after building the engine's.
# A plausible path, the wrong file, and no error -- the exact shape of a
# verification step that cannot fail. BUILT_PRODUCTS_DIR is the build's own
# answer and cannot drift from it.
# ---------------------------------------------------------------------------
BUILT_PRODUCTS_DIR=$(xcodebuild "${XCODE_ARGS[@]}" -showBuildSettings 2>/dev/null \
    | awk -F' = ' '/ BUILT_PRODUCTS_DIR = /{gsub(/^[ \t]+|[ \t]+$/, "", $2); print $2; exit}')

if [[ -z "$BUILT_PRODUCTS_DIR" ]]; then
    echo "ERROR: could not resolve BUILT_PRODUCTS_DIR from xcodebuild -showBuildSettings." >&2
    exit 1
fi

ENGINE_LIB="$BUILT_PRODUCTS_DIR/libMTEngineSDL.a"

if [[ ! -f "$ENGINE_LIB" ]]; then
    echo "ERROR: build reported success but the product is missing:" >&2
    echo "       $ENGINE_LIB" >&2
    exit 1
fi

echo ""
echo "MTEngineSDL built successfully."
lipo -info "$ENGINE_LIB" 2>/dev/null || true
echo "$ENGINE_LIB"
