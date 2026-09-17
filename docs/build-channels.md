# Why a build recompiled everything

Three separate causes, found 2026-09-09/10 while chasing one complaint: *"I have
to rebuild the WHOLE project in Xcode after ANY code or test run by the agent."*
Two are fixed. The third is structural and only bites if you alternate the two
build channels.

Measured throughout on c64d, one shared DerivedData, 529 engine TUs and 484 app
TUs.

## 1. The engine revision in `MT_CAPS_OUT` -- FIXED, and this was the big one

`out_dir()` carried `engine_rev()` as a path segment, and `engine_rev()` is
`<short-HEAD>[-dirty-<sha256 of git status + git diff HEAD>]`. So the path moved
on every engine commit **and on every uncommitted edit**, by anyone.

`MT_CAPS_OUT` is written into the generated xcconfigs that both capability
channels hand to Xcode. Nothing reads its value at compile time -- which is why
"it has zero references in project.pbxproj" looked like an alibi -- but a
changed xcconfig is a changed configuration for every target that includes it.
So the app AND the engine recompiled.

Two consecutive no-change builds, same channel, measured before the fix:

```
first : 111s, 802 TUs   MT_CAPS_OUT  bc93e327-dirty-da433333 -> ef700aab
second: 122s, 529 TUs   MT_CAPS_OUT  ef700aab -> ef700aab-dirty-ab2daa1d
```

Four apps share one engine checkout, so an edit made for one app rebuilt the
other three. One app's capabilities had not changed in months and it rebuilt
anyway.

**What the key was for had already left.** The rationale in `engine_rev()` is
stale-archive reuse -- but the third-party archives moved to `deps_dir`, keyed
by the capability set (`caps-lib.sh`: *"engine revision is worse than
irrelevant, it changes on every commit and on every dirty edit"*), and the
objects and generated `include/` moved to `build_dir` in 2026-09-01 for exactly
the same reason one level down. Measured across all four apps afterwards, the
rev-keyed tree held **zero** `.a` archives: only fragments and stamps, whose
content depends on the resolved set and not on the revision, plus dSYMs.

`out_dir()` is now rev-free like `build_dir()`. After the fix:

```
no change            5s, 0 TUs
EDIT A FILE IN THE ENGINE, then build   4s, 0 TUs
```

Disk was the smaller half: 57 revision directories for c64d, 63 for another
private app,
29.9 GB of cache against 4.2 GB of actual shared dependency archives.
`tools/appbuild/mtengine-gc.py --prune` reclaimed 25.1 GB.

## 2. A duplicated `-I` in the wrapper -- FIXED (9d675d16)

The wrapper passed `HEADER_SEARCH_PATHS="<caps include>" $(inherited)`, and
every target's own `HEADER_SEARCH_PATHS` already begins with
`$(MT_CAPS_INCLUDE_DIR)`. So a wrapper build compiled with the capability
include in the list twice -- a different `-I` list on every TU, and XCBuild
signs each compile task with its command line.

It now passes `MT_CAPS_INCLUDE_DIR`, exactly as the pre-action does. Verified
with `-showBuildSettings`: the effective `HEADER_SEARCH_PATHS` is byte-identical
between the channels.

## 3. The shared engine xcconfig -- STRUCTURAL, costs only on a channel switch

`MTEngineSDL.xcconfig` `#include?`s `.mtengine-ide/_current/engine.xcconfig`.
The IDE channel creates it at pre-action and deletes it at post-action; a
wrapper build never writes it. Its **presence** differs between the channels,
which is a change of configuration whatever the values say -- so the engine
target's 529 TUs recompile when you alternate them. The values are not the
problem: the settings keys, their values and the resulting `clang` command line
are all identical, measured.

It cannot be fixed by having the wrapper write the file:

- the `#include?` path cannot be made per-app (Xcode resolves includes before
  interpreting any build setting -- nine probes, all negative), so the engine
  channel must be one shared file;
- deletion is what stops a standalone `xcodebuild -scheme MTEngineSDL` picking
  up an app's capability set, which `xcode-backstop.sh` fails the build over;
- the wrapper keeps off the file so the app lock and the IDE-channel lock stay
  **disjoint** -- deliberately, so a CLI build of one app runs beside an Xcode
  build of another. Having the wrapper take the IDE lock would undo that.

Giving each app its own engine checkout would make the file per-app and unblock
"stop deleting it". That is a decision about the five-repo programme, not a
build fix.

## The two channels, and which to use

**The wrapper.** `./build-macos.sh` -> `tools/appbuild/app-build-macos.sh`.
Resolves `mtengine.caps` and passes the whole set on the `xcodebuild` command
line, where it outranks every xcconfig. Sets `MT_CAPS_WRAPPER=1`, which tells
the pre-action to skip its own resolve.

**The IDE.** Xcode's Build, or `xcodebuild -scheme` from a terminal -- the
scheme's build pre-action resolves the manifest and writes the xcconfigs that
tracked xcconfigs `#include?`. A terminal `xcodebuild -scheme` **is** this
channel (verified: the log carries `mtcaps pre-action: <app> -> ...` and
`MT_CAPS_SUPPLIED=YES`). It is not the bare `xcodebuild` the app guidance files
forbid; that warning is about invoking xcodebuild with **no** capability
channel, which links against a caps-keyed libs directory built with other
settings and dies on missing `_mbedtls_*`.

With #1 fixed, ordinary work no longer forces rebuilds whichever channel you
use. #3 still makes alternating them cost the engine target, so an agent
working alongside a maintainer should stay on the channel the maintainer uses
(c64d's `build-macos-ide.sh` wraps it), and run tests with
`tests/run_test.sh --skip-build` -- the runner otherwise builds through the
wrapper and puts the switch back.

`./build-macos.sh` remains the tool for what only it does: the first build in a
fresh clone (it stages the vendored uSockets and the dependency archives), any
change to `mtengine.caps` / tier / `--logs` / `--symbols`, and `--prod` or
`--clean`.
