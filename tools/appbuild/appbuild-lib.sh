#!/usr/bin/env bash
# appbuild-lib.sh -- shared pieces of the app-build drivers (stub-shrink,
# 2026-08-31). Sourced by app-build-macos.sh and app-build-linux.sh.
#
# The MTENGINE_REF verification used to live in every app's stub -- ~85 lines
# pasted eight times across four repos. It is READ-ONLY (a build never moves
# an existing checkout), so it belongs to the driver; the stub keeps only the
# one thing the driver cannot do for itself: clone the engine when absent.

# mt_appbuild_verify_ref <app-dir> <engine-dir>
#
# MTENGINE_REF names the engine revision the app is built against. A branch
# is a TRACK (behind prints a note, never fatal -- ahead or dirty is ordinary
# engine work), a SHA/tag is a PIN (mismatch warns, MTENGINE_PIN_STRICT=1
# makes it fatal). An existing checkout is never moved by a build.
mt_appbuild_verify_ref() {
    local APP_DIR="$1"
    local MTENGINE_DIR="$2"
    local REF_FILE="$APP_DIR/MTENGINE_REF"
    [[ -f "$REF_FILE" ]] || { echo "ERROR: no MTENGINE_REF in $APP_DIR" >&2; return 1; }
    local MTENGINE_REF
    MTENGINE_REF="$(grep -v '^[[:space:]]*#' "$REF_FILE" | grep -v '^[[:space:]]*$' | head -n 1 | tr -d '[:space:]')"
    [[ -n "$MTENGINE_REF" ]] || { echo "ERROR: $REF_FILE names no revision" >&2; return 1; }

    local is_branch=false
    if git -C "$MTENGINE_DIR" show-ref --verify --quiet "refs/remotes/$MTENGINE_REF" \
       || git -C "$MTENGINE_DIR" show-ref --verify --quiet "refs/heads/$MTENGINE_REF"; then
        is_branch=true
    fi

    local mt_have mt_want
    mt_have="$(git -C "$MTENGINE_DIR" rev-parse HEAD)"
    if [[ "$is_branch" == "true" ]]; then
        # A remote-tracking ref is only as fresh as the last fetch; offline,
        # keep building.
        git -C "$MTENGINE_DIR" fetch --quiet origin || true
        mt_want="$(git -C "$MTENGINE_DIR" rev-parse --verify --quiet "$MTENGINE_REF^{commit}" || true)"
        if [[ -n "$mt_want" && "$mt_have" != "$mt_want" ]] \
           && git -C "$MTENGINE_DIR" merge-base --is-ancestor "$mt_have" "$mt_want"; then
            local mt_n
            mt_n="$(git -C "$MTENGINE_DIR" rev-list --count "$mt_have..$mt_want")"
            echo "NOTE: MTEngineSDL is $mt_n commit(s) behind $MTENGINE_REF -- git -C \"$MTENGINE_DIR\" pull"
        fi
    else
        mt_want="$(git -C "$MTENGINE_DIR" rev-parse --verify --quiet "$MTENGINE_REF^{commit}" || true)"
        if [[ -z "$mt_want" ]]; then
            git -C "$MTENGINE_DIR" fetch --quiet origin || true
            mt_want="$(git -C "$MTENGINE_DIR" rev-parse --verify --quiet "$MTENGINE_REF^{commit}" || true)"
        fi
        if [[ -n "$mt_want" && "$mt_have" != "$mt_want" ]]; then
            if [[ "${MTENGINE_PIN_STRICT:-0}" == "1" ]]; then
                echo "ERROR: MTEngineSDL is at $mt_have but MTENGINE_REF pins $mt_want" >&2
                echo "       git -C \"$MTENGINE_DIR\" checkout --detach $MTENGINE_REF" >&2
                return 1
            fi
            echo "WARNING: MTEngineSDL is at ${mt_have:0:12}, MTENGINE_REF pins ${mt_want:0:12} -- building what is checked out"
        fi
    fi
    return 0
}

# mt_appbuild_check_stub <app-dir> <engine-dir> <stub-filename>
#
# The stubs are the ONE remaining copied file in an app repo, so the engine
# owns their canonical text (tools/appbuild/stubs/) and the driver flags
# drift on every build -- a stale stub is a warning naming the fix, never a
# silent divergence. Warning, not error: an old stub that still reaches this
# driver is by definition still working.
mt_appbuild_check_stub() {
    local APP_DIR="$1"
    local ENGINE_DIR="$2"
    local NAME="$3"
    local TEMPLATE="$ENGINE_DIR/tools/appbuild/stubs/$NAME"
    [[ -f "$TEMPLATE" && -f "$APP_DIR/$NAME" ]] || return 0
    if ! cmp -s "$TEMPLATE" "$APP_DIR/$NAME"; then
        echo "NOTE: $NAME differs from the engine's canonical stub template." >&2
        echo "      Refresh it: cp \"$TEMPLATE\" \"$APP_DIR/$NAME\"" >&2
    fi
}

# mt_appbuild_prod_binary <app-dir> <binary-basename>
#
# Echoes "<prod-dir>|<binary-path>" for the newest release package that holds a
# runnable binary, or nothing at all. Both halves matter and the FIRST is the
# one people forget.
#
# WHY THE DIRECTORY IS PART OF THE ANSWER. An MTEngineSDL app finds its assets
# through the CURRENT WORKING DIRECTORY and nothing else: RES_ResolveResourceDir
# has exactly two candidate roots, the relative path itself and
# gPathToResources, and on Windows SYS_InitFileSystem builds that second one as
# the working directory plus "\Resources\". The executable's own location is
# never consulted. So "where the binary is" does not determine whether it can
# load anything -- "where you launched it from" does.
#
# The release package is the directory laid out to satisfy that: the binary,
# assets/, LICENSES.txt and any runtime DLLs together. A test that runs the
# binary out of the build tree with the repo root as its CWD only works for an
# app whose repo root happens to have assets/ in it; an app that needs assets
# cannot start at all that way. Hence the rule, from the maintainer 2026-09-02:
# apps are built to prod and the tests run FROM there.
#
# NO ARCH MAPPING, deliberately. The obvious implementation reads `uname -m`
# and maps it to the directory the driver used -- but that mapping differs per
# platform (ARM64/x64 on Windows, arm64/x86_64 on macOS, aarch64 on Linux) AND
# per shell (`uname -m` under Git Bash on an ARM64 Windows box reports the
# emulated x86_64). Three of those six answers would be wrong. Globbing the
# packages that actually EXIST and taking the newest cannot be wrong about a
# name it never has to spell.
mt_appbuild_prod_binary() {
    local APP_DIR="$1"
    local NAME="$2"
    local best="" best_mtime=0 prod bin mtime

    for prod in "$APP_DIR"/platform/*/prod/*/; do
        [[ -d "$prod" ]] || continue
        # macOS keeps the executable inside the bundle; the CWD is still the
        # package directory, which is where assets/ sits.
        for bin in "$prod$NAME"*.app/Contents/MacOS/"$NAME" \
                   "$prod$NAME"*.exe "$prod$NAME"*; do
            [[ -f "$bin" && -x "$bin" ]] || continue
            case "$bin" in *.app) continue ;; esac
            mtime=$(mt_appbuild_mtime "$bin") || continue
            if [[ "$mtime" -gt "$best_mtime" ]]; then
                best_mtime="$mtime"
                best="${prod%/}|$bin"
            fi
        done
    done

    [[ -n "$best" ]] && printf '%s\n' "$best"
}

# stat(1) is not portable: BSD/macOS wants -f %m, GNU/Linux and Git Bash want
# -c %Y. Both are tried rather than branching on `uname`, because Git Bash is a
# GNU stat on a system that says it is Windows.
mt_appbuild_mtime() {
    stat -c %Y "$1" 2>/dev/null || stat -f %m "$1" 2>/dev/null
}
