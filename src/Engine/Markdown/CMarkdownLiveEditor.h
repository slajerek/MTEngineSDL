#pragma once

// CMarkdownLiveEditor: rendered ("live") Markdown editing as a Dear ImGui
// widget. The spike for the engine's Markdown editor.
//
// THE MODEL. The Markdown source is the single truth (CMarkdownDocument). Every
// frame the widget lays the source out as RENDERED text -- headings large,
// emphasis in the bold/italic fonts, lists with bullets, tables as a grid --
// except for the block the caret is in, which is shown as SOURCE with its
// syntax visible but dimmed. Moving the caret into a block reveals its
// syntax; moving out renders it again. Inside a table the reveal unit is the
// cell, not the table, so the grid stays a grid while a cell is edited.
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
// grouped Undo/Redo, Ctrl/Cmd+1/2/3 heading level, Ctrl/Cmd+0 paragraph,
// Ctrl/Cmd+B / Ctrl/Cmd+I emphasis toggles, Tab/Shift+Tab between table
// cells, Enter to the cell below. Fonts come from the host (SetFonts); any
// null font falls back to the ImGui default font at the requested size.
//
// NOT YET (production items, see the M0 report): incremental reparse, images,
// link activation, IME composition display, word-level caret jumps, find.

#include "CMarkdownDocument.h"
#include "imgui.h"

#include <functional>
#include <string>
#include <string_view>
#include <vector>

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

	// ---- Document
	void SetSource(std::string_view source);
	const std::string &GetSource() const { return doc.GetSource(); }
	const CMarkdownDocument &GetDocument() const { return doc; }
	bool IsDirty() const { return dirty; }
	void ClearDirty() { dirty = false; }
	void SetReadOnly(bool value) { readOnly = value; }
	bool IsReadOnly() const { return readOnly; }

	// ---- Appearance
	void SetFonts(const CMarkdownEditorFonts &f) { fonts = f; layoutDirty = true; }
	// Base font size in pixels; 0 = the font size current when Render() runs.
	void SetBaseFontSize(float size) { baseFontSize = size; layoutDirty = true; }
	// Size multipliers for heading levels 1..6.
	void SetHeadingScale(int level, float scale);

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

	// ---- Editing. Each call is one undo step unless grouped with the
	// previous one (typing within 500 ms, adjacent).
	void InsertText(std::string_view text);              // replaces the selection
	void Backspace();
	void DeleteForward();
	void InsertNewline();                                // continues a list item
	void Cut();
	void Copy() const;
	void Paste();
	bool Undo();
	bool Redo();
	bool CanUndo() const { return !undoStack.empty(); }
	bool CanRedo() const { return !redoStack.empty(); }

	// Structural commands on the block at the caret.
	bool SetHeadingLevel(int level);                     // 0 = paragraph
	void ToggleStrong();                                 // `**`
	void ToggleEmphasis();                               // `*`
	// Table navigation; false when the caret is not in a cell.
	bool MoveToNextCell(bool backwards);
	bool MoveToCellBelow();

	// ---- Introspection, mostly for tests.
	// The block whose syntax is currently revealed (top-level block of the
	// caret, or the caret's cell inside a table). -1 when nothing is revealed.
	int32_t GetRevealedBlock() const { return revealedBlock; }
	bool IsFocused() const { return focused; }
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
	};
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
private:

	void EnsureLayout(float width);
	void LayoutBlock(Emit &e, int32_t block, bool sourceMode);
	void LayoutRuns(Emit &e, int32_t block, bool sourceMode, uint32_t rangeBeg, uint32_t rangeEnd, int headingLevel);
	void LayoutSourceRange(Emit &e, int32_t block, uint32_t beg, uint32_t end, int headingLevel, bool asCode);
	void LayoutTable(Emit &e, int32_t table);
	void StartLine(Emit &e, float spacingBefore);
	void EndLine(Emit &e);
	void PutGlyph(Emit &e, ImWchar cp, uint32_t offset, const Style &st, bool synthetic, bool syntax, bool breakable);
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
	bool HitTestContent(ImVec2 contentPos, uint32_t *offset, bool *hit) const;
	int FindLineOfOffset(uint32_t offset) const;      // in current layout; -1 none
	float XOfOffset(const Line &line, uint32_t offset) const;
	uint32_t OffsetAtX(const Line &line, float x) const;
	void MoveVertical(int direction, bool extend);
	void MoveHorizontal(int direction, bool extend);
	void MoveLineEdge(bool toEnd, bool extend);
	void EnsureCaretVisible();

	// ---- Editing internals
	enum class EEditKind : uint8_t { Insert, Backspace, DeleteForward, Structural };
	void ApplyReplace(uint32_t beg, uint32_t end, std::string_view text, EEditKind kind);
	void PushUndo(EEditKind kind);
	void Changed();
	void ComputeRevealed();
	bool CaretInCell(int32_t *cellOut) const;

	CMarkdownDocument doc;
	uint32_t caret = 0;
	uint32_t anchor = 0;
	bool dirty = false;
	bool readOnly = false;
	bool focused = false;

	CMarkdownEditorFonts fonts;
	float baseFontSize = 0;
	float headingScale[7] = {1.0f, 2.0f, 1.6f, 1.3f, 1.15f, 1.05f, 1.0f};

	Layout layout;
	bool layoutDirty = true;
	float layoutWidth = -1;
	float layoutFontSize = -1;
	int32_t revealedBlock = -1;
	int32_t layoutRevealedBlock = -2;
	uint32_t layoutCount = 0;
	ImVec2 itemMin, itemMax, contentOrigin;
	float scrollY = 0;
	bool scrollToCaret = false;
	bool caretRectValid = false;
	ImVec2 caretMin, caretMax;
	float preferredX = -1;     // for vertical movement
	bool dragging = false;

	struct UndoState { std::string source; uint32_t caret, anchor; };
	std::vector<UndoState> undoStack, redoStack;
	double lastEditTime = -1;
	uint32_t lastEditEnd = UINT32_MAX;
	EEditKind lastEditKind = EEditKind::Insert;
	bool groupOpen = false;
};
