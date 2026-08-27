#!/usr/bin/env bash
#
# Shared helpers for capability-aware acquisition. Sourced by the engine's
# platform/*/build-*.sh scripts and by the two wrappers.
#
# Everything here is about ONE question: what does this build actually need
# fetched and built? Before this existed the answer was "all of it" -- a clean
# clone of c64d pulled 1.25 GB of mbedTLS, llama.cpp and FTXUI it does not want,
# on every platform, on every build.

# ---------------------------------------------------------------------------
# mt_caps_read_flags <generated-fragment.xcconfig or .cmake>
#
# Exports every MT_ENABLE_* and MT_CAP_* from a generated fragment into the
# environment, so an acquisition script can read `$MT_ENABLE_LLAMA_CPP` directly
# rather than re-parsing a blob. Each value is emitted as a STANDALONE assignment
# by the generator precisely so this is possible; folding them into one
# preprocessor blob would make the check its own source of bugs.
# ---------------------------------------------------------------------------
mt_caps_read_flags() {
    local fragment="$1"
    if [[ ! -f "$fragment" ]]; then
        echo "ERROR: mt_caps_read_flags: no fragment at $fragment" >&2
        return 1
    fi

    # The .cmake fragment spells flags `set(NAME 1)`, not `NAME = 1`, so accept
    # BOTH rather than silently reading nothing. Reading nothing is the dangerous
    # outcome here: every consumer defaults to ${MT_ENABLE_X:-1}, so a fragment
    # this function could not parse means "fetch and build everything", with no
    # diagnostic -- the exact opposite of what the caller asked for.
    local normalised="$fragment"
    case "$fragment" in
        *.cmake)
            normalised="$(mktemp -t mtcapsflags.XXXXXX)"
            sed -n 's/^set(\(MT_[A-Z0-9_]*\) \([01]\))$/\1 = \2/p' "$fragment" > "$normalised"
            ;;
    esac

    local line key value found=0
    while IFS= read -r line; do
        case "$line" in
            MT_ENABLE_*|MT_CAP_*|MT_CAMERA_CAPTURE_ENABLED*)
                key="${line%% =*}"
                value="${line##*= }"
                # Only bare 0/1 assignments; skip MT_CAPS_DEFINES and friends,
                # which are lists rather than flags.
                case "$value" in
                    0|1) export "$key=$value"; found=$((found + 1)) ;;
                esac
                ;;
        esac
    done < "$normalised"

    [[ "$normalised" == "$fragment" ]] || rm -f "$normalised"

    if [[ "$found" -eq 0 ]]; then
        echo "ERROR: mt_caps_read_flags: parsed no flags out of $fragment" >&2
        echo "       Every consumer defaults an unset flag to ON, so continuing" >&2
        echo "       would silently fetch and build everything." >&2
        return 1
    fi
}

# ---------------------------------------------------------------------------
# mt_caps_submodules
#
# Prints the submodule paths this capability set actually needs, one per line.
# Reads the exported MT_ENABLE_* flags; with none exported it prints all three,
# which is the standalone case.
#
# THE THREE GATED SUBMODULES, and what they cost on disk:
#   other/lib/mbedtls     542 MB   MT_CAP_HTTPS
#   other/lib/llama.cpp   598 MB   MT_CAP_LLM
#   other/lib/ftxui       140 MB   MT_CAP_FTXUI
# ---------------------------------------------------------------------------
mt_caps_submodules() {
    [[ "${MT_ENABLE_MBEDTLS:-1}"    == "1" ]] && echo "other/lib/mbedtls"
    [[ "${MT_ENABLE_LLAMA_CPP:-1}"  == "1" ]] && echo "other/lib/llama.cpp"
    [[ "${MT_ENABLE_FTXUI:-1}"      == "1" ]] && echo "other/lib/ftxui"
    return 0
}

# ---------------------------------------------------------------------------
# mt_caps_init_submodules <engine-dir>
#
# Initialises ONLY what the resolved set needs. Replaces the blanket
# `git submodule update --init --recursive`, which ran on every build in every
# app script and undid any selective checkout anything else did.
#
# It does NOT deinit what is no longer needed. Removing a 598 MB checkout the
# moment a manifest flips would make an accidental edit expensive to undo, and
# the clean-clone proof is about what a FRESH clone fetches, not about pruning an
# existing one.
# ---------------------------------------------------------------------------
mt_caps_init_submodules() {
    local engine_dir="$1"
    # `=()` on both, and the ${arr[@]+...} form below: bash 3.2 -- which is
    # /bin/bash on macOS -- treats an EMPTY array as UNSET under `set -u`, so a
    # bare "${missing[@]}" aborts the build with "unbound variable" in exactly
    # the case this function exists to serve: a manifest that wants none of the
    # three gated submodules.
    local -a needed=()
    local -a missing=()
    local sm
    while IFS= read -r sm; do
        [[ -n "$sm" ]] && needed+=("$sm")
    done < <(mt_caps_submodules)

    if [[ ${#needed[@]} -eq 0 ]]; then
        echo "Capabilities: no gated submodule is needed by this build"
        return 0
    fi

    for sm in "${needed[@]}"; do
        if [[ ! -f "$engine_dir/$sm/CMakeLists.txt" ]]; then
            missing+=("$sm")
        fi
    done

    if [[ ${#missing[@]} -eq 0 ]]; then
        return 0
    fi

    echo "Fetching ${#missing[@]} of 3 gated submodules: ${missing[*]}"
    ( cd "$engine_dir" && git submodule update --init --recursive "${missing[@]+"${missing[@]}"}" )
}

# ---------------------------------------------------------------------------
# mt_caps_stub_archive <out.a> <symbol-prefix>
#
# Emits a stub static archive, so a project that links this library
# unconditionally still builds when the capability is off.
#
# This is the generalisation of what build-mbedtls.sh already did. A
# PBXBuildFile cannot be conditioned on a build setting, so skipping an
# acquisition step would fail the LINK on a missing .a; the alternative was
# moving every archive into OTHER_LDFLAGS across four different app-side linking
# idioms. Stubs are cheaper, uniform, and they preserve the symbol proof -- a
# stub carries no library symbols, so `nm` still distinguishes on from off.
# ---------------------------------------------------------------------------
mt_caps_stub_archive() {
    local out_lib="$1"
    local prefix="$2"
    local tmp_dir; tmp_dir="$(dirname "$out_lib")/.${prefix}_stub"

    rm -rf "$tmp_dir"
    mkdir -p "$tmp_dir" "$(dirname "$out_lib")"

    printf 'int %s_disabled_stub = 0;\n' "$prefix" > "$tmp_dir/stub.c"
    cc -c "$tmp_dir/stub.c" -o "$tmp_dir/stub.o"
    rm -f "$out_lib"

    if command -v libtool >/dev/null 2>&1 && [[ "$(uname -s)" == "Darwin" ]]; then
        libtool -static -o "$out_lib" "$tmp_dir/stub.o"
    else
        ar rcs "$out_lib" "$tmp_dir/stub.o"
    fi
    ranlib "$out_lib" 2>/dev/null || true

    rm -rf "$tmp_dir"
    echo "Capability off: emitted stub archive $out_lib"
}

# ---------------------------------------------------------------------------
# mt_caps_lib_dir
#
# Where a dependency archive belongs: OUTSIDE the engine checkout.
#
# THE RULE forbids a build writing inside this repository, and until now the six
# acquisition scripts all ignored it -- every one of them staged its archive into
# platform/MacOS/libs/. That was not merely untidy. Those archives DIFFER PER
# CAPABILITY SET, so two apps with different manifests genuinely fought over one
# libllama_cpp.a, and the whole build lock exists to serialize that fight.
#
# WHY NOT $MT_CAPS_OUT, which is what this first used. That root is keyed by app,
# ENGINE REVISION, platform, arch, config, licence mode, backend and capability
# hash. Every one of those but the last is irrelevant to a third-party archive --
# and engine revision is worse than irrelevant, it changes on every commit and on
# every dirty edit. Keying there does not cost "duplicated disk"; it costs a full
# rebuild of SDL3, FFmpeg, libvpx and llama.cpp per engine commit, per app. That
# is twenty minutes of work to produce a byte-identical archive.
#
# So the key is the RESOLVED CAPABILITY SET, plus the three things that change an
# archive WITHOUT being capabilities: arch (only macOS builds universal
# archives), backend (MT_LLAMA_CUDA / MT_GGML_NATIVE select a different
# llama.cpp under an identical capability set), and config on Windows alone
# (MSVC cannot mix a Debug and a Release CRT). The capability set is the
# discriminator that matters: llama.cpp on vs off is what makes two archives
# differ (the acquisition scripts write a stub archive with a `disabled:` stamp
# when a capability is off), and four apps whose manifests resolve alike share
# one build instead of four.
#
# THE PATH IS BUILT IN ONE PLACE, tools/mtcaps/resolve.py:deps_dir, and emitted
# as `deps_dir=`. This function only reads it. It used to hash the set here too,
# and Windows and Linux were about to add a third and a fourth copy.
#
# Correctness does not rest on the path in any case -- every acquisition script
# stamps its archive with its own full inputs (upstream sha, script sha, versions,
# deployment target, licence mode) and rebuilds on a mismatch. The key only
# decides how often that mismatch happens.
#
# $MT_CAPS_LIBS_DIR wins when set, because the wrapper passes the resolved path
# to xcodebuild and the IDE pre-action writes it into the generated xcconfig; a
# script phase must use the SAME directory the linker was pointed at, not
# recompute one from an environment it may not have inherited.
#
# The fallback is the same one build-llama_cpp.sh has used for the generated
# llama header since that file stopped being tracked: a root under the build
# root, still outside every checkout, for `clone the engine and build it` where
# no manifest exists.
# mt_caps_strip_host_build_env
#
# Remove the HOST build system's exported settings before handing control to a
# vendored third-party build system.
#
# These scripts run as Xcode SCRIPT PHASES, and a script phase inherits every
# build setting as an environment variable -- around 600 of them. `make` imports
# every environment variable as a make variable, so any vendored makefile using
# `?=` silently takes Xcode's value instead of its own.
#
# MEASURED, and this is not hypothetical: libvpx's build/make/Makefile does
#
#     BUILD_ROOT?=.
#     CFLAGS+=-I$(BUILD_PFX)$(BUILD_ROOT) ...
#
# so Xcode's BUILD_ROOT (.../DerivedData/<app>/Build/Products) replaced the
# libvpx build directory in the include path and EVERY `#include "vpx_config.h"`
# failed with "file not found". Bisecting the 613 inherited variables one half at
# a time landed on BUILD_ROOT exactly.
#
# It went unnoticed for as long as it did because the archives used to be staged
# INSIDE the checkout: once built by hand, the stamp was always current and the
# phase always self-skipped. Moving the archives out gave every fresh capability
# set a real build -- and the phase had never actually worked.
#
# The list is the generic-sounding half of Xcode's namespace, the names a
# makefile or configure script might plausibly define for itself. Anything Xcode
# needs downstream (SDKROOT, DEVELOPER_DIR, PATH, and the MT_* settings these
# scripts read) is deliberately kept.
mt_caps_strip_host_build_env() {
    unset BUILD_ROOT BUILD_DIR BUILD_STYLE BUILT_PRODUCTS_DIR \
          OBJROOT SYMROOT DSTROOT SRCROOT SOURCE_ROOT PROJECT_DIR \
          TARGET_BUILD_DIR TARGET_TEMP_DIR TEMP_DIR TEMP_ROOT \
          DERIVED_FILE_DIR DERIVED_SOURCES_DIR CONFIGURATION \
          PLATFORM_NAME PRODUCT_NAME TARGET_NAME TARGETNAME \
          ARCHS VALID_ARCHS CURRENT_ARCH arch variant \
          MAKEFLAGS MFLAGS 2>/dev/null || true
}

mt_caps_lib_dir() {
    # A READER, not a computer. mt_caps_deps_key used to hash the resolved set
    # here, which made two implementations of one key -- this one and
    # resolve.caps_hash -- and two implementations are one edit away from
    # disagreeing. tools/mtcaps owns the path now and emits it as `deps_dir=`;
    # every wrapper reads that line and exports it as MT_CAPS_LIBS_DIR.
    if [[ -n "${MT_CAPS_LIBS_DIR:-}" ]]; then
        echo "$MT_CAPS_LIBS_DIR"
        return
    fi
    # `clone the engine and build it`: no manifest, so no capability set to key
    # by, and nothing has resolved a path to pass in. Still outside every
    # checkout, and under a `standalone` prefix so it can never be mistaken for
    # a keyed bucket. Same shape build-deps.ps1 falls back to on Windows.
    local root="${MTENGINE_BUILD_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/mtengine}"
    local plat; plat="$(uname -s | tr '[:upper:]' '[:lower:]')"
    [[ "$plat" == "darwin" ]] && plat="macos"
    echo "$root/_deps/standalone/$plat/$(uname -m)/common/libs"
}
