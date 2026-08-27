#!/usr/bin/env bash
#
# The Xcode scheme BUILD PRE-ACTION that makes an IDE build capability-correct.
#
#   xcode-preaction.sh <app-repo-dir> <app-name>
#
# WHY THIS EXISTS
#
# Xcode cannot be handed a parameter list, so the script path (build-macos.sh)
# passes the resolved set as `xcodebuild` settings and the IDE path has no
# equivalent. Without this, pressing Build or Archive in Xcode compiled against
# the ENGINE's defaults rather than the app's mtengine.caps -- so an Archive
# could ship libraries the manifest turns off.
#
# HOW IT WORKS, and both halves are measured (claude/spikes/xcconfig/README.md
# in the DummyApp repo):
#
#   * a scheme pre-action's WRITE takes effect in the SAME build. Verified with a
#     file that did not exist when the build started and whose value the build
#     then resolved;
#   * an xcconfig can `#include?` a RELATIVE path, and a missing file degrades
#     silently rather than erroring -- so a developer who has never run this
#     still gets a working build. Relative and not absolute because $(HOME) and
#     build settings do NOT expand inside an include path (measured), and a
#     hardcoded absolute path is both forbidden here and wrong elsewhere.
#
# It writes the settings build-macos.sh passes on the command line, so the IDE
# path and the script path converge: the in-target backstop verifies both
# identically, and a command-line setting still overrides the include when the
# wrapper is driving.
#
# The two sets are close but NOT identical, and the differences are deliberate:
#   * only the pre-action writes MT_CAPS_INCLUDE_DIR, because only the IDE path
#     needs it -- the targets' HEADER_SEARCH_PATHS reference $(MT_CAPS_INCLUDE_DIR)
#     and the wrapper instead passes HEADER_SEARCH_PATHS outright;
#   * only the wrapper passes HEADER_SEARCH_PATHS, for that same reason.
# Anything else appearing in one and not the other is a bug: a setting present
# here but absent from the CLI list leaks a stale value into a wrapper build,
# because the wrapper no longer rewrites this file.
#
# TWO FILES, because the engine target cannot know which app is building it:
#
#   ../.mtengine-ide/<App>/caps.xcconfig      the app's own set, per app
#   ../.mtengine-ide/_current/engine.xcconfig this build's set, for the engine
#
# The engine one is a "last writer wins" file. An earlier version of this comment
# claimed that was safe because "two IDE builds racing is not a scenario a human
# in Xcode produces; two SCRIPT builds racing never touch these files". BOTH
# HALVES WERE FALSE, and measurement is what showed it:
#
#   * `xcodebuild -scheme` runs this pre-action too, so the SCRIPT path writes
#     this file on every build -- it is an aggressor, not a bystander. (It is
#     never a VICTIM: its command-line settings outrank the include.)
#   * so the dangerous pairing is not "two IDE builds" but "any build while an
#     IDE build is in flight", which three agents on three apps produce trivially.
#
# The agreement check below cannot catch the result either: a foreign file
# supplies BOTH the stamp path and the resolved string, so the pair is
# internally consistent and passes (measured, exit 0).
#
# Hence the build lock above, and the OBJROOT-vs-MT_CAPS_APP check in the
# backstop as the net under it. See tools/mtcaps/build-lock.sh.
#
# NOTHING HERE IS WRITTEN INSIDE ANY CHECKOUT.

set -euo pipefail

APP_DIR="${1:?usage: xcode-preaction.sh <app-repo-dir> <app-name>}"
APP_NAME="${2:?usage: xcode-preaction.sh <app-repo-dir> <app-name>}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
MANIFEST="$APP_DIR/mtengine.caps"

# THE BUILD LOCK, taken before anything else this build does.
#
# The engine checkout is shared by four apps and cannot serve two at once -- see
# build-lock.sh's header for the two things that actually collide. Acquiring HERE
# is what makes one choke point serve both channels: MEASURED, `xcodebuild
# -scheme` runs this pre-action too, so the script path queues behind an IDE
# build and vice versa.
#
# It BLOCKS rather than failing: three agents building three apps should queue,
# not error. A lock held by a build that has already died is broken
# automatically, so a crashed or cancelled build cannot wedge the others.
"$SCRIPT_DIR/build-lock.sh" acquire "$APP_NAME" || {
    echo "mtcaps pre-action: could not acquire the build lock; see the note above." >&2
    exit 1
}

# Fill in what the outer acquire could not know. build-macos.sh takes the lock
# BEFORE xcodebuild -- it stages dependencies inside the engine checkout first --
# so OBJROOT does not exist yet and the lock records no build directory. Without
# this the idle probe can never decide for the whole CLI channel, leaving the TTL
# as its only recovery. We are inside xcodebuild now, so OBJROOT is real.
"$SCRIPT_DIR/build-lock.sh" annotate "$APP_NAME" || true

# `.mtengine-ide/` BESIDE the five repos, not under $XDG_CACHE_HOME, and the
# reason is a measurement rather than a preference: $(HOME) and build settings do
# NOT expand inside an xcconfig `#include?` path -- only a literal absolute path
# works there, and hardcoding one is forbidden and wrong for anyone else's
# machine. A RELATIVE include resolves from the including file, so the target has
# to sit at a fixed offset from the tracked xcconfigs. The parent of the five
# checkouts is that place: inside none of them, and requiring only the sibling
# layout the whole programme already assumes.
IDE_ROOT="$(cd "$ENGINE_DIR/.." && pwd)/.mtengine-ide"
APP_XCCONFIG="$IDE_ROOT/$APP_NAME/caps.xcconfig"
ENGINE_XCCONFIG="$IDE_ROOT/_current/engine.xcconfig"

# A pre-action's failure does NOT stop the build -- Xcode reports it and carries
# on. So every failure path leaves the include file ABSENT rather than stale:
# `#include?` then contributes nothing, the target falls back to its defaults,
# and the in-target backstop is what stops the build. Failing loudly here while
# quietly leaving a wrong file behind would be the worst combination.
fail() {
    echo "mtcaps pre-action: $*" >&2
    rm -f "$APP_XCCONFIG"
    # ONLY remove the shared engine file if THIS build is the kind that writes
    # it. A wrapper build does not (see MT_CAPS_WRAPPER below), so deleting it
    # here destroyed the IDE channel's state for all four apps whenever a CLI
    # build hit a missing manifest or a failed resolve -- and because a
    # not-supplied engine build is PERMITTED, the next engine build then
    # compiled against defaults without a word.
    [[ "${MT_CAPS_WRAPPER:-0}" == "1" ]] || rm -f "$ENGINE_XCCONFIG"
    exit 1
}

[[ -f "$MANIFEST" ]] || fail "no manifest at $MANIFEST"

PYTHON3="$(command -v python3 || true)"
[[ -n "$PYTHON3" ]] || fail "python3 not found; install it with: brew install python3"

# Xcode gives the pre-action the scheme's build settings ONLY when the scheme
# entry's "Provide build settings from" names a target. Fall back to something
# sane so a misconfigured scheme still produces a usable set rather than nothing.
ARCH="${CURRENT_ARCH:-${NATIVE_ARCH_ACTUAL:-$(uname -m)}}"
[[ "$ARCH" == "undefined_arch" ]] && ARCH="$(uname -m)"
CONFIG="${CONFIGURATION:-Debug}"

case "$ARCH" in
    arm64|aarch64) ENGINE_OPTION="MT_GGML_NATIVE=OFF" ;;
    *)             ENGINE_OPTION="MT_GGML_NATIVE=ON" ;;
esac

OUTPUT="$("$PYTHON3" -B "$SCRIPT_DIR/mtcaps.py" resolve \
    --manifest "$MANIFEST" --app "$APP_NAME" \
    --platform macos --arch "$ARCH" --config "$CONFIG" \
    --engine-dir "$ENGINE_DIR" --engine-option "$ENGINE_OPTION")" \
    || fail "mtcaps resolve failed for $MANIFEST"

RESOLVED="$(printf '%s\n' "$OUTPUT" | sed -n 's/^resolved=//p')"
MT_OUT="$(printf '%s\n' "$OUTPUT" | sed -n 's/^out_dir=//p')"
[[ -n "$RESOLVED" && -n "$MT_OUT" ]] || fail "mtcaps resolve produced no out_dir/resolved"

# Read from the SAME resolve that produced $MT_OUT above, so the directory the
# linker is pointed at and the directory the script phases stage into cannot
# drift apart. It used to be recomputed here through mt_caps_lib_dir with a
# locally hashed key; tools/mtcaps owns the path now and emits it, which removes
# the second implementation AND guarantees this build's <backend> -- the resolve
# above passes --engine-option, and a recomputation had no way to know that.
LIBS_DIR="$(printf '%s\n' "$OUTPUT" | sed -n 's/^deps_dir=//p')"
[[ -n "$LIBS_DIR" ]] || fail "mtcaps resolve produced no deps_dir"

GEN="$MT_OUT/MTEngineCaps.xcconfig"
[[ -f "$GEN" ]] || fail "generated fragment missing at $GEN"

caps_value() { sed -n "s/^$1 = //p" "$GEN"; }
DEFINES="$(caps_value MT_CAPS_DEFINES)"
DEFINES_ENGINE="$(caps_value MT_CAPS_DEFINES_ENGINE)"

# Every MT_ENABLE_*/MT_CAP_* ALSO as a standalone setting, exactly as
# build-macos.sh passes them on the command line. A script phase sees build
# settings as environment variables, so this is what lets the engine's six
# ACQUISITION phases honour the manifest in an IDE build too -- without it the
# binary was already correct (nothing linked), but llama.cpp, FTXUI and mbedTLS
# were still fetched and built for a manifest that does not want them.
FLAG_SETTINGS="$(grep -E '^(MT_ENABLE_|MT_CAP_|MT_CAMERA_CAPTURE_ENABLED)[A-Z0-9_]* = [01]$' "$GEN" || true)"

write_xcconfig() {
    local path="$1" which="$2" extra="$3"
    mkdir -p "$(dirname "$path")"
    {
        echo "// generated by mtcaps xcode-preaction.sh -- do not edit, do not commit"
        echo "//"
        echo "// Written at the start of an Xcode build of $APP_NAME, and consumed by a"
        echo "// tracked xcconfig's #include?. This is what makes an IDE Build or Archive"
        echo "// honour $APP_NAME/mtengine.caps instead of the engine's defaults."
        echo "//"
        echo "// $which"
        echo ""
        echo "MT_CAPS_SUPPLIED = YES"
        # WHICH APP wrote this. The engine target has no app identity of its own,
        # so this is the only way its backstop can notice it was handed the
        # WRONG app's file by a concurrent build -- see xcode-backstop.sh.
        echo "MT_CAPS_APP = $APP_NAME"
        echo "MT_CAPS_DEFINES = $DEFINES"
        echo "MT_CAPS_DEFINES_ENGINE = $extra"
        echo "MT_CAPS_INCLUDE_DIR = $MT_OUT/include"
        echo "MT_CAPS_OUT = $MT_OUT"
        # The dependency-archive directory, so the IDE channel links against the
        # SAME archives the acquisition script phases stage into. It is keyed by
        # the resolved capability set rather than by $MT_OUT -- see
        # mt_caps_lib_dir in platform/caps-lib.sh. Without this line the tracked
        # xcconfig's standalone fallback applies, and an IDE build would search a
        # directory no phase in that build ever wrote to.
        echo "MT_CAPS_LIBS_DIR = $LIBS_DIR"
        echo "MT_CAPS_RESOLVED = $RESOLVED"
        echo ""
        echo "// Each flag standalone, so the acquisition script phases can read one"
        echo "// directly rather than re-parsing a define blob in six shell scripts."
        printf '%s\n' "$FLAG_SETTINGS"
    } > "$path"
}

# The app gets the app-visible set only; the engine additionally gets the flags
# no app guards on. That asymmetry is the per-target split -- the same one the
# command-line path makes, for the same reason.
write_xcconfig "$APP_XCCONFIG"    "For the APP target." ""

# THE ENGINE FILE IS THE ONLY SHARED ONE, so a build that does not need it does
# not write it.
#
# build-macos.sh passes MT_CAPS_WRAPPER=1 (measured: a command-line build setting
# does reach a pre-action). A wrapper build supplies the whole resolved set on
# the xcodebuild command line, and a command-line setting outranks the tracked
# xcconfig's `#include?` -- so the wrapper never READS this file. Writing it
# anyway made every CLI build an aggressor against any concurrent IDE build, for
# no benefit to itself: two agents building two apps could silently hand a third
# app's Xcode build the wrong engine capability set.
#
# This is defence in depth, NOT a replacement for the build lock. The lock still
# has to exist for a collision this cannot touch: the engine's six dependency
# phases configure and compile under other/lib/*/build INSIDE the checkout,
# shared by all four apps. Their finished archives now land outside it, keyed by
# capability set, but the intermediate trees that produce them do not.
if [[ "${MT_CAPS_WRAPPER:-0}" == "1" ]]; then
    echo "mtcaps pre-action: wrapper build -- not writing the shared engine xcconfig"
else
    write_xcconfig "$ENGINE_XCCONFIG" "For the ENGINE target, for THIS build." "$DEFINES_ENGINE"
fi

echo "mtcaps pre-action: $APP_NAME -> $MT_OUT"
