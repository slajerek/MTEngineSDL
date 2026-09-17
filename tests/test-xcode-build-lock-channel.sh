#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="$(mktemp -d -t mt-xcode-lock.XXXXXX)"
trap 'rm -rf "$WORK_DIR"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }

make_layout() {
    local name="$1"
    local root="$WORK_DIR/$name"
    mkdir -p "$root/engine/tools/mtcaps" "$root/app" "$root/.mtengine-ide/_current"
    cp "$ROOT_DIR/tools/mtcaps/xcode-preaction.sh" "$root/engine/tools/mtcaps/"
    cp "$ROOT_DIR/tools/mtcaps/xcode-postaction.sh" "$root/engine/tools/mtcaps/"
    cat > "$root/engine/tools/mtcaps/build-lock.sh" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
shared="$root/.mtengine-ide/_current/engine.xcconfig"
kind=app
case "${MTENGINE_LOCK_DIR:-}" in
    *ide-channel.lock) kind=ide ;;
esac
state=missing
[[ -e "$shared" ]] && state=present
printf '%s %s %s shared=%s wrapper=%s objroot=%s\n' \
    "${1:-}" "$kind" "${2:-}" "$state" "${MT_CAPS_WRAPPER:-0}" "${OBJROOT:-}" \
    >> "${MT_LOCK_TEST_LOG:?}"
EOF
    chmod +x "$root/engine/tools/mtcaps/build-lock.sh"
    printf '%s\n' 'MT_CAPS_SUPPLIED = YES' > "$root/.mtengine-ide/_current/engine.xcconfig"
    printf '%s\n' "$root"
}

make_real_lock_layout() {
    local name="$1"
    local root="$WORK_DIR/$name"
    mkdir -p "$root/engine/tools/mtcaps"
    cp "$ROOT_DIR/tools/mtcaps/build-lock.sh" "$root/engine/tools/mtcaps/"
    chmod +x "$root/engine/tools/mtcaps/build-lock.sh"
    printf '%s\n' "$root"
}

assert_contains() {
    local file="$1" needle="$2"
    grep -Fq "$needle" "$file" || fail "expected $file to contain: $needle"
}

assert_not_contains() {
    local file="$1" needle="$2"
    if grep -Fq "$needle" "$file"; then
        fail "expected $file not to contain: $needle"
    fi
}

# The post-action asks the lock helper whether it really owns ide-channel.lock
# before deleting the shared xcconfig. Exercise that ownership check with the
# real lock helper, in an isolated layout.
root="$(make_real_lock_layout owns)"
lock_script="$root/engine/tools/mtcaps/build-lock.sh"
MT_BUILD_LOCK_PID=$$ MT_BUILD_LOCK_KIND=test bash "$lock_script" acquire DummyApp
MT_BUILD_LOCK_PID=$$ bash "$lock_script" owns DummyApp \
    || fail "build-lock owns did not recognize the recorded owner"
if MT_BUILD_LOCK_PID=999999 bash "$lock_script" owns DummyApp; then
    fail "build-lock owns accepted a different owner pid"
fi
MT_BUILD_LOCK_PID=$$ bash "$lock_script" release DummyApp

# A corrupt owner file is exactly when the diagnostic/recovery path matters.
# Nonnumeric LOCK_AT must not abort bash arithmetic under `set -u`.
root="$(make_real_lock_layout corrupt-lock-at)"
lock_script="$root/engine/tools/mtcaps/build-lock.sh"
lock_dir="$root/.mtengine-ide/app-DummyApp.lock"
mkdir -p "$lock_dir"
cat > "$lock_dir/owner" <<EOF
LOCK_APP='OtherApp'
LOCK_PID='$$'
LOCK_AT='abc'
LOCK_HOST='test'
LOCK_KIND='test'
LOCK_OBJROOT=''
EOF
set +e
corrupt_output="$(MT_BUILD_LOCK_PID=999999 MTENGINE_BUILD_LOCK_WAIT=0 \
    bash "$lock_script" acquire DummyApp 2>&1)"
corrupt_status=$?
set -e
[[ "$corrupt_status" -ne 0 ]] || fail "corrupt LOCK_AT acquisition unexpectedly succeeded"
case "$corrupt_output" in
    *'unbound variable'*|*'syntax error'*|*'value too great'*)
        fail "corrupt LOCK_AT crashed shell arithmetic: $corrupt_output"
        ;;
esac

# Xcode.app can outlive a failed build. A later build from the same Xcode
# process must not pass through that stale same-app lock before the idle OBJROOT
# probe has a chance to break and republish it.
root="$(make_real_lock_layout stale-same-owner)"
lock_script="$root/engine/tools/mtcaps/build-lock.sh"
lock_dir="$root/.mtengine-ide/app-DummyApp.lock"
old_objroot="$root/old-objroot"
new_objroot="$root/new-objroot"
mkdir -p "$lock_dir" "$old_objroot/XCBuildData" "$new_objroot"
touch -t 200001010000 "$old_objroot/XCBuildData/build.db" "$old_objroot/XCBuildData" "$old_objroot"
cat > "$lock_dir/owner" <<EOF
LOCK_APP='DummyApp'
LOCK_PID='$$'
LOCK_AT='$(date +%s)'
LOCK_HOST='test'
LOCK_KIND='ide'
LOCK_OBJROOT='$old_objroot'
EOF
OBJROOT="$new_objroot" MT_BUILD_LOCK_PID=$$ MT_BUILD_LOCK_KIND=ide \
    MTENGINE_BUILD_LOCK_IDLE=0 bash "$lock_script" acquire DummyApp
grep -Fq "LOCK_OBJROOT='$new_objroot'" "$lock_dir/owner" \
    || fail "same-owner stale lock passed through without refreshing OBJROOT"
MT_BUILD_LOCK_PID=$$ bash "$lock_script" release DummyApp

# A stale waiter may have read an old Xcode lock just before another waiter
# breaks it and the same Xcode process republishes a new one. Same PID is not a
# sufficient proof that the moved lock is still the stale one.
root="$(make_real_lock_layout same-pid-break-race)"
lock_script="$root/engine/tools/mtcaps/build-lock.sh"
lock_dir="$root/.mtengine-ide/app-DummyApp.lock"
old_objroot="$root/race-old-objroot"
fresh_objroot="$root/race-fresh-objroot"
waiter_objroot="$root/race-waiter-objroot"
mkdir -p "$lock_dir" "$old_objroot/XCBuildData" "$fresh_objroot/XCBuildData" "$waiter_objroot" "$root/bin"
touch -t 200001010000 "$old_objroot/XCBuildData/build.db" "$old_objroot/XCBuildData" "$old_objroot"
touch "$fresh_objroot/XCBuildData/build.db" "$fresh_objroot/XCBuildData" "$fresh_objroot"
cat > "$lock_dir/owner" <<EOF
LOCK_APP='DummyApp'
LOCK_PID='$$'
LOCK_AT='$(date +%s)'
LOCK_HOST='test'
LOCK_KIND='ide'
LOCK_OBJROOT='$old_objroot'
EOF
cat > "$root/bin/mv" <<'EOF'
#!/usr/bin/env bash
set -euo pipefail
if [[ "${1:-}" == "${MT_LOCK_RACE_DIR:-}" ]]; then
    case "${2:-}" in
        "${MT_LOCK_RACE_DIR}.dead."*)
            if [[ ! -e "${MT_LOCK_RACE_STAMP:?}" ]]; then
                : > "$MT_LOCK_RACE_STAMP"
                rm -rf "$MT_LOCK_RACE_DIR"
                mkdir -p "$MT_LOCK_RACE_DIR"
                cat > "$MT_LOCK_RACE_DIR/owner" <<OWNER
LOCK_APP='DummyApp'
LOCK_PID='$MT_LOCK_RACE_PID'
LOCK_AT='$(date +%s)'
LOCK_HOST='test'
LOCK_KIND='ide'
LOCK_OBJROOT='$MT_LOCK_RACE_FRESH_OBJROOT'
OWNER
            fi
            ;;
    esac
fi
exec /bin/mv "$@"
EOF
chmod +x "$root/bin/mv"
PATH="$root/bin:$PATH" OBJROOT="$waiter_objroot" MT_BUILD_LOCK_PID=$$ MT_BUILD_LOCK_KIND=ide \
    MTENGINE_BUILD_LOCK_IDLE=300 MTENGINE_BUILD_LOCK_WAIT=2 \
    MT_LOCK_RACE_DIR="$lock_dir" MT_LOCK_RACE_STAMP="$root/raced" \
    MT_LOCK_RACE_PID=$$ MT_LOCK_RACE_FRESH_OBJROOT="$fresh_objroot" \
    bash "$lock_script" acquire DummyApp
grep -Fq "LOCK_OBJROOT='$fresh_objroot'" "$lock_dir/owner" \
    || fail "break_lock removed a freshly republished same-pid lock"
MT_BUILD_LOCK_PID=$$ bash "$lock_script" release DummyApp

# If the user rebuilds immediately after an IDE failure, the old OBJROOT may not
# be idle yet. Acquire must pass through to avoid self-deadlock, and annotate is
# then responsible for replacing the stale build directory with this build's one.
root="$(make_real_lock_layout quick-same-owner-rebuild)"
lock_script="$root/engine/tools/mtcaps/build-lock.sh"
lock_dir="$root/.mtengine-ide/app-DummyApp.lock"
old_objroot="$root/recent-old-objroot"
new_objroot="$root/recent-new-objroot"
mkdir -p "$lock_dir" "$old_objroot/XCBuildData" "$new_objroot"
touch "$old_objroot/XCBuildData/build.db" "$old_objroot/XCBuildData" "$old_objroot"
cat > "$lock_dir/owner" <<EOF
LOCK_APP='DummyApp'
LOCK_PID='$$'
LOCK_AT='$(date +%s)'
LOCK_HOST='test'
LOCK_KIND='ide'
LOCK_OBJROOT='$old_objroot'
EOF
OBJROOT="$new_objroot" MT_BUILD_LOCK_PID=$$ MT_BUILD_LOCK_KIND=ide \
    MTENGINE_BUILD_LOCK_IDLE=300 bash "$lock_script" acquire DummyApp
OBJROOT="$new_objroot" MT_BUILD_LOCK_PID=$$ bash "$lock_script" annotate DummyApp
grep -Fq "LOCK_OBJROOT='$new_objroot'" "$lock_dir/owner" \
    || fail "same-owner annotate preserved stale OBJROOT on quick rebuild"
MT_BUILD_LOCK_PID=$$ bash "$lock_script" release DummyApp

# CLI wrapper builds deliberately never own the IDE channel. Their scheme
# post-action must therefore not delete the shared file a concurrent Xcode.app
# build may have written.
root="$(make_layout wrapper-post)"
log="$root/post.log"
MT_LOCK_TEST_LOG="$log" MT_CAPS_WRAPPER=1 \
    bash "$root/engine/tools/mtcaps/xcode-postaction.sh" DummyApp
[[ -f "$root/.mtengine-ide/_current/engine.xcconfig" ]] \
    || fail "wrapper post-action deleted the shared engine xcconfig"
assert_contains "$log" 'release ide DummyApp shared=present wrapper=1'
assert_contains "$log" 'release app DummyApp shared=present wrapper=1'

# An IDE build does own the shared file, but cleanup belongs to the IDE-channel
# critical section. By the time that lock is released, the file should already be
# gone so the next IDE build cannot have its freshly-written file removed by this
# build's post-action.
root="$(make_layout ide-post)"
log="$root/post.log"
MT_LOCK_TEST_LOG="$log" bash "$root/engine/tools/mtcaps/xcode-postaction.sh" DummyApp
[[ ! -e "$root/.mtengine-ide/_current/engine.xcconfig" ]] \
    || fail "IDE post-action left the shared engine xcconfig behind"
assert_contains "$log" 'release ide DummyApp shared=missing wrapper=0'
assert_contains "$log" 'release app DummyApp shared=missing wrapper=0'

# The wrapper pre-action must still annotate the already-held app lock with
# OBJROOT, but it must not acquire either lock or touch the IDE channel.
root="$(make_layout wrapper-pre)"
log="$root/pre.log"
rm -f "$root/.mtengine-ide/_current/engine.xcconfig"
MT_LOCK_TEST_LOG="$log" MT_CAPS_WRAPPER=1 OBJROOT="$root/DerivedData/App/Build/Intermediates.noindex" \
    bash "$root/engine/tools/mtcaps/xcode-preaction.sh" "$root/app" DummyApp
assert_contains "$log" 'annotate app DummyApp shared=missing wrapper=1 objroot='
assert_not_contains "$log" 'acquire '
assert_not_contains "$log" ' ide '
[[ ! -e "$root/.mtengine-ide/_current/engine.xcconfig" ]] \
    || fail "wrapper pre-action wrote the shared engine xcconfig"

echo "OK: Xcode IDE channel cleanup is isolated from CLI wrapper builds"
