# Markdown editor

`src/Engine/Markdown/` holds the engine's Markdown editing model and the live
editor widget built on it. Status: the M0 spike of the Agenda Scene programme;
the architecture is decided, the production items are listed at the end.

## The two classes

**`CMarkdownDocument`** is a Markdown source string plus the structure md4c
finds in it, with everything mapped back to byte ranges of the source:

- `GetBlocks()`: Document, Quote, lists and items, Heading, Code, Html,
  Paragraph, HorizontalRule, Table / rows / cells. Each block has `[beg, end)`
  over the source, its parent, depth and the detail md4c reports (heading
  level, list mark and start, task mark and its offset, fence char and
  language range, column count, cell alignment).
- `GetRuns()`: the content, as md4c's text runs with their source range, the
  inline styles they are inside (emphasis, strong, code, link + href, image,
  strikethrough, underline, math, wikilink) and the kind (normal, code,
  entity, hard/soft break, HTML). A run that md4c synthesizes (the " " of a
  soft break, the "\n" of a hard break, code-block indentation) has
  `beg == end` and carries the text in `synthesized`.
- `IsContentByte(offset)`: a byte inside a run is content; every other byte
  of a block is syntax. That one rule is what the editor hides.
- `Replace(beg, end, text)`: the only mutation. It splices bytes and
  reparses. `SetHeadingLevel()` and `ListContinuationPrefix()` are built on it
  and document their exact byte effect.
- Lookups: `FindBlockAt`, `TopLevelOf`, `Next/PrevTopLevel`, `RowsOf`,
  `CellsOf`, `ColumnOf`, line and UTF-8 code point helpers.

**`CMarkdownLiveEditor`** is a Dear ImGui widget over a `CMarkdownDocument`.
`Render(id, size)` draws the document rendered -- headings at
`SetHeadingScale()` sizes, emphasis through `SetFonts()`, bullets and numbers,
tables as a grid with the delimiter row hidden -- except the block the caret
is in, which shows its source with syntax dimmed. In a table the reveal unit
is the cell, so the grid stays while a cell is edited. Mouse places the caret
and drags a selection; keyboard: arrows (across wrapped lines and blocks),
Home/End, Shift for selection, Ctrl/Cmd+A/C/X/V, Ctrl/Cmd+Z and
Ctrl/Cmd+Shift+Z (grouped by 500 ms and adjacency), Ctrl/Cmd+1..6 heading,
Ctrl/Cmd+Shift+0 paragraph, Ctrl/Cmd+B / I toggles, Enter with list
continuation (Enter on an empty item ends the list), Tab / Shift+Tab between
cells, Enter to the cell below. Cmd replaces Ctrl on macOS through ImGui's
`ConfigMacOSXBehaviors`.

**`CGuiViewMarkdownEditor`** (`GUI/Controls`) is the window: it owns an
editor, fills its window with it and takes the fonts from `CGuiFontManager`
when `LoadMarkdownFonts()` was called, else the current font. Reach anything
else through `GetEditor()`.

## Why this shape

Three editor architectures exist in the wild: block widgets backed by source
(Typora, MarkText), a rich document tree serialized to Markdown (ProseMirror,
Milkdown, Tiptap), and source-is-truth with decorations near the caret
(Obsidian Live Preview, Zettlr). The third was chosen:

- No serializer. An edit is a byte replacement, so the round trip is exact by
  construction. Every surveyed editor of the other two classes has documented
  rewrites on save (indentation, quotes, escapes).
- Immediate mode. ImGui redraws from (source, caret) each frame anyway; there
  is no retained DOM to reconcile, which is where the other two spend their
  complexity.
- It reuses the engine's parser. md4c is already vendored (imgui_md uses it).

The cost is the known caret jump when markers appear and disappear around the
caret. The spike reveals a whole top-level block (a whole list) rather than a
line or a span; narrowing that is a production item, not a redesign.

## Where the ranges come from

md4c reports no offsets (mity/md4c#91, declined upstream). `CMarkdownDocument`
derives them, documented at the top of its .cpp: text runs come as pointers
into the input buffer, so content bytes are exact; a leaf block is its content
widened to full lines, then to the lines md4c never names (fences, setext
underline, table delimiter row); an empty block is placed on the first
non-blank line after the previous block; a container is the union of its
children. `CTestMarkdownDocument` pins every one of these cases. If md4c is
upgraded, re-run it: the set of literals md4c passes out-of-buffer is what the
synthesized-run detection relies on.

## Tests

- `CTestMarkdownDocument` (`src/Engine/Tests`): model, ranges, content mask,
  edits, degenerate inputs. Register it in a host's `CTestSuite`.
- `RegisterMarkdownEditorTests(engine)` (`CImGuiMarkdownEditorTests.h`):
  imgui_test_engine tests that create one `CGuiViewMarkdownEditor`, add it to
  `guiMain` and drive it with real input. Register next to the host's own.

## Production items (not in the spike)

- Incremental reparse: the whole buffer is reparsed on every edit. md4c is
  linear-time and agenda documents are small, but a long document deserves a
  per-block reparse.
- Reveal granularity: line or span instead of top-level block; the caret
  jump on reveal.
- Images (alt text is drawn), link activation and hover, entities (drawn as
  written), IME composition display, word jumps, find/replace, mouse word
  selection, autoscroll while dragging.
- Table authoring: insert/delete row and column, alignment, column resize;
  the model has the ranges, the commands are not written.
- `MD_FLAG`s beyond `MD_DIALECT_GITHUB` (math, wiki links, underline) are
  parsed when set but drawn as plain styled text.
