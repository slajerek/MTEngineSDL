#!/usr/bin/env bash
#
# The in-target capability backstop, run as an Xcode build phase.
#
#   xcode-backstop.sh engine     # on the MTEngineSDL target
#   xcode-backstop.sh app        # on an app target
#
# WHY A BUILD PHASE AND NOT ONLY A SCHEME PRE-ACTION.
#
# `xcodebuild -target` (and a user-scoped scheme, and IDE Build) never runs a
# scheme pre-action -- measured, and it is the trap: `-target` is a perfectly
# ordinary invocation and it degrades to "wrong capability set, build succeeds".
# A phase on the target itself is the one thing every invocation reaches.
#
# WHAT IT CANNOT DO, stated because an earlier design assumed it could.
#
# It cannot compare against "the manifest on disk". In every case this backstop
# exists for there is no command-line setting, so the engine target has no app
# identity and no $MT_OUT -- it has no way to know WHICH of four mtengine.caps it
# should read. Nothing crosses from an app target to the engine target except an
# xcodebuild setting.
#
# So it tests a path the tracked xcconfig defaults to "not supplied": defined
# means the wrapper set it and freshness can be checked against the stamp;
# undefined means this invocation bypassed the wrapper.
#
# ENGINE AND APP TARGETS DIFFER, DELIBERATELY.
#
#   engine + not supplied  -> STANDALONE, and permitted. MTEngineSDL ships on
#                             public GitHub and "clone it, open it, build it" is
#                             the first thing an external contributor does.
#   app    + not supplied  -> FAILS. An app built without its manifest gets the
#                             ENGINE's defaults, not its own set: a silently
#                             wrong binary, which is the whole failure mode here.

set -euo pipefail

MODE="${1:-engine}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDE_SHARED_FILE="$(cd "$SCRIPT_DIR/../../.." && pwd)/.mtengine-ide/_current/engine.xcconfig"

if [[ "${MT_CAPS_SUPPLIED:-NO}" != "YES" ]]; then
    if [[ "$MODE" == "engine" ]]; then
        echo "note: MTEngineSDL standalone build -- engine defaults, no manifest."
        exit 0
    fi
    # ---------------------------------------------------------------------
    # THE IDE PATH.
    #
    # Xcode cannot be handed a parameter list, so an IDE build has no resolved
    # set and compiles with the tracked xcconfigs' fallback defaults -- which are
    # the engine's own, i.e. everything on.
    #
    # An earlier form of this simply FAILED here, which made Xcode unusable for
    # an app: not just after editing a manifest, but always. That is a worse
    # trade than it looked. Build-and-debug in the IDE is an ordinary workflow,
    # and refusing it outright buys nothing when the defaults happen to BE what
    # the manifest asks for.
    #
    # So decide it by comparing, rather than by assuming: resolve the app's
    # manifest and resolve an EMPTY one (the pure defaults). Equal means this IDE
    # build is exactly what the manifest wants and there is nothing to warn
    # about. Different means the binary would not be what the manifest asks for,
    # and that still fails -- with the differing keys named.
    # ---------------------------------------------------------------------
    MANIFEST="${MT_CAPS_MANIFEST:-$PROJECT_DIR/../../mtengine.caps}"
    if [[ ! -f "$MANIFEST" ]]; then
        echo "error: this app was built without a capability set, and no manifest" >&2
        echo "       was found at $MANIFEST to check the defaults against." >&2
        echo "note: build via the app's ./build-macos.sh." >&2
        exit 1
    fi

    PYTHON3="$(command -v python3 || true)"
    if [[ -z "$PYTHON3" ]]; then
        echo "error: python3 not found, and the capability check needs it." >&2
        exit 1
    fi

    ARCH="${CURRENT_ARCH:-$(uname -m)}"
    [[ "$ARCH" == "undefined_arch" ]] && ARCH="$(uname -m)"
    CFG="${CONFIGURATION:-Debug}"
    ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"

    resolve_of() {
        "$PYTHON3" -B "$SCRIPT_DIR/mtcaps.py" resolve \
            --manifest "$1" --app "${PRODUCT_NAME:-ide}" \
            --platform macos --arch "$ARCH" --config "$CFG" \
            --engine-dir "$ENGINE_DIR" \
            --out-dir "${TMPDIR:-/tmp}/mtcaps-ide-check" 2>/dev/null \
            | sed -n 's/^resolved=//p'
    }

    EMPTY_MANIFEST="$(mktemp -t mtcaps-empty.XXXXXX)"
    printf '# empty: the pure vocabulary defaults\n' > "$EMPTY_MANIFEST"

    WANTED="$(resolve_of "$MANIFEST")"
    DEFAULTS="$(resolve_of "$EMPTY_MANIFEST")"
    rm -f "$EMPTY_MANIFEST"

    if [[ -z "$WANTED" || -z "$DEFAULTS" ]]; then
        echo "error: could not resolve $MANIFEST to check this IDE build." >&2
        echo "note: build via the app's ./build-macos.sh." >&2
        exit 1
    fi

    if [[ "$WANTED" == "$DEFAULTS" ]]; then
        echo "note: IDE build -- no capability set was passed, but this app's"
        echo "      manifest resolves to exactly the defaults, so the binary"
        echo "      matches mtengine.caps. Release builds still go through"
        echo "      ./build-macos.sh."
        exit 0
    fi

    echo "error: this IDE build would NOT match $MANIFEST." >&2
    echo "note: Xcode cannot be handed a capability set, so it compiles with the" >&2
    echo "      engine's defaults. These keys differ:" >&2
    ( IFS=';'; for kv in $WANTED; do
        case ";$DEFAULTS;" in *";$kv;"*) ;; *) echo "        manifest wants $kv" >&2 ;; esac
      done )
    echo "note: build via the app's ./build-macos.sh, which runs tools/mtcaps and" >&2
    echo "      passes the resolved set to xcodebuild." >&2
    echo "note: to accept the defaults for a debug session anyway, set" >&2
    echo "      MT_CAPS_IDE_OK=1 in the scheme's environment." >&2
    [[ "${MT_CAPS_IDE_OK:-0}" == "1" ]] && {
        echo "note: MT_CAPS_IDE_OK=1 -- continuing with the defaults." >&2
        exit 0
    }
    exit 1
fi

# ---------------------------------------------------------------------------
# CROSS-CONTAMINATION CHECK -- did this target get ANOTHER app's capability set?
#
# The engine target reads .mtengine-ide/_current/engine.xcconfig, which is ONE
# file for four apps because an xcconfig `#include?` path cannot be made
# per-build (Xcode resolves includes BEFORE interpreting any build setting, so
# no variable of any spelling expands there -- nine probes, literal control,
# all negative). The build lock is what normally prevents two apps overlapping;
# this is what catches it if the lock was bypassed or broken.
#
# The agreement check below CANNOT catch this. It compares MT_CAPS_RESOLVED
# against MT_CAPS_OUT's stamp, and a foreign file supplies BOTH -- so the pair
# is internally consistent and passes. MEASURED: fed a real LightHeroes pair, it
# exits 0.
#
# What does differ is where the products land: a cross-project engine target
# builds into the DEPENDING APP's DerivedData, whose directory is named after
# that app. So OBJROOT names the app that is really building, and MT_CAPS_APP
# names the app whose file we were handed. They must agree.
#
# ADVISORY WHERE IT CANNOT DECIDE. A custom DerivedData location (per-workspace
# "Build" directory) breaks the naming assumption, and an older _current file
# has no MT_CAPS_APP at all -- both warn rather than fail, because a check that
# cannot tell right from wrong must not stop a correct build.
# ---------------------------------------------------------------------------
if [[ -z "${MT_CAPS_APP:-}" ]]; then
    # Only reachable from a shared file written before MT_CAPS_APP existed, or a
    # channel that does not pass it. Silence here reads as "checked and fine".
    echo "note: no MT_CAPS_APP in this build's settings, so the cross-app capability" >&2
    echo "      check was skipped. Re-run the app's build-macos.sh to refresh it." >&2
elif [[ -z "${OBJROOT:-}" ]]; then
    echo "note: OBJROOT is unset, so the cross-app capability check was skipped." >&2
fi

if [[ -n "${MT_CAPS_APP:-}" && -n "${OBJROOT:-}" ]]; then
    case "$OBJROOT" in
        */DerivedData/*)
            DD_NAME="${OBJROOT##*/DerivedData/}"
            DD_NAME="${DD_NAME%%/*}"

            # THE COMPONENT MUST LOOK LIKE <App>-<hash>, or this cannot read an
            # app name out of it and must say so instead of guessing.
            #
            # `-derivedDataPath .../build/DerivedData` -- an ordinary CI spelling,
            # and c64d's build script already uses a custom path -- gives
            # OBJROOT=.../DerivedData/Build/Intermediates.noindex, so DD_NAME is
            # "Build" and the old code read DD_APP="Build". A correct, solitary
            # build was then hard-failed and told to wait for a concurrent build
            # that did not exist. Both this file's comment below and
            # docs/build-lock.md promised this case WARNS; it did not.
            case "$DD_NAME" in
                *-*[a-z0-9][a-z0-9][a-z0-9][a-z0-9][a-z0-9][a-z0-9][a-z0-9][a-z0-9]) ;;
                *)  echo "note: '$DD_NAME' does not look like Xcode's <App>-<hash> build" >&2
                    echo "      directory, so the cross-app capability check was skipped." >&2
                    DD_NAME="" ;;
            esac
            DD_APP="${DD_NAME%-*}"

            # A STANDALONE ENGINE BUILD IS NOT A CROSS-APP COLLISION.
            #
            # When MTEngineSDL.xcodeproj is built directly, the products land in
            # DerivedData/MTEngineSDL-<hash>, so DD_APP is this project's own
            # name and can never equal an APP name. Without this branch the
            # check fired on every standalone build and reported a concurrent
            # build that was not happening -- breaking "clone the engine, open
            # it, build it", which this file's own header calls a requirement.
            #
            # Reaching here at all still means something is wrong, just not
            # that: MT_CAPS_APP can only have come from the shared IDE file,
            # which a standalone build has no business reading. It is left over
            # from an app's IDE build that did not clean up (a FAILED one --
            # post-actions do not run then), and the engine has just been
            # compiled with that app's capability set instead of its own
            # defaults. So this fails too, but says the true thing and gives the
            # one-line fix.
            # AGREEMENT FIRST. When DD_APP and MT_CAPS_APP match there is nothing
            # to report, and that is the ordinary case for BOTH targets: the app
            # target's own PROJECT_NAME equals its DerivedData name, so testing
            # PROJECT_NAME before agreement made every app build look like a
            # standalone engine build with a foreign set.
            if [[ -n "$DD_APP" && "$DD_APP" == "$MT_CAPS_APP" ]]; then
                : # this build and this capability set belong to the same app
            elif [[ -z "${PROJECT_NAME:-}" ]]; then
                # Without PROJECT_NAME the standalone case cannot be told from a
                # real collision, and calling a standalone build a collision is
                # the mistake this whole branch exists to undo. Warn, do not fail.
                echo "note: PROJECT_NAME is unset, so a standalone engine build cannot be" >&2
                echo "      distinguished from a cross-app one. Capability set belongs to" >&2
                echo "      '$MT_CAPS_APP'; this build is producing into '$DD_APP'." >&2
            elif [[ "$DD_APP" == "$PROJECT_NAME" ]]; then
                echo "error: a STANDALONE $PROJECT_NAME build picked up '$MT_CAPS_APP''s capability set." >&2
                echo "" >&2
                echo "note: it came from the shared IDE file, left behind by an app's Xcode build" >&2
                echo "      that did not release it (a failed build never runs its post-action)." >&2
                echo "      A standalone engine build should compile with the ENGINE's defaults." >&2
                echo "" >&2
                echo "note: remove it and build again:" >&2
                echo "      rm -f $IDE_SHARED_FILE" >&2
                exit 1
            elif [[ -n "$DD_APP" ]]; then
                echo "error: this build was handed ANOTHER app's capability set." >&2
                echo "       building into : $DD_APP (from OBJROOT)" >&2
                echo "       set belongs to: $MT_CAPS_APP (from MT_CAPS_APP)" >&2
                echo "" >&2
                echo "note: the engine target reads ONE shared file for all four apps, so this" >&2
                echo "      build was configured from a set another app's build wrote there." >&2
                echo "      Usually that build is still running -- the lock status below says" >&2
                echo "      whether it is. If the lock is free, the file was simply left behind" >&2
                echo "      by an earlier build that did not clean up." >&2
                echo "" >&2
                echo "note: THIS BUILD CANNOT BE SALVAGED -- its engine translation units were" >&2
                echo "      already configured from the wrong file. Wait for the other build to" >&2
                echo "      finish and START THIS ONE AGAIN; a rebuild is enough, nothing is" >&2
                echo "      permanently broken." >&2
                echo "" >&2
                echo "note: who holds the build lock right now:" >&2
                "$SCRIPT_DIR/build-lock.sh" status >&2 || true
                echo "" >&2
                echo "note: builds normally QUEUE on that lock instead of colliding. Seeing this" >&2
                echo "      means the lock was bypassed -- a build started with the scheme's" >&2
                echo "      pre-action disabled, or an xcodebuild -target invocation, which" >&2
                echo "      MEASURED does not run pre-actions at all." >&2
                exit 1
            fi
            ;;
        *)
            echo "note: OBJROOT is not under DerivedData, so the cross-app check was skipped." >&2
            ;;
    esac
fi

if [[ -z "${MT_CAPS_OUT:-}" || -z "${MT_CAPS_RESOLVED:-}" ]]; then
    echo "error: MT_CAPS_SUPPLIED=YES but MT_CAPS_OUT/MT_CAPS_RESOLVED are empty." >&2
    echo "note: without both this is a presence test, not an agreement check." >&2
    exit 1
fi

STAMP="$MT_CAPS_OUT/resolved.stamp"
if [[ ! -f "$STAMP" ]]; then
    echo "error: no resolved.stamp at $STAMP" >&2
    echo "note: re-run the app's ./build-macos.sh to regenerate it." >&2
    exit 1
fi

PYTHON3="$(command -v python3 || true)"
if [[ -z "$PYTHON3" ]]; then
    echo "error: python3 not found, and the capability check needs it." >&2
    exit 1
fi

# `check` RE-RESOLVES from the stamp's recorded inputs at check time. Comparing
# the build's fragment against an artefact the same generator run emitted would
# let both go stale together -- which is exactly the `-target` case this phase
# exists for: the pre-action did not run, both artefacts are stale from the
# previous run, they agree, and the silent wrong build returns.
"$PYTHON3" -B "$SCRIPT_DIR/mtcaps.py" check \
    --stamp "$STAMP" --resolved "$MT_CAPS_RESOLVED"
