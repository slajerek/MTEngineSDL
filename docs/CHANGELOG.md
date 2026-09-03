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

## 3.21.1 — development

Build-system work on top of 3.21, and the first Windows fixes that came out of
building every host application on that platform. No engine source behaviour
changes on macOS or Linux; the D3D11 addition is new code on a path that
previously had none.

**A build store per dependency.** Dependency archives used to be keyed by the
whole acquisition capability set: switching one capability off moved the bucket
for *every* dependency, so changing a terminal-UI library rebuilt SDL3, mbedTLS,
the image codecs and FFmpeg from scratch to produce the same bytes. Each
dependency now builds and stamps in a store keyed only by the capabilities it
actually reads, then copies its outputs into the same shared `libs` directory
consumers link against — nothing downstream changed. Dependencies that read no
capability at all (SDL3, FreeType, libuv, uSockets) get one bucket per machine,
shared by every application on it. Measured: turning FTXUI off rebuilds FTXUI
and nothing else.

**The FFmpeg decoder policy has one home.** The decoder, parser and demuxer
lists live in `tools/mtcaps/vocabulary.json` and reach the codec scripts as
resolved values; the scripts no longer carry their own copies. In the same
change the built decoder set follows the *resolved build mode* rather than the
licence tier, which are not the same question — a non-commercial tier can
legitimately resolve to a restricted decoder set.

**A release package on every platform.** `platform/<Platform>/prod/<arch>/`
holds the application, its assets and `LICENSES.txt`, on macOS and Linux as it
already did on Windows, and the deploy step fails rather than shipping a package
without its licence document. Applications are tested *from* that package,
because an engine application resolves its assets through the working directory
and never through the executable's own location.

**Windows.** The generated CMake fragment could not carry a Windows path: a
backslash in a quoted value is a character escape, so the line was a parse error
that took the whole fragment down. An IDE build compiled applications with no
capability defines at all. An IDE build had no way to populate its dependency
directory short of building the dependencies inside Visual Studio, and now syncs
them from the stores instead (`platform/Windows/sync-deps-view.ps1`). IDE and
command-line builds no longer write their executable to the same path, where
each could see the other's as up to date. The app-side MSBuild properties moved
into the engine (`platform/Windows/MTEngineApp.props`), leaving an application
with a stub that declares its identity.

**D3D11: the masked-tile shader.** It had been left unimplemented on this
backend with a note that every caller drew an unshaded fallback; one did not.
Ported from the GLSL original deliberately without its Y flip, since
`SV_Position` is already top-left where `gl_FragCoord` is bottom-left.

**Cache maintenance.** `tools/appbuild/mtengine-gc.py` understands the per-unit
stores and keeps them per dependency rather than globally. Its liveness check
resolved without engine options, which put every path it computed under a
backend segment that no build on an ARM machine ever writes to — it protected
directories that did not exist and could have deleted a live bucket.

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
