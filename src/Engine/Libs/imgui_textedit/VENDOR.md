# Vendored: goossens/ImGuiColorTextEdit

    repo   https://github.com/goossens/ImGuiColorTextEdit
    commit a20e4493bb618a8b738bbb33e00932f842ae8595
    date   2026-08-30
    files  TextEditor.{h,cpp} TextDiff.{h,cpp} dtl.h

**Unmodified.** If a local change ever becomes unavoidable, record it here
with the reason, or the next update silently reverts it.

## Why this one

Balázs Jákó's original (2017-2019) is unmaintained and its forks disagree;
Dear ImGui's own "Useful Extensions" wiki lists this rewrite as current.
C++17, STL only, no regex, no boost. It compiles clean against our ImGui
1.93.0 WIP with zero warnings under -Wall -Wextra, in about a second.

## Why TextDiff and dtl.h are here without a caller

TextDiff is a side-by-side diff widget built on the editor; dtl.h is the diff
algorithm only it uses. 78 kB beside the editor's 552 kB, taken so a future
caller need not re-vendor a matching pair. Both .cpp files are compiled so
the uncalled one cannot rot.

## Licences

* TextEditor, TextDiff -- MIT, Copyright (c) 2024-2026 Johan A. Goossens
* dtl.h -- BSD 3-clause, Copyright (c) 2015 Tatsuhiko Kubo
* some unicode algorithms derive from the Unicode Character Database, under
  the Unicode Terms of Use

All permit commercial use. Declared in tools/mtcaps/vocabulary.json as core
dependencies with commercial_safe: true.

## Updating

Refetch the five files at the new commit, update the header above, rebuild,
run DummyApp's suites. Upstream syncs its versioning to Dear ImGui's; check
our ImGui version at the same time.
