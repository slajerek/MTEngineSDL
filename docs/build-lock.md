# The build lock — what still collides, and what no longer does

`tools/mtcaps/build-lock.sh` serializes builds that would otherwise corrupt each
other. It used to be a single checkout-wide mutex; it is now **two locks**,
because only one of the two things it was written for is still true.

## What actually collides

**1. `.mtengine-ide/_current/engine.xcconfig` — still one file, still a real
mutex.** The engine target cannot know which app is building it, so the IDE
capability channel writes one file and the most recent writer wins. A second
app's build landing between this build's pre-action and its settings resolution
hands the engine target the *other* app's capability set — while the app target,
which reads a per-app file, gets the right one. The result is a binary whose app
and engine translation units disagree about capabilities, with no error.

**This applies to the IDE channel only.** A driver build passes
`MT_CAPS_WRAPPER=1`; the pre-action then exits before resolving or writing
anything, and every setting the build depends on rides the `xcodebuild` command
line, which outranks any xcconfig. A CLI build may still parse the optional
include because the tracked engine xcconfig names it, but it does not depend on
those values and it must not mutate that file.

**2. The engine's in-checkout writes — GONE (2026-09-09).** This document used to
say that `build-macos.sh` builds vendored uSockets inside the checkout and the
dependency scripts configure under `other/lib/*/build`. They have all moved:
`build-usockets.sh` copies the tree to `mt_caps_work_dir` first, SDL3/mbedtls/
ftxui/llama build in `mt_caps_work_dir`, and the codecs build in their own
`CACHE_DIR`. MEASURED: a full app build wrote **zero** files inside the engine
checkout and left `git status` clean.

What remains is repo **acquisition** — the guarded submodule init and the
uSockets git restore — which runs only when something is genuinely missing, not
on an ordinary build.

## The two locks

| lock | taken by | protects |
|---|---|---|
| `app-<APP>.lock` | both channels | that app's DerivedData and `$MT_OUT`, which a CLI build and an Xcode build of the *same* app share |
| `ide-channel.lock` | the IDE path only, on top of the app lock | `_current/engine.xcconfig` |

Always **app first, then IDE**: two IDE builds of different apps hold different
app locks and queue on the IDE lock, and a consistent order is what stops them
deadlocking.

### What that buys

- **A CLI build of one private app and an Xcode build of another run at the same
  time.** They hold disjoint locks. Under the single checkout-wide mutex the
  Xcode build queued behind the CLI one, which is the case a maintainer hits.
- Two CLI builds of different apps likewise. MEASURED: two drivers running
  concurrently, both locks held at once, neither log containing `waiting`, both
  builds succeeding.
- Two builds of the **same** app still serialize, whichever channel they come
  from. That is not a limitation to remove — they share one DerivedData.

The dependency stores are serialized separately and far more finely, through
`MTENGINE_LOCK_DIR`: one lock under each selected store, held only while that
store is being synced or built.


## Why not fix it properly instead

The obvious fix — make the xcconfig include path per-build, e.g. under each app's
own `DerivedData` — **is not available in Xcode**, and that is measured rather
than assumed. Apple's own rule explains why:

> references to other build configuration files are processed **before
> interpreting any build settings**

There are no settings yet when the include is resolved, so nothing can expand
there. Nine probes with a literal-path control, all negative:

| form | result |
|---|---|
| `"/abs/path/f.xcconfig"` (control) | **resolves** |
| `$(SRCROOT)` · `${SRCROOT}` · `$SRCROOT` · `<SRCROOT>` | none resolve |
| `$(SOURCE_ROOT)` · `${SOURCE_ROOT}` · `$SOURCE_ROOT` · `<SOURCE_ROOT>` | none resolve |
| `$(VAR)` from an `xcodebuild` command-line setting | does not resolve |
| `$(VAR)` from an environment variable | does not resolve |

`<DEVELOPER_DIR>` is a hardcoded special case for one name — Xcode knows its own
location before parsing anything — not a general mechanism.

### The other two workarounds, and why they also fail

Both get proposed independently, so both are recorded with their measurement
rather than left to be re-derived.

**"Keep the file in the app's own DerivedData / OBJROOT."** Conceptually right —
DerivedData *is* per-app, so there would be no shared state at all. It cannot be
expressed: the include path must be a literal, and a DerivedData path carries a
per-app hash. More fundamentally, `MTEngineSDL.xcconfig` lives in the **engine**
checkout and the path is relative to *that* file, so any literal written there
names the same place for all four apps — wherever that place is.

**"Set the `MT_CAP_*` defines in the app's own `.xcodeproj` instead, and drop the
include."** This one is worth stating precisely, because it sounds like it should
work and the trade it offers (losing the engine's standalone defaults) would be
acceptable. **The trade is not available: an app-project define does not reach
the engine target.** Measured, with a positive control on both sides — a probe
added to `MTEngineSDLDummyApp.xcodeproj`'s build settings, read back from the
real compiler response files:

| target | probe from the app's project | its own define (proving the file is the right one) |
|---|---|---|
| app | **present** | `MT_CAP_LLM=1` |
| engine | **absent** | `MT_ENABLE_LLAMA_CPP=1` |

The engine target lives in `MTEngineSDL.xcodeproj` and resolves settings from
*that* project; a cross-project reference inherits nothing from the depending
project. Only **command-line** settings cross that boundary (spike G Q3) — which
is exactly why `build-macos.sh` works and why the IDE, which has no command line,
needed the shared file in the first place.

So that variant would give the *app* translation units the manifest's set and
leave the *engine* on its defaults: the app/engine disagreement this whole
mechanism exists to prevent, made permanent instead of occasional. Putting the
defines in `MTEngineSDL.xcodeproj` instead is worse again — one tracked file
shared by four apps, last writer wins, inside the checkout THE RULE forbids
writing to.

**"Move `.mtengine-ide/` into the engine checkout and gitignore it."** Considered
and declined on 2026-08-24. It buys one `..` in the include path and costs the
one invariant the concurrency gates assert. THE RULE is explicit that gitignoring
is not the escape hatch — *"tracked **or ignored**"* — and its own rationale names
this exact case: *"A gitignored output at a fixed path is shared mutable state
between four apps."* That is precisely what this file is; the lock exists because
of it. `git status --porcelain --untracked-files=all --ignored` would also start
reporting the directory on every run, which is the check itself, not a cosmetic.

The current location is **forced, not chosen**: the parent of the five checkouts
is the only place a *relative literal* in the engine's tracked xcconfig can reach
that is outside every repo — see the nine include-path probes above. A symlink
inside the checkout pointing out was considered too; the symlink is itself an
in-checkout artefact, so it trades the problem for the same problem plus a moving
part.

If this is ever revisited, the honest order is to amend THE RULE first, with its
reasoning, and only then the code — never to leave code contradicting a rule that
still stands.

The only shape that would genuinely work is each app owning its own copy of the
engine *target* rather than referencing it — four parallel copies of the engine's
source list to keep in step. That cure is worse than the disease.

So serializing is the honest answer, not a shortcut.

## A wrapper build is no longer an aggressor

Since the CLI wrapper supplies the whole resolved set on the `xcodebuild` command
line, and a command-line setting outranks the tracked xcconfig's `#include?`, it
does not depend on `_current/engine.xcconfig`. Xcode may still parse the optional
include, because the tracked engine xcconfig names it, but the command-line
settings win. The wrapper used to write the shared file anyway, which made every
agent build a hazard to any concurrent IDE build for no benefit to itself — two
agents on two apps could silently hand a third app's Xcode build the wrong engine
capability set.

`build-macos.sh` now passes `MT_CAPS_WRAPPER=1` (measured: a command-line build
setting does reach a pre-action, as does an exported environment variable) and the
pre-action skips the IDE resolve and shared-file write. It still calls
`build-lock.sh annotate <app>` so the outer app-lock gains `OBJROOT` for stale
recovery. The post-action also treats `MT_CAPS_WRAPPER=1` as not owning the IDE
channel, so it does not delete `_current/engine.xcconfig` for a file it never
wrote.

`MT_CAPS_APP` had to move onto the command line for this to work. It previously
existed only in the shared file, so a wrapper build that no longer writes that
file would read a **previous** build's app name and the backstop's cross-app check
would fire on a perfectly correct build. It did, the first time this was tried.

**This does not remove the app lock.** It still protects the app's DerivedData and
`$MT_OUT`, which are shared by the CLI and IDE channels for the same app. The
dependency outputs and work directories have moved outside the checkout, but they
are now guarded by their own store locks instead of by the app lock.

So the honest split is:

| risk | status |
|---|---|
| a CLI build silently corrupting an IDE build's capability set | **removed** — the wrapper no longer writes or deletes the shared file |
| concurrent builds fighting over the staged `*.a` | **removed** — the archives moved to `$MT_CAPS_LIBS_DIR`, keyed by capability set |
| concurrent builds fighting over dependency work directories | **moved out of the checkout and guarded per store** |

Repo acquisition is the remaining special case: guarded submodule init and the
uSockets restore write tracked checkout state only when something is missing.

## How it behaves

```sh
tools/mtcaps/build-lock.sh status          # who holds it, and whether it is building
tools/mtcaps/build-lock.sh steal           # manual escape hatch
```

**Three entry points participate**, and all three matter: the central app driver
takes the app lock for CLI builds, the scheme pre-action takes app+IDE locks for
Xcode.app builds, and **MTEngineSDL's own `build-macos.sh`** takes an app-style
lock for standalone engine builds. Store-producing dependency scripts use their
own `MTENGINE_LOCK_DIR` locks. Nested calls pass through on process ancestry, so
an app wrapper driving the engine script is one build, not three.

`--print-settings` deliberately takes no lock: it is a pure query, called from
inside an app's build, where blocking to answer a question would be a deadlock
waiting to happen.

**IDE acquire is in the scheme's build PRE-ACTION.** Measured:
`xcodebuild -scheme` runs pre-actions too, so the wrapper marks that invocation
with `MT_CAPS_WRAPPER=1`. The pre-action then skips the IDE resolve and the
IDE-channel lock, because command-line settings already carry the resolved set.
Without that wrapper marker, the pre-action takes the app lock and then the
IDE-channel lock. It **blocks** rather than failing: two IDE builds should queue,
not error.

**Release has two paths, and neither alone is sufficient:**

- the scheme's build POST-action — measured to run under `xcodebuild -scheme`
  as well, but measured **not** to run when the build **fails**. Wrapper
  post-actions do not release or clean up the IDE channel they never owned;
- the app's `build-macos.sh`, from an `EXIT` trap, which does survive a failure.

**The real guarantee is two liveness probes, because one is not enough.**

*Is the owner process still there?* The pre-action records the pid of the
`xcodebuild`/`Xcode` process it runs under — walked up the process chain, because
a pre-action's own shell dies immediately — and a waiter breaks the lock of a
holder that is gone. This settles every **crash** case exactly: Xcode quits,
`xcodebuild` is killed, an agent hits Ctrl-C. The lock frees on the next poll.

*Is the build actually progressing?* The pid outliving the build is the case the
first probe cannot see — **Xcode.app stays running after a build fails**, and a
failed build never releases (post-actions do not run on failure). So the lock
also records `OBJROOT`, and a waiter breaks it when that build directory has been
idle for `MTENGINE_BUILD_LOCK_IDLE` (default 300s). A running build writes into
its own DerivedData constantly; a dead one stops. The threshold is deliberately
generous — a long link step can go quiet, and breaking the lock of a build that
*is* running is far worse than waiting a few extra minutes for one that is not.

The two probes cover different channels, which is why both are needed:

| channel | who records the pid | which probe decides |
|---|---|---|
| `build-macos.sh` (agent/CLI) | the script's own `$$`, which dies with it | pid liveness, exact |
| Xcode.app (human) | Xcode, which outlives the build | the idle probe |

`MTENGINE_BUILD_LOCK_TTL` (default 1800s) is only the last resort, for when
neither probe can decide — no `OBJROOT` was recorded, or the build directory was
never created. `MTENGINE_BUILD_LOCK_WAIT` (default 3600s) bounds how long a
waiter queues.

`build-lock.sh status` reports both signals (`pid: … (alive|gone)` and
`building: yes|NO`), so a stuck lock can be diagnosed rather than guessed at.

Passthrough is keyed on the **owning process**, not the app name. The wrapper
takes the lock and then runs `xcodebuild`, whose pre-action annotates that same
lock; that is one build and must not deadlock against itself. Two *independent*
builds of the same app are two builds and the second one **queues** — a name-only
test let both proceed.

The dead/idle probes run before passthrough. Xcode.app can outlive a failed
build, and a later build from that same process must refresh the stale lock
rather than pass through it with the old `OBJROOT` still recorded.

Release is stricter still: it requires an **exact** owner match, because ancestry
alone let the nested post-action free the outer wrapper's lock mid-invocation. A
holder whose process is gone is exempt, since its lock is breakable anyway.

`MTENGINE_BUILD_LOCK_GRACE` (default 10s) is how long an owner-less lock
directory is treated as a claim in progress rather than as wreckage.

**`annotate` exists because of an ordering problem.** `build-macos.sh` must take
the app lock *before* `xcodebuild`, and at that moment `OBJROOT` does not exist,
so the lock records no build directory and the idle probe can never decide for
the entire CLI channel. That left the TTL as its only recovery: exactly what the
idle probe was added to remove, still present in the channel whose long clean
build justified adding it. The wrapper pre-action runs *inside* `xcodebuild`,
where `OBJROOT` is real, and fills the field before it exits.

## The net under the lock

The backstop (`xcode-backstop.sh`, a build phase on the engine target) checks
`MT_CAPS_APP` — written into the shared file by the pre-action — against
`OBJROOT`. A cross-project engine target builds into the *depending app's*
DerivedData, whose directory is named after that app, so the two must agree.

This exists because **the agreement check cannot catch cross-contamination**: a
foreign file supplies both the stamp path and the resolved string, so the pair is
internally consistent and passes (measured, exit 0). The `MT_CAPS_APP`/`OBJROOT`
check is the only thing that notices.

It fails **hard**, and says so plainly: the engine translation units were already
configured from the wrong file, so the build cannot be salvaged — wait for the
other build and start this one again.

It **warns instead of failing** wherever it cannot actually tell right from
wrong, and each of those was a real false positive first:

- the build directory is not under `DerivedData/`;
- the component after `DerivedData/` does not look like `<App>-<hash>` — a
  `-derivedDataPath …/build/DerivedData` gives `Build`, and a solitary correct
  build was being told to wait for a build that did not exist;
- `PROJECT_NAME` is unset, so a standalone engine build cannot be told from a
  cross-app one;
- a `_current` file predating `MT_CAPS_APP`.

A standalone engine build that picks up an app's set is its own case: it fails,
but names the stale shared file and gives the one-line `rm -f` rather than
blaming a concurrent build.

Seeing that error at all means the lock was bypassed — a scheme with the
pre-action removed, for instance.

Note that `xcodebuild -target` cannot reach this check: without `-scheme`,
`OBJROOT` is the project's own `platform/MacOS/build`, so the `DerivedData`
branch never matches. `-target` remains the reason the phase exists, but it is
the *stamp re-resolution* below that catches it, not this check.

The IDE post-action deletes `_current/engine.xcconfig` while it still owns
`ide-channel.lock`, before releasing that lock. Wrapper post-actions skip that
cleanup because they never owned the IDE channel. A failed IDE build still leaves
one behind — post-actions do not run then — which is exactly what the standalone
case above detects.

## Wiring a new app

1. Scheme build **pre-action** → `tools/mtcaps/xcode-preaction.sh <app-dir> <App>`
2. Scheme build **post-action** → `tools/mtcaps/xcode-postaction.sh <App>`
   — both need `<EnvironmentBuildable>` naming the app target, or Xcode passes
   them no build settings and `$PROJECT_DIR` is empty. For the post-action that
   fails the whole build, on an action whose only job is cleanup.
3. `build-macos.sh` should be the current thin stub that execs the engine-owned
   `tools/appbuild/app-build-macos.sh`. The central driver acquires the app lock
   with `MT_BUILD_LOCK_PID=$$` before the build and releases from an `EXIT` trap.
   The explicit pid matters: without it the lock records the short-lived helper
   process and looks abandoned to the next waiter.

All four apps need the scheme pre-action and post-action, plus the current thin
stub. The app repos do not own the wrapper-level lock code any more; keeping it
central is what stops one app from drifting out of the lock protocol.
