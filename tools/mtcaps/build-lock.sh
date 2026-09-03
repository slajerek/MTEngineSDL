#!/usr/bin/env bash
#
# THE BUILD LOCK -- serializes builds that share one MTEngineSDL checkout.
#
#   build-lock.sh acquire <app>    # blocks until free, then takes it
#   build-lock.sh release <app>    # releases it, but only if <app> owns it
#   build-lock.sh status           # prints the current holder, exit 1 if free
#   build-lock.sh annotate <app>   # fill in a field the outer acquire could not know
#   build-lock.sh steal            # manual escape hatch, prints what it broke
#
# WHY THIS EXISTS.
#
# Four apps build ONE engine checkout, and two things they share are not
# per-app:
#
#   1. .mtengine-ide/_current/engine.xcconfig -- the engine target cannot know
#      WHICH app is building it, so the IDE channel writes one file and the most
#      recent writer wins. A second app's build that lands between this build's
#      pre-action and its settings resolution silently hands the engine target
#      the OTHER app's capability set. MEASURED: xcodebuild -scheme runs the
#      pre-action too, so the script path is an aggressor here, not just Xcode.
#   2. The engine's own in-checkout writes. The STAGED ARCHIVES have since moved
#      out (see mt_caps_lib_dir), but the work that produces them has not: the
#      vendored uSockets tree is `make`d in place, and the codec scripts
#      configure and compile under other/lib/*/build and install into
#      other/lib/*/install -- all inside the checkout, all shared. Two concurrent
#      runs there is a plain collision (MEASURED: a second build's `rm -rf` of the
#      shared libvpx build directory makes the first one fail with
#      "vpx_config.h file not found"), and it has nothing to do with
#      capabilities.
#
# So the lock is not a capability mechanism; it is a checkout-wide mutex, and it
# covers both. Serializing is the honest answer: the alternative -- making the
# xcconfig include path per-build -- was measured impossible (Xcode processes
# includes BEFORE interpreting any build setting, so no variable of any spelling
# expands there; nine probes with a literal control, all negative).
#
# WHERE IT LIVES: beside the five repos, inside none of them. Same reasoning and
# same directory as the IDE channel's own files -- THE RULE forbids writing in
# the engine checkout, and this file is written on every build.
#
# HOW OWNERSHIP IS PROVEN, and why it is not just a timestamp.
#
# `mkdir` is atomic on POSIX, so the directory IS the lock. The `owner` file
# inside it records who took it. Releasing is the happy path, and there are two
# releasers:
#
#   * the scheme's build POST-action (both channels -- MEASURED: post-actions do
#     run under `xcodebuild -scheme`);
#   * the app's build-macos.sh, from an EXIT trap.
#
# Neither is sufficient alone, because MEASURED: a post-action does NOT run when
# the build FAILS. A compile error would otherwise leak the lock and block every
# other app until someone noticed. So a waiter also breaks a lock whose owner
# process is gone (`kill -0`), and that is the primary recovery path rather than
# the fallback: the pre-action records the pid of the xcodebuild/Xcode process it
# is running under -- walked up the process chain, since the pre-action's own
# shell dies immediately -- and that process lives exactly as long as the build.
#
# `kill -0` settles every CRASH case exactly -- Xcode quits, xcodebuild is
# killed, an agent hits Ctrl-C -- but not the one where the pid outlives the
# build: Xcode.app stays running after a build FAILS. So there is a second probe,
# asking whether the build is actually PROGRESSING rather than merely existing --
# the lock records OBJROOT and a waiter breaks it when that build directory has
# been idle (MTENGINE_BUILD_LOCK_IDLE, default 300s).
#
# The TTL is only the last resort, for when neither probe can decide -- no
# OBJROOT was recorded, or the build directory was never created. Override with
# MTENGINE_BUILD_LOCK_TTL (seconds).

set -euo pipefail

ACTION="${1:-status}"
APP="${2:-}"

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENGINE_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
IDE_ROOT="$(cd "$ENGINE_DIR/.." && pwd)/.mtengine-ide"
LOCK_DIR="$IDE_ROOT/build.lock"

# A SECOND THING TO SERIALIZE, with the same rules (L16).
#
# The lock above is per engine CHECKOUT: it is what stops four apps sharing one
# checkout from configuring the same other/lib/*/build tree at once. The per-unit
# dependency stores are not per checkout -- they hang off the cache root, so a
# SECOND clone of the engine builds into the very same `_depstore/.../sdl3/common`
# while holding a different build lock. That is not exotic for the shared units:
# `common` means one bucket per machine, so two clones collide there by design.
#
# The recovery logic below -- dead owner, idle owner, the atomic rename that stops
# two waiters from both "winning" a stale lock -- is the part that was hard to get
# right, and it does not care what is being serialized. So the caller supplies the
# directory and inherits all of it, rather than a second mutex being written from
# scratch beside a hardened one.
if [[ -n "${MTENGINE_LOCK_DIR:-}" ]]; then
    LOCK_DIR="$MTENGINE_LOCK_DIR"
    IDE_ROOT="$(dirname "$LOCK_DIR")"
fi
OWNER_FILE="$LOCK_DIR/owner"

TTL="${MTENGINE_BUILD_LOCK_TTL:-1800}"          # 30 min; only reached when neither liveness probe can decide
IDLE="${MTENGINE_BUILD_LOCK_IDLE:-300}"         # 5 min with no build-directory activity == not building
WAIT_TIMEOUT="${MTENGINE_BUILD_LOCK_WAIT:-3600}" # how long to queue before giving up
CLAIM_GRACE="${MTENGINE_BUILD_LOCK_GRACE:-10}"  # how long an owner-less lock may be a claim in progress

now() { date +%s; }

read_owner() {
    # Reset ALL of them: acquire re-reads in a loop, and a field missing from an
    # older owner file would otherwise keep the previous iteration's value.
    LOCK_APP=""; LOCK_PID=""; LOCK_AT=""; LOCK_HOST=""; LOCK_KIND=""; LOCK_OBJROOT=""
    [[ -f "$OWNER_FILE" ]] || return 1
    # shellcheck disable=SC1090
    . "$OWNER_FILE" 2>/dev/null || return 1
    return 0
}

# The pid to watch is NOT this shell's -- a pre-action's shell exits immediately.
# Walk up to the xcodebuild/Xcode process that lives for the whole build.
# MEASURED chain: <pre-action sh> -> /bin/sh -> xcodebuild -> ...
owning_pid() {
    # An explicit answer wins. build-macos.sh passes its OWN pid here, because
    # this script runs as its child and would otherwise record a pid that dies
    # the instant `acquire` returns -- making the lock look abandoned to the very
    # next waiter, which is the opposite of the point.
    if [[ -n "${MT_BUILD_LOCK_PID:-}" ]]; then
        echo "$MT_BUILD_LOCK_PID"
        return 0
    fi
    local p="$PPID" i cmd
    for i in 1 2 3 4 5 6; do
        [[ -z "$p" || "$p" == "1" || "$p" == "0" ]] && break
        cmd="$(ps -o comm= -p "$p" 2>/dev/null || true)"
        case "$cmd" in
            *xcodebuild|*Xcode|*XCBBuildService*) echo "$p"; return 0 ;;
        esac
        p="$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ' || true)"
    done
    # No build process above us (a bare shell invocation): own pid, which is
    # right for build-macos.sh calling this directly.
    echo "$$"
}

# Is `pid` this process or one of its ancestors? Walks up from $$ rather than
# down, so it costs a handful of `ps` calls and terminates at init.
pid_is_self_or_ancestor() {
    local target="$1" p="$$" i
    [[ -z "$target" ]] && return 1
    for i in 1 2 3 4 5 6 7 8; do
        [[ -z "$p" || "$p" == "0" ]] && break
        [[ "$p" == "$target" ]] && return 0
        [[ "$p" == "1" ]] && break
        p="$(ps -o ppid= -p "$p" 2>/dev/null | tr -d ' ' || true)"
    done
    # MT_BUILD_LOCK_PID is what build-macos.sh recorded for itself; a pre-action
    # running under that same script may not see it as an ancestor if the
    # process chain is longer than the walk above, so accept it explicitly.
    [[ -n "${MT_BUILD_LOCK_PID:-}" && "$target" == "$MT_BUILD_LOCK_PID" ]]
}

# Prepare the owner record BEFORE claiming, so the window between `mkdir` and a
# readable owner file is one `mv` rather than a `ps` walk plus three
# subprocesses. Everything here is known before the loop starts and never
# changes, so it is computed once.
OWNER_TMP=""
prepare_owner() {
    OWNER_TMP="$IDE_ROOT/.owner.$$"
    cat > "$OWNER_TMP" <<EOF
LOCK_APP='$APP'
LOCK_PID='$(owning_pid)'
LOCK_AT='$(now)'
LOCK_HOST='$(hostname -s 2>/dev/null || echo unknown)'
LOCK_KIND='${MT_BUILD_LOCK_KIND:-unknown}'
LOCK_OBJROOT='${OBJROOT:-}'
EOF
}

# One atomic rename into the directory we just created.
#
# The timestamp is rewritten HERE, not in prepare_owner. Preparing the record
# before the claim is what makes the publish a single rename -- but it also
# stamped LOCK_AT at QUEUE-START, so a build that waited 40 minutes acquired a
# lock already 40 minutes old and the next waiter broke it on the spot. The
# defaults make that reachable: WAIT_TIMEOUT (3600s) is longer than TTL (1800s).
publish_owner() {
    local now_at; now_at="$(now)"
    if command -v sed >/dev/null 2>&1; then
        sed "s/^LOCK_AT=.*/LOCK_AT='$now_at'/" "$OWNER_TMP" > "$OWNER_TMP.at" \
            && mv -f "$OWNER_TMP.at" "$OWNER_TMP"
    fi
    mv -f "$OWNER_TMP" "$OWNER_FILE" || {
        # The specific diagnosis first: break_lock exits on its own failure, so
        # anything printed after it may never be reached.
        echo "error: build-lock: could not publish the owner record into $LOCK_DIR." >&2
        # Never leave a claimed-but-empty directory behind: the next waiter would
        # sit through the whole grace period for a lock nobody holds.
        break_lock
        exit 1
    }
}

# A holder is dead when its build process is gone. This is the PRIMARY recovery
# path, not a fallback -- see the header on failed builds never releasing.
holder_is_dead() {
    [[ -z "${LOCK_PID:-}" ]] && return 0
    kill -0 "$LOCK_PID" 2>/dev/null && return 1
    return 0
}

# IS THE BUILD ACTUALLY PROGRESSING, as opposed to merely "a process exists"?
#
# `kill -0` above answers the crash cases exactly -- Xcode quits, xcodebuild is
# killed, an agent hits Ctrl-C -- and for those the lock is broken on the next
# poll. It does NOT answer the one that is left: a build that FAILED while
# Xcode.app stayed running. The pid is alive, the post-action never ran (measured:
# post-actions do not run on a failed build), and without this the lock would sit
# there for the full TTL blocking every other app.
#
# A running build writes into its own DerivedData constantly; a finished or
# failed one stops. So the pre-action records OBJROOT and this asks when that
# tree was last touched. XCBuildData/build.db is the cheapest honest signal --
# the build system updates it as tasks complete -- with the directory itself as
# the fallback.
#
# The threshold is deliberately generous (5 min): a single long link step can go
# quiet for a while, and breaking the lock of a build that IS running is far
# worse than waiting a few extra minutes for one that is not.
holder_is_idle() {
    [[ -z "${LOCK_OBJROOT:-}" ]] && return 1        # nothing recorded -- cannot say
    [[ -d "$LOCK_OBJROOT" ]] || return 1            # never created one -- cannot say
    local newest=0 f t
    for f in "$LOCK_OBJROOT/XCBuildData/build.db" "$LOCK_OBJROOT/XCBuildData" "$LOCK_OBJROOT"; do
        [[ -e "$f" ]] || continue
        t="$(stat -f %m "$f" 2>/dev/null || echo 0)"
        [[ "$t" -gt "$newest" ]] && newest="$t"
    done
    [[ "$newest" -eq 0 ]] && return 1
    [[ $(( $(now) - newest )) -gt "$IDLE" ]]
}

# A missing LOCK_AT means CANNOT DECIDE, not "expired". It used to mean expired,
# which turned a half-written owner file into a licence to break a live holder's
# lock -- the pid was alive and the lock was taken anyway. The atomic write above
# should make a torn record impossible; this makes it harmless if one appears.
holder_expired() {
    [[ -z "${LOCK_AT:-}" ]] && return 1
    local age=$(( $(now) - LOCK_AT ))
    [[ "$age" -gt "$TTL" ]]
}

# Can the idle probe say anything at all? #3 depends on this: the TTL must only
# be consulted when the probe CANNOT decide, never as a second opinion over a
# probe that just reported the build as running.
idle_probe_can_decide() {
    [[ -n "${LOCK_OBJROOT:-}" ]] && [[ -d "$LOCK_OBJROOT" ]]
}

# Breaking a lock must never fail SILENTLY. `rm -rf` under `set -e` exits with
# no diagnostic when the directory is not ours to remove (another user, a
# read-only mount), and the caller then looks like it simply stopped.
# Break a lock ATOMICALLY, and only the one we judged stale.
#
# `rm -rf` then `continue` then `mkdir` is three steps, not one. Two waiters that
# independently decided the same holder was dead used to race: the first removed
# and re-created and published; the second -- still acting on its now-stale read
# -- removed the FIRST ONE'S BRAND NEW LOCK and created its own. Both then held
# it. Measured with 8 waiters on one stale lock: 1, 2, 4, 1, 3 simultaneous
# winners across five runs. A mutex that is not mutually exclusive on its most
# common recovery path is not a mutex.
#
# `rename(2)` IS atomic, so exactly one racer can move the directory aside. The
# winner then checks the record it moved is the one it judged stale; if another
# waiter had already replaced it with a live lock, that lock is put back and the
# caller re-loops rather than stealing it.
#
# Returns non-zero when the caller must re-evaluate. Callers `continue`.
break_lock() {
    local dead="$LOCK_DIR.dead.$$"
    if ! mv "$LOCK_DIR" "$dead" 2>/dev/null; then
        # Lost the race to move it, or it is already gone. Either way there is
        # nothing of ours to clean up; re-read and decide again.
        [[ -d "$LOCK_DIR" ]] || return 1
        echo "error: build-lock: could not remove $LOCK_DIR -- check its ownership and permissions." >&2
        exit 1
    fi
    # We moved SOMETHING. Prove it was the record we judged stale: between our
    # read and this rename another waiter may have broken and re-taken the lock.
    if [[ -n "${LOCK_PID:-}" && -f "$dead/owner" ]] \
       && ! grep -q "LOCK_PID='${LOCK_PID}'" "$dead/owner" 2>/dev/null; then
        mv "$dead" "$LOCK_DIR" 2>/dev/null || rm -rf "$dead" 2>/dev/null || true
        return 1
    fi
    rm -rf "$dead" 2>/dev/null || true
    return 0
}

describe_holder() {
    echo "  app     : ${LOCK_APP:-?}"
    # NOT `${LOCK_PID:-0}`: `kill -0 0` signals the caller's OWN process group and
    # always succeeds, so a missing pid printed as "alive".
    if [[ -z "${LOCK_PID:-}" ]]; then
        echo "  pid     : ? (unknown)"
    elif kill -0 "$LOCK_PID" 2>/dev/null; then
        echo "  pid     : $LOCK_PID (alive)"
    else
        echo "  pid     : $LOCK_PID (gone)"
    fi
    # `:-` guards EMPTY only. A garbage value reaches $(( )) as a variable name
    # and dies under `set -u` -- which took out `status`, the waiter's announce
    # path, AND `steal`, the documented escape hatch, on exactly the corrupt
    # record they exist to recover from.
    case "${LOCK_AT:-}" in
        ''|*[!0-9]*) echo "  started : unknown" ;;
        *)           echo "  started : $(( $(now) - LOCK_AT ))s ago" ;;
    esac
    echo "  host    : ${LOCK_HOST:-?}"
    echo "  channel : ${LOCK_KIND:-unknown}"
    if [[ -n "${LOCK_OBJROOT:-}" && -d "${LOCK_OBJROOT:-/nonexistent}" ]]; then
        if holder_is_idle; then
            echo "  building: NO -- build dir idle for more than ${IDLE}s (lock is breakable)"
        else
            echo "  building: yes -- build dir touched recently"
        fi
    else
        echo "  building: unknown -- no build directory recorded"
    fi
}

case "$ACTION" in

acquire)
    [[ -n "$APP" ]] || { echo "build-lock: acquire needs an app name" >&2; exit 2; }
    mkdir -p "$IDE_ROOT"
    prepare_owner
    trap 'rm -f "$OWNER_TMP" 2>/dev/null || true' EXIT
    START="$(now)"
    ANNOUNCED=0
    while true; do
        if mkdir "$LOCK_DIR" 2>/dev/null; then
            publish_owner
            [[ "$ANNOUNCED" == "1" ]] && echo "mtcaps build-lock: acquired after waiting."
            exit 0
        fi

        if ! read_owner; then
            # A lock directory with no readable owner is ALMOST ALWAYS a live
            # acquirer between its `mkdir` and its `mv` -- not an abandoned one.
            # Treating the two as the same thing is how a waiter used to delete a
            # healthy holder's lock and leave both builds running.
            #
            # So wait out a grace period first, and only then conclude the writer
            # died mid-claim. The publish above is a single rename, so anything
            # still owner-less after this long really is wreckage.
            # Clamped at 0: a future-dated mtime (clock skew, a restored backup)
            # made LOCK_AGE permanently negative, parking the waiter in this
            # branch for the full WAIT_TIMEOUT.
            LOCK_AGE=$(( $(now) - $(stat -f %m "$LOCK_DIR" 2>/dev/null || now) ))
            [[ "$LOCK_AGE" -lt 0 ]] && LOCK_AGE=0
            if [[ "$LOCK_AGE" -lt "$CLAIM_GRACE" ]]; then
                # Still bounded by WAIT_TIMEOUT: `continue` here used to jump
                # over the timeout check at the bottom of the loop, so a waiter
                # could sit in this branch past its own deadline.
                if [[ $(( $(now) - START )) -gt "$WAIT_TIMEOUT" ]]; then
                    echo "error: build-lock: gave up after ${WAIT_TIMEOUT}s waiting for a claim in progress." >&2
                    exit 1
                fi
                sleep 1
                continue
            fi
            echo "mtcaps build-lock: lock with no owner record after ${CLAIM_GRACE}s -- taking it." >&2
            break_lock || true
            continue
        fi

        # OUR OWN WRAPPER -- and "ours" means the same PROCESS TREE, not merely
        # the same app name.
        #
        # build-macos.sh takes the lock and then runs xcodebuild, whose
        # pre-action lands here for the same app; that is one build and must
        # pass through rather than deadlock against itself. But two AGENTS
        # building the SAME app are two builds, and a name-only test let both
        # proceed -- straight into the concurrent `make` in the vendored
        # uSockets tree and the shared other/lib/*/build intermediate trees that
        # this lock exists to serialize.
        #
        # So: pass through only when the holder's pid is this process or an
        # ancestor of it. That is exactly "the lock was taken by the build I am
        # part of".
        if [[ "${LOCK_APP:-}" == "$APP" ]] && pid_is_self_or_ancestor "${LOCK_PID:-}"; then
            exit 0
        fi

        if holder_is_dead; then
            echo "mtcaps build-lock: holder '${LOCK_APP:-?}' (pid ${LOCK_PID:-?}) is gone -- breaking its lock." >&2
            break_lock || true
            continue
        fi

        if holder_is_idle; then
            echo "mtcaps build-lock: holder '${LOCK_APP:-?}' is alive but its build has not" >&2
            echo "  touched $LOCK_OBJROOT for over ${IDLE}s -- it is not building. Breaking its lock." >&2
            echo "  (a failed build never releases: post-actions do not run on failure)" >&2
            break_lock || true
            continue
        fi

        # ONLY when the idle probe cannot decide. Consulting the TTL
        # unconditionally broke the lock of a build the probe had just certified
        # as running -- a clean build here fetches llama.cpp, SDL3, ffmpeg,
        # mbedTLS, FTXUI and the image codecs, so exceeding 30 minutes while
        # perfectly healthy is ordinary, not exotic.
        if ! idle_probe_can_decide && holder_expired; then
            echo "mtcaps build-lock: holder '${LOCK_APP:-?}' exceeded the ${TTL}s lease -- breaking its lock." >&2
            echo "  (no build directory was recorded, so progress could not be checked)" >&2
            echo "  (raise MTENGINE_BUILD_LOCK_TTL if this machine legitimately builds for longer)" >&2
            break_lock || true
            continue
        fi

        if [[ "$ANNOUNCED" == "0" ]]; then
            echo "mtcaps build-lock: '$APP' is waiting -- another app is building against this engine."
            describe_holder
            echo "  waiting (this is correct: the engine checkout is shared and cannot serve two apps at once)"
            ANNOUNCED=1
        fi

        if [[ $(( $(now) - START )) -gt "$WAIT_TIMEOUT" ]]; then
            echo "error: build-lock: gave up after ${WAIT_TIMEOUT}s waiting for '${LOCK_APP:-?}'." >&2
            echo "note: if that build is genuinely gone, clear it with:" >&2
            echo "      $SCRIPT_DIR/build-lock.sh steal" >&2
            exit 1
        fi
        sleep 2
    done
    ;;

release)
    [[ -n "$APP" ]] || { echo "build-lock: release needs an app name" >&2; exit 2; }
    [[ -d "$LOCK_DIR" ]] || exit 0
    # Releasing someone ELSE'S lock is worse than leaking our own, and "else"
    # is again a process question rather than a name one: with two builds of the
    # same app, a name-only test let one build's post-action free the other
    # build's lock.
    if read_owner; then
        [[ "${LOCK_APP:-}" != "$APP" ]] && exit 0
        # Same app, but is it OUR build? Normally yes: the post-action runs
        # under the same xcodebuild the pre-action recorded, and build-macos.sh's
        # EXIT trap runs as its own child. When it is not -- a second build of
        # the same app -- refuse, or one build would free the other's lock.
        #
        # A holder whose process is GONE is the exception: its lock is breakable
        # by any waiter anyway, so refusing to release it protects nothing and
        # only strands it until the next poll. This is also what keeps the manual
        # `acquire X` / `release X` pair usable from two separate shells.
        # EXACT match, not ancestry. Ancestry answers "was this lock taken by
        # SOME process above me", which the nested scheme post-action satisfies
        # -- so it released the OUTER wrapper's lock mid-invocation, while
        # build-macos.sh was still running and still writing into the engine
        # checkout. Measured. Only the process that recorded itself as the owner
        # may release, which is why build-macos.sh passes MT_BUILD_LOCK_PID on
        # BOTH its acquire and its release.
        #
        # A dead holder stays exempt: its lock is breakable by any waiter
        # anyway, so refusing to release it protects nothing.
        if [[ "$(owning_pid)" != "${LOCK_PID:-}" ]] && ! holder_is_dead; then
            exit 0
        fi
    fi
    break_lock
    exit 0
    ;;

annotate)
    # ENRICH A LOCK WE ALREADY HOLD.
    #
    # build-macos.sh must take the lock BEFORE xcodebuild -- it stages
    # dependencies inside the engine checkout first -- and at that moment OBJROOT
    # does not exist yet, so LOCK_OBJROOT is empty and the idle probe can never
    # decide for the whole CLI channel. That left the TTL as its only recovery:
    # the exact situation the idle probe was added to remove, still present in
    # the channel whose long clean build justified adding it.
    #
    # The pre-action runs INSIDE xcodebuild, where OBJROOT does exist, and passes
    # through because the lock is already ours. This is that passthrough filling
    # in what the outer acquire could not know.
    [[ -n "$APP" ]] || { echo "build-lock: annotate needs an app name" >&2; exit 2; }
    [[ -d "$LOCK_DIR" ]] || exit 0
    [[ -n "${OBJROOT:-}" ]] || exit 0
    read_owner || exit 0
    [[ "${LOCK_APP:-}" == "$APP" ]] || exit 0
    [[ -z "${LOCK_OBJROOT:-}" ]] || exit 0          # already recorded, leave it
    pid_is_self_or_ancestor "${LOCK_PID:-}" || exit 0
    TMP="$IDE_ROOT/.annot.$$"
    { sed '/^LOCK_OBJROOT=/d' "$OWNER_FILE"; echo "LOCK_OBJROOT='$OBJROOT'"; } > "$TMP" \
        && mv -f "$TMP" "$OWNER_FILE"
    rm -f "$TMP" 2>/dev/null || true
    exit 0
    ;;

status)
    if [[ ! -d "$LOCK_DIR" ]]; then
        echo "mtcaps build-lock: free"
        exit 1
    fi
    if read_owner; then
        echo "mtcaps build-lock: HELD"
        describe_holder
    else
        echo "mtcaps build-lock: HELD, but the owner record is unreadable"
    fi
    exit 0
    ;;

steal)
    if [[ ! -d "$LOCK_DIR" ]]; then
        echo "mtcaps build-lock: already free, nothing to steal"
        exit 0
    fi
    read_owner || true
    echo "mtcaps build-lock: breaking the lock held by:"
    describe_holder
    break_lock
    echo "mtcaps build-lock: cleared"
    exit 0
    ;;

*)
    echo "usage: build-lock.sh {acquire <app>|release <app>|annotate <app>|status|steal}" >&2
    exit 2
    ;;
esac
