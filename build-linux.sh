#!/usr/bin/env bash
set -euo pipefail

# Master build script for MTEngineSDL on Linux.
# Builds all required libraries (mbedtls, uSockets, llama.cpp, FTXUI)
# and then compiles MTEngineSDL static library via CMake.
#
# Usage:
#   ./build-linux.sh              # Build everything (standalone: engine defaults)
#   ./build-linux.sh --deps-only  # Build dependencies only (no CMake)
#
# CAPABILITIES
#   ./build-linux.sh --manifest ../MyApp/mtengine.caps --app MyApp
#
#   With --manifest and --app this script runs tools/mtcaps, writes the generated
#   fragments to $MT_OUT -- OUTSIDE every checkout -- and configures the engine
#   against them. Without them it builds STANDALONE: the engine's own option()
#   defaults, which is what "clone the engine and build it" must keep doing.
#
#   ./build-linux.sh --manifest M --app A --print-out-dir
#       resolve only, and print $MT_OUT. An app's own build-linux.sh uses this to
#       include the same fragment in its OWN cmake configure -- necessary,
#       because the engine is configured by a SEPARATE cmake process that runs
#       first, so a fragment included only by the app would set the app's defines
#       and reach the engine's compile line not at all.
#
#   ./build-linux.sh --manifest M --app A --print-deps-dir
#       resolve only, and print the dependency-archive directory. An app asks
#       THIS rather than resolving for itself, and the reason is <backend>: the
#       wrapper decides MT_GGML_NATIVE from `uname -m` a hundred lines below, and
#       that value is a component of the path. An app re-resolving without it
#       gets backend=default while the engine populated backend=<hash>, links an
#       empty directory, and fails with a wall of undefined llama symbols that
#       looks like a broken project. The engine owns the backend choice; the app
#       asks what it chose.
#
#   --build-dir DIR
#       Where the engine's own CMake tree goes. Defaults OUTSIDE this checkout.
#       It used to be a fixed $SCRIPT_DIR/build shared by all four apps, so two
#       apps building concurrently against one engine checkout collided in it
#       whatever else was parameterised.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_ONLY=false
MANIFEST=""
APP_NAME=""
PRINT_OUT_DIR=false
PRINT_DEPS_DIR=false
BUILD_DIR=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --deps-only)     DEPS_ONLY=true ;;
    --manifest)      MANIFEST="${2:-}"; [[ -n "$MANIFEST" ]] || { echo "ERROR: --manifest needs a value" >&2; exit 2; }; shift ;;
    --app)           APP_NAME="${2:-}"; [[ -n "$APP_NAME" ]] || { echo "ERROR: --app needs a value" >&2; exit 2; }; shift ;;
    --build-dir)     BUILD_DIR="${2:-}"; [[ -n "$BUILD_DIR" ]] || { echo "ERROR: --build-dir needs a value" >&2; exit 2; }; shift ;;
    --print-out-dir) PRINT_OUT_DIR=true ;;
    --print-deps-dir) PRINT_DEPS_DIR=true ;;
    *) echo "Unknown argument: $1" >&2; exit 2 ;;
  esac
  shift
done

# ---------------------------------------------------------------------------
# Resolve the capability set. Same contract as build-macos.sh.
# ---------------------------------------------------------------------------
MT_OUT=""
MT_CAPS_ARGS=()

if [[ -n "$MANIFEST" || -n "$APP_NAME" ]]; then
  if [[ -z "$MANIFEST" || -z "$APP_NAME" ]]; then
    echo "ERROR: --manifest and --app go together; omit BOTH for a standalone build." >&2
    exit 2
  fi
  [[ -f "$MANIFEST" ]] || { echo "ERROR: manifest not found: $MANIFEST" >&2; exit 2; }

  PYTHON3="$(command -v python3 || true)"
  if [[ -z "$PYTHON3" ]]; then
    echo "ERROR: python3 not found, and tools/mtcaps needs it." >&2
    echo "       Install it with: sudo apt-get install -y python3" >&2
    exit 2
  fi

  # <backend> is the WRAPPER's to resolve, not the app's: MT_GGML_NATIVE's
  # effective value is decided from `uname -m` a few lines below, and mtcaps runs
  # before that. An app passing it would replicate the engine's ARM rule in four
  # places, which is the private-copy-that-drifts the whole programme exists to
  # remove.
  case "$(uname -m)" in
    aarch64|arm64) MT_ENGINE_OPTION="MT_GGML_NATIVE=OFF" ;;
    *)             MT_ENGINE_OPTION="MT_GGML_NATIVE=ON" ;;
  esac

  if [[ -n "${MTENGINE_PRERESOLVED_OUT:-}" ]]; then
    # PRE-RESOLVED (Phase 3): the app-build driver already ran the ONE mtcaps
    # resolve and hands its outputs down; a second resolve here would re-open
    # the exactly-one-resolve criterion. The driver exports MT_CAPS_LIBS_DIR.
    MT_OUT="$MTENGINE_PRERESOLVED_OUT"
    [[ -f "$MT_OUT/MTEngineCapabilities.cmake" ]] || { echo "ERROR: MTENGINE_PRERESOLVED_OUT has no MTEngineCapabilities.cmake: $MT_OUT" >&2; exit 2; }
    [[ -n "${MT_CAPS_LIBS_DIR:-}" ]] || { echo "ERROR: MTENGINE_PRERESOLVED_OUT requires MT_CAPS_LIBS_DIR in the environment." >&2; exit 2; }
    MTCAPS_OUTPUT="resolved=$(sed -n 's/^set(MT_CAPS_RESOLVED "\(.*\)")$/\1/p' "$MT_OUT/MTEngineCapabilities.cmake")
out_dir=$MT_OUT
deps_dir=$MT_CAPS_LIBS_DIR"
  else
  MTCAPS_OUTPUT="$("$PYTHON3" -B "$SCRIPT_DIR/tools/mtcaps/mtcaps.py" resolve \
      --manifest "$MANIFEST" --app "$APP_NAME" \
      --platform linux --arch "$(uname -m)" --config Release \
      --engine-dir "$SCRIPT_DIR" --engine-option "$MT_ENGINE_OPTION")" || {
    echo "ERROR: mtcaps resolve failed for $MANIFEST" >&2
    exit 2
  }
  fi
  MT_OUT="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^out_dir=//p')"
  [[ -n "$MT_OUT" ]] || { echo "ERROR: mtcaps resolve produced no out_dir" >&2; exit 2; }

  # From the SAME resolve, so the fragments and the archives cannot land under
  # different <backend>s. See resolve.deps_dir for what keys it and why it is
  # deliberately not $MT_OUT.
  MT_CAPS_LIBS_DIR="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^deps_dir=//p')"
  [[ -n "$MT_CAPS_LIBS_DIR" ]] || { echo "ERROR: mtcaps resolve produced no deps_dir" >&2; exit 2; }
  export MT_CAPS_LIBS_DIR

  if [[ "$PRINT_OUT_DIR" == "true" ]]; then
    printf '%s\n' "$MT_OUT"
    exit 0
  fi

  if [[ "$PRINT_DEPS_DIR" == "true" ]]; then
    printf '%s\n' "$MT_CAPS_LIBS_DIR"
    exit 0
  fi

  MT_CAPS_ARGS=(-DMT_CAPS_FRAGMENT="$MT_OUT/MTEngineCapabilities.cmake")

  # Export the flags so the acquisition scripts can read one directly.
  # mt_caps_read_flags understands the .cmake fragment's `set(NAME 0)` spelling
  # itself now; the sed translation that used to live here was a second copy of
  # that knowledge, and the function silently read nothing without it.
  # shellcheck source=platform/caps-lib.sh
  source "$SCRIPT_DIR/platform/caps-lib.sh"
  mt_caps_read_flags "$MT_OUT/MTEngineCapabilities.cmake"

  # The licence mode reaches the acquisition scripts as a PARAMETER -- one
  # switch, one channel -- rather than each script reading a file of its own.
  MT_CAPS_RESOLVED_LINE="$(printf '%s\n' "$MTCAPS_OUTPUT" | sed -n 's/^resolved=//p')"
  case "$MT_CAPS_RESOLVED_LINE" in
    *MT_COMMERCIAL_BUILD=1*) export COMMERCIAL=1 ;;
    *)                       export COMMERCIAL=0 ;;
  esac

  echo -e "\e[94mCapabilities: $APP_NAME -> $MT_OUT\e[0m"
elif [[ "$PRINT_OUT_DIR" == "true" ]]; then
  echo "ERROR: --print-out-dir needs --manifest and --app." >&2
  exit 2
elif [[ "$PRINT_DEPS_DIR" == "true" ]]; then
  echo "ERROR: --print-deps-dir needs --manifest and --app." >&2
  exit 2
fi

# THE DEPENDENCY DIRECTORY, and it is set on BOTH paths.
#
# A bare `./build-linux.sh` with no manifest is a supported build -- "clone the
# engine and build it" -- so setting this only in the branch above would turn a
# working build into an error the moment the six acquisition scripts started
# reading it. With no capability set there is nothing to key by, and
# mt_caps_lib_dir's own fallback is the answer: still outside every checkout,
# under a `standalone` prefix that cannot be mistaken for a keyed bucket.
if [[ -z "${MT_CAPS_LIBS_DIR:-}" ]]; then
  if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
    # shellcheck source=platform/caps-lib.sh
    source "$SCRIPT_DIR/platform/caps-lib.sh"
  fi
  MT_CAPS_LIBS_DIR="$(mt_caps_lib_dir)"
fi

# THE RULE is not a convention a later refactor gets to drop quietly. An empty or
# relative value here would silently recreate platform/Linux/libs, which is the
# whole thing this replaces.
case "$MT_CAPS_LIBS_DIR" in
  "$SCRIPT_DIR"/*) echo "ERROR: dependency dir is inside the engine checkout: $MT_CAPS_LIBS_DIR" >&2; exit 2 ;;
  /*) ;;
  *)  echo "ERROR: dependency dir is not absolute: $MT_CAPS_LIBS_DIR" >&2; exit 2 ;;
esac

export MT_CAPS_LIBS_DIR
mkdir -p "$MT_CAPS_LIBS_DIR"
echo -e "\e[94mDependencies: $MT_CAPS_LIBS_DIR\e[0m"

# The engine's own CMake tree. Outside the checkout by default: it used to be a
# fixed $SCRIPT_DIR/build that all four apps shared, which is both a write inside
# the engine checkout and the collision that makes two concurrent app builds
# unsafe however well the archives are keyed.
if [[ -z "$BUILD_DIR" ]]; then
  if [[ -n "$MT_OUT" ]]; then
    # L9 (2026-09-01): the engine's compiled tree lives under the REV-FREE
    # build dir -- a new engine rev must not force a from-scratch rebuild;
    # CMake's own dependency scan owns incremental correctness, exactly as
    # in any in-checkout build. The path comes from the fragment of the
    # SAME resolve.
    MT_CAPS_BUILD_DIR="$(sed -n 's/^set(MT_CAPS_BUILD_DIR "\(.*\)")$/\1/p' "$MT_OUT/MTEngineCapabilities.cmake")"
    BUILD_DIR="${MT_CAPS_BUILD_DIR:-$MT_OUT}/engine-build"
  else
    BUILD_DIR="${MTENGINE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/mtengine}/_standalone/linux/$(uname -m)/Release/engine-build"
  fi
fi

# STANDALONE ONLY: llama_cpp_version.h. CGuiViewLlamaModelLoader.cpp includes it
# unconditionally, and a manifest build gets it for free -- mtcaps' emit_all
# (invoked by the `resolve` call above) writes the placeholder into
# $MT_OUT/include. A bare `./build-linux.sh` never calls mtcaps at all, so
# nothing has ever put the placeholder where CMakeLists.txt's own
# MT_STANDALONE_INCLUDE branch (:62-69) looks for it -- "clone the engine and
# build it" has never actually worked, just never in a path anyone exercised.
if [[ -z "$MT_OUT" ]]; then
  PYTHON3="$(command -v python3 || true)"
  if [[ -z "$PYTHON3" ]]; then
    echo "ERROR: python3 not found, and it is needed to write the llama_cpp_version.h placeholder for a standalone build." >&2
    exit 2
  fi
  MT_STANDALONE_INCLUDE="${MTENGINE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/mtengine}/_standalone/include"
  "$PYTHON3" -B -c "
import sys
sys.path.insert(0, '$SCRIPT_DIR/tools/mtcaps')
from emit import emit_llama_version_placeholder
emit_llama_version_placeholder('$MT_STANDALONE_INCLUDE')
"
fi

# SELECTIVE submodule acquisition, not blanket. This used to fetch all three
# gated submodules -- 1.25 GB -- whenever any one was missing. Now it fetches
# only what the resolved set asks for; for c64d that is none of them.
#
# With no manifest (a standalone build) every flag defaults to 1 and this
# behaves exactly as before.
if ! declare -f mt_caps_init_submodules >/dev/null 2>&1; then
  # shellcheck source=platform/caps-lib.sh
  source "$SCRIPT_DIR/platform/caps-lib.sh"
fi
mt_caps_init_submodules "$SCRIPT_DIR"

echo -e "\e[94m=== Building MTEngineSDL dependencies ===\e[0m"

# 0. SDL3
echo -e "\n\e[94mBuilding \e[31mSDL3\e[0m"
# SDL3 FIRST: everything else that links the engine needs it, and CMake now
# FATAL_ERRORs if the vendored archive is missing rather than silently falling
# back to a distro libsdl2 (which is the violation this replaces -- see
# platform/Linux/build-sdl3.sh).
bash "$SCRIPT_DIR/platform/Linux/build-sdl3.sh"

# 1. mbedTLS
echo -e "\n\e[94mBuilding \e[31mmbedTLS\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-mbedtls.sh"

# 2. uSockets
echo -e "\n\e[94mBuilding \e[31muSockets\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-usockets.sh"

# 3. Image codecs (TIFF, WebP, AVIF, LibRaw — static, built from source)
echo -e "\n\e[94mBuilding \e[31mImage Codecs (TIFF, WebP, AVIF, LibRaw)\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-image_codecs.sh"

# 4. Video codecs (FFmpeg shared decode libs + vpx/opus static, built from
#    source). Produces other/lib/video-codecs/install-linux-$(uname -m)/
#    (FFmpeg .so + headers, consumed by CMakeLists.txt's MT_ENABLE_FFMPEG
#    dir-exists auto-default) and platform/Linux/libs/libmt_video_codecs.a
#    (vpx+opus, linked unconditionally whenever it exists).
echo -e "\n\e[94mBuilding \e[31mVideo Codecs (FFmpeg, vpx, opus)\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-video_codecs.sh"

# 5. FTXUI (built standalone; also built by CMake if MT_ENABLE_FTXUI=ON,
#    but pre-building ensures the lib is available for non-CMake workflows)
echo -e "\n\e[94mBuilding \e[31mFTXUI\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-ftxui.sh"

# 6. llama.cpp is built by CMake add_subdirectory, no separate script needed

if [[ "$DEPS_ONLY" == "true" ]]; then
  echo -e "\n\e[1;92mAll dependencies built. Skipping CMake (--deps-only).\e[0m"
  exit 0
fi

# 7. Build MTEngineSDL via CMake
echo -e "\n\e[94mBuilding \e[31mMTEngineSDL\e[0m"
mkdir -p "$BUILD_DIR"
mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$SCRIPT_DIR"
cd "$BUILD_DIR"
echo -e "\e[94mEngine build dir: $BUILD_DIR\e[0m"

# ggml/llama.cpp: on ARM the -mcpu=native dotprod auto-detection can emit `sdot`
# instructions the assembler rejects ("selected processor does not support sdot").
# Disable native CPU optimizations on ARM (baseline armv8-a); keep native on x86_64.
GGML_ARCH_ARGS=""
case "$(uname -m)" in
	aarch64|arm64)
		echo -e "\e[94mARM detected - building ggml without native CPU optimizations (MT_GGML_NATIVE=OFF)\e[0m"
		GGML_ARCH_ARGS="-DMT_GGML_NATIVE=OFF"
		;;
esac

# ${CMAKE_EXTRA_ARGS:-}, not ${CMAKE_EXTRA_ARGS}. This script runs under
# `set -u`, so a bare expansion ABORTS THE BUILD with "unbound variable" for
# anyone who did not export the variable first. the photo app's build-linux.sh
# exports it defensively and says so in a comment; the game app's does not, and
# neither does running this script directly -- which is its documented usage.
# Making the caller responsible for a variable this script invented is the wrong
# way round.
# -DMT_CAPS_LIBS_DIR is passed UNCONDITIONALLY, outside MT_CAPS_ARGS: that array
# is empty on a standalone build, and CMake needs the directory on both paths --
# the archives it links exist either way, only the key differs.
cmake "$SCRIPT_DIR" ${GGML_ARCH_ARGS} -DMT_CAPS_LIBS_DIR="$MT_CAPS_LIBS_DIR" \
      "${MT_CAPS_ARGS[@]+"${MT_CAPS_ARGS[@]}"}" ${CMAKE_EXTRA_ARGS:-}
make -j"$(nproc)" MTEngineSDL

# `cmake "$SCRIPT_DIR"`, not `cmake ../`: the build directory is no longer a
# child of the source directory, so a relative source path no longer resolves.
# And "${MT_CAPS_ARGS[@]+...}" rather than a bare "${MT_CAPS_ARGS[@]}": under
# `set -u`, bash 4.2 and older treat an empty array as unset and abort.

echo -e "\n\e[1;92mMTEngineSDL built successfully.\e[0m"
