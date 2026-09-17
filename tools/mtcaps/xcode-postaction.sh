#!/usr/bin/env bash
#
# The Xcode scheme BUILD POST-ACTION. Releases the build lock this app's
# pre-action took.
#
#   xcode-postaction.sh <app-name>
#
# MEASURED, and both halves matter:
#
#   * a post-action DOES run under `xcodebuild -scheme`, not only in Xcode.app --
#     so wrapper builds reach this script too, but they do not own the IDE
#     channel and leave their app-lock release to the wrapper's EXIT trap;
#   * a post-action does NOT run when the build FAILS. So this is the HAPPY PATH
#     ONLY. It is never the thing that guarantees the lock is freed -- that is
#     build-lock.sh's owner-process liveness check, which breaks the lock of a
#     build that is no longer running. Read this script as an optimisation
#     (release immediately instead of at the next waiter's first poll), not as
#     the correctness mechanism.
#
# It exits 0 unconditionally. A post-action that fails would report an error on a
# build that actually succeeded, and there is nothing here worth failing a green
# build over -- the lock recovers on its own either way.

APP_NAME="${1:-}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [[ -z "$APP_NAME" ]]; then
    echo "mtcaps post-action: no app name given, not releasing anything" >&2
    exit 0
fi

# Beside the five repos, the same place the pre-action takes it from.
IDE_LOCK_DIR="$(cd "$SCRIPT_DIR/../../.." && pwd)/.mtengine-ide/ide-channel.lock"

# `env -u MT_BUILD_LOCK_PID`, and this is load-bearing.
#
# The wrapper EXPORTS its own pid so build-lock.sh can recognise it as the owner.
# xcodebuild's children inherit that export -- including this post-action -- so
# without stripping it here, owning_pid() reports the WRAPPER's pid, the exact
# owner match succeeds, and this action frees a lock the wrapper is still using.
# That is the very bug the exact-match rule was introduced to close; exporting
# the pid quietly reopened it.
#
# With it unset, owning_pid() walks to the xcodebuild/Xcode process instead --
# which IS what the pre-action recorded in the IDE channel, so a genuine IDE
# release still works, and is NOT the wrapper's pid, so a CLI build correctly
# declines and leaves the release to the wrapper's own EXIT trap.
# BOTH LOCKS, in the reverse of the order the pre-action took them: the IDE
# channel first, then this app's. Releasing the app lock while still holding the
# IDE one would let a CLI build of the same app start against a DerivedData an
# IDE build has not finished with.
#
# A CLI build declines both, exactly as before -- see the pid note above -- and
# leaves them to the wrapper's own EXIT trap.

# DELETE THE SHARED ENGINE XCCONFIG while the IDE-channel lock is still held, so
# it exists only while a build needs it and the next IDE build cannot have its
# freshly-written file removed by this post-action after it acquires the lock.
#
# Wrapper builds deliberately never own the IDE channel. Their pre-action exits
# before writing this file, and the build uses command-line settings instead, so
# deleting here would destroy a concurrent Xcode.app build's channel state.
IDE_SHARED_FILE="$(cd "$SCRIPT_DIR/../../.." && pwd)/.mtengine-ide/_current/engine.xcconfig"
if [[ "${MT_CAPS_WRAPPER:-0}" != "1" ]] \
   && env -u MT_BUILD_LOCK_PID MTENGINE_LOCK_DIR="$IDE_LOCK_DIR" \
        "$SCRIPT_DIR/build-lock.sh" owns "$APP_NAME"; then
    rm -f "$IDE_SHARED_FILE" 2>/dev/null || true
fi

env -u MT_BUILD_LOCK_PID MTENGINE_LOCK_DIR="$IDE_LOCK_DIR" \
    "$SCRIPT_DIR/build-lock.sh" release "$APP_NAME" || true
env -u MT_BUILD_LOCK_PID "$SCRIPT_DIR/build-lock.sh" release "$APP_NAME" || true

exit 0
