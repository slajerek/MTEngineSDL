# Changelog

## Version numbers

**Odd minor versions are development; even minor versions are stable.**

- `3.21`, `3.23`, … live on `devel`. They are where work lands, and they may
  change under you: an interface added in one commit can be reshaped in the
  next.
- `3.22`, `3.24`, … live on `master`. A stable release is made by merging
  `devel` into `master` and bumping to the next even number; nothing goes
  onto `master` that has not been built and tested on macOS, Linux and
  Windows first.

An app built on this engine follows the same pairing: its `devel` tracks the
engine's `devel`, its `master` the engine's `master`.

The convention starts at 3.21. Earlier numbers below predate it and carry no
stability meaning.

---

## 3.21 — development

The capability system, and a build that keeps its output out of your checkout.

**Capabilities.** What the engine compiles in is chosen per app rather than
fixed here. An app declares a manifest naming what it needs — video playback,
photo codecs, LLM inference, HTTPS, websockets, MIDI, the terminal, the test
engine — and the build resolves that into compiler defines, the set of
dependencies to acquire, and the licence documents to ship. A capability that
is off costs nothing: its dependency is never built and its code is never
compiled. `tools/mtcaps/vocabulary.json` is the authoritative list, with each
capability's dependencies, licences and acquisition scripts;
`tools/mtcaps/tests/test_mtcaps.py` is its test suite and runs standalone.

**The engine owns the build flow.** `tools/appbuild/app-build-{macos,linux}.sh`
and `app-build-windows.ps1` carry it — resolve, dependency acquisition, engine
build, app build, licence gate, symbols. An app repository keeps parameters and
a thin stub per platform, not a copy of the machinery. `--set KEY=VALUE`
(`-Set` on Windows) overrides a capability for one build without editing a
tracked manifest.

**Nothing a build produces is written inside the checkout.** Objects,
dependency archives, generated headers, symbols and binaries live under a cache
root outside it: `${XDG_CACHE_HOME:-~/.cache}/mtengine`, or
`%USERPROFILE%\.cache\mtengine` on Windows. `MTENGINE_BUILD_ROOT` overrides it.
`tools/appbuild/mtengine-gc.py` reports on that cache and prunes it, keeping
the revision a build would write now plus the newest few per app.

**Dependencies are cached by what they actually depend on.** The cache key is
the capabilities that own an acquisition script, so flipping a capability no
dependency has ever heard of no longer rebuilds SDL3, FFmpeg and llama.cpp to
produce the same bytes. A test scans every dependency script for the capability
flags it really reads and fails if one falls outside the key.

**Visual Studio builds what the command line builds.** The IDE path reads the
resolved capability set through `platform/Windows/MTEngineApp.targets`, which an
app imports rather than copies.

**Windows dependencies are built from source.** SDL3, FreeType, libuv and
uSockets are compiled by `platform/Windows/build-*.ps1`; the tracked prebuilt
archives are gone, as is SDL2, which nothing linked.

**Fixes.** libuv's `src/{unix,win}/core.c` are in the tree again — an
unanchored `core.*` ignore rule had been matching them, and on a
case-insensitive filesystem `src/Engine/Core/` and SDL's `src/core/` with
them. A log written without `--log-dir` goes to the system temp directory
rather than the current one. Two absolute paths under a developer's home
directory left the sources. Building two checkouts of the engine on one
machine no longer collides over the shared dependency work trees.

## 3.20 — 2026-08-31

Capability programme, first public cut: the engine-owned build drivers,
dependency archives relocated outside every checkout, and the Windows
from-source dependency builds.

## 3.19 — 2026-08-05

`.gitignore`: SDL2's `src/core/` was being swallowed by the legacy `core`
rule. CI on macOS and Windows builds static SDL2 from the vendored source;
clang `_m_prefetch` redefinition fixed.

## 3.18 — 2026-06-04

Windows dependency builds auto-detect the Visual Studio CMake generator
instead of hardcoding one. `SYS_Defs.h` includes `<stdint.h>` so fixed-width
types resolve on GCC 13 and later. `CTestSuite`/`CTest` became the shared base
for app subclasses. ggml native CPU optimisations disabled on ARM.

## 3.16 — 2025-12-24

## 3.15 — 2025-02-22

Event ordering fixed so a key shortcut cannot consume an event before the
focused view sees it.

## 3.14 and earlier

See the commit history.
