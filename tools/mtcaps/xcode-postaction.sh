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
#     so this one releaser serves both channels, exactly as the pre-action's
#     acquire does;
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
env -u MT_BUILD_LOCK_PID "$SCRIPT_DIR/build-lock.sh" release "$APP_NAME" || true

# DELETE THE SHARED ENGINE XCCONFIG, so it exists only while a build needs it.
#
# It is written by the pre-action and read by the engine target, both inside this
# build -- nothing needs it afterwards. Leaving it lying around is what let one
# app's set leak into a LATER build that no pre-action ran for: a standalone
# `xcodebuild -scheme MTEngineSDL`, an `xcodebuild -target` invocation, or a
# contributor opening the engine on a machine where an app was once built. All
# three then compiled against some app's manifest instead of the engine's own
# defaults, silently.
#
# A failed build still leaves it (post-actions do not run then), which is why the
# backstop also detects the leftover rather than trusting this cleanup.
IDE_SHARED_FILE="$(cd "$SCRIPT_DIR/../../.." && pwd)/.mtengine-ide/_current/engine.xcconfig"
rm -f "$IDE_SHARED_FILE" 2>/dev/null || true

exit 0
