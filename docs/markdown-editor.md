# Markdown editor

`src/Engine/Markdown/` holds the engine's Markdown editing model and the live
editor widget built on it; `GUI/Controls/CGuiViewMarkdownEditor` is its
window. Status: the editor foundation (M2 of the Agenda Scene programme).
What is not done yet is listed at the end.

## The classes

**`CMarkdownDocument`** is a Markdown source string plus the structure md4c
finds in it, with everything mapped back to byte ranges of the source:

- `GetBlocks()`: Document, Quote, lists and items, Heading, Code, Html,
  Paragraph, HorizontalRule, Table / rows / cells, each with `[beg, end)`
  over the source, its parent, depth and md4c's detail (heading level, list
  mark and start, task mark and its offset, fence char and language range,
  column count, cell alignment).
- `GetRuns()`: the content, as md4c's text runs with their source range, the
  inline styles they are inside, their kind (normal, code, entity, hard/soft
  break, HTML) and their innermost span. A run md4c synthesizes (a soft
  break, a hard break, code indentation) has `beg == end` and carries its
  text in `synthesized`.
- `GetSpans()`: inline spans (emphasis, strong, code, link, image,
  strikethrough, underline, math, wikilink) with their range including the
  markers, their content range and their destination. `SpansTouching()`,
  `FindSpanAt()`.
- `IsContentByte(offset)`: a byte inside a run is content; every other byte
  of a block is syntax.
- `Replace(beg, end, text)`: the only mutation. It splices bytes and reparses
  **incrementally** when that is provably the same as a full parse (see
  below). Built on it, each documenting its exact byte effect:
  `SetHeadingLevel()`, `ToggleTask()`, `EditTable()` (insert, delete, move
  rows and columns, alignment), `ListContinuationPrefix()`.
- Lookups: `FindBlockAt`, `TopLevelOf`, `Next/PrevTopLevel`, `RowsOf`,
  `CellsOf`, `ColumnOf`, `FindTaskItemAt`, line and UTF-8 helpers.

**`CMarkdownLiveEditor`** is a Dear ImGui widget over a `CMarkdownDocument`.
`Render(id, size)` draws the document rendered -- headings at
`SetHeadingScale()` sizes, emphasis through `SetFonts()`, numbers, drawn
bullets and task boxes, tables as a grid with column alignment, quote bars,
code backgrounds, pictures through the host's `imageResolver` -- except the
**reveal unit** the caret is in, which shows its source:

- The unit is the innermost leaf: a paragraph, a heading, a code block, a
  rule, one list item's own line, a table cell (a table outside its cells
  reveals whole). Units partition the source -- each owns the bytes up to the
  next unit's start -- so blank lines and container syntax always have a
  caret position. The rest of a list, quote or table stays rendered.
- Inside the unit, block markers (`# `, `- `, `> `, fences) show dimmed;
  inline markers (`**`, `` ` ``, `[`, `](url)`) show only for the spans the
  caret touches. `SetRevealAllInlineSyntax(true)` shows them all.

Interaction: mouse caret and drag selection; a click on a task box toggles
it in place; Ctrl/Cmd+click opens a link (`onLinkActivated`, default the
platform shell) and hovering shows its destination; right-click in a table
opens the table menu. Keyboard: arrows across wrapped lines and units,
Home/End, Shift to select, Ctrl/Cmd+A/C/X/V, Enter with list continuation,
Shift+Enter, Tab / Shift+Tab between cells (Tab in the last cell adds a row),
Enter to the cell below, plus the remappable commands below. Ctrl/Cmd+F
opens the find bar above the text: matches in the source, highlighted, the
current one selected; Enter / Shift+Enter, F3 / Shift+F3, Esc.

**`CMarkdownEditorBindings`** holds the remappable commands as
`CSlrKeyboardShortcut` objects in their own zone (`kZone`), registered with
`guiMain->keyboardShortcuts` when an editor first renders; a host's shortcut
screen lists, remaps and stores them with the rest. CGuiMain dispatches only
the GLOBAL zone itself, so these act only in a focused editor, which turns
each binding into an ImGui chord every frame (a change applies from the next
frame). Defaults: Ctrl/Cmd+B, I, K (link), 1..6 (headings), Shift+0
(paragraph), Z, Shift+Z and Y (undo/redo), Enter (task box, or a table row
below), F (find), F3 / Shift+F3, Alt+Shift+arrows (move table row/column).
Movement, deletion, Enter, Tab and the clipboard are fixed.

**`CGuiViewMarkdownEditor`** is the window: it owns an editor, fills its
window with it and takes fonts from `CGuiFontManager` after
`LoadMarkdownFonts()`, else the current font (then bold and italic look
like regular text). Reach anything else through `GetEditor()`.

## Undo

Every edit is one `CUndoAction` in a Tools/Undo `CUndoManager` -- the
editor's own, or the host's through `SetUndoManager()`, so document edits
share the app's history. A burst of typing, Backspace or forward Delete
within 500 ms grows the action on top of the manager in place
(`CUndoManager::PeekUndo()`); a command made of several replacements (an
emphasis toggle, a table edit) is one action. Actions reach the editor
through a weak handle: an editor that is destroyed, or loads a new source,
leaves no-op actions in a shared manager.

`MT_CAP_UNDO` gates this. Its symbol scan rejects any `CUndoManager` symbol
in a build without the capability, so the editor references the class only
under `MT_ENABLE_UNDO` and `SetUndoManager()` is inline. Without the
capability the editor works and records nothing.

## Text input and IME

While focused and editable, the editor publishes ImGui's platform IME data
at the caret every frame (as `InputText` does). That is what makes the SDL3
backend start text input at all, and it places the OS composition and
candidate windows at the caret. Inline preedit drawing is not done: claiming
IME UI would apply to every text field of the app.

## Strings

Labels the editor draws (link hint, find bar, table menu) and the command
names in `CMarkdownEditorBindings` have translation keys
(`markdown.editor.*`) and English fallbacks. A host installs
`CMarkdownLiveEditor::SetTranslateFunction()` to translate them, the pattern
of the engine's media views.

## Why this shape

Three editor architectures exist: block widgets backed by source (Typora,
MarkText), a rich document tree serialized to Markdown (ProseMirror,
Milkdown), and source-is-truth with decorations near the caret (Obsidian
Live Preview, Zettlr). The third was chosen: an edit is a byte replacement,
so the round trip is exact by construction (no serializer to rewrite the
user's formatting); ImGui redraws from (source, caret) each frame, so there
is no retained DOM to reconcile; and it reuses md4c, already vendored.

## Where the ranges come from

md4c reports no offsets (mity/md4c#91). Content runs are pointers into the
input buffer, so content bytes are exact. A leaf is its content widened to
full lines, then by the lines that hold only syntax: md4c's line breaks
before and after the content say how many, code spans and links extend to
their closing backticks or `](dest)`, a fenced block to its fence past blank
lines, a setext heading to its underline, a quote to its marker-only lines.
Empty items and quotes are placed in document order on the first non-blank
line after the previous block. A container is the union of its children.
Spans are closed with their block: md4c can pair a `~` with one inside a
later link destination and emit the span's end in another block.
`CTestMarkdownDocument` pins all of this.

## Incremental reparse

`Replace()` reparses the top-level blocks an edit touches plus one
neighbour on each side and the gaps between, then splices blocks, runs and
spans and shifts the rest. It falls back to a full parse when the neighbours
do not come back with the same type and range, or when the document may
contain link reference definitions (document-global). A randomized test in
`CTestMarkdownDocument` applies 3000 edits drawn from Markdown syntax
characters and compares every result with a full parse, field by field; 301
seeds were run during development. Typing in a 57 KB document costs about
0.13 ms per keystroke against 8.2 ms for a full parse.

## Tests

- `CTestMarkdownDocument`: model, ranges, spans, table edits, the randomized
  incremental-vs-full comparison, degenerate inputs.
- `CTestMarkdownLiveEditor`: the command layer without a window -- undo
  (grouping, compound commands, a shared host manager, orphaned actions),
  the reveal partition over every offset, tasks, links, table commands,
  bindings, find.
- `RegisterMarkdownEditorTests(engine)`: imgui_test_engine tests that drive
  one `CGuiViewMarkdownEditor` with real input, and one that captures the
  window in four states and checks the pixels (set `MT_MD_CAPTURE_DIR` to
  keep the captures as raw RGBA files).

Register the first two in a host's `CTestSuite` and the third next to its
own UI tests.

## Not done yet

- Inline preedit (see Text input), word-wise caret movement and selection,
  double-click word selection, autoscroll while dragging, replace in find.
- Entities are drawn as written; math, wikilinks and underline are parsed
  when their `MD_FLAG`s are set but drawn as plain styled text.
- Column resize in tables; a table with more columns than fit is squeezed,
  not scrolled.
- Converging with `TextEditor` on shared pieces (clipboard, undo grouping)
  when a raw-source mode is wired next to the live one.
