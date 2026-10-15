#include "CMarkdownLiveEditor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"   // ImTextCharFromUtf8 / ImTextCharToUtf8

#include "CUndoAction.h"
#if MT_ENABLE_UNDO
#include "CUndoManager.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
	constexpr float kLineHeightFactor = 1.35f;
	constexpr float kParagraphSpacing = 0.55f;   // in base font sizes
	constexpr float kHeadingSpacingBefore = 0.9f;
	constexpr float kListIndent = 1.6f;
	constexpr float kQuoteIndent = 1.2f;
	constexpr float kCellPadding = 0.35f;
	constexpr double kUndoGroupSeconds = 0.5;

	ImU32 WithAlpha(ImU32 col, float alpha)
	{
		ImVec4 c = ImGui::ColorConvertU32ToFloat4(col);
		c.w *= alpha;
		return ImGui::ColorConvertFloat4ToU32(c);
	}

	int Utf8Length(unsigned char lead)
	{
		if (lead < 0x80) return 1;
		if ((lead & 0xE0) == 0xC0) return 2;
		if ((lead & 0xF0) == 0xE0) return 3;
		if ((lead & 0xF8) == 0xF0) return 4;
		return 1;
	}
}

// ---- Undo action --------------------------------------------------------------------
//
// One replacement: at `pos`, `removed` became `inserted`. Undo puts `removed`
// back and restores the caret from before; Redo re-applies. The editor is
// reached through a weak handle, so an action that outlives its editor (in a
// host's shared manager) or its document (after SetSource) does nothing.

class CMarkdownLiveEditor::EditAction : public CUndoAction
{
public:
	std::weak_ptr<UndoTarget> target;
	uint32_t pos = 0;
	std::string removed, inserted;
	uint32_t caretBefore = 0, anchorBefore = 0, caretAfter = 0, anchorAfter = 0;

	void Undo() override
	{
		if (auto t = target.lock())
			t->editor->RestoreEdit(pos, (uint32_t)inserted.size(), removed, caretBefore, anchorBefore);
	}
	void Redo() override
	{
		if (auto t = target.lock())
			t->editor->RestoreEdit(pos, (uint32_t)removed.size(), inserted, caretAfter, anchorAfter);
	}
	std::string GetDescription() override { return "Markdown edit"; }
};

CMarkdownLiveEditor::CMarkdownLiveEditor()
{
	undoTarget = std::make_shared<UndoTarget>(UndoTarget{this});
#if MT_ENABLE_UNDO
	ownedUndoManager = new CUndoManager();
	undoManager = ownedUndoManager;
#endif
}

CMarkdownLiveEditor::~CMarkdownLiveEditor()
{
	undoTarget.reset();   // actions left in a shared manager become no-ops
#if MT_ENABLE_UNDO
	delete ownedUndoManager;
#endif
}

// ---- Document -----------------------------------------------------------------

void CMarkdownLiveEditor::SetSource(std::string_view source)
{
	doc.SetSource(source);
	caret = anchor = 0;
	// A new document starts a new history. The old actions cannot be removed
	// from a host's manager, so they are detached instead.
	undoTarget = std::make_shared<UndoTarget>(UndoTarget{this});
#if MT_ENABLE_UNDO
	if (ownedUndoManager) ownedUndoManager->Clear();
#endif
	lastAction = nullptr;
	groupOpen = false;
	dirty = false;
	layoutDirty = true;
	ComputeRevealed();
}

void CMarkdownLiveEditor::SetHeadingScale(int level, float scale)
{
	if (level >= 1 && level <= 6) { headingScale[level] = scale; layoutDirty = true; }
}

// ---- Caret ----------------------------------------------------------------------

void CMarkdownLiveEditor::SetCaret(uint32_t offset)
{
	SetSelection(offset, offset);
}

void CMarkdownLiveEditor::SetSelection(uint32_t anchorOffset, uint32_t caretOffset)
{
	const uint32_t size = doc.Size();
	caret = std::min(caretOffset, size);
	anchor = std::min(anchorOffset, size);
	groupOpen = false;
	ComputeRevealed();
}

void CMarkdownLiveEditor::SelectAll()
{
	SetSelection(0, doc.Size());
}

std::string CMarkdownLiveEditor::GetSelectedText() const
{
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	return doc.GetSource().substr(a, b - a);
}

bool CMarkdownLiveEditor::CaretInCell(int32_t *cellOut) const
{
	const int32_t block = doc.FindBlockAt(caret);
	const auto &blocks = doc.GetBlocks();
	if (block > 0 && blocks[block].IsCell())
	{
		// FindBlockAt treats `end` as inclusive, which is right for the caret
		// at the end of a cell's content.
		if (cellOut) *cellOut = block;
		return true;
	}
	return false;
}

// The reveal units in document order: leaves (outside tables), list items
// with text of their own (or none at all), and whole tables. Each owns the
// bytes up to the next unit's start; the first also owns everything before it.
void CMarkdownLiveEditor::RebuildUnits()
{
	if (unitsParseCount == doc.GetParseCount()) return;
	unitsParseCount = doc.GetParseCount();
	units.clear();
	const auto &blocks = doc.GetBlocks();
	for (int32_t i = 1; i < (int32_t)blocks.size(); i++)
	{
		const CMarkdownBlock &b = blocks[i];
		if (b.type != EMarkdownBlockType::Table && doc.TableOf(i) > 0) continue;
		const bool unit = b.type == EMarkdownBlockType::Table
			|| (b.IsLeaf() && !b.IsCell())
			|| (b.type == EMarkdownBlockType::ListItem && (doc.OwnsContentPublic(i) || doc.GetChildren(i).empty()));
		if (unit) units.push_back({i, b.beg, b.end});
	}
	std::stable_sort(units.begin(), units.end(), [](const RevealUnit &a, const RevealUnit &b) { return a.beg < b.beg; });
	// Two units cannot start on the same byte; keep the outer (earlier) one.
	units.erase(std::unique(units.begin(), units.end(), [](const RevealUnit &a, const RevealUnit &b) { return a.beg == b.beg; }), units.end());
	for (size_t i = 0; i < units.size(); i++)
	{
		units[i].end = i + 1 < units.size() ? units[i + 1].beg : doc.Size();
		if (i == 0) units[i].beg = 0;
	}
}

int32_t CMarkdownLiveEditor::FindUnitAt(uint32_t offset) const
{
	if (units.empty()) return -1;
	// The last unit starting at or before offset.
	size_t lo = 0, hi = units.size();
	while (hi - lo > 1)
	{
		const size_t mid = (lo + hi) / 2;
		if (units[mid].beg <= offset) lo = mid; else hi = mid;
	}
	return (int32_t)lo;
}

bool CMarkdownLiveEditor::IsDescendant(int32_t block, int32_t ancestor) const
{
	const auto &blocks = doc.GetBlocks();
	while (block > 0 && block < (int32_t)blocks.size())
	{
		if (block == ancestor) return true;
		block = blocks[block].parent;
	}
	return false;
}

void CMarkdownLiveEditor::ComputeRevealed()
{
	touchedSpans.clear();
	if (doc.Size() == 0 || doc.GetBlocks().size() <= 1) { revealedBlock = -1; revealBeg = revealEnd = 0; return; }
	RebuildUnits();
	const int32_t u = FindUnitAt(caret);
	if (u < 0) { revealedBlock = -1; revealBeg = revealEnd = 0; return; }
	revealedBlock = units[u].block;
	revealBeg = units[u].beg;
	revealEnd = units[u].end;
	int32_t cell = -1;
	if (doc.GetBlocks()[revealedBlock].type == EMarkdownBlockType::Table && CaretInCell(&cell))
	{
		revealedBlock = cell;
		revealBeg = doc.GetBlocks()[cell].beg;
		revealEnd = doc.GetBlocks()[cell].end;
	}
	if (!revealAllInlineSyntax)
		for (int32_t sp : doc.SpansTouching(caret))
		{
			const CMarkdownSpan &span = doc.GetSpans()[sp];
			if (span.beg >= revealBeg && span.end <= std::max(revealEnd, revealBeg)) touchedSpans.push_back(sp);
		}
}

// ---- Editing -----------------------------------------------------------------------

void CMarkdownLiveEditor::Changed()
{
	dirty = true;
	layoutDirty = true;
	ComputeRevealed();
	if (onChanged) onChanged();
}

void CMarkdownLiveEditor::RecordEdit(uint32_t pos, std::string_view removed, std::string_view inserted,
	uint32_t caretBefore, uint32_t anchorBefore, uint32_t caretAfter, uint32_t anchorAfter, EEditKind kind)
{
#if MT_ENABLE_UNDO
	if (undoManager == nullptr || compoundDepth > 0) return;
	if (removed.empty() && inserted.empty()) return;

	// One undo step per burst of the SAME kind of edit at the caret: typing
	// groups with typing, Backspace with Backspace, forward Delete with forward
	// Delete. Mixing kinds, moving the caret, a selection, a structural command
	// or a pause longer than kUndoGroupSeconds starts a new step -- the
	// CodeMirror / ProseMirror defaults, which users have under their fingers.
	// The burst grows the action on top of the manager in place, which is only
	// valid while nothing else was pushed or undone since.
	const double now = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
	EditAction *last = lastAction;
	const bool merge = last != nullptr && groupOpen && kind == lastEditKind && kind != EEditKind::Structural
		&& (now - lastEditTime) < kUndoGroupSeconds && caretBefore == anchorBefore && caretBefore == last->caretAfter
		&& undoManager->PeekUndo() == last && !undoManager->CanRedo();
	const std::string &s1 = doc.GetSource();
	const uint32_t b = pos, e = pos + (uint32_t)removed.size();
	const uint32_t p = last ? last->pos : 0;
	const uint32_t pEnd = last ? p + (uint32_t)last->inserted.size() : 0;
	if (merge && b <= pEnd && e >= p)
	{
		// Compose two replacements over the union of their ranges, in the
		// coordinates of the source between them (s1).
		const uint32_t u0 = std::min(p, b), u1 = std::max(pEnd, e);
		std::string oldText = s1.substr(u0, p - u0) + last->removed + s1.substr(pEnd, u1 - pEnd);
		std::string newText = s1.substr(u0, b - u0) + std::string(inserted) + s1.substr(e, u1 - e);
		last->pos = u0;
		last->removed = std::move(oldText);
		last->inserted = std::move(newText);
		last->caretAfter = caretAfter;
		last->anchorAfter = anchorAfter;
	}
	else
	{
		auto action = std::make_unique<EditAction>();
		action->target = undoTarget;
		action->pos = pos;
		action->removed = std::string(removed);
		action->inserted = std::string(inserted);
		action->caretBefore = caretBefore;
		action->anchorBefore = anchorBefore;
		action->caretAfter = caretAfter;
		action->anchorAfter = anchorAfter;
		lastAction = action.get();
		undoManager->Push(std::move(action));
	}
	lastEditTime = now;
	lastEditKind = kind;
	groupOpen = kind != EEditKind::Structural;
#else
	(void)pos; (void)removed; (void)inserted; (void)caretBefore; (void)anchorBefore;
	(void)caretAfter; (void)anchorAfter; (void)kind;
#endif
}

void CMarkdownLiveEditor::BeginCompound()
{
	if (compoundDepth++ == 0)
	{
		compoundBefore = doc.GetSource();
		compoundCaret = caret;
		compoundAnchor = anchor;
	}
}

void CMarkdownLiveEditor::EndCompound()
{
	if (compoundDepth <= 0 || --compoundDepth > 0) return;
	const std::string &before = compoundBefore;
	const std::string &after = doc.GetSource();
	size_t prefix = 0;
	const size_t minLen = std::min(before.size(), after.size());
	while (prefix < minLen && before[prefix] == after[prefix]) prefix++;
	size_t suffix = 0;
	while (suffix < minLen - prefix && before[before.size() - 1 - suffix] == after[after.size() - 1 - suffix]) suffix++;
	const std::string removed = before.substr(prefix, before.size() - prefix - suffix);
	const std::string inserted = after.substr(prefix, after.size() - prefix - suffix);
	// RecordEdit works on the source BEFORE the edit; the compound is already
	// applied, so record against a fresh action (structural, never merged).
	if (!removed.empty() || !inserted.empty())
	{
		groupOpen = false;
		lastAction = nullptr;
#if MT_ENABLE_UNDO
		if (undoManager)
		{
			auto action = std::make_unique<EditAction>();
			action->target = undoTarget;
			action->pos = (uint32_t)prefix;
			action->removed = removed;
			action->inserted = inserted;
			action->caretBefore = compoundCaret;
			action->anchorBefore = compoundAnchor;
			action->caretAfter = caret;
			action->anchorAfter = anchor;
			undoManager->Push(std::move(action));
		}
#endif
	}
	compoundBefore.clear();
	compoundBefore.shrink_to_fit();
}

void CMarkdownLiveEditor::RestoreEdit(uint32_t pos, uint32_t removeLength, const std::string &text, uint32_t newCaret, uint32_t newAnchor)
{
	doc.Replace(pos, pos + removeLength, text);
	const uint32_t size = doc.Size();
	caret = std::min(newCaret, size);
	anchor = std::min(newAnchor, size);
	groupOpen = false;
	lastAction = nullptr;
	preferredX = -1;
	scrollToCaret = true;
	Changed();
}

bool CMarkdownLiveEditor::CanUndo() const
{
#if MT_ENABLE_UNDO
	return undoManager != nullptr && undoManager->CanUndo();
#else
	return false;
#endif
}

bool CMarkdownLiveEditor::CanRedo() const
{
#if MT_ENABLE_UNDO
	return undoManager != nullptr && undoManager->CanRedo();
#else
	return false;
#endif
}

void CMarkdownLiveEditor::ApplyReplace(uint32_t beg, uint32_t end, std::string_view text, EEditKind kind)
{
	if (readOnly) return;
	const uint32_t size = doc.Size();
	beg = std::min(beg, size);
	end = std::min(std::max(beg, end), size);
	const uint32_t caretAfter = beg + (uint32_t)text.size();
	RecordEdit(beg, std::string_view(doc.GetSource()).substr(beg, end - beg), text, caret, anchor, caretAfter, caretAfter, kind);
	const uint32_t after = doc.Replace(beg, end, text);
	caret = anchor = after;
	preferredX = -1;
	scrollToCaret = true;
	Changed();
}

void CMarkdownLiveEditor::InsertText(std::string_view text)
{
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	ApplyReplace(a, b, text, EEditKind::Insert);
}

void CMarkdownLiveEditor::Backspace()
{
	if (HasSelection()) { InsertText(""); return; }
	if (caret == 0) return;
	const uint32_t prev = doc.PrevCodepoint(caret);
	ApplyReplace(prev, caret, "", EEditKind::Backspace);
}

void CMarkdownLiveEditor::DeleteForward()
{
	if (HasSelection()) { InsertText(""); return; }
	if (caret >= doc.Size()) return;
	if (readOnly) return;
	const uint32_t next = doc.NextCodepoint(caret);
	// A forward delete keeps the caret in place: the burst continues while the
	// caret stays where the previous delete left it.
	RecordEdit(caret, std::string_view(doc.GetSource()).substr(caret, next - caret), "", caret, anchor, caret, caret,
		EEditKind::DeleteForward);
	doc.Replace(caret, next, "");
	anchor = caret;
	preferredX = -1;
	scrollToCaret = true;
	Changed();
}

void CMarkdownLiveEditor::InsertNewline()
{
	std::string text = "\n";
	if (!HasSelection())
	{
		const std::string prefix = doc.ListContinuationPrefix(caret);
		if (!prefix.empty())
		{
			// An empty item followed by Enter ends the list instead of adding
			// another empty item (the behaviour every editor has).
			const uint32_t lineBeg = doc.LineStart(caret), lineEnd = doc.LineEnd(caret);
			const std::string line = doc.GetSource().substr(lineBeg, lineEnd - lineBeg);
			bool onlyMarker = true;
			size_t i = 0;
			while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
			while (i < line.size() && line[i] != ' ') i++;   // the marker
			for (; i < line.size(); i++) if (line[i] != ' ' && line[i] != '\t') { onlyMarker = false; break; }
			if (onlyMarker)
			{
				// The line keeps its '\n': removing the marker leaves a blank
				// line, which is what ends the list.
				ApplyReplace(lineBeg, lineEnd, "", EEditKind::Structural);
				return;
			}
			text += prefix;
		}
	}
	InsertText(text);
}

void CMarkdownLiveEditor::Cut()
{
	if (!HasSelection() || readOnly) return;
	Copy();
	InsertText("");
}

void CMarkdownLiveEditor::Copy() const
{
	if (!HasSelection()) return;
	ImGui::SetClipboardText(GetSelectedText().c_str());
}

void CMarkdownLiveEditor::Paste()
{
	const char *text = ImGui::GetClipboardText();
	if (text == nullptr || *text == 0) return;
	InsertText(text);
}

bool CMarkdownLiveEditor::Undo()
{
	if (readOnly || !CanUndo()) return false;
#if MT_ENABLE_UNDO
	groupOpen = false;
	lastAction = nullptr;
	undoManager->PerformUndo();
	return true;
#else
	return false;
#endif
}

bool CMarkdownLiveEditor::Redo()
{
	if (readOnly || !CanRedo()) return false;
#if MT_ENABLE_UNDO
	groupOpen = false;
	lastAction = nullptr;
	undoManager->PerformRedo();
	return true;
#else
	return false;
#endif
}

bool CMarkdownLiveEditor::SetHeadingLevel(int level)
{
	if (readOnly) return false;
	BeginCompound();
	uint32_t c = caret;
	const bool changed = doc.SetHeadingLevel(caret, level, &c);
	if (changed) caret = anchor = c;
	EndCompound();
	if (!changed) return false;
	scrollToCaret = true;
	Changed();
	return true;
}

static void ToggleDelimiter(CMarkdownLiveEditor &ed, CMarkdownDocument &doc, uint32_t &caret, uint32_t &anchor,
	const char *delim, std::function<void(uint32_t, uint32_t, std::string_view, bool)> apply)
{
	(void)ed;
	const uint32_t n = (uint32_t)strlen(delim);
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	const std::string &src = doc.GetSource();
	if (a == b)
	{
		// No selection: insert a pair and put the caret between.
		std::string pair = std::string(delim) + delim;
		apply(a, a, pair, false);
		caret = anchor = a + n;
		return;
	}
	// Selection already wrapped (outside or inside): unwrap.
	if (a >= n && b + n <= src.size() && src.compare(a - n, n, delim) == 0 && src.compare(b, n, delim) == 0)
	{
		apply(b, b + n, "", true);
		apply(a - n, a, "", true);
		anchor = a - n;
		caret = b - n;
		return;
	}
	if (b - a >= 2 * n && src.compare(a, n, delim) == 0 && src.compare(b - n, n, delim) == 0)
	{
		apply(b - n, b, "", true);
		apply(a, a + n, "", true);
		anchor = a;
		caret = b - 2 * n;
		return;
	}
	apply(b, b, delim, false);
	apply(a, a, delim, false);
	anchor = a + n;
	caret = b + n;
}

void CMarkdownLiveEditor::ToggleStrong()
{
	if (readOnly) return;
	BeginCompound();
	auto apply = [this](uint32_t beg, uint32_t end, std::string_view text, bool) { doc.Replace(beg, end, text); };
	ToggleDelimiter(*this, doc, caret, anchor, "**", apply);
	EndCompound();
	scrollToCaret = true;
	Changed();
}

void CMarkdownLiveEditor::ToggleEmphasis()
{
	if (readOnly) return;
	BeginCompound();
	auto apply = [this](uint32_t beg, uint32_t end, std::string_view text, bool) { doc.Replace(beg, end, text); };
	ToggleDelimiter(*this, doc, caret, anchor, "*", apply);
	EndCompound();
	scrollToCaret = true;
	Changed();
}

bool CMarkdownLiveEditor::MoveToNextCell(bool backwards)
{
	int32_t cell = -1;
	if (!CaretInCell(&cell)) return false;
	const int32_t table = doc.TableOf(cell);
	std::vector<int32_t> all;
	for (int32_t row : doc.RowsOf(table))
		for (int32_t c : doc.CellsOf(row)) all.push_back(c);
	auto it = std::find(all.begin(), all.end(), cell);
	if (it == all.end()) return false;
	size_t idx = (size_t)(it - all.begin());
	if (backwards) { if (idx == 0) return false; idx--; }
	else { if (idx + 1 >= all.size()) return false; idx++; }
	const CMarkdownBlock &target = doc.GetBlocks()[all[idx]];
	SetSelection(target.beg, target.end);   // select the cell content, like every spreadsheet
	scrollToCaret = true;
	return true;
}

bool CMarkdownLiveEditor::MoveToCellBelow()
{
	int32_t cell = -1;
	if (!CaretInCell(&cell)) return false;
	const int32_t table = doc.TableOf(cell);
	const int32_t row = doc.RowOf(cell);
	const int32_t col = doc.ColumnOf(cell);
	const std::vector<int32_t> rows = doc.RowsOf(table);
	auto it = std::find(rows.begin(), rows.end(), row);
	if (it == rows.end() || it + 1 == rows.end()) return false;
	const std::vector<int32_t> cells = doc.CellsOf(*(it + 1));
	if (col < 0 || col >= (int32_t)cells.size()) return false;
	const CMarkdownBlock &target = doc.GetBlocks()[cells[col]];
	SetSelection(target.beg, target.end);
	scrollToCaret = true;
	return true;
}

// ---- Styles -------------------------------------------------------------------------

ImFont *CMarkdownLiveEditor::FontFor(uint32_t styles, bool code) const
{
	ImFont *f = nullptr;
	if (code || (styles & MD_STYLE_CODE)) f = fonts.mono;
	else
	{
		const bool strong = (styles & MD_STYLE_STRONG) != 0;
		const bool em = (styles & MD_STYLE_EMPHASIS) != 0;
		if (strong && em) f = fonts.boldItalic ? fonts.boldItalic : fonts.bold;
		else if (strong) f = fonts.bold;
		else if (em) f = fonts.italic;
		else f = fonts.regular;
	}
	if (f == nullptr) f = fonts.regular;
	if (f == nullptr) f = ImGui::GetFont();
	return f;
}

float CMarkdownLiveEditor::FontSizeFor(int headingLevel) const
{
	const float base = baseFontSize > 0 ? baseFontSize : ImGui::GetFontSize();
	if (headingLevel >= 1 && headingLevel <= 6) return base * headingScale[headingLevel];
	return base;
}

CMarkdownLiveEditor::Style CMarkdownLiveEditor::StyleFor(uint32_t styles, int headingLevel, bool syntax, bool code, bool tableHeader) const
{
	Style st;
	uint32_t s = styles;
	if (tableHeader) s |= MD_STYLE_STRONG;
	if (headingLevel > 0) s |= MD_STYLE_STRONG;
	st.font = FontFor(s, code);
	st.size = FontSizeFor(headingLevel);
	ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
	if (styles & (MD_STYLE_LINK | MD_STYLE_WIKILINK)) col = ImGui::GetColorU32(ImGuiCol_ButtonHovered);
	if (styles & MD_STYLE_IMAGE) col = ImGui::GetColorU32(ImGuiCol_PlotHistogram);
	if (syntax) col = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0.45f);
	st.color = col;
	st.strike = (styles & MD_STYLE_STRIKETHROUGH) != 0 && !syntax;
	st.underline = (styles & (MD_STYLE_UNDERLINE | MD_STYLE_LINK)) != 0 && !syntax;
	st.codeBg = code || (styles & MD_STYLE_CODE) != 0;
	return st;
}

float CMarkdownLiveEditor::Advance(const Style &st, ImWchar cp) const
{
	if (cp == '\t') return st.font->GetFontBaked(st.size)->GetCharAdvance(' ') * 4.0f;
	return st.font->GetFontBaked(st.size)->GetCharAdvance(cp);
}

float CMarkdownLiveEditor::LineHeightFor(const Style &st) const
{
	return st.size * kLineHeightFactor;
}

// ---- Layout --------------------------------------------------------------------------

void CMarkdownLiveEditor::StartLine(Emit &e, float spacingBefore)
{
	if (e.pendingLine) EndLine(e);
	e.pendingLine = true;
	e.spacingBefore = std::max(e.spacingBefore, spacingBefore);
	e.line = nullptr;
	e.lastBreakGlyph = -1;
	e.x = e.lineLeft;
}

void CMarkdownLiveEditor::EndLine(Emit &e)
{
	if (!e.pendingLine) return;
	if (e.line == nullptr)
	{
		// An empty line still exists: it is where the caret goes on a blank
		// source line, in an empty cell, on a rule.
		Line line;
		line.y = e.y + e.spacingBefore;
		line.height = FontSizeFor(0) * kLineHeightFactor;
		line.ascent = FontSizeFor(0);
		line.indent = e.lineLeft;
		line.width = e.lineRight - e.lineLeft;
		line.block = e.block;
		line.begOffset = line.endOffset = e.curOffset;
		line.quoteDepth = e.quoteDepth;
		line.codeBackground = e.codeBg;
		line.bgColor = e.bgColor;
		e.layout->lines.push_back(line);
		e.line = &e.layout->lines.back();
	}
	Line &line = *e.line;
	if (line.begOffset == UINT32_MAX) line.begOffset = line.endOffset = e.curOffset;
	e.y = line.y + line.height;
	e.layout->height = std::max(e.layout->height, e.y);
	e.spacingBefore = 0;
	e.pendingLine = false;
	e.line = nullptr;
	e.lastBreakGlyph = -1;
	e.x = e.lineLeft;
}

void CMarkdownLiveEditor::PutGlyph(Emit &e, ImWchar cp, uint32_t offset, const Style &st, bool synthetic, bool syntax, bool breakable)
{
	if (!e.pendingLine) StartLine(e, 0);
	if (e.line == nullptr)
	{
		Line line;
		line.y = e.y + e.spacingBefore;
		line.height = 0;
		line.ascent = 0;
		line.indent = e.lineLeft;
		line.width = e.lineRight - e.lineLeft;
		line.block = e.block;
		line.begOffset = UINT32_MAX;
		line.endOffset = offset;
		line.quoteDepth = e.quoteDepth;
		line.codeBackground = e.codeBg;
		line.bgColor = e.bgColor;
		e.layout->lines.push_back(line);
		e.line = &e.layout->lines.back();
		e.x = e.lineLeft;
	}

	const float adv = Advance(st, cp);
	Line *line = e.line;

	// Wrap: move the tail after the last breakable glyph to a fresh line.
	if (e.x + adv > e.lineRight && !line->glyphs.empty() && !synthetic)
	{
		std::vector<Glyph> tail;
		if (e.lastBreakGlyph >= 0 && e.lastBreakGlyph + 1 < (int)line->glyphs.size())
		{
			tail.assign(line->glyphs.begin() + e.lastBreakGlyph + 1, line->glyphs.end());
			line->glyphs.erase(line->glyphs.begin() + e.lastBreakGlyph + 1, line->glyphs.end());
		}
		else if (e.lastBreakGlyph < 0)
		{
			// No break opportunity: break right here (a very long word).
		}
		else
		{
			// The break glyph is the last one: nothing to move.
		}
		// Recompute the end offset of the shortened line.
		if (!line->glyphs.empty())
		{
			const Glyph &last = line->glyphs.back();
			line->endOffset = last.synthetic ? last.offset : last.offset + (uint32_t)Utf8Length((unsigned char)doc.GetSource()[last.offset]);
		}
		else line->endOffset = line->begOffset;

		const uint32_t savedOffset = e.curOffset;
		EndLine(e);
		StartLine(e, 0);
		e.curOffset = savedOffset;
		for (const Glyph &g : tail)
		{
			// Re-emit the tail with its own style: look it up through the
			// layout's style table.
			const Style &gs = e.layout->styles[g.style];
			PutGlyph(e, g.cp, g.offset, gs, g.synthetic, g.syntax, g.cp == ' ');
		}
		PutGlyph(e, cp, offset, st, synthetic, syntax, breakable);
		return;
	}

	// Intern the style.
	uint16_t styleIndex = 0;
	{
		auto &styles = e.layout->styles;
		size_t i = 0;
		for (; i < styles.size(); i++)
		{
			const Style &o = styles[i];
			if (o.font == st.font && o.size == st.size && o.color == st.color && o.strike == st.strike
				&& o.underline == st.underline && o.codeBg == st.codeBg) break;
		}
		if (i == styles.size()) styles.push_back(st);
		styleIndex = (uint16_t)i;
	}

	Glyph g;
	g.offset = offset;
	g.x0 = e.x;
	g.x1 = e.x + adv;
	g.cp = cp;
	g.style = styleIndex;
	g.synthetic = synthetic;
	g.syntax = syntax;
	g.isNewline = false;
	line->glyphs.push_back(g);
	if (breakable) e.lastBreakGlyph = (int)line->glyphs.size() - 1;
	e.x += adv;

	const float h = LineHeightFor(st);
	const float ascent = st.font->GetFontBaked(st.size)->Ascent + (h - st.size) * 0.5f;
	line->height = std::max(line->height, h);
	line->ascent = std::max(line->ascent, ascent);
	if (line->begOffset == UINT32_MAX || offset < line->begOffset) line->begOffset = offset;
	if (!synthetic)
	{
		const uint32_t next = offset + (uint32_t)Utf8Length((unsigned char)doc.GetSource()[offset]);
		line->endOffset = std::max(line->endOffset, next);
		e.curOffset = next;
	}
	else
	{
		line->endOffset = std::max(line->endOffset, offset);
	}
}

void CMarkdownLiveEditor::PutNewline(Emit &e, uint32_t offset)
{
	// A revealed '\n': the caret can sit before it (end of this line) and the
	// next line starts after it.
	if (!e.pendingLine) StartLine(e, 0);
	if (e.line == nullptr)
	{
		Style st = StyleFor(0, 0, true, false, false);
		PutGlyph(e, ' ', offset, st, true, true, false);
		e.line->glyphs.pop_back();   // a placeholder only to create the line
		e.x = e.lineLeft;
	}
	e.line->endOffset = std::max(e.line->endOffset, offset);
	if (e.line->begOffset == UINT32_MAX) e.line->begOffset = offset;
	e.curOffset = offset + 1;
	EndLine(e);
	StartLine(e, 0);
	e.curOffset = offset + 1;
}

void CMarkdownLiveEditor::PutSynthetic(Emit &e, const char *text, uint32_t anchorOffset, const Style &st)
{
	const char *p = text;
	const char *end = text + strlen(text);
	while (p < end)
	{
		unsigned int cp = 0;
		const int n = ImTextCharFromUtf8(&cp, p, end);
		PutGlyph(e, (ImWchar)cp, anchorOffset, st, true, false, false);
		p += n > 0 ? n : 1;
	}
}

// Rendered content of a block's own runs.
void CMarkdownLiveEditor::LayoutRuns(Emit &e, int32_t block, bool, uint32_t, uint32_t, int headingLevel)
{
	const auto &blocks = doc.GetBlocks();
	const auto &runs = doc.GetRuns();
	const CMarkdownBlock &b = blocks[block];
	const bool asCode = b.type == EMarkdownBlockType::Code || b.type == EMarkdownBlockType::Html;
	const bool header = b.type == EMarkdownBlockType::TableHeaderCell;
	const std::string &src = doc.GetSource();

	for (uint32_t r = b.firstRun; r < b.firstRun + b.runCount; r++)
	{
		const CMarkdownRun &run = runs[r];
		if (run.block != block) continue;
		const Style st = StyleFor(run.styles, headingLevel, false, asCode, header);

		if (run.IsSynthesized() || run.beg == run.end)
		{
			if (run.kind == EMarkdownRunKind::HardBreak || run.synthesized == "\n")
			{
				// A rendered line break: the caret before the break maps to run.beg.
				e.curOffset = run.beg;
				if (e.line) e.line->endOffset = std::max(e.line->endOffset, run.beg);
				EndLine(e);
				StartLine(e, 0);
				e.curOffset = run.beg + (asCode ? 1 : 0);
			}
			else if (run.kind == EMarkdownRunKind::NullChar)
			{
				PutGlyph(e, 0xFFFD, run.beg, st, true, false, false);
			}
			else
			{
				// Soft break or collapsed whitespace or code indent: spaces.
				const size_t n = run.synthesized.empty() ? 1 : run.synthesized.size();
				for (size_t i = 0; i < n; i++) PutGlyph(e, ' ', run.beg, st, true, false, true);
			}
			continue;
		}

		uint32_t off = run.beg;
		while (off < run.end)
		{
			unsigned int cp = 0;
			const int n = ImTextCharFromUtf8(&cp, src.data() + off, src.data() + run.end);
			if (cp == '\n') { PutNewline(e, off); off += 1; continue; }
			PutGlyph(e, (ImWchar)cp, off, st, false, false, cp == ' ');
			off += n > 0 ? (uint32_t)n : 1;
		}
	}
}

// Revealed source: every byte of [beg, end) as a glyph, syntax dimmed.
void CMarkdownLiveEditor::LayoutSourceRange(Emit &e, int32_t block, uint32_t beg, uint32_t end, int headingLevel, bool asCode)
{
	const std::string &src = doc.GetSource();
	const auto &runs = doc.GetRuns();
	const bool header = block > 0 && doc.GetBlocks()[block].type == EMarkdownBlockType::TableHeaderCell;
	e.curOffset = beg;
	uint32_t off = beg;
	// Runs are in document order; walk them alongside the bytes.
	size_t r = 0;
	while (r < runs.size() && (runs[r].IsSynthesized() || runs[r].end <= off)) r++;
	while (off < end)
	{
		while (r < runs.size() && (runs[r].IsSynthesized() || runs[r].end <= off)) r++;
		const bool content = r < runs.size() && off >= runs[r].beg && off < runs[r].end;
		if (!content && off >= revealBeg && off - revealBeg < hiddenSyntax.size() && hiddenSyntax[off - revealBeg])
		{
			// An inline marker of a span the caret does not touch: not drawn.
			off = doc.NextCodepoint(off);
			e.curOffset = off;
			continue;
		}
		const uint32_t styles = content ? runs[r].styles : 0;
		const bool runCode = content && runs[r].kind == EMarkdownRunKind::Code;
		const Style st = StyleFor(styles, headingLevel, !content, asCode || runCode, header);

		unsigned int cp = 0;
		const int n = ImTextCharFromUtf8(&cp, src.data() + off, src.data() + end);
		if (cp == '\n') { PutNewline(e, off); off += 1; continue; }
		PutGlyph(e, (ImWchar)cp, off, st, false, !content, cp == ' ');
		off += n > 0 ? (uint32_t)n : 1;
	}
	e.curOffset = end;
}

void CMarkdownLiveEditor::LayoutTable(Emit &e, int32_t table)
{
	const auto &blocks = doc.GetBlocks();
	const CMarkdownBlock &t = blocks[table];
	const std::vector<int32_t> rows = doc.RowsOf(table);
	const int cols = std::max<int>(1, t.columnCount);
	const float base = FontSizeFor(0);
	const float pad = base * kCellPadding;

	// Natural widths.
	std::vector<float> natural((size_t)cols, base * 2.0f);
	for (int32_t row : rows)
	{
		const std::vector<int32_t> cells = doc.CellsOf(row);
		for (size_t c = 0; c < cells.size() && (int)c < cols; c++)
		{
			const CMarkdownBlock &cell = blocks[cells[c]];
			float w = 0;
			if (cells[c] == revealedBlock)
			{
				const std::string &src = doc.GetSource();
				uint32_t off = cell.beg;
				while (off < cell.end)
				{
					unsigned int cp = 0;
					const int n = ImTextCharFromUtf8(&cp, src.data() + off, src.data() + cell.end);
					w += Advance(StyleFor(0, 0, false, false, cell.type == EMarkdownBlockType::TableHeaderCell), (ImWchar)cp);
					off += n > 0 ? (uint32_t)n : 1;
				}
			}
			else
			{
				const auto &runs = doc.GetRuns();
				for (uint32_t r = cell.firstRun; r < cell.firstRun + cell.runCount; r++)
				{
					const CMarkdownRun &run = runs[r];
					if (run.block != cells[c]) continue;
					const Style st = StyleFor(run.styles, 0, false, false, cell.type == EMarkdownBlockType::TableHeaderCell);
					if (run.IsSynthesized()) { w += Advance(st, ' '); continue; }
					const std::string &src = doc.GetSource();
					uint32_t off = run.beg;
					while (off < run.end)
					{
						unsigned int cp = 0;
						const int n = ImTextCharFromUtf8(&cp, src.data() + off, src.data() + run.end);
						w += Advance(st, (ImWchar)cp);
						off += n > 0 ? (uint32_t)n : 1;
					}
				}
			}
			natural[c] = std::max(natural[c], w + 2 * pad + base * 0.5f);
		}
	}
	float total = 0;
	for (float w : natural) total += w;
	const float avail = e.lineRight - e.lineLeft;
	std::vector<float> widths = natural;
	if (total > avail && total > 0)
		for (float &w : widths) w = w * avail / total;

	TableLayout tl;
	tl.table = table;
	tl.x0 = e.lineLeft;
	tl.colX.push_back(e.lineLeft);
	for (float w : widths) tl.colX.push_back(tl.colX.back() + w);
	tl.x1 = tl.colX.back();

	EndLine(e);
	float y = e.y + std::max(e.spacingBefore, base * kParagraphSpacing);
	e.spacingBefore = 0;
	tl.rowY.push_back(y);

	const float savedLeft = e.lineLeft, savedRight = e.lineRight;
	for (int32_t row : rows)
	{
		const std::vector<int32_t> cells = doc.CellsOf(row);
		float rowBottom = y;
		for (size_t c = 0; c < cells.size() && (int)c < cols; c++)
		{
			const CMarkdownBlock &cell = blocks[cells[c]];
			CellLayout cl;
			cl.cell = cells[c];
			cl.min = ImVec2(tl.colX[c], y);
			cl.max = ImVec2(tl.colX[c + 1], y);
			cl.firstLine = e.layout->lines.size();

			e.lineLeft = tl.colX[c] + pad;
			e.lineRight = tl.colX[c + 1] - pad;
			e.y = y + pad * 0.6f;
			e.block = cells[c];
			e.pendingLine = false;
			e.line = nullptr;
			StartLine(e, 0);
			e.curOffset = cell.beg;
			if (cells[c] == revealedBlock)
				LayoutSourceRange(e, cells[c], cell.beg, cell.end, 0, false);
			else
				LayoutRuns(e, cells[c], false, 0, 0, 0);
			EndLine(e);
			cl.lineCount = e.layout->lines.size() - cl.firstLine;
			rowBottom = std::max(rowBottom, e.y + pad * 0.6f);
			tl.cells.push_back(cl);
		}
		for (size_t i = tl.cells.size() - std::min(tl.cells.size(), cells.size()); i < tl.cells.size(); i++)
			tl.cells[i].max.y = rowBottom;
		y = rowBottom;
		tl.rowY.push_back(y);
	}
	e.lineLeft = savedLeft;
	e.lineRight = savedRight;
	e.y = y;
	e.layout->height = std::max(e.layout->height, y);
	e.spacingBefore = base * kParagraphSpacing;
	e.layout->tables.push_back(tl);
}

// The revealed unit: every byte of its range as a glyph, block syntax dimmed,
// inline markers of untouched spans hidden.
void CMarkdownLiveEditor::LayoutRevealed(Emit &e, int32_t block)
{
	const CMarkdownBlock &b = doc.GetBlocks()[block];
	const float base = FontSizeFor(0);
	const float savedLeft = e.lineLeft;
	if (revealLeftOverride >= 0) { e.lineLeft = revealLeftOverride; revealLeftOverride = -1; }
	e.block = block;
	const int headingLevel = b.type == EMarkdownBlockType::Heading ? b.level : 0;
	StartLine(e, b.type == EMarkdownBlockType::Heading ? base * kHeadingSpacingBefore : base * kParagraphSpacing);
	e.x = e.lineLeft;
	e.curOffset = revealBeg;
	const bool asCode = b.type == EMarkdownBlockType::Code || b.type == EMarkdownBlockType::Html;
	if (b.type == EMarkdownBlockType::Code) e.codeBg = true;
	LayoutSourceRange(e, block, revealBeg, revealEnd, headingLevel, asCode);
	e.codeBg = false;
	// The caret may sit at the range's end (just before the next unit): the
	// trailing line's endOffset already covers it when the range ended with '\n'.
	if (e.pendingLine && e.line == nullptr) e.curOffset = revealEnd;
	EndLine(e);
	e.lineLeft = savedLeft;
	e.spacingBefore = base * kParagraphSpacing;
}

void CMarkdownLiveEditor::LayoutBlock(Emit &e, int32_t block, bool)
{
	const auto &blocks = doc.GetBlocks();
	const CMarkdownBlock &b = blocks[block];
	const float base = FontSizeFor(0);
	e.block = block;

	if (block == revealedBlock && !b.IsCell() && b.type != EMarkdownBlockType::ListItem)
	{
		LayoutRevealed(e, block);
		return;
	}

	switch (b.type)
	{
		case EMarkdownBlockType::Heading:
			StartLine(e, base * kHeadingSpacingBefore);
			e.curOffset = b.beg;
			LayoutRuns(e, block, false, 0, 0, b.level);
			EndLine(e);
			e.spacingBefore = base * kParagraphSpacing;
			break;

		case EMarkdownBlockType::Paragraph:
			StartLine(e, base * kParagraphSpacing);
			e.curOffset = b.beg;
			LayoutRuns(e, block, false, 0, 0, 0);
			EndLine(e);
			e.spacingBefore = base * kParagraphSpacing;
			break;

		case EMarkdownBlockType::Code:
		case EMarkdownBlockType::Html:
			e.codeBg = true;
			StartLine(e, base * kParagraphSpacing);
			e.curOffset = b.beg;
			LayoutRuns(e, block, false, 0, 0, 0);
			EndLine(e);
			e.codeBg = false;
			e.spacingBefore = base * kParagraphSpacing;
			break;

		case EMarkdownBlockType::HorizontalRule:
		{
			StartLine(e, base * kParagraphSpacing);
			e.curOffset = b.beg;
			EndLine(e);
			Line &line = e.layout->lines.back();
			line.begOffset = b.beg;
			line.endOffset = b.end;
			e.layout->rules.push_back({line.y, line.height});
			e.spacingBefore = base * kParagraphSpacing;
			break;
		}

		case EMarkdownBlockType::Quote:
		{
			const float savedLeft = e.lineLeft;
			e.lineLeft += base * kQuoteIndent;
			e.quoteDepth++;
			for (int32_t c : doc.GetChildren(block)) LayoutBlock(e, c, false);
			e.quoteDepth--;
			e.lineLeft = savedLeft;
			e.block = block;
			break;
		}

		case EMarkdownBlockType::UnorderedList:
		case EMarkdownBlockType::OrderedList:
		{
			const bool ordered = b.type == EMarkdownBlockType::OrderedList;
			uint32_t number = b.listStart;
			const float savedLeft = e.lineLeft;
			const Style st = StyleFor(0, 0, false, false, false);
			const float indent = base * kListIndent;
			bool first = true;
			for (int32_t li : doc.GetChildren(block))
			{
				const CMarkdownBlock &item = blocks[li];
				e.block = li;
				StartLine(e, first ? base * kParagraphSpacing : (b.listIsTight ? 0.0f : base * kParagraphSpacing * 0.5f));
				first = false;
				e.curOffset = item.beg;

				// The revealed unit starts on this item's marker line (the item's
				// own text, or its first child): the line shows as source from
				// the list's left edge -- no rendered marker -- and the rest of
				// the item renders as usual.
				const bool ownRevealed = li == revealedBlock;
				const bool lineRevealed = ownRevealed || (revealedBlock > 0 && IsDescendant(revealedBlock, li)
					&& doc.LineStart(blocks[revealedBlock].beg) == doc.LineStart(item.beg));
				if (lineRevealed)
				{
					e.lineLeft = savedLeft + indent;
					if (ownRevealed)
					{
						revealLeftOverride = savedLeft;
						LayoutRevealed(e, li);
					}
					for (int32_t c : doc.GetChildren(li))
					{
						EndLine(e);
						if (!ownRevealed && IsDescendant(revealedBlock, c)) revealLeftOverride = savedLeft;
						LayoutBlock(e, c, false);
					}
					revealLeftOverride = -1;
					EndLine(e);
					e.lineLeft = savedLeft;
					continue;
				}
				// Marker glyphs, right-aligned in the indent column.
				char marker[32];
				if (ordered) snprintf(marker, sizeof(marker), "%u%c", number++, b.listMark ? b.listMark : '.');
				else snprintf(marker, sizeof(marker), "%s", "\xE2\x80\xA2");   // U+2022 bullet
				float mw = 0;
				{
					const char *p = marker;
					while (*p) { unsigned int cp = 0; const int n = ImTextCharFromUtf8(&cp, p, nullptr); mw += Advance(st, (ImWchar)cp); p += n > 0 ? n : 1; }
				}
				e.lineLeft = savedLeft + indent;
				e.x = std::max(savedLeft, savedLeft + indent - mw - Advance(st, ' '));
				PutSynthetic(e, marker, item.beg, st);
				e.x = savedLeft + indent;
				if (item.isTask)
				{
					const bool checked = item.taskMark == 'x' || item.taskMark == 'X';
					PutSynthetic(e, checked ? "\xE2\x98\x91 " : "\xE2\x98\x90 ", item.taskMarkOffset, st);   // U+2611 / U+2610
				}

				// The item's own text (tight list) continues on the marker line.
				LayoutRuns(e, li, false, 0, 0, 0);
				// Children: the first paragraph continues the marker line too.
				bool firstChild = true;
				for (int32_t c : doc.GetChildren(li))
				{
					const CMarkdownBlock &child = blocks[c];
					if (firstChild && child.type == EMarkdownBlockType::Paragraph && !doc.OwnsContentPublic(li))
					{
						e.block = c;
						e.curOffset = child.beg;
						LayoutRuns(e, c, false, 0, 0, 0);
						EndLine(e);
						e.spacingBefore = b.listIsTight ? 0.0f : base * kParagraphSpacing * 0.5f;
					}
					else
					{
						EndLine(e);
						LayoutBlock(e, c, false);
					}
					firstChild = false;
				}
				EndLine(e);
				e.lineLeft = savedLeft;
			}
			e.lineLeft = savedLeft;
			e.block = block;
			e.spacingBefore = base * kParagraphSpacing;
			break;
		}

		case EMarkdownBlockType::Table:
			LayoutTable(e, block);
			break;

		default:
			// Table internals are laid out by LayoutTable; Document by EnsureLayout.
			for (int32_t c : doc.GetChildren(block)) LayoutBlock(e, c, false);
			break;
	}
}

void CMarkdownLiveEditor::EnsureLayout(float width)
{
	const float fontSize = FontSizeFor(0);
	if (!layoutDirty && width == layoutWidth && fontSize == layoutFontSize && revealedBlock == layoutRevealedBlock
		&& touchedSpans == layoutTouchedSpans) return;

	layout = Layout();
	layoutCount++;
	layoutDirty = false;
	layoutWidth = width;
	layoutFontSize = fontSize;
	layoutRevealedBlock = revealedBlock;
	layoutTouchedSpans = touchedSpans;

	// Which bytes of the revealed range are inline markers to hide: those of
	// every span in the range, minus those of the spans the caret touches.
	hiddenSyntax.assign(revealEnd > revealBeg ? revealEnd - revealBeg : 0, 0);
	if (!revealAllInlineSyntax && !hiddenSyntax.empty())
	{
		const auto &spans = doc.GetSpans();
		auto mark = [&](const CMarkdownSpan &sp, uint8_t v) {
			for (uint32_t o = sp.beg; o < sp.end; o++)
			{
				if (o >= sp.contentBeg && o < sp.contentEnd) continue;
				if (o >= revealBeg && o < revealEnd) hiddenSyntax[o - revealBeg] = v;
			}
		};
		for (const CMarkdownSpan &sp : spans)
			if (sp.beg >= revealBeg && sp.end <= revealEnd) mark(sp, 1);
		for (int32_t t : touchedSpans) mark(spans[t], 0);
	}

	Emit e;
	e.layout = &layout;
	e.width = width;
	e.x = 0;
	e.y = 0;
	e.lineLeft = 0;
	e.lineRight = std::max(width, fontSize * 2.0f);
	e.block = 0;
	e.line = nullptr;
	e.lastBreakGlyph = -1;
	e.spacingBefore = 0;
	e.pendingLine = false;
	e.quoteDepth = 0;
	e.codeBg = false;
	e.bgColor = 0;
	e.curOffset = 0;

	const auto &blocks = doc.GetBlocks();
	if (doc.Size() == 0 || blocks.size() <= 1)
	{
		// Empty document: one empty line at offset 0 so the caret has a home.
		StartLine(e, 0);
		e.curOffset = 0;
		EndLine(e);
		// Leading blank-only source still needs the caret reachable.
		if (doc.Size() > 0)
		{
			layout = Layout();
			e.layout = &layout; e.y = 0; e.pendingLine = false; e.line = nullptr;
			StartLine(e, 0);
			LayoutSourceRange(e, 0, 0, doc.Size(), 0, false);
			EndLine(e);
		}
		return;
	}

	// Every block lays itself out rendered unless it is the revealed unit; a
	// table with a revealed cell stays a grid and LayoutTable reveals the cell.
	for (int32_t top : doc.GetChildren(0))
		LayoutBlock(e, top, false);
	// Trailing blank lines after the last block belong to the last block's
	// source view; when it is not revealed the caret can still be at Size():
	// give it an (invisible) empty line.
	if (!layout.lines.empty() && layout.lines.back().endOffset < doc.Size())
	{
		StartLine(e, 0);
		e.curOffset = doc.Size();
		EndLine(e);
	}
}

// ---- Geometry queries ----------------------------------------------------------------

int CMarkdownLiveEditor::FindLineOfOffset(uint32_t offset) const
{
	int fallback = -1;
	for (size_t i = 0; i < layout.lines.size(); i++)
	{
		const Line &line = layout.lines[i];
		for (const Glyph &g : line.glyphs)
			if (!g.synthetic && g.offset == offset) return (int)i;
		if (offset >= line.begOffset && offset <= line.endOffset) fallback = (int)i;
	}
	return fallback;
}

float CMarkdownLiveEditor::XOfOffset(const Line &line, uint32_t offset) const
{
	for (const Glyph &g : line.glyphs)
		if (!g.synthetic && g.offset >= offset) return g.x0;
	if (!line.glyphs.empty()) return line.glyphs.back().x1;
	return line.indent;
}

uint32_t CMarkdownLiveEditor::OffsetAtX(const Line &line, float x) const
{
	uint32_t best = line.begOffset;
	float bestDist = 1e30f;
	for (const Glyph &g : line.glyphs)
	{
		if (g.synthetic) continue;
		const float d0 = std::fabs(g.x0 - x);
		if (d0 < bestDist) { bestDist = d0; best = g.offset; }
	}
	// After the last glyph.
	const float endX = line.glyphs.empty() ? line.indent : line.glyphs.back().x1;
	if (std::fabs(endX - x) < bestDist || line.glyphs.empty()) best = line.endOffset;
	// Clicking a synthetic-only line (a bullet with no text yet) lands at its anchor.
	bool anyReal = false;
	for (const Glyph &g : line.glyphs) if (!g.synthetic) { anyReal = true; break; }
	if (!anyReal && !line.glyphs.empty()) best = line.endOffset;
	return best;
}

bool CMarkdownLiveEditor::HitTestContent(ImVec2 p, uint32_t *offset, bool *hit) const
{
	if (layout.lines.empty()) return false;
	// Nearest line by y.
	int best = -1;
	float bestDist = 1e30f;
	for (size_t i = 0; i < layout.lines.size(); i++)
	{
		const Line &line = layout.lines[i];
		if (p.y >= line.y && p.y < line.y + line.height)
		{
			// Several lines can share a y band (table cells): pick by x too.
			const float lx0 = line.indent, lx1 = line.indent + line.width;
			const float dx = p.x < lx0 ? lx0 - p.x : (p.x > lx1 ? p.x - lx1 : 0);
			if (dx < bestDist) { bestDist = dx; best = (int)i; }
			if (dx == 0) break;
		}
	}
	if (best == -1)
	{
		for (size_t i = 0; i < layout.lines.size(); i++)
		{
			const Line &line = layout.lines[i];
			const float d = p.y < line.y ? line.y - p.y : p.y - (line.y + line.height);
			if (d < bestDist) { bestDist = d; best = (int)i; }
		}
		if (hit) *hit = false;
	}
	else if (hit) *hit = true;
	if (best == -1) return false;
	*offset = OffsetAtX(layout.lines[best], p.x);
	return true;
}

bool CMarkdownLiveEditor::HitTest(ImVec2 screenPos, uint32_t *offset) const
{
	bool hit = false;
	const ImVec2 p(screenPos.x - contentOrigin.x, screenPos.y - contentOrigin.y);
	if (!HitTestContent(p, offset, &hit)) return false;
	return hit;
}

bool CMarkdownLiveEditor::GetOffsetScreenPos(uint32_t offset, ImVec2 *pos, float *height) const
{
	const int li = FindLineOfOffset(offset);
	if (li < 0) return false;
	const Line &line = layout.lines[li];
	*pos = ImVec2(contentOrigin.x + XOfOffset(line, offset), contentOrigin.y + line.y);
	if (height) *height = line.height;
	return true;
}

bool CMarkdownLiveEditor::GetCaretScreenRect(ImVec2 *min, ImVec2 *max) const
{
	if (!caretRectValid) return false;
	*min = caretMin;
	*max = caretMax;
	return true;
}

// ---- Movement ---------------------------------------------------------------------------

void CMarkdownLiveEditor::MoveHorizontal(int direction, bool extend)
{
	uint32_t target = caret;
	if (!extend && HasSelection())
		target = direction < 0 ? std::min(anchor, caret) : std::max(anchor, caret);
	else
		target = direction < 0 ? doc.PrevCodepoint(caret) : doc.NextCodepoint(caret);
	const uint32_t a = extend ? anchor : target;
	SetSelection(a, target);
	preferredX = -1;
	scrollToCaret = true;
}

void CMarkdownLiveEditor::MoveVertical(int direction, bool extend)
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return;
	const Line &line = layout.lines[li];
	const float x = preferredX >= 0 ? preferredX : XOfOffset(line, caret);
	// Next line in reading order that is not a sibling cell on the same band.
	int target = li + direction;
	while (target >= 0 && target < (int)layout.lines.size())
	{
		const Line &t = layout.lines[target];
		if (direction > 0 ? t.y > line.y : t.y < line.y) break;
		target += direction;
	}
	if (target < 0 || target >= (int)layout.lines.size())
	{
		// Beyond the ends: caret to the start / end of the document.
		const uint32_t o = direction < 0 ? 0 : doc.Size();
		SetSelection(extend ? anchor : o, o);
		return;
	}
	// Among lines on the same band as the target, prefer the one under x.
	int bestLine = target;
	float bestDist = 1e30f;
	for (int i = target; i >= 0 && i < (int)layout.lines.size(); i += direction)
	{
		const Line &t = layout.lines[i];
		if (t.y != layout.lines[target].y) break;
		const float lx0 = t.indent, lx1 = t.indent + t.width;
		const float dx = x < lx0 ? lx0 - x : (x > lx1 ? x - lx1 : 0);
		if (dx < bestDist) { bestDist = dx; bestLine = i; }
	}
	const uint32_t o = OffsetAtX(layout.lines[bestLine], x);
	SetSelection(extend ? anchor : o, o);
	preferredX = x;
	scrollToCaret = true;
}

void CMarkdownLiveEditor::MoveLineEdge(bool toEnd, bool extend)
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return;
	const Line &line = layout.lines[li];
	uint32_t o = toEnd ? line.endOffset : line.begOffset;
	// A line that ends with a revealed newline: the end is before the '\n'.
	if (toEnd && o > 0 && o <= doc.Size() && o > line.begOffset && doc.GetSource()[o - 1] == '\n' && line.endOffset == o) o--;
	SetSelection(extend ? anchor : o, o);
	preferredX = -1;
	scrollToCaret = true;
}

void CMarkdownLiveEditor::EnsureCaretVisible()
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return;
	const Line &line = layout.lines[li];
	const float viewTop = ImGui::GetScrollY();
	const float viewH = ImGui::GetWindowHeight();
	if (line.y < viewTop) ImGui::SetScrollY(std::max(0.0f, line.y - line.height));
	else if (line.y + line.height > viewTop + viewH) ImGui::SetScrollY(line.y + line.height - viewH + line.height);
}

// ---- Input -----------------------------------------------------------------------------------

void CMarkdownLiveEditor::ClickAt(ImVec2 screenPos, bool extendSelection)
{
	uint32_t offset = 0;
	const ImVec2 p(screenPos.x - contentOrigin.x, screenPos.y - contentOrigin.y);
	bool hit = false;
	if (!HitTestContent(p, &offset, &hit)) return;
	SetSelection(extendSelection ? anchor : offset, offset);
	preferredX = -1;
	focused = true;
}

void CMarkdownLiveEditor::HandleMouse(ImVec2)
{
	const ImGuiIO &io = ImGui::GetIO();
	const bool hovered = ImGui::IsItemHovered();
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		ClickAt(io.MousePos, io.KeyShift);
		dragging = true;
	}
	if (dragging)
	{
		if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
		{
			if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f))
			{
				uint32_t offset = 0;
				bool hit = false;
				const ImVec2 p(io.MousePos.x - contentOrigin.x, io.MousePos.y - contentOrigin.y);
				if (HitTestContent(p, &offset, &hit) && offset != caret)
				{
					const uint32_t a = anchor;
					caret = offset;
					anchor = a;
					ComputeRevealed();
				}
			}
		}
		else dragging = false;
	}
	if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
}

void CMarkdownLiveEditor::ProcessKeyboard()
{
	ImGuiIO &io = ImGui::GetIO();
	io.WantCaptureKeyboard = true;
	io.WantTextInput = true;
	const bool shift = io.KeyShift;
	const ImGuiInputFlags rep = ImGuiInputFlags_Repeat;

	// Table navigation first: Tab is otherwise a character.
	if (ImGui::Shortcut(ImGuiKey_Tab, rep) && MoveToNextCell(false)) {}
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Tab, rep) && MoveToNextCell(true)) {}
	else if (ImGui::Shortcut(ImGuiKey_LeftArrow, rep)) MoveHorizontal(-1, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_LeftArrow, rep)) MoveHorizontal(-1, true);
	else if (ImGui::Shortcut(ImGuiKey_RightArrow, rep)) MoveHorizontal(1, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_RightArrow, rep)) MoveHorizontal(1, true);
	else if (ImGui::Shortcut(ImGuiKey_UpArrow, rep)) MoveVertical(-1, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_UpArrow, rep)) MoveVertical(-1, true);
	else if (ImGui::Shortcut(ImGuiKey_DownArrow, rep)) MoveVertical(1, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_DownArrow, rep)) MoveVertical(1, true);
	else if (ImGui::Shortcut(ImGuiKey_Home, rep)) MoveLineEdge(false, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Home, rep)) MoveLineEdge(false, true);
	else if (ImGui::Shortcut(ImGuiKey_End, rep)) MoveLineEdge(true, false);
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_End, rep)) MoveLineEdge(true, true);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Home)) SetCaret(0);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_End)) SetCaret(doc.Size());
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) SelectAll();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C)) Copy();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) Cut();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, rep)) Paste();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, rep)) Undo();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, rep)) Redo();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, rep)) Redo();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B)) ToggleStrong();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_I)) ToggleEmphasis();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_1)) SetHeadingLevel(1);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_2)) SetHeadingLevel(2);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_3)) SetHeadingLevel(3);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_4)) SetHeadingLevel(4);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_5)) SetHeadingLevel(5);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_6)) SetHeadingLevel(6);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_0)) SetHeadingLevel(0);
	else if (ImGui::Shortcut(ImGuiKey_Backspace, rep)) Backspace();
	else if (ImGui::Shortcut(ImGuiKey_Delete, rep)) DeleteForward();
	else if (ImGui::Shortcut(ImGuiKey_Enter, rep) || ImGui::Shortcut(ImGuiKey_KeypadEnter, rep))
	{
		if (!MoveToCellBelow()) InsertNewline();
	}
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Enter, rep)) InsertText("\n");
	(void)shift;

	// Typed characters.
	if (!readOnly && io.InputQueueCharacters.Size > 0)
	{
		std::string text;
		for (int i = 0; i < io.InputQueueCharacters.Size; i++)
		{
			const ImWchar c = io.InputQueueCharacters[i];
			if (c == '\t' || c == '\n' || c == '\r' || c < 32) continue;   // handled above or control
			char buf[5];
			const int n = ImTextCharToUtf8(buf, (unsigned int)c);
			text.append(buf, (size_t)n);
		}
		if (!text.empty()) InsertText(text);
		io.InputQueueCharacters.resize(0);
	}
}

// ---- Drawing -----------------------------------------------------------------------------------

void CMarkdownLiveEditor::Draw(ImDrawList *dl, ImVec2 origin)
{
	const std::string &src = doc.GetSource();
	const ImU32 selCol = ImGui::GetColorU32(ImGuiCol_TextSelectedBg);
	const ImU32 codeBg = ImGui::GetColorU32(ImGuiCol_FrameBg);
	const ImU32 border = ImGui::GetColorU32(ImGuiCol_Border);
	const ImU32 quoteBar = ImGui::GetColorU32(ImGuiCol_Separator);
	const uint32_t selA = std::min(anchor, caret), selB = std::max(anchor, caret);
	const float base = FontSizeFor(0);

	// Code backgrounds and quote bars, per line.
	for (const Line &line : layout.lines)
	{
		if (line.codeBackground)
			dl->AddRectFilled(ImVec2(origin.x + line.indent - base * 0.3f, origin.y + line.y),
				ImVec2(origin.x + line.indent + line.width, origin.y + line.y + line.height), codeBg);
		for (int q = 0; q < line.quoteDepth; q++)
		{
			const float bx = origin.x + line.indent - base * kQuoteIndent * (q + 1) + base * 0.3f;
			dl->AddRectFilled(ImVec2(bx, origin.y + line.y), ImVec2(bx + base * 0.18f, origin.y + line.y + line.height), quoteBar);
		}
	}
	for (const auto &rule : layout.rules)
	{
		const float y = origin.y + rule.first + rule.second * 0.5f;
		dl->AddLine(ImVec2(origin.x, y), ImVec2(origin.x + layoutWidth, y), border, 1.0f);
	}
	for (const TableLayout &tl : layout.tables)
	{
		for (size_t r = 0; r < tl.rowY.size(); r++)
			dl->AddLine(ImVec2(origin.x + tl.x0, origin.y + tl.rowY[r]), ImVec2(origin.x + tl.x1, origin.y + tl.rowY[r]), border, r == 1 ? 2.0f : 1.0f);
		for (float x : tl.colX)
			dl->AddLine(ImVec2(origin.x + x, origin.y + tl.rowY.front()), ImVec2(origin.x + x, origin.y + tl.rowY.back()), border, 1.0f);
	}

	caretRectValid = false;
	const int caretLine = FindLineOfOffset(caret);
	const bool showCaret = focused;

	std::string segment;
	for (size_t li = 0; li < layout.lines.size(); li++)
	{
		const Line &line = layout.lines[li];
		const float baseY = origin.y + line.y + line.ascent;

		// Selection.
		if (selA != selB)
		{
			float sx0 = -1, sx1 = -1;
			for (const Glyph &g : line.glyphs)
			{
				if (g.synthetic) continue;
				if (g.offset >= selA && g.offset < selB)
				{
					if (sx0 < 0) sx0 = g.x0;
					sx1 = g.x1;
				}
			}
			// A selected newline at the end of a line: a small stub.
			if (line.endOffset > line.begOffset && line.endOffset <= selB && line.endOffset - 1 >= selA
				&& line.endOffset - 1 < src.size() && src[line.endOffset - 1] == '\n')
			{
				const float ex = line.glyphs.empty() ? line.indent : line.glyphs.back().x1;
				if (sx0 < 0) sx0 = ex;
				sx1 = std::max(sx1, ex + base * 0.3f);
			}
			if (sx0 >= 0)
				dl->AddRectFilled(ImVec2(origin.x + sx0, origin.y + line.y), ImVec2(origin.x + sx1, origin.y + line.y + line.height), selCol);
		}

		// Text, in segments of equal style.
		size_t i = 0;
		while (i < line.glyphs.size())
		{
			const uint16_t style = line.glyphs[i].style;
			const float x0 = line.glyphs[i].x0;
			segment.clear();
			size_t j = i;
			float x1 = x0;
			while (j < line.glyphs.size() && line.glyphs[j].style == style && line.glyphs[j].x0 >= x1 - 0.01f)
			{
				const Glyph &g = line.glyphs[j];
				char buf[5];
				const int n = ImTextCharToUtf8(buf, (unsigned int)g.cp);
				segment.append(buf, (size_t)n);
				x1 = g.x1;
				j++;
			}
			const Style &st = layout.styles[style];
			const float glyphAscent = st.font->GetFontBaked(st.size)->Ascent;
			const ImVec2 pos(origin.x + x0, baseY - glyphAscent - (LineHeightFor(st) - st.size) * 0.5f);
			if (st.codeBg && !line.codeBackground)
				dl->AddRectFilled(ImVec2(origin.x + x0, pos.y), ImVec2(origin.x + x1, pos.y + st.size * 1.15f), codeBg, 2.0f);
			dl->AddText(st.font, st.size, pos, st.color, segment.c_str(), segment.c_str() + segment.size());
			if (st.strike)
				dl->AddLine(ImVec2(origin.x + x0, baseY - st.size * 0.3f), ImVec2(origin.x + x1, baseY - st.size * 0.3f), st.color, 1.0f);
			if (st.underline)
				dl->AddLine(ImVec2(origin.x + x0, baseY + 1.0f), ImVec2(origin.x + x1, baseY + 1.0f), st.color, 1.0f);
			i = j;
		}

		// Caret.
		if (showCaret && (int)li == caretLine)
		{
			const float cx = origin.x + XOfOffset(line, caret);
			caretMin = ImVec2(cx, origin.y + line.y);
			caretMax = ImVec2(cx + 1.5f, origin.y + line.y + line.height);
			caretRectValid = true;
			const double t = ImGui::GetTime();
			const bool on = ((int)(t * 2.0) % 2) == 0 || HasSelection() || scrollToCaret;
			if (on) dl->AddRectFilled(caretMin, caretMax, ImGui::GetColorU32(ImGuiCol_Text));
		}
	}
}

void CMarkdownLiveEditor::Render(const char *id, ImVec2 size)
{
	ImGui::PushID(id);
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove;
	ImGui::BeginChild("##md_live_editor", size, ImGuiChildFlags_None, flags);

	const float pad = ImGui::GetStyle().FramePadding.x;
	const float width = std::max(10.0f, ImGui::GetContentRegionAvail().x - 2 * pad);
	EnsureLayout(width);

	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	contentOrigin = ImVec2(cursor.x + pad, cursor.y + pad);
	const float itemHeight = std::max(layout.height + 2 * pad, ImGui::GetContentRegionAvail().y);
	ImGui::InvisibleButton("##md_live_editor_area", ImVec2(width + 2 * pad, itemHeight), ImGuiButtonFlags_MouseButtonLeft);
	itemMin = ImGui::GetItemRectMin();
	itemMax = ImGui::GetItemRectMax();

	HandleMouse(contentOrigin);

	const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	if (windowFocused && (ImGui::IsItemActive() || ImGui::IsItemHovered() || focused)) focused = true;
	if (!windowFocused) focused = false;
	if (focused) ProcessKeyboard();

	// Edits and caret moves may have invalidated the layout: redo it before
	// drawing so the frame shows the new state.
	EnsureLayout(width);
	if (scrollToCaret)
	{
		EnsureCaretVisible();
		scrollToCaret = false;
	}

	Draw(ImGui::GetWindowDrawList(), contentOrigin);

	ImGui::EndChild();
	ImGui::PopID();
}
