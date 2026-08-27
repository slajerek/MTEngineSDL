# Vendored SDL 3.4.14 — what was changed and why

Vendored 2026-08-18 for the S-2 SDL3 upgrade
(`PhotoCruise/specs/superpowers/plans/2026-08-17-s2-sdl-3.4.14-upgrade.md`).

## Provenance

| Channel | Artifact | SHA256 |
|---|---|---|
| Release asset | `SDL3-3.4.14.tar.gz` | `30d4aa2b3037718142b32dffd4e72f917ebb6cc5227150e7bb9c45efb2153aeb` |
| Tag archive | `release-3.4.14.tar.gz` | `9d57b178fb297e121ef2605275937b7afaa7cd24d99ce1f95953e69e7a2535d6` |

**The two hashes differ, and that is expected — it is not a mismatch.** GitHub's
tag archive is regenerated from the tree and carries `.github/` + `.gitignore`;
the release tarball carries `.git-hash` and `REVISION.txt` and has its
`SDL_revision.h` stamped. The verification is `diff -rq` **between the extracted
trees**, which returned exactly those five metadata differences and nothing
else: every source file, header and CMake file is byte-identical across the two
channels. This copy is the **release asset** (`release-3.4.14-0-g147a8ee32`).

Two independent channels agreeing is the standard here — the same method the
libpng 1.6.58 upgrade used.

## Local changes to the vendored tree

**Two file renames. No source changes.**

| Upstream name | Renamed to | Why |
|---|---|---|
| `CLAUDE.md` | `SDL-UPSTREAM-CLAUDE.md` | see below |
| `AGENTS.md` | `SDL-UPSTREAM-AGENTS.md` | see below |

SDL ships contributor instructions at its repo root under both names. Claude
Code and similar tools **auto-load `CLAUDE.md`/`AGENTS.md` from the directories
they are working in**, so a vendored copy at
`other/lib/SDL-release-3.4.14-static/CLAUDE.md` would silently become
instructions for anyone touching this tree — inside a repo that has its own
`CLAUDE.md` saying something different. Renaming keeps the text and its
provenance while stopping it being executed as policy for *our* repo.

**Their content still matters, and it is this:** *"AI must not be used to
generate code for contributions to this project."* That is SDL's rule for
**contributions upstream to SDL**. It does not restrict using SDL as a
dependency, which is all we do. But it does mean: **if we ever fix something in
this vendored tree and want to send it to SDL, the patch must be written by
hand.** Local-only patches are a different thing — and if any are ever added
here, they get a row in the table above, the way `MTENGINE_PATCHES.md` does for
ImGui.

## Build

`platform/MacOS/build-sdl3.sh` produces `platform/MacOS/libs/libSDL3.a`
(universal arm64 + x86_64, static). Windows `.lib` files are **checked into
git** under `platform/Windows/libs/<Platform>/<Config>/` and must be built on a
Windows machine — see the plan's Task 2 Step 3.

**`-DSDL_STATIC=ON -DSDL_SHARED=OFF` are not optional.** SDL3's CMake defaults
to building the shared library ONLY when `BUILD_SHARED_LIBS` is undefined
(`CMakeLists.txt` ~line 220, *"Default to just building the shared library"*),
so a plain `cmake ..` produces no static archive at all.
