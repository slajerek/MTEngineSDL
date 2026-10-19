#pragma once

// CMarkdownLiveEditor: rendered ("live") Markdown editing as a Dear ImGui
// widget. The spike for the engine's Markdown editor.
//
// THE MODEL. The Markdown source is the single truth (CMarkdownDocument). Every
// frame the widget lays the source out as RENDERED text -- headings large,
// emphasis in the bold/italic fonts, lists with bullets, tables as a grid --
// except for the REVEAL UNIT the caret is in, which is shown as SOURCE with
// its block syntax (`# `, `- `, `> `, fences) visible but dimmed. A unit is the
// innermost leaf: a paragraph, a heading, a code block, a rule, one list
// item's own line, a table cell (a table outside its cells reveals whole).
// The source is partitioned into unit ranges -- each unit owns the bytes from
// its start to the next unit's start -- so blank lines and container syntax
// always have a caret position. Inside the revealed unit, inline markers
// (`**`, `*`, `` ` ``, `~~`, `[`, `](url)`) are shown only for the spans the
// caret touches (Obsidian's rule); SetRevealAllInlineSyntax(true) shows all.
//
// This is the "source is truth, decorations near the caret" class of editor
// (Obsidian Live Preview, Zettlr, iA Writer), chosen over block widgets
// (Typora, MarkText) and over a rich document tree (ProseMirror, Milkdown)
// because it needs no serializer -- an edit is a byte replacement, so the
// round trip is exact by construction -- and because an immediate-mode GUI
// redraws everything from (source, caret) each frame anyway; there is no
// retained DOM to keep in sync.
//
// WHAT WORKS (the spike's brief): caret placement by click, caret movement
// by keyboard across visual lines, typing, Backspace/Delete, Enter with list
// continuation, selection (Shift+arrows, mouse drag), Cut/Copy/Paste,
// grouped Undo/Redo through Tools/Undo, Ctrl/Cmd+1/2/3 heading level, Ctrl/Cmd+0 paragraph,
// Ctrl/Cmd+B / Ctrl/Cmd+I emphasis toggles, Tab/Shift+Tab between table
// cells, Enter to the cell below. Fonts come from the host (SetFonts); any
// null font falls back to the ImGui default font at the requested size.
//
// Since M2: incremental reparse, images, link activation, find, word and
// paragraph moves and the rest of a word processor's keys
// (ProcessMovementAndDeletionKeys). NOT YET: IME composition display.

#include "CMarkdownDocument.h"
#include "CMarkdownEditorBindings.h"
#include "imgui.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

class CUndoManager;
class CUndoAction;

// How the editor presents the Markdown it edits.
enum class EMarkdownEditMode : uint8_t
{
	Rendered,           // Word-like: no syntax is ever shown; the caret moves over rendered text
	RevealNearCaret,    // the unit under the caret shows its source (Obsidian-like)
	Source              // the whole document as Markdown, syntax dimmed
};

// Inline styles the Word-like commands toggle.
// Underline is written `<u>text</u>` (Markdown has none; inline HTML is how
// GitHub, Obsidian, Typora and VS Code take it).
enum class EMarkdownInlineStyle : uint8_t { Strong, Emphasis, Strikethrough, Code, Underline, Count };

// Block kinds the toolbar's paragraph-level commands set.
enum class EMarkdownListKind : uint8_t { Bullet, Numbered, Task };

// The caret's block as the toolbar's style box names it.
enum class EMarkdownCaretBlock : uint8_t { Paragraph, Heading, BulletList, NumberedList, TaskList, Quote, Table, Code, FrontMatter, Other };

struct CMarkdownEditorFonts
{
	ImFont *regular = nullptr;
	ImFont *bold = nullptr;
	ImFont *italic = nullptr;
	ImFont *boldItalic = nullptr;
	ImFont *mono = nullptr;
};

class CMarkdownLiveEditor
{
public:
	CMarkdownLiveEditor();
	~CMarkdownLiveEditor();
	CMarkdownLiveEditor(const CMarkdownLiveEditor &) = delete;
	CMarkdownLiveEditor &operator=(const CMarkdownLiveEditor &) = delete;

	// ---- Document
	void SetSource(std::string_view source);
	const std::string &GetSource() const { return doc.GetSource(); }
	const CMarkdownDocument &GetDocument() const { return doc; }
	bool IsDirty() const { return dirty; }
	void ClearDirty() { dirty = false; }
	void SetReadOnly(bool value) { readOnly = value; }
	bool IsReadOnly() const { return readOnly; }
	// YAML front matter at the start (CMarkdownDocument::SetFrontMatter),
	// off by default. On, it is drawn as metadata, a code box in a muted
	// colour, instead of a rule and a heading; its text edits as code.
	void SetFrontMatter(bool value) { doc.SetFrontMatter(value); layoutDirty = true; ComputeRevealed(); }
	bool GetFrontMatter() const { return doc.GetFrontMatter(); }

	// ---- Mode. Rendered (Word-like) is the default.
	void SetEditMode(EMarkdownEditMode mode);
	EMarkdownEditMode GetEditMode() const { return editMode; }

	// ---- Toolbar above the text (formatting buttons). Shown by default; a
	// host that has its own can hide it.
	void SetToolbarVisible(bool value) { toolbarVisible = value; }
	// The caret: blinking (it shows solid for a moment after every edit or
	// move, then blinks) or steady, and its colour; 0 is the theme's text colour.
	void SetCaretBlink(bool value) { caretBlink = value; }
	bool GetCaretBlink() const { return caretBlink; }
	void SetCaretColor(ImU32 color) { caretColor = color; }
	ImU32 GetCaretColor() const { return caretColor; }
	// Is the caret drawn in the current blink phase (for the tests)?
	bool IsCaretShownNow() const;
	bool IsToolbarVisible() const { return toolbarVisible; }

	// ---- Appearance
	void SetFonts(const CMarkdownEditorFonts &f) { fonts = f; layoutDirty = true; }
	// Base font size in pixels; 0 = the font size current when Render() runs.
	void SetBaseFontSize(float size) { baseFontSize = size; layoutDirty = true; }
	// Size multipliers for heading levels 1..6.
	void SetHeadingScale(int level, float scale);
	// Show every inline marker of the revealed unit, not only those of the
	// spans the caret touches.
	void SetRevealAllInlineSyntax(bool value) { revealAllInlineSyntax = value; layoutDirty = true; }

	// ---- Key bindings. The formatting, heading, undo, link, task, table and
	// find commands go through CSlrKeyboardShortcut objects
	// (CMarkdownEditorBindings): the shared default set unless the host gives
	// another. Movement, deletion, Enter, Tab and the clipboard keys are fixed.
	void SetBindings(CMarkdownEditorBindings *b) { bindings = b; }
	CMarkdownEditorBindings *GetBindings() const { return bindings ? bindings : CMarkdownEditorBindings::GetDefault(); }

	// ---- Rendering. Draws into the current window as one item of `size`
	// (0 = available). Handles mouse and keyboard when the window is focused.
	void Render(const char *id, ImVec2 size = ImVec2(0, 0));

	// ---- Caret and selection (byte offsets into the source)
	uint32_t GetCaret() const { return caret; }
	uint32_t GetAnchor() const { return anchor; }
	bool HasSelection() const { return anchor != caret; }
	void SetCaret(uint32_t offset);                      // collapses the selection
	void SetSelection(uint32_t anchorOffset, uint32_t caretOffset);
	void SelectAll();
	std::string GetSelectedText() const;

	// ---- Undo. Edits are recorded as Tools/Undo actions (CUndoAction) in a
	// CUndoManager: the editor's own by default, or the host's, so document
	// edits and the host's other undoable operations share one history.
	// Passing nullptr goes back to the editor's own manager. The manager must
	// outlive the editor or be handed back first; actions left in a shared
	// manager after the editor is destroyed, or after SetSource(), do nothing.
	// Requires MT_CAP_UNDO: without it no edit is recorded and Undo() /
	// Redo() return false.
	// Inline on purpose: MT_CAP_UNDO's symbol gate rejects any engine symbol
	// whose mangled name mentions CUndoManager in an app built without it, and
	// a parameter type is part of the mangled name. Inline, the symbol exists
	// only in a host that calls it -- one that has the capability.
	void SetUndoManager(CUndoManager *manager)
	{
		undoManager = manager ? manager : ownedUndoManager;
		lastAction = nullptr;
		groupOpen = false;
	}
	CUndoManager *GetUndoManager() const { return undoManager; }

	// ---- Editing. Each call is one undo step unless grouped with the
	// previous one (the same kind of edit -- typing, Backspace, forward
	// Delete -- at the caret, within 500 ms).
	void InsertText(std::string_view text);              // replaces the selection
	void Backspace();
	void DeleteForward();
	void InsertNewline();                                // continues a list item
	void InsertLineBreak();                              // Shift+Enter
	void Cut();
	void Copy() const;
	void Paste();
	bool Undo();
	bool Redo();
	bool CanUndo() const;
	bool CanRedo() const;

	// Structural commands on the block at the caret.
	bool SetHeadingLevel(int level);                     // 0 = paragraph
	void ToggleStrong();                                 // `**`
	void ToggleEmphasis();                               // `*`
	// Any inline style. With a selection: wrap it, or -- when it is already
	// all in that style -- take the style off (splitting the span). Without
	// one, in Rendered mode: the style applies to what is typed next.
	void ToggleInlineStyle(EMarkdownInlineStyle style);
	// The style at the caret as the toolbar shows it (the text left of the
	// caret, plus a pending toggle).
	bool IsInlineStyleActive(EMarkdownInlineStyle style) const;
	// The heading level of the caret's block: 1..6, 0 for a paragraph, -1 for
	// anything else (a list item, a table cell, code).
	int GetHeadingLevelAtCaret() const;
	// Lists and quotes on the caret's block (or every block the selection
	// touches): turn into / out of a list of that kind, a quote; indent and
	// outdent a list item (with its children).
	bool ToggleList(EMarkdownListKind kind);
	bool IsInList(EMarkdownListKind kind) const;
	bool ToggleQuote();
	bool IsInQuote() const;
	bool IndentListItem(bool outdent);
	// What the caret is in: a table cell, then code, a list item, a heading,
	// a quote, a paragraph (innermost first).
	EMarkdownCaretBlock GetBlockKindAtCaret() const;
	// A table with `columns` x `rows` (header row included) after the caret's
	// block; the caret goes to the first header cell.
	bool InsertTable(int columns, int rows);
	// The Word-like link dialog (Ctrl/Cmd+K in Rendered mode): text and
	// address; OK writes `[text](address)` over the selection.
	void OpenLinkDialog();
	bool IsLinkDialogOpen() const { return linkDialogOpen; }
	// What the dialog's OK does, callable directly.
	bool ApplyLink(std::string_view text, std::string_view url);
	// Task items: flip the box of the task at the caret (Ctrl/Cmd+Enter), or
	// of the one whose mark byte is `markOffset` (a click on the box). One
	// byte changes; the caret stays.
	bool ToggleTask();
	bool ToggleTaskAt(uint32_t markOffset);
	// Ctrl/Cmd+K: `[selection]()` with the caret in the destination, `[](url)`
	// when the selection is a URL, `[]()` with the caret in the text otherwise.
	void InsertLink();
	// Open the link, wikilink or image under `offset` through onLinkActivated
	// (default: the platform shell). False when there is none.
	bool ActivateLinkAt(uint32_t offset);

	// Table structure at the caret's cell (CMarkdownDocument::EditTable): one
	// undo step; the caret lands in the new, moved or neighbouring cell. Also
	// on a right-click menu in a table, Alt+Shift+arrows (move row / column)
	// and Ctrl/Cmd+Enter (row below). False when not in a cell or refused.
	bool EditTable(EMarkdownTableEdit edit);
	bool IsInTable() const { return CaretInCell(nullptr); }
	// The caret's whole table, with one of the blank lines around it: one
	// undo step; the caret goes to where the table was.
	bool DeleteTable();

	// ---- Find (Ctrl/Cmd+F). A bar above the text: matches of the query in the
	// SOURCE (so `bold` finds `**bold**`; a query with markers finds them),
	// all highlighted, the current one selected -- which reveals its unit.
	// Enter / Shift+Enter in the bar, F3 / Shift+F3 in the text step through
	// them, wrapping; Esc closes and returns to the text. Case-insensitive
	// matching folds ASCII only.
	void OpenFind();
	void CloseFind();
	bool IsFindOpen() const { return findOpen; }
	void SetFindQuery(std::string_view query);
	const std::string &GetFindQuery() const { return findQuery; }
	void SetFindCaseSensitive(bool value);
	bool IsFindCaseSensitive() const { return findCaseSensitive; }
	// Matches as [beg, end) source ranges, in order.
	const std::vector<std::pair<uint32_t, uint32_t>> &GetFindMatches() const;
	// Index of the selected match, -1 when the selection is not a match.
	int GetCurrentFindMatch() const;
	// Select the next / previous match from the caret, wrapping. False when
	// there is none.
	bool FindNext();
	bool FindPrevious();
	// Replace (Ctrl+H; Cmd+Alt+F on a Mac): the find bar with a second row.
	// The replacement is written into the source as typed, as the query is
	// matched in it. ReplaceCurrent() replaces the selected match and selects
	// the next one (with no match selected it only finds the next); false when
	// nothing was replaced. ReplaceAll() is one undo step and returns how many.
	void OpenReplace();
	bool IsReplaceOpen() const { return findOpen && replaceOpen; }
	void SetReplaceText(std::string_view text);
	const std::string &GetReplaceText() const { return replaceText; }
	bool ReplaceCurrent();
	int ReplaceAll();

	// Table navigation; false when the caret is not in a cell. Tab in the last
	// cell appends a row.
	bool MoveToNextCell(bool backwards);
	bool MoveToCellBelow();

	// ---- Introspection, mostly for tests.
	// The block whose syntax is currently revealed (top-level block of the
	// caret, or the caret's cell inside a table). -1 when nothing is revealed.
	int32_t GetRevealedBlock() const { return revealedBlock; }
	// The source range the revealed unit shows as source: [beg, end).
	void GetRevealedRange(uint32_t *beg, uint32_t *end) const { *beg = revealBeg; *end = revealEnd; }
	bool IsFocused() const { return focused; }
	// Give the text the keyboard on the next Render() (after a host's menu
	// or button acted on the editor).
	void RequestFocus() { returnFocus = true; }
	// The screen rectangle of the caret as drawn in the last Render(); false
	// when it was not visible.
	bool GetCaretScreenRect(ImVec2 *min, ImVec2 *max) const;
	// Source offset under a screen position in the last Render(); false when
	// outside the content.
	bool HitTest(ImVec2 screenPos, uint32_t *offset) const;
	// Screen position (left edge, top) of the glyph at `offset` in the last
	// layout; false when hidden or outside.
	bool GetOffsetScreenPos(uint32_t offset, ImVec2 *pos, float *height) const;
	// Number of layout passes so far.
	uint32_t GetLayoutCount() const { return layoutCount; }
	// The last Render()'s item rectangle.
	ImVec2 GetItemMin() const { return itemMin; }
	ImVec2 GetItemMax() const { return itemMax; }

	// Keyboard handling can be run from a test without a focused window.
	void ProcessKeyboard();
	// Pointer handling from a test: click at a screen position.
	void ClickAt(ImVec2 screenPos, bool extendSelection);

	// Called after every change to the source (for the host's dirty tracking).
	std::function<void()> onChanged;
	// Ctrl/Cmd+click on a link. Unset: ImGui's Platform_OpenInShellFn.
	std::function<void(const std::string &href)> onLinkActivated;
	// Images: given the destination as written (`![alt](src)`), the host
	// returns a texture and its size in pixels, or false to keep the alt text.
	// Called during layout, so the host caches.
	std::function<bool(const std::string &src, ImTextureRef *texture, ImVec2 *size)> imageResolver;

	// Labels the editor draws itself (tooltips, the find bar, the table
	// menu) are English unless the host installs a translation function:
	// `key` in, the translated text out, or `key` / nullptr when it has none.
	// The same pattern as the engine's media views.
	static void SetTranslateFunction(const char *(*fn)(const char *key)) { translateFunc = fn; }
	static const char *L(const char *key, const char *fallback);

public:
	// ---- Layout (public so tests can inspect; not an API)
	struct Glyph
	{
		uint32_t offset;      // source byte this glyph maps to (caret before it); synthetic glyphs map to their anchor
		float x0, x1;         // relative to the line's left edge... absolute within the content (content-space x)
		ImWchar cp;
		uint16_t style;       // index into Layout::styles
		bool synthetic;       // not a source byte (bullet, rendered soft break space, checkbox)
		bool syntax;          // a revealed syntax byte (dimmed)
		bool isNewline;       // a revealed '\n': zero width, ends the line
		float height;         // the line height this glyph needs
		float ascent;         // its ascent from the line top
		uint32_t next;        // source offset after it (an image covers its whole span)
		int16_t image;        // index into Layout::images, -1 for text
		uint8_t deco;         // drawn shape instead of text: kDecoNone / Box / BoxChecked / Bullet
	};
	enum : uint8_t { kDecoNone = 0, kDecoBox = 1, kDecoBoxChecked = 2, kDecoBullet = 3 };
	struct Line
	{
		float y;              // content-space top
		float height;
		float ascent;
		float indent;         // content-space x of the first glyph
		float width;          // available width for this line
		int32_t block;        // the innermost block this line belongs to
		uint32_t begOffset;   // caret range this line covers: [begOffset, endOffset]
		uint32_t endOffset;
		std::vector<Glyph> glyphs;
		// per-line decorations
		bool codeBackground = false;
		int quoteDepth = 0;
		ImU32 bgColor = 0;
	};
	struct CellLayout { int32_t cell; ImVec2 min, max; size_t firstLine, lineCount; };
	struct TableLayout { int32_t table; std::vector<CellLayout> cells; std::vector<float> colX; std::vector<float> rowY; float x0, x1; };
	struct Style { ImFont *font; float size; ImU32 color; bool strike; bool underline; bool codeBg; };
	struct Layout
	{
		std::vector<Line> lines;
		std::vector<Style> styles;
		std::vector<TableLayout> tables;
		std::vector<std::pair<float, float>> rules;   // y, height for horizontal rules
		std::vector<ImTextureRef> images;
		float height = 0;
	};

	// Emission state used while laying out
	struct Emit
	{
		Layout *layout;
		float width;          // content width
		float x;              // current pen x (content space)
		float y;              // current pen y
		float lineLeft;       // left edge for continuation lines
		float lineRight;      // right edge
		int32_t block;
		Line *line;           // the line being filled
		int lastBreakGlyph;   // index in line->glyphs of the last breakable glyph, -1 none
		float spacingBefore;  // pending vertical gap before the next line
		bool pendingLine;     // a line was requested but not yet started
		int quoteDepth;
		bool codeBg;
		ImU32 bgColor;
		uint32_t curOffset;   // source offset the next glyph would start at
	};

public:
	// Internal layout structures are public for the test (it inspects lines).
	const std::vector<Line> &GetLayoutLines() const { return layout.lines; }
	const std::vector<Style> &GetLayoutStyles() const { return layout.styles; }
	// Caret movement as the arrow keys do it (vertical and line-edge moves
	// need a layout, so they act after the first Render()).
	void MoveHorizontal(int direction, bool extend);
	void MoveVertical(int direction, bool extend);
	void MoveLineEdge(bool toEnd, bool extend);
	// Page Up / Page Down: the caret one visible page up or down (at the
	// column it keeps for vertical moves) and the view scrolls with it.
	void MovePage(int direction, bool extend);
	// Word processor moves (Ctrl+arrows; Alt+arrows on a Mac, where
	// io.ConfigMacOSXBehaviors picks the Mac rules). A word is a run of letters
	// and digits, a run of punctuation counts as one, and a paragraph break
	// is one step of its own. Right goes to the next word's start (a Mac: to
	// the word's end); left to the word's start.
	void MoveWord(int direction, bool extend);
	// To the start of this / the previous paragraph (up), or of the next one
	// (down; a Mac: this paragraph's end, then the next one's).
	void MoveParagraph(int direction, bool extend);
	void MoveDocumentEdge(bool toEnd, bool extend);      // Ctrl+Home / Ctrl+End
	// Ctrl+Backspace / Ctrl+Delete (Alt on a Mac): the word before / after the
	// caret goes; at a paragraph's edge, the paragraph break does, as
	// Backspace / Delete would. A selection goes instead.
	void DeleteWord(int direction);
	void DeleteToLineEdge(bool toEnd);                   // Cmd+Backspace / Cmd+Delete on a Mac
	// Double and triple click.
	void SelectWordAt(uint32_t offset);
	void SelectParagraphAt(uint32_t offset);
	// Word-like: is this caret position a blank line between blocks (an
	// empty paragraph not yet typed into)?
	bool IsGapPosition(uint32_t offset) const;
private:

	void EnsureLayout(float width);
	void LayoutBlock(Emit &e, int32_t block, bool sourceMode);
	void LayoutRuns(Emit &e, int32_t block, bool sourceMode, uint32_t rangeBeg, uint32_t rangeEnd, int headingLevel);
	void LayoutSourceRange(Emit &e, int32_t block, uint32_t beg, uint32_t end, int headingLevel, bool asCode);
	void LayoutTable(Emit &e, int32_t table);
	void StartLine(Emit &e, float spacingBefore);
	void EndLine(Emit &e);
	void PutGlyph(Emit &e, ImWchar cp, uint32_t offset, const Style &st, bool synthetic, bool syntax, bool breakable);
	void PutGlyphG(Emit &e, Glyph g, const Style &st, bool breakable, float advance);
	void PutDeco(Emit &e, uint8_t deco, uint32_t anchor, const Style &st, float advance);
	bool EmitImage(Emit &e, int32_t span, const Style &st);
	int32_t ImageSpanOfRun(uint32_t run) const;
	void PutNewline(Emit &e, uint32_t offset);
	void PutSynthetic(Emit &e, const char *text, uint32_t anchor, const Style &st);
	Style StyleFor(uint32_t styles, int headingLevel, bool syntax, bool code, bool tableHeader) const;
	ImFont *FontFor(uint32_t styles, bool code) const;
	float FontSizeFor(int headingLevel) const;
	float Advance(const Style &st, ImWchar cp) const;
	float LineHeightFor(const Style &st) const;

	// ---- Drawing / input
	void Draw(ImDrawList *dl, ImVec2 origin);
	void HandleMouse(ImVec2 origin);
	bool ProcessMovementAndDeletionKeys();          // the fixed word-processor keys; true when one was pressed
	bool HitTestContent(ImVec2 contentPos, uint32_t *offset, bool *hit) const;
	const Glyph *GlyphAtScreen(ImVec2 screenPos) const;
	bool LinkOfGlyph(const Glyph &g, std::string *href) const;
	void OpenLink(const std::string &href);
	int FindLineOfOffset(uint32_t offset) const;      // in current layout; -1 none
	float XOfOffset(const Line &line, uint32_t offset) const;
	uint32_t OffsetAtX(const Line &line, float x) const;
	void EnsureCaretVisible(float viewTop = -1);   // viewTop: the scroll already requested this frame

	// ---- Editing internals
	enum class EEditKind : uint8_t { Insert, Backspace, DeleteForward, Structural };
	// caretAfter: where the caret goes (default: after the inserted text).
	void ApplyReplace(uint32_t beg, uint32_t end, std::string_view text, EEditKind kind, uint32_t caretAfter = UINT32_MAX);
	// Records the replacement of [pos, pos + removed.size()) by `inserted` on
	// the CURRENT source (call before applying it), merging into the previous
	// action when it continues the same burst.
	void RecordEdit(uint32_t pos, std::string_view removed, std::string_view inserted,
		uint32_t caretBefore, uint32_t anchorBefore, uint32_t caretAfter, uint32_t anchorAfter, EEditKind kind);
	// A command made of several replacements is recorded as one: the source
	// before and after are diffed into a single replacement.
	void BeginCompound();
	void EndCompound();
	// Applied by an undo action: no recording.
	void RestoreEdit(uint32_t pos, uint32_t removeLength, const std::string &text, uint32_t newCaret, uint32_t newAnchor);
	void Changed();
	void ComputeRevealed();
	void RenderTableMenu();
	void RenderTableMenuItems();   // the table menu's items, shared by the right-click menu and the toolbar
	void RenderToolbar();
	void RenderLinkDialog();

	// ---- Word-like editing (CMarkdownLiveEditorWord.cpp)
	struct Stop { uint32_t offset; int32_t block; };
	// An empty paragraph: a blank line beyond the one that separates two
	// blocks (every blank line at the document's start or end). The caret
	// stands after the line's own prefix (quote markers, indentation).
	struct Gap { uint32_t lineStart; uint32_t caret; std::string prefix; };
	void RebuildStops() const;
	const std::vector<Stop> &GetStops() const { RebuildStops(); return stops; }
	bool IsContentAt(uint32_t offset) const;
	const std::vector<Gap> &GetGaps() const;              // the empty paragraphs, in order
	int32_t GapIndexOfLine(uint32_t offset) const;        // the gap whose line holds offset, -1
	uint32_t NextPosition(uint32_t offset, int direction) const;   // the next stop or gap past offset, UINT32_MAX at the ends
	void GapAffixes(int32_t gap, std::string *lead, std::string *tail) const;   // what text typed on a gap needs around it
	void ConvertEmptyItemToGap();                          // the caret's empty list item becomes an empty paragraph
	uint32_t Snap(uint32_t offset) const;                 // the caret position for a raw offset
	int32_t StopIndexAtOrBefore(uint32_t offset) const;
	uint32_t BlockFirstStop(int32_t unitBlock) const;
	int32_t UnitBlockOf(uint32_t offset) const;
	bool CodeContentRange(int32_t block, uint32_t *beg, uint32_t *end) const;   // a code block's bytes between its fences
	bool InCodeAt(uint32_t offset) const;
	uint32_t DelimiterChainEnd(uint32_t at, uint8_t *styles) const;
	bool AtBlockTextEnd(uint32_t offset) const;          // no more text in the caret's block after it
	std::string EscapeTyped(std::string_view text, uint32_t at) const;
	void WordInsertText(std::string_view text);
	void WordBackspace();
	void WordDeleteForward();
	void WordNewline(bool lineBreak);
	uint32_t JoinBlocks(uint32_t prevStop, uint32_t rightStop);   // the break between two blocks goes; returns the caret
	uint32_t DeleteRangeSafe(uint32_t a, uint32_t b);     // returns the caret position after
	void SplitAt(uint32_t at, const std::string &separator, const std::string &afterPrefix);
	std::string ContainerPrefixAt(uint32_t offset, bool forParagraph) const;
	bool TryAutoformat();
	void WordMoveHorizontal(int direction, bool extend);
	// Word and paragraph steps over caret positions (stops in Rendered mode,
	// code points otherwise).
	enum class ECharClass : uint8_t { Break, Space, Punct, Word };
	uint32_t StepPosition(uint32_t offset, int direction) const;   // UINT32_MAX at the ends
	ECharClass ClassBetween(uint32_t a, uint32_t b) const;       // what a step from a to b (a < b) crosses
	uint32_t WordTarget(uint32_t from, int direction) const;
	int64_t ParagraphKey(uint32_t offset) const;                // equal for the positions of one paragraph
	uint32_t ParagraphEdge(uint32_t from, int direction) const;  // the paragraph's first / last position
	uint32_t ParagraphTarget(uint32_t from, int direction) const;
	uint32_t LineEdgeOffset(bool toEnd) const;                  // UINT32_MAX without a layout
	void MoveCaretTo(uint32_t offset, bool extend);
	void DeleteTo(uint32_t target);                              // the range caret..target, as a selection would go
	int32_t InnermostSpanOfType(EMarkdownSpanType type, uint32_t a, uint32_t b) const;
	bool ToggleInlineInBlock(EMarkdownSpanType type, const char *open, const char *close, uint32_t a, uint32_t b, uint32_t *newA, uint32_t *newB);
	std::vector<int32_t> SelectedLeafBlocks() const;
	void RenderFindBar();
	void UpdateFindMatches() const;
	bool SelectFirstCellOfRow();
	struct RevealUnit { int32_t block; uint32_t beg; uint32_t end; };
	void RebuildUnits();
	int32_t FindUnitAt(uint32_t offset) const;
	void LayoutRevealed(Emit &e, int32_t block);
	bool IsDescendant(int32_t block, int32_t ancestor) const;
	bool CaretInCell(int32_t *cellOut) const;

	CMarkdownDocument doc;
	uint32_t caret = 0;
	uint32_t anchor = 0;
	bool dirty = false;
	bool readOnly = false;
	bool focused = false;

	CMarkdownEditorBindings *bindings = nullptr;
	CMarkdownEditorFonts fonts;
	float baseFontSize = 0;
	float headingScale[7] = {1.0f, 2.0f, 1.6f, 1.3f, 1.15f, 1.05f, 1.0f};

	Layout layout;
	bool layoutDirty = true;
	float layoutWidth = -1;
	float layoutFontSize = -1;
	int32_t revealedBlock = -1;
	int32_t layoutRevealedBlock = -2;
	uint32_t revealBeg = 0, revealEnd = 0;
	std::vector<RevealUnit> units;
	uint32_t unitsParseCount = UINT32_MAX;
	bool revealAllInlineSyntax = false;
	std::vector<int32_t> touchedSpans, layoutTouchedSpans;
	std::vector<uint8_t> hiddenSyntax;          // per byte of [revealBeg, revealEnd): an inline marker not shown
	float revealLeftOverride = -1;
	std::vector<int32_t> revealImages;          // image spans in the revealed range the caret does not touch
	static const char *(*translateFunc)(const char *key);

	EMarkdownEditMode editMode = EMarkdownEditMode::Rendered;
	EMarkdownEditMode layoutEditMode = EMarkdownEditMode::Rendered;
	bool toolbarVisible = true;
	mutable std::vector<Stop> stops;
	mutable std::vector<uint8_t> contentMask;
	mutable uint32_t stopsParseCount = UINT32_MAX;
	mutable std::vector<Gap> gaps;
	size_t layoutNextGap = 0;                  // gaps emitted so far by the running layout
	void EmitGapsBefore(Emit &e, uint32_t lineStart);
	// Rendered mode: inline styles toggled with no selection apply to the next
	// typed text (bit per EMarkdownInlineStyle); cleared when the caret moves.
	uint8_t pendingOn = 0, pendingOff = 0;
	uint32_t pendingAt = UINT32_MAX;
	bool linkDialogOpen = false, linkDialogFocus = false;
	char linkText[256] = {0}, linkUrl[512] = {0};
	uint32_t linkA = 0, linkB = 0;

	bool findOpen = false;
	bool findFocusRequest = false;
	bool returnFocus = false;
	ImVec2 tableMenuPos = ImVec2(0, 0), tableMenuPivot = ImVec2(0, 0);   // under the toolbar's table button
	bool findCaseSensitive = false;
	std::string findQuery;
	char findBuffer[256] = {0};
	bool replaceOpen = false;
	bool replaceFocusRequest = false;
	std::string replaceText;
	char replaceBuffer[256] = {0};
	mutable std::vector<std::pair<uint32_t, uint32_t>> findMatches;
	mutable uint32_t findMatchesParse = UINT32_MAX;
	mutable std::string findMatchesQuery;
	mutable bool findMatchesCase = false;              // the revealed unit starts on a list marker line: lay it out from the list's left edge
	uint32_t layoutCount = 0;
	ImVec2 itemMin, itemMax, contentOrigin;
	float scrollY = 0;
	bool scrollToCaret = false;
	bool caretRectValid = false;
	ImVec2 caretMin, caretMax;
	float preferredX = -1;     // for vertical movement
	float pageHeight = 0;      // the text area's visible height at the last Render()
	bool caretBlink = true;
	ImU32 caretColor = 0;
	double caretBlinkStart = 0;   // when the caret last moved or the text changed
	uint32_t caretSeenOffset = UINT32_MAX, caretSeenAnchor = UINT32_MAX, caretSeenParse = UINT32_MAX;
	float pendingScroll = 0;   // a page move's scroll, applied in Render()
	bool dragging = false;

	// Undo. The actions hold the target weakly: a destroyed editor, or one
	// that loaded a new source, leaves no-op actions behind in a shared manager.
	class EditAction;
	struct UndoTarget { CMarkdownLiveEditor *editor; };
	std::shared_ptr<UndoTarget> undoTarget;
	CUndoManager *undoManager = nullptr;       // the one in use: owned or the host's
	CUndoManager *ownedUndoManager = nullptr;  // created only with MT_ENABLE_UNDO
	EditAction *lastAction = nullptr;          // the action a burst merges into
	double lastEditTime = -1;
	EEditKind lastEditKind = EEditKind::Insert;
	bool groupOpen = false;
	int compoundDepth = 0;
	std::string compoundBefore;
	uint32_t compoundCaret = 0, compoundAnchor = 0;
};
