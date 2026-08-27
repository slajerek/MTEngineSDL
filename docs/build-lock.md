# The build lock — why four apps cannot build this engine at once

Four apps share one MTEngineSDL checkout. `tools/mtcaps/build-lock.sh` serializes
their builds. This document is why it exists, what it does not fix, and what an
agent driving several apps needs to know.

## What actually collides

Two things, and only the first is about capabilities:

**1. `.mtengine-ide/_current/engine.xcconfig`.** The engine target cannot know
which app is building it, so the IDE capability channel writes one file and the
most recent writer wins. A second app's build landing between this build's
pre-action and its settings resolution hands the engine target the *other* app's
capability set — while the app target, which reads a per-app file, gets the right
one. The result is a binary whose app and engine translation units disagree about
capabilities, with no error.

**2. The engine's own in-checkout writes.** `build-macos.sh` builds vendored
uSockets *inside* the checkout, and the six dependency scripts configure and
compile under `other/lib/*/build` and install into `other/lib/*/install` — all
inside the checkout, all shared by the four apps. Two concurrent `make` runs in
one source tree collide regardless of capabilities. MEASURED: a second build's
`rm -rf` of the shared libvpx build directory makes the first one fail with
`vpx_config.h file not found`.

The finished *archives* no longer live in the checkout — they moved to
`$MT_CAPS_LIBS_DIR` (see below) — but the intermediate trees that produce them
did not, so this remains a standing THE RULE violation, recorded in the
modularization tracker. The lock contains the damage; it does not fix it.

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
still stands in `CLAUDE.md`.

The only shape that would genuinely work is each app owning its own copy of the
engine *target* rather than referencing it — four parallel copies of the engine's
source list to keep in step. That cure is worse than the disease.

So serializing is the honest answer, not a shortcut.

## A wrapper build is no longer an aggressor

Since the CLI wrapper supplies the whole resolved set on the `xcodebuild` command
line, and a command-line setting outranks the tracked xcconfig's `#include?`, it
**never reads** `_current/engine.xcconfig`. It used to write it anyway, which made
every agent build a hazard to any concurrent IDE build for no benefit to itself —
two agents on two apps could silently hand a third app's Xcode build the wrong
engine capability set.

`build-macos.sh` now passes `MT_CAPS_WRAPPER=1` (measured: a command-line build
setting does reach a pre-action, as does an exported environment variable) and the
pre-action skips writing the shared file. Verified: with a foreign `_current` in
place, a CLI build succeeds *and* leaves the file byte-identical.

`MT_CAPS_APP` had to move onto the command line for this to work. It previously
existed only in the shared file, so a wrapper build that no longer writes that
file would read a **previous** build's app name and the backstop's cross-app check
would fire on a perfectly correct build. It did, the first time this was tried.

**This does not remove the lock**, and the reason is worth being precise about:
all six of the engine's dependency phases build inside the checkout. Their
outputs now go to `$MT_CAPS_LIBS_DIR` —

```
Build llama_cpp    -> $(MT_CAPS_LIBS_DIR)/libllama_cpp.a
Build mbedtls      -> $(MT_CAPS_LIBS_DIR)/libmbedtls_bundle.a
Build ftxui        -> $(PROJECT_DIR)/libs/libftxui.a
Build image_codecs -> $(PROJECT_DIR)/libs/libmt_image_codecs.a
Build video_codecs -> $(PROJECT_DIR)/libs/libmt_video_codecs.a
Build SDL3         -> $(PROJECT_DIR)/libs/libSDL3.a
```

— inside the engine checkout, shared by all four apps, and **those archives differ
per capability set**: one app with `MT_CAP_LLM=1` and another with `0` genuinely
fight over `libllama_cpp.a`. Plus `build-macos.sh` builds vendored uSockets in the
checkout's own source tree.

So the honest split is:

| risk | status |
|---|---|
| a CLI build silently corrupting an IDE build's capability set | **removed** — the wrapper no longer writes the shared file |
| concurrent builds fighting over the staged `*.a` | **removed** — the archives moved to `$MT_CAPS_LIBS_DIR`, keyed by capability set |
| concurrent builds fighting over `other/lib/*/build` and the vendored uSockets tree | **serialized by the lock**, not fixed |

Moving the six archives out was the first half of that work and is done. Making
three builds actually run *in parallel* now needs the second half: the
intermediate trees — `other/lib/*/build`, `other/lib/*/install`, and the
vendored uSockets source tree — are still inside the checkout and still shared,
which is the remaining deviation recorded in the modularization tracker. That is
not something the lock can substitute for. The lock makes concurrency **safe**;
it does not make it **parallel**.

## How it behaves

```sh
tools/mtcaps/build-lock.sh status          # who holds it, and whether it is building
tools/mtcaps/build-lock.sh steal           # manual escape hatch
```

**Three scripts take it**, and all three matter: an app's `build-macos.sh`, the
scheme pre-action, and **MTEngineSDL's own `build-macos.sh`** — that last one
because it is the script that actually writes inside the checkout (`make` in the
vendored uSockets tree, and the codec scripts' `other/lib/*/build`), and running it
directly is documented usage. Nested calls pass through on process ancestry, so
an app wrapper driving the engine script is one build, not three.

`--print-settings` deliberately takes no lock: it is a pure query, called from
inside an app's build, where blocking to answer a question would be a deadlock
waiting to happen.

**Acquire is in the scheme's build PRE-ACTION**, which is the one choke point
both channels pass through — measured: `xcodebuild -scheme` runs pre-actions too,
so a script build queues behind an IDE build and vice versa. It **blocks** rather
than failing: three agents on three apps should queue, not error.

**Release has two paths, and neither alone is sufficient:**

- the scheme's build POST-action — measured to run under `xcodebuild -scheme`
  as well, but measured **not** to run when the build **fails**;
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
takes the lock and then runs `xcodebuild`, whose pre-action asks for it again;
that is one build and passes through. Two *independent* builds of the same app
are two builds and the second one **queues** — a name-only test let both proceed,
straight into the concurrent `make` in the vendored uSockets tree.

Release is stricter still: it requires an **exact** owner match, because ancestry
alone let the nested post-action free the outer wrapper's lock mid-invocation. A
holder whose process is gone is exempt, since its lock is breakable anyway.

`MTENGINE_BUILD_LOCK_GRACE` (default 10s) is how long an owner-less lock
directory is treated as a claim in progress rather than as wreckage.

**`annotate` exists because of an ordering problem.** `build-macos.sh` must take
the lock *before* `xcodebuild` — it stages dependencies into the checkout first —
and at that moment `OBJROOT` does not exist, so the lock records no build
directory and the idle probe can never decide for the entire CLI channel. That
left the TTL as its only recovery: exactly what the idle probe was added to
remove, still present in the channel whose long clean build justified adding it.
The pre-action runs *inside* `xcodebuild`, where `OBJROOT` is real, and fills the
field in as it passes through.

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

The post-action deletes `_current/engine.xcconfig` when the build succeeds, so
the shared file exists only while a build needs it. A failed build still leaves
one behind — post-actions do not run then — which is exactly what the standalone
case above detects.

## Wiring a new app

1. Scheme build **pre-action** → `tools/mtcaps/xcode-preaction.sh <app-dir> <App>`
2. Scheme build **post-action** → `tools/mtcaps/xcode-postaction.sh <App>`
   — both need `<EnvironmentBuildable>` naming the app target, or Xcode passes
   them no build settings and `$PROJECT_DIR` is empty. For the post-action that
   fails the whole build, on an action whose only job is cleanup.
3. `build-macos.sh` → acquire with `MT_BUILD_LOCK_PID=$$` before the build and
   release from an `EXIT` trap. The explicit pid matters: without it the lock
   records the short-lived helper process and looks abandoned to the next waiter.

All four apps now have the scheme pre-action and post-action. Only
`MTEngineSDLDummyApp` also has step 3 — the other three `build-macos.sh` scripts
do not take the lock themselves, so their CLI builds are protected only from the
pre-action onward, and the dependency staging that runs before `xcodebuild`
is not covered.

An earlier version of this section said those three had *none* of it. That was
wrong in the way that mattered: they had the pre-action, which **acquires**, and
no post-action, which **releases** — so every IDE build of them took the lock and
left it, and never cleaned up the shared engine xcconfig either. Taking a lock
without releasing it is worse than not taking one.
