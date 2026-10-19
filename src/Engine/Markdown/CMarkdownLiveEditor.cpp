#include "CMarkdownLiveEditor.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include "imgui.h"
#include "imgui_internal.h"   // ImTextCharFromUtf8 / ImTextCharToUtf8

#include "CUndoAction.h"
#if MT_ENABLE_UNDO
#include "CUndoManager.h"
#endif

#include <algorithm>
#include <array>
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

const char *(*CMarkdownLiveEditor::translateFunc)(const char *key) = nullptr;

const char *CMarkdownLiveEditor::L(const char *key, const char *fallback)
{
	if (translateFunc != nullptr)
	{
		const char *t = translateFunc(key);
		if (t != nullptr && strcmp(t, key) != 0) return t;
	}
	return fallback;
}

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
	// Word-like: the caret never stands inside syntax.
	if (editMode == EMarkdownEditMode::Rendered) offset = Snap(std::min(offset, doc.Size()));
	SetSelection(offset, offset);
}

void CMarkdownLiveEditor::SetSelection(uint32_t anchorOffset, uint32_t caretOffset)
{
	const uint32_t size = doc.Size();
	caret = std::min(caretOffset, size);
	anchor = std::min(anchorOffset, size);
	groupOpen = false;
	// A style toggled for the next typed text applies only where it was toggled.
	if (caret != pendingAt) pendingOn = pendingOff = 0;
	ComputeRevealed();
}

void CMarkdownLiveEditor::SetEditMode(EMarkdownEditMode mode)
{
	if (mode == editMode) return;
	editMode = mode;
	pendingOn = pendingOff = 0;
	if (mode == EMarkdownEditMode::Rendered) caret = anchor = Snap(caret);
	layoutDirty = true;
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
	if (editMode != EMarkdownEditMode::RevealNearCaret)
	{
		// Rendered: nothing is revealed (empty paragraphs are drawn by the
		// layout from the source alone). Source: everything is source.
		revealedBlock = -1;
		revealBeg = revealEnd = 0;
		return;
	}
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

void CMarkdownLiveEditor::ApplyReplace(uint32_t beg, uint32_t end, std::string_view text, EEditKind kind, uint32_t caretAfter)
{
	if (readOnly) return;
	const uint32_t size = doc.Size();
	beg = std::min(beg, size);
	end = std::min(std::max(beg, end), size);
	if (caretAfter == UINT32_MAX) caretAfter = beg + (uint32_t)text.size();
	RecordEdit(beg, std::string_view(doc.GetSource()).substr(beg, end - beg), text, caret, anchor, caretAfter, caretAfter, kind);
	doc.Replace(beg, end, text);
	caret = anchor = caretAfter;
	preferredX = -1;
	scrollToCaret = true;
	Changed();
}

void CMarkdownLiveEditor::InsertText(std::string_view text)
{
	if (editMode == EMarkdownEditMode::Rendered) { WordInsertText(text); return; }
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	ApplyReplace(a, b, text, EEditKind::Insert);
}

void CMarkdownLiveEditor::Backspace()
{
	if (editMode == EMarkdownEditMode::Rendered) { WordBackspace(); return; }
	if (HasSelection()) { InsertText(""); return; }
	if (caret == 0) return;
	const uint32_t prev = doc.PrevCodepoint(caret);
	ApplyReplace(prev, caret, "", EEditKind::Backspace);
}

void CMarkdownLiveEditor::DeleteForward()
{
	if (editMode == EMarkdownEditMode::Rendered) { WordDeleteForward(); return; }
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
	if (editMode == EMarkdownEditMode::Rendered) { WordNewline(false); return; }
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
	// Backspace takes a selection in every mode (InsertText("") does nothing
	// in Rendered mode).
	Backspace();
}

void CMarkdownLiveEditor::Copy() const
{
	if (!HasSelection()) return;
	ImGui::SetClipboardText(GetSelectedText().c_str());
}

void CMarkdownLiveEditor::InsertLineBreak()
{
	if (editMode == EMarkdownEditMode::Rendered) WordNewline(true);
	else InsertText("\n");
}

void CMarkdownLiveEditor::Paste()
{
	const char *text = ImGui::GetClipboardText();
	if (text == nullptr || *text == 0) return;
	if (editMode == EMarkdownEditMode::Rendered && !readOnly)
	{
		// Copy takes the source, so a paste is Markdown: what was copied in
		// the editor comes back with its formatting.
		BeginCompound();
		if (HasSelection()) caret = anchor = DeleteRangeSafe(std::min(anchor, caret), std::max(anchor, caret));
		const uint32_t at = caret;
		doc.Replace(at, at, text);
		caret = anchor = Snap(at + (uint32_t)strlen(text));
		EndCompound();
		preferredX = -1;
		scrollToCaret = true;
		Changed();
		return;
	}
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
	if (editMode == EMarkdownEditMode::Rendered && !HasSelection() && level > 0 && IsGapPosition(caret))
	{
		// An empty paragraph becomes a heading, as text typed there would.
		const int32_t g = GapIndexOfLine(caret);
		std::string lead, tail;
		GapAffixes(g, &lead, &tail);
		const uint32_t at = GetGaps()[g].caret;
		const std::string marker = std::string((size_t)level, '#') + " ";
		BeginCompound();
		doc.Replace(at, at, lead + marker + tail);
		caret = anchor = Snap(at + (uint32_t)(lead.size() + marker.size()));
		EndCompound();
		scrollToCaret = true;
		Changed();
		return true;
	}
	BeginCompound();
	uint32_t c = caret;
	const bool changed = doc.SetHeadingLevel(caret, level, &c);
	if (changed) caret = anchor = editMode == EMarkdownEditMode::Rendered ? Snap(c) : c;
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
	if (editMode == EMarkdownEditMode::Rendered) { ToggleInlineStyle(EMarkdownInlineStyle::Strong); return; }
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
	if (editMode == EMarkdownEditMode::Rendered) { ToggleInlineStyle(EMarkdownInlineStyle::Emphasis); return; }
	if (readOnly) return;
	BeginCompound();
	auto apply = [this](uint32_t beg, uint32_t end, std::string_view text, bool) { doc.Replace(beg, end, text); };
	ToggleDelimiter(*this, doc, caret, anchor, "*", apply);
	EndCompound();
	scrollToCaret = true;
	Changed();
}

bool CMarkdownLiveEditor::ToggleTask()
{
	const int32_t item = doc.FindTaskItemAt(caret);
	if (item < 0) return false;
	return ToggleTaskAt(doc.GetBlocks()[item].taskMarkOffset);
}

bool CMarkdownLiveEditor::ToggleTaskAt(uint32_t markOffset)
{
	if (readOnly) return false;
	const int32_t item = doc.FindTaskItemByMark(markOffset);
	if (item < 0) return false;
	BeginCompound();
	const bool done = doc.ToggleTask(item);
	EndCompound();
	if (done) Changed();
	return done;
}

void CMarkdownLiveEditor::InsertLink()
{
	if (readOnly) return;
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	const std::string sel = doc.GetSource().substr(a, b - a);
	const bool isUrl = sel.rfind("http://", 0) == 0 || sel.rfind("https://", 0) == 0 || sel.rfind("mailto:", 0) == 0;
	BeginCompound();
	if (sel.empty() || sel.find('\n') != std::string::npos)
	{
		doc.Replace(b, b, "[]()");
		caret = anchor = b + 1;
	}
	else if (isUrl)
	{
		doc.Replace(a, b, "[](" + sel + ")");
		caret = anchor = a + 1;
	}
	else
	{
		doc.Replace(a, b, "[" + sel + "]()");
		caret = anchor = a + (uint32_t)sel.size() + 3;
	}
	EndCompound();
	scrollToCaret = true;
	Changed();
}

bool CMarkdownLiveEditor::EditTable(EMarkdownTableEdit edit)
{
	if (readOnly) return false;
	int32_t cell = -1;
	if (!CaretInCell(&cell)) return false;
	BeginCompound();
	uint32_t c = caret;
	const bool done = doc.EditTable(cell, edit, &c);
	if (done) caret = anchor = c;
	EndCompound();
	if (!done) return false;
	preferredX = -1;
	scrollToCaret = true;
	Changed();
	return true;
}

bool CMarkdownLiveEditor::DeleteTable()
{
	if (readOnly) return false;
	int32_t cell = -1;
	if (!CaretInCell(&cell)) return false;
	const int32_t table = doc.TableOf(cell);
	if (table <= 0) return false;
	const std::string &src = doc.GetSource();
	const uint32_t size = (uint32_t)src.size();
	const CMarkdownBlock &t = doc.GetBlocks()[table];
	uint32_t beg = doc.LineStart(t.beg), end = doc.LineEnd(t.end);
	if (end < size) end++;   // its last line's newline
	// Blank lines on both sides: one of them goes too, so no empty paragraph
	// is left where the table was. At the end of the text, the one before.
	const bool blankBefore = beg >= 2 && src[beg - 1] == '\n' && src[beg - 2] == '\n';
	if (blankBefore && end < size && src[end] == '\n') end++;
	else if (blankBefore && end >= size) beg--;
	BeginCompound();
	doc.Replace(beg, end, "");
	caret = anchor = std::min(beg, (uint32_t)doc.GetSource().size());
	EndCompound();
	preferredX = -1;
	scrollToCaret = true;
	Changed();
	return true;
}

bool CMarkdownLiveEditor::SelectFirstCellOfRow()
{
	int32_t cell = -1;
	if (!CaretInCell(&cell)) return false;
	const std::vector<int32_t> cells = doc.CellsOf(doc.RowOf(cell));
	if (cells.empty()) return false;
	const CMarkdownBlock &first = doc.GetBlocks()[cells[0]];
	SetSelection(first.beg, first.end);
	return true;
}

void CMarkdownLiveEditor::RenderTableMenu()
{
	if (!ImGui::BeginPopup("##md_table_menu")) return;
	RenderTableMenuItems();
	ImGui::EndPopup();
}

void CMarkdownLiveEditor::RenderTableMenuItems()
{
	struct Item { const char *id; const char *key; const char *fallback; EMarkdownTableEdit edit; bool separatorBefore; };
	static const Item items[] = {
		{"md_table_row_above", "markdown.editor.table.insert_row_above", "Insert row above", EMarkdownTableEdit::InsertRowAbove, false},
		{"md_table_row_below", "markdown.editor.table.insert_row_below", "Insert row below", EMarkdownTableEdit::InsertRowBelow, false},
		{"md_table_row_up", "markdown.editor.table.move_row_up", "Move row up", EMarkdownTableEdit::MoveRowUp, false},
		{"md_table_row_down", "markdown.editor.table.move_row_down", "Move row down", EMarkdownTableEdit::MoveRowDown, false},
		{"md_table_row_delete", "markdown.editor.table.delete_row", "Delete row", EMarkdownTableEdit::DeleteRow, false},
		{"md_table_col_left", "markdown.editor.table.insert_column_left", "Insert column left", EMarkdownTableEdit::InsertColumnLeft, true},
		{"md_table_col_right", "markdown.editor.table.insert_column_right", "Insert column right", EMarkdownTableEdit::InsertColumnRight, false},
		{"md_table_col_move_left", "markdown.editor.table.move_column_left", "Move column left", EMarkdownTableEdit::MoveColumnLeft, false},
		{"md_table_col_move_right", "markdown.editor.table.move_column_right", "Move column right", EMarkdownTableEdit::MoveColumnRight, false},
		{"md_table_col_delete", "markdown.editor.table.delete_column", "Delete column", EMarkdownTableEdit::DeleteColumn, false},
		{"md_table_align_left", "markdown.editor.table.align_left", "Align left", EMarkdownTableEdit::AlignLeft, true},
		{"md_table_align_center", "markdown.editor.table.align_center", "Align center", EMarkdownTableEdit::AlignCenter, false},
		{"md_table_align_right", "markdown.editor.table.align_right", "Align right", EMarkdownTableEdit::AlignRight, false},
		{"md_table_align_default", "markdown.editor.table.align_default", "Default alignment", EMarkdownTableEdit::AlignDefault, false},
	};
	// The keys the editor gives these commands, shown beside them.
	CMarkdownEditorBindings *kb = GetBindings();
	auto chordOf = [kb](EMarkdownTableEdit edit) -> ImGuiKeyChord {
		switch (edit)
		{
			case EMarkdownTableEdit::InsertRowBelow: return kb->GetChord(EMarkdownEditorCommand::ToggleTaskOrInsertRow);
			case EMarkdownTableEdit::MoveRowUp: return kb->GetChord(EMarkdownEditorCommand::TableMoveRowUp);
			case EMarkdownTableEdit::MoveRowDown: return kb->GetChord(EMarkdownEditorCommand::TableMoveRowDown);
			case EMarkdownTableEdit::MoveColumnLeft: return kb->GetChord(EMarkdownEditorCommand::TableMoveColumnLeft);
			case EMarkdownTableEdit::MoveColumnRight: return kb->GetChord(EMarkdownEditorCommand::TableMoveColumnRight);
			default: return 0;
		}
	};
	for (const Item &it : items)
	{
		if (it.separatorBefore) ImGui::Separator();
		// `###id`: the item's identity does not depend on the translation.
		const std::string label = std::string(L(it.key, it.fallback)) + "###" + it.id;
		const ImGuiKeyChord chord = chordOf(it.edit);
		if (ImGui::MenuItem(label.c_str(), chord != 0 ? ImGui::GetKeyChordName(chord) : nullptr, false, !readOnly))
		{
			EditTable(it.edit);
			returnFocus = true;
		}
	}
	ImGui::Separator();
	const std::string del = std::string(L("markdown.editor.table.delete_table", "Delete table")) + "###md_table_delete";
	if (ImGui::MenuItem(del.c_str(), nullptr, false, !readOnly))
	{
		DeleteTable();
		returnFocus = true;
	}
}

// ---- Find ----------------------------------------------------------------------------

void CMarkdownLiveEditor::UpdateFindMatches() const
{
	if (findMatchesParse == doc.GetParseCount() && findMatchesQuery == findQuery && findMatchesCase == findCaseSensitive) return;
	findMatchesParse = doc.GetParseCount();
	findMatchesQuery = findQuery;
	findMatchesCase = findCaseSensitive;
	findMatches.clear();
	if (findQuery.empty()) return;
	const std::string &src = doc.GetSource();
	auto fold = [this](char c) { return (!findCaseSensitive && c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; };
	const size_t n = findQuery.size();
	for (size_t i = 0; i + n <= src.size(); )
	{
		size_t k = 0;
		while (k < n && fold(src[i + k]) == fold(findQuery[k])) k++;
		if (k == n) { findMatches.push_back({(uint32_t)i, (uint32_t)(i + n)}); i += n; }
		else i++;
	}
}

const std::vector<std::pair<uint32_t, uint32_t>> &CMarkdownLiveEditor::GetFindMatches() const
{
	UpdateFindMatches();
	return findMatches;
}

int CMarkdownLiveEditor::GetCurrentFindMatch() const
{
	const auto &m = GetFindMatches();
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	for (size_t i = 0; i < m.size(); i++)
		if (m[i].first == a && m[i].second == b) return (int)i;
	return -1;
}

void CMarkdownLiveEditor::OpenFind()
{
	findOpen = true;
	replaceOpen = false;
	findFocusRequest = true;
	// A selection on one line seeds the query, as in every editor.
	if (HasSelection())
	{
		const std::string sel = GetSelectedText();
		if (sel.find('\n') == std::string::npos && sel.size() < sizeof(findBuffer)) SetFindQuery(sel);
	}
}

void CMarkdownLiveEditor::OpenReplace()
{
	OpenFind();
	replaceOpen = true;
	// With a query already there, straight to the replacement.
	if (!findQuery.empty()) { findFocusRequest = false; replaceFocusRequest = true; }
}

void CMarkdownLiveEditor::SetReplaceText(std::string_view text)
{
	replaceText.assign(text.data(), std::min(text.size(), sizeof(replaceBuffer) - 1));
	memcpy(replaceBuffer, replaceText.data(), replaceText.size());
	replaceBuffer[replaceText.size()] = 0;
}

bool CMarkdownLiveEditor::ReplaceCurrent()
{
	if (readOnly || findQuery.empty()) return false;
	const int cur = GetCurrentFindMatch();
	if (cur < 0) { FindNext(); return false; }
	const std::pair<uint32_t, uint32_t> m = GetFindMatches()[cur];
	BeginCompound();
	doc.Replace(m.first, m.second, replaceText);
	caret = anchor = m.first + (uint32_t)replaceText.size();
	EndCompound();
	Changed();
	// The next match after the replacement (not inside it).
	if (!FindNext()) SetCaret(caret);
	return true;
}

int CMarkdownLiveEditor::ReplaceAll()
{
	if (readOnly || findQuery.empty()) return 0;
	const std::vector<std::pair<uint32_t, uint32_t>> m = GetFindMatches();
	if (m.empty()) return 0;
	BeginCompound();
	for (size_t i = m.size(); i-- > 0; ) doc.Replace(m[i].first, m[i].second, replaceText);
	// The caret after the last replacement.
	const int64_t delta = (int64_t)replaceText.size() - (int64_t)findQuery.size();
	caret = anchor = (uint32_t)((int64_t)m.back().first + delta * (int64_t)(m.size() - 1) + (int64_t)replaceText.size());
	if (editMode == EMarkdownEditMode::Rendered) caret = anchor = Snap(caret);
	EndCompound();
	preferredX = -1;
	scrollToCaret = true;
	ComputeRevealed();
	Changed();
	return (int)m.size();
}

void CMarkdownLiveEditor::CloseFind()
{
	if (!findOpen) return;
	findOpen = false;
	returnFocus = true;
}

void CMarkdownLiveEditor::SetFindQuery(std::string_view query)
{
	findQuery.assign(query.data(), std::min(query.size(), sizeof(findBuffer) - 1));
	memcpy(findBuffer, findQuery.data(), findQuery.size());
	findBuffer[findQuery.size()] = 0;
}

void CMarkdownLiveEditor::SetFindCaseSensitive(bool value)
{
	findCaseSensitive = value;
}

bool CMarkdownLiveEditor::FindNext()
{
	const auto &m = GetFindMatches();
	if (m.empty()) return false;
	const uint32_t from = std::max(anchor, caret);
	size_t i = 0;
	while (i < m.size() && m[i].first < from) i++;
	// The match already selected is not "next".
	if (i < m.size() && GetCurrentFindMatch() == (int)i) i++;
	if (i >= m.size()) i = 0;
	SetSelection(m[i].first, m[i].second);
	preferredX = -1;
	scrollToCaret = true;
	return true;
}

bool CMarkdownLiveEditor::FindPrevious()
{
	const auto &m = GetFindMatches();
	if (m.empty()) return false;
	const uint32_t from = std::min(anchor, caret);
	int i = (int)m.size() - 1;
	while (i >= 0 && m[i].first >= from) i--;
	if (i < 0) i = (int)m.size() - 1;
	SetSelection(m[i].first, m[i].second);
	preferredX = -1;
	scrollToCaret = true;
	return true;
}

void CMarkdownLiveEditor::RenderFindBar()
{
	ImGui::PushID("##md_find");
	const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const float h = ImGui::GetFrameHeight();
	const float buttonW = h * 1.2f;
	const float countW = ImGui::CalcTextSize("0000 / 0000").x + ImGui::GetStyle().ItemSpacing.x;
	const float inputW = std::max(h * 4.0f, ImGui::GetContentRegionAvail().x - countW - 4 * (buttonW + ImGui::GetStyle().ItemSpacing.x));

	// The bar's buttons hand the keyboard back to its field, so Esc and Enter
	// keep working after a click.
	if (findFocusRequest) { ImGui::SetKeyboardFocusHere(); findFocusRequest = false; }
	ImGui::SetNextItemWidth(inputW);
	const bool enter = ImGui::InputTextWithHint("###md_find_query", L("markdown.editor.find.hint", "Find"), findBuffer, sizeof(findBuffer),
		ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
	const bool inputActive = ImGui::IsItemActive() || ImGui::IsItemDeactivated();   // Esc deactivates it in the same frame
	if (std::string_view(findBuffer) != findQuery)
	{
		// Typing searches as it goes: the first match from where the caret is.
		findQuery = findBuffer;
		const uint32_t from = std::min(anchor, caret);
		const auto &m = GetFindMatches();
		size_t i = 0;
		while (i < m.size() && m[i].first < from) i++;
		if (i >= m.size()) i = 0;
		if (!m.empty()) { SetSelection(m[i].first, m[i].second); scrollToCaret = true; }
	}
	if (enter)
	{
		if (ImGui::GetIO().KeyShift) FindPrevious(); else FindNext();
		ImGui::SetKeyboardFocusHere(-1);   // keep typing in the bar
	}
	if (inputActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CloseFind();
	if (inputActive && ImGui::IsKeyPressed(ImGuiKey_F3)) { if (ImGui::GetIO().KeyShift) FindPrevious(); else FindNext(); }

	ImGui::SameLine();
	const auto &m = GetFindMatches();
	const int cur = GetCurrentFindMatch();
	char count[64];
	if (findQuery.empty()) count[0] = 0;
	else if (m.empty()) snprintf(count, sizeof(count), "%s", L("markdown.editor.find.none", "No results"));
	else snprintf(count, sizeof(count), "%d / %d", cur >= 0 ? cur + 1 : 0, (int)m.size());
	ImGui::AlignTextToFramePadding();
	ImGui::TextUnformatted(count);
	// Drawn glyphs on the bar's buttons: chevrons up and down, a cross.
	const float gt = std::max(1.0f, h * 0.08f);
	auto glyph = [&](int kind) {
		const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		const ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
		const float s = h * 0.2f;
		const ImU32 col = ImGui::GetColorU32(ImGuiCol_Text);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		if (kind == 2)
		{
			dl->AddLine(ImVec2(c.x - s, c.y - s), ImVec2(c.x + s, c.y + s), col, gt);
			dl->AddLine(ImVec2(c.x - s, c.y + s), ImVec2(c.x + s, c.y - s), col, gt);
			return;
		}
		const float dy = kind == 0 ? -s * 0.5f : s * 0.5f;
		dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(c.x - s, c.y - dy), ImVec2(c.x, c.y + dy), ImVec2(c.x + s, c.y - dy) }.data(), 3, col, 0, gt);
	};
	ImGui::SameLine(lineRight - 4 * buttonW - 3 * ImGui::GetStyle().ItemSpacing.x);
	if (ImGui::Button("###md_find_prev", ImVec2(buttonW, h))) { FindPrevious(); findFocusRequest = true; }
	glyph(0);
	ImGui::SetItemTooltip("%s", L("markdown.editor.find.previous", "Previous match (Shift+Enter)"));
	ImGui::SameLine();
	if (ImGui::Button("###md_find_next", ImVec2(buttonW, h))) { FindNext(); findFocusRequest = true; }
	glyph(1);
	ImGui::SetItemTooltip("%s", L("markdown.editor.find.next", "Next match (Enter)"));
	ImGui::SameLine();
	if (findCaseSensitive) ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetColorU32(ImGuiCol_ButtonActive));
	const bool caseClicked = ImGui::Button("Aa###md_find_case", ImVec2(buttonW, h));
	if (findCaseSensitive) ImGui::PopStyleColor();
	if (caseClicked) { findCaseSensitive = !findCaseSensitive; findFocusRequest = true; }
	ImGui::SetItemTooltip("%s", L("markdown.editor.find.match_case", "Match case"));
	ImGui::SameLine();
	if (ImGui::Button("###md_find_close", ImVec2(buttonW, h))) CloseFind();
	glyph(2);
	ImGui::SetItemTooltip("%s", L("markdown.editor.find.close", "Close (Esc)"));

	if (replaceOpen)
	{
		const char *replaceLabel = L("markdown.editor.replace.one", "Replace");
		const char *allLabel = L("markdown.editor.replace.all", "Replace all");
		const ImGuiStyle &st = ImGui::GetStyle();
		const float oneW = ImGui::CalcTextSize(replaceLabel).x + 2 * st.FramePadding.x;
		const float allW = ImGui::CalcTextSize(allLabel).x + 2 * st.FramePadding.x;
		if (replaceFocusRequest) { ImGui::SetKeyboardFocusHere(); replaceFocusRequest = false; }
		ImGui::SetNextItemWidth(std::max(h * 4.0f, lineRight - ImGui::GetCursorPosX() - oneW - allW - 2 * st.ItemSpacing.x));
		const bool replaceEnter = ImGui::InputTextWithHint("###md_replace_text", L("markdown.editor.replace.hint", "Replace with"),
			replaceBuffer, sizeof(replaceBuffer), ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
		const bool replaceActive = ImGui::IsItemActive() || ImGui::IsItemDeactivated();
		replaceText = replaceBuffer;
		if (replaceEnter)
		{
			ReplaceCurrent();
			ImGui::SetKeyboardFocusHere(-1);
		}
		if (replaceActive && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) CloseFind();
		ImGui::SameLine();
		ImGui::BeginDisabled(readOnly || m.empty());
		if (ImGui::Button((std::string(replaceLabel) + "###md_replace_one").c_str(), ImVec2(oneW, h))) { ReplaceCurrent(); replaceFocusRequest = true; }
		ImGui::SetItemTooltip("%s", L("markdown.editor.replace.one_tip", "Replace this match and go to the next (Enter)"));
		ImGui::SameLine();
		if (ImGui::Button((std::string(allLabel) + "###md_replace_all").c_str(), ImVec2(allW, h))) { ReplaceAll(); replaceFocusRequest = true; }
		ImGui::SetItemTooltip("%s", L("markdown.editor.replace.all_tip", "Replace every match"));
		ImGui::EndDisabled();
	}
	ImGui::PopID();
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
	Glyph g{};
	g.offset = offset;
	g.cp = cp;
	g.synthetic = synthetic;
	g.syntax = syntax;
	g.isNewline = false;
	g.height = LineHeightFor(st);
	g.ascent = st.font->GetFontBaked(st.size)->Ascent + (g.height - st.size) * 0.5f;
	g.next = synthetic ? offset : offset + (uint32_t)Utf8Length((unsigned char)doc.GetSource()[offset]);
	g.image = -1;
	g.deco = kDecoNone;
	PutGlyphG(e, g, st, breakable, Advance(st, cp));
}

void CMarkdownLiveEditor::PutDeco(Emit &e, uint8_t deco, uint32_t anchorOffset, const Style &st, float advance)
{
	Glyph g{};
	g.offset = anchorOffset;
	g.cp = ' ';
	g.synthetic = true;
	g.syntax = false;
	g.isNewline = false;
	g.height = LineHeightFor(st);
	g.ascent = st.font->GetFontBaked(st.size)->Ascent + (g.height - st.size) * 0.5f;
	g.next = anchorOffset;
	g.image = -1;
	g.deco = deco;
	PutGlyphG(e, g, st, false, advance);
}

void CMarkdownLiveEditor::PutGlyphG(Emit &e, Glyph g, const Style &st, bool breakable, float adv)
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
		line.endOffset = g.offset;
		line.quoteDepth = e.quoteDepth;
		line.codeBackground = e.codeBg;
		line.bgColor = e.bgColor;
		e.layout->lines.push_back(line);
		e.line = &e.layout->lines.back();
		// The pen stays where the caller put it: StartLine() already moved it to
		// the left edge, and a list marker is placed left of that on purpose.
	}

	Line *line = e.line;

	// Wrap: move the tail after the last breakable glyph to a fresh line.
	if (e.x + adv > e.lineRight && !line->glyphs.empty() && !g.synthetic)
	{
		std::vector<Glyph> tail;
		if (e.lastBreakGlyph >= 0 && e.lastBreakGlyph + 1 < (int)line->glyphs.size())
		{
			tail.assign(line->glyphs.begin() + e.lastBreakGlyph + 1, line->glyphs.end());
			line->glyphs.erase(line->glyphs.begin() + e.lastBreakGlyph + 1, line->glyphs.end());
		}
		// (No break opportunity -- a very long word -- breaks right here; a
		// break glyph that is the last one leaves nothing to move.)
		if (!line->glyphs.empty())
		{
			const Glyph &last = line->glyphs.back();
			line->endOffset = last.synthetic ? last.offset : last.next;
			line->height = 0;
			line->ascent = 0;
			for (const Glyph &k : line->glyphs) { line->height = std::max(line->height, k.height); line->ascent = std::max(line->ascent, k.ascent); }
		}
		else line->endOffset = line->begOffset;

		const uint32_t savedOffset = e.curOffset;
		EndLine(e);
		StartLine(e, 0);
		e.curOffset = savedOffset;
		for (const Glyph &t : tail)
		{
			// Re-emit the tail with its own style and width.
			const Style &ts = e.layout->styles[t.style];
			PutGlyphG(e, t, ts, t.cp == ' ' && t.deco == kDecoNone, t.x1 - t.x0);
		}
		PutGlyphG(e, g, st, breakable, adv);
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

	g.x0 = e.x;
	g.x1 = e.x + adv;
	g.style = styleIndex;
	line->glyphs.push_back(g);
	if (breakable) e.lastBreakGlyph = (int)line->glyphs.size() - 1;
	e.x += adv;

	line->height = std::max(line->height, g.height);
	line->ascent = std::max(line->ascent, g.ascent);
	if (line->begOffset == UINT32_MAX || g.offset < line->begOffset) line->begOffset = g.offset;
	if (!g.synthetic)
	{
		line->endOffset = std::max(line->endOffset, g.next);
		e.curOffset = g.next;
	}
	else
	{
		line->endOffset = std::max(line->endOffset, g.offset);
	}
}

int32_t CMarkdownLiveEditor::ImageSpanOfRun(uint32_t r) const
{
	const auto &runs = doc.GetRuns();
	const auto &spans = doc.GetSpans();
	if (r >= runs.size()) return -1;
	for (int32_t sp = runs[r].span; sp >= 0; sp = spans[sp].parent)
		if (spans[sp].type == EMarkdownSpanType::Image) return sp;
	return -1;
}

// An image span as one inline box, when the host resolves it.
bool CMarkdownLiveEditor::EmitImage(Emit &e, int32_t span, const Style &st)
{
	if (!imageResolver || span < 0) return false;
	const CMarkdownSpan &sp = doc.GetSpans()[span];
	ImTextureRef tex;
	ImVec2 size(0, 0);
	if (!imageResolver(sp.href, &tex, &size) || size.x <= 0 || size.y <= 0) return false;
	const float maxW = std::max(1.0f, e.lineRight - e.lineLeft);
	if (size.x > maxW) { size.y *= maxW / size.x; size.x = maxW; }
	e.layout->images.push_back(tex);
	Glyph g{};
	g.offset = sp.beg;
	g.cp = 0xFFFC;
	g.synthetic = false;
	g.syntax = false;
	g.isNewline = false;
	g.height = size.y + st.size * 0.35f;
	g.ascent = size.y + st.size * 0.1f;
	g.next = sp.end;
	g.image = (int16_t)(e.layout->images.size() - 1);
	g.deco = kDecoNone;
	PutGlyphG(e, g, st, true, size.x);
	return true;
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

	// Images with no alt text have no run; they sit before the run they precede.
	const auto &spans = doc.GetSpans();
	std::vector<int32_t> bare;
	if (imageResolver)
		for (int32_t sp = 0; sp < (int32_t)spans.size(); sp++)
			if (spans[sp].type == EMarkdownSpanType::Image && spans[sp].runCount == 0 && spans[sp].block == block) bare.push_back(sp);
	size_t nextBare = 0;
	int32_t shownImage = -1;
	// A code block's text ends with the newline of its last line: that one
	// ends the block, it does not open an empty line inside it.
	uint32_t lastContentEnd = 0;
	if (asCode)
		for (uint32_t r = b.firstRun; r < b.firstRun + b.runCount; r++)
			if (runs[r].block == block && !runs[r].IsSynthesized()) lastContentEnd = std::max(lastContentEnd, runs[r].end);
	// Front matter is metadata: muted, so it never competes with the text.
	const ImU32 metaColor = WithAlpha(ImGui::GetColorU32(ImGuiCol_Text), 0.6f);
	auto styleOf = [&](uint32_t styles) {
		Style st = StyleFor(styles, headingLevel, false, asCode, header);
		if (b.isFrontMatter) st.color = metaColor;
		return st;
	};
	const Style plain = styleOf(0);

	for (uint32_t r = b.firstRun; r < b.firstRun + b.runCount; r++)
	{
		while (nextBare < bare.size() && spans[bare[nextBare]].firstRun <= r) EmitImage(e, bare[nextBare++], plain);
		const CMarkdownRun &run = runs[r];
		if (run.block != block) continue;
		const Style st = styleOf(run.styles);

		// A resolved image replaces its alt text with the picture.
		if (run.styles & MD_STYLE_IMAGE)
		{
			const int32_t isp = ImageSpanOfRun(r);
			if (isp >= 0 && isp == shownImage) continue;
			if (isp >= 0 && EmitImage(e, isp, st)) { shownImage = isp; continue; }
		}

		if (run.IsSynthesized() || run.beg == run.end)
		{
			// A code block's closing newline (md4c may pass it as a literal):
			// the block ends there, no empty line inside it.
			if (asCode && run.synthesized == "\n")
			{
				bool laterContent = false;
				for (uint32_t k = r + 1; k < b.firstRun + b.runCount; k++)
					if (runs[k].block == block && (!runs[k].IsSynthesized() || runs[k].synthesized == "\n")) { laterContent = true; break; }
				if (!laterContent) continue;
			}
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
			if (cp == '\n')
			{
				if (asCode && off + 1 == lastContentEnd)
				{
					if (e.line) e.line->endOffset = std::max(e.line->endOffset, off);
					e.curOffset = off;
					off += 1;
					continue;
				}
				PutNewline(e, off);
				off += 1;
				continue;
			}
			PutGlyph(e, (ImWchar)cp, off, st, false, false, cp == ' ');
			off += n > 0 ? (uint32_t)n : 1;
		}
	}
	while (nextBare < bare.size()) EmitImage(e, bare[nextBare++], plain);
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
		// An image the caret does not touch stays a picture inside the revealed unit.
		bool pictured = false;
		for (int32_t sp : revealImages)
		{
			const CMarkdownSpan &span = doc.GetSpans()[sp];
			if (span.beg == off && span.end > off && EmitImage(e, sp, StyleFor(0, headingLevel, false, false, header)))
			{
				off = span.end;
				e.curOffset = off;
				pictured = true;
				break;
			}
		}
		if (pictured) continue;
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
		if (cp == '\n')
		{
			// The range's final newline: the caret goes before it; after it is
			// the next unit's first byte, laid out by that unit.
			if (off + 1 == end && end < doc.Size())
			{
				if (!e.pendingLine) StartLine(e, 0);
				if (e.line == nullptr) { e.curOffset = off; EndLine(e); }
				if (e.line) e.line->endOffset = std::max(e.line->endOffset, off);
				e.curOffset = off;
				off += 1;
				continue;
			}
			PutNewline(e, off);
			off += 1;
			continue;
		}
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
			// Column alignment from the delimiter row: shift each line of the cell.
			if (cell.align == EMarkdownCellAlign::Right || cell.align == EMarkdownCellAlign::Center)
				for (size_t li = cl.firstLine; li < cl.firstLine + cl.lineCount; li++)
				{
					Line &ln = e.layout->lines[li];
					if (ln.glyphs.empty()) continue;
					const float free = (tl.colX[c + 1] - pad) - ln.glyphs.back().x1;
					const float dx = cell.align == EMarkdownCellAlign::Right ? free : free * 0.5f;
					if (dx <= 0) continue;
					for (Glyph &g : ln.glyphs) { g.x0 += dx; g.x1 += dx; }
				}
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
	// A list item's line was already started by the list (with the list's
	// spacing); starting another would leave an empty line above it.
	const float spacing = b.type == EMarkdownBlockType::Heading ? base * kHeadingSpacingBefore
		: (b.type == EMarkdownBlockType::ListItem ? 0.0f : base * kParagraphSpacing);
	if (e.pendingLine && e.line == nullptr) e.spacingBefore = std::max(e.spacingBefore, spacing);
	else StartLine(e, spacing);
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
	// A list item is followed by its siblings at the list's spacing, which the
	// list sets; other units by a paragraph gap.
	if (b.type != EMarkdownBlockType::ListItem) e.spacingBefore = base * kParagraphSpacing;
}

void CMarkdownLiveEditor::LayoutBlock(Emit &e, int32_t block, bool)
{
	const auto &blocks = doc.GetBlocks();
	const CMarkdownBlock &b = blocks[block];
	const float base = FontSizeFor(0);
	e.block = block;
	if (b.IsLeaf() || b.type == EMarkdownBlockType::Table) EmitGapsBefore(e, doc.LineStart(b.beg));

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
			// Numbers start at one column, as in a word processor: a proportional
			// font's "1." is narrower than "2.", so right-aligning them in the
			// indent made the "1." stand out to the right. The column leaves room
			// for the list's widest number.
			float numbersX = savedLeft;
			if (ordered)
			{
				const float gap = std::max(Advance(st, ' '), st.size * 0.4f);
				float widest = 0;
				uint32_t n = b.listStart;
				for (size_t k = 0; k < doc.GetChildren(block).size(); k++, n++)
				{
					char marker[32];
					snprintf(marker, sizeof(marker), "%u%c", n, b.listMark ? b.listMark : '.');
					float mw = 0;
					for (const char *p = marker; *p; p++) mw += Advance(st, (ImWchar)(unsigned char)*p);
					widest = std::max(widest, mw);
				}
				numbersX = std::max(savedLeft, savedLeft + indent - widest - gap);
			}
			bool first = true;
			for (int32_t li : doc.GetChildren(block))
			{
				const CMarkdownBlock &item = blocks[li];
				EmitGapsBefore(e, doc.LineStart(item.beg));
				e.block = li;
				StartLine(e, first ? base * kParagraphSpacing : (b.listIsTight ? 0.0f : base * kParagraphSpacing * 0.5f));
				first = false;
				e.curOffset = item.beg;
				const uint32_t itemNumber = number++;

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
				// A proportional font's space is narrow (Inter: a quarter em); the
				// marker needs a clear gap before the text.
				const float markerGap = std::max(Advance(st, ' '), st.size * 0.4f);
				// Marker in the indent column: the number as text (left-aligned at
				// the list's number column), a bullet and a task box as drawn shapes
				// (fonts do not reliably carry U+2022 / U+2610, and ImGui's default
				// font does not).
				e.lineLeft = savedLeft + indent;
				if (ordered)
				{
					char marker[32];
					snprintf(marker, sizeof(marker), "%u%c", itemNumber, b.listMark ? b.listMark : '.');
					e.x = numbersX;
					PutSynthetic(e, marker, item.beg, st);
				}
				else
				{
					const float bw = st.size * 0.5f;
					e.x = std::max(savedLeft, savedLeft + indent - bw - markerGap);
					PutDeco(e, kDecoBullet, item.beg, st, bw);
				}
				e.x = savedLeft + indent;
				if (item.isTask)
				{
					const bool checked = item.taskMark == 'x' || item.taskMark == 'X';
					PutDeco(e, checked ? kDecoBoxChecked : kDecoBox, item.taskMarkOffset, st, st.size * 1.35f);
				}

				// The item's own text (tight list) continues on the marker line.
				LayoutRuns(e, li, false, 0, 0, 0);
				// Children: the first paragraph continues the marker line too.
				bool firstChild = true;
				for (int32_t c : doc.GetChildren(li))
				{
					const CMarkdownBlock &child = blocks[c];
					if (firstChild && child.type == EMarkdownBlockType::Paragraph && !doc.OwnsContentPublic(li)
						&& doc.LineStart(child.beg) == doc.LineStart(item.beg))
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
				// An item with no text on its marker line: the line's range runs to
				// the line's end, where the caret stands (RebuildStops).
				if (e.line != nullptr)
				{
					bool real = false;
					for (const Glyph &g : e.line->glyphs) if (!g.synthetic) { real = true; break; }
					if (!real)
					{
						e.line->begOffset = item.beg;
						e.line->endOffset = doc.LineEnd(item.beg);
						e.curOffset = e.line->endOffset;
					}
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
		&& touchedSpans == layoutTouchedSpans && editMode == layoutEditMode) return;
	layoutEditMode = editMode;
	layoutNextGap = 0;

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
	revealImages.clear();
	if (!revealAllInlineSyntax && imageResolver)
	{
		const auto &spans = doc.GetSpans();
		for (int32_t i = 0; i < (int32_t)spans.size(); i++)
			if (spans[i].type == EMarkdownSpanType::Image && spans[i].beg >= revealBeg && spans[i].end <= revealEnd
				&& std::find(touchedSpans.begin(), touchedSpans.end(), i) == touchedSpans.end())
				revealImages.push_back(i);
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

	if (editMode == EMarkdownEditMode::Source)
	{
		// The whole document as source, syntax dimmed, nothing hidden.
		StartLine(e, 0);
		e.curOffset = 0;
		LayoutSourceRange(e, 0, 0, doc.Size(), 0, false);
		EndLine(e);
		return;
	}

	// Every block lays itself out rendered unless it is the revealed unit; a
	// table with a revealed cell stays a grid and LayoutTable reveals the cell.
	// Rendered mode: empty paragraphs (GetGaps()) are drawn as empty lines
	// where they are, by LayoutBlock before each block and here at the end.
	for (int32_t top : doc.GetChildren(0)) LayoutBlock(e, top, false);
	if (editMode == EMarkdownEditMode::Rendered)
	{
		EmitGapsBefore(e, UINT32_MAX);
		return;
	}
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

void CMarkdownLiveEditor::EmitGapsBefore(Emit &e, uint32_t lineStart)
{
	if (editMode != EMarkdownEditMode::Rendered) return;
	const std::vector<Gap> &all = GetGaps();
	const int32_t savedBlock = e.block;
	while (layoutNextGap < all.size() && all[layoutNextGap].lineStart < lineStart)
	{
		e.block = 0;
		StartLine(e, FontSizeFor(0) * kParagraphSpacing);
		e.curOffset = all[layoutNextGap].caret;
		EndLine(e);
		e.spacingBefore = FontSizeFor(0) * kParagraphSpacing;
		layoutNextGap++;
	}
	e.block = savedBlock;
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
	if (line.glyphs.empty()) return line.indent;
	const Glyph &last = line.glyphs.back();
	bool anyReal = false;
	for (const Glyph &g : line.glyphs) if (!g.synthetic) { anyReal = true; break; }
	if (!anyReal) return line.indent;   // a bullet with no text yet: the caret at the text column
	float x = last.x1;
	// Blanks typed at a line's end are not content (Markdown drops them) but
	// the caret after them still moves right, as in any editor.
	if (editMode == EMarkdownEditMode::Rendered && !last.synthetic && last.next < offset && offset <= doc.Size())
	{
		const std::string &src = doc.GetSource();
		int blanks = 0;
		for (uint32_t o = last.next; o < offset; o++)
		{
			if (src[o] == ' ') blanks++;
			else if (src[o] == '\t') blanks += 4;
			else if (src[o] == '\n') break;
		}
		if (blanks > 0 && last.style < layout.styles.size())
		{
			const Style &st = layout.styles[last.style];
			x += blanks * st.font->CalcTextSizeA(st.size, FLT_MAX, 0, " ").x;
		}
	}
	return x;
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
	if (editMode == EMarkdownEditMode::Rendered) { WordMoveHorizontal(direction, extend); return; }
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
		uint32_t o = direction < 0 ? 0 : doc.Size();
		if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
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
	uint32_t o = OffsetAtX(layout.lines[bestLine], x);
	if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
	SetSelection(extend ? anchor : o, o);
	preferredX = x;
	scrollToCaret = true;
}

uint32_t CMarkdownLiveEditor::LineEdgeOffset(bool toEnd) const
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return UINT32_MAX;
	const Line &line = layout.lines[li];
	uint32_t o = toEnd ? line.endOffset : line.begOffset;
	// A line that ends with a revealed newline: the end is before the '\n'.
	if (toEnd && o > 0 && o <= doc.Size() && o > line.begOffset && doc.GetSource()[o - 1] == '\n' && line.endOffset == o) o--;
	if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
	return o;
}

void CMarkdownLiveEditor::MoveLineEdge(bool toEnd, bool extend)
{
	const uint32_t o = LineEdgeOffset(toEnd);
	if (o != UINT32_MAX) MoveCaretTo(o, extend);
}

void CMarkdownLiveEditor::MoveCaretTo(uint32_t offset, bool extend)
{
	SetSelection(extend ? anchor : offset, offset);
	preferredX = -1;
	scrollToCaret = true;
}

void CMarkdownLiveEditor::MoveDocumentEdge(bool toEnd, bool extend)
{
	uint32_t o = toEnd ? doc.Size() : 0;
	if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
	MoveCaretTo(o, extend);
}

// ---- Words and paragraphs ------------------------------------------------------------------

uint32_t CMarkdownLiveEditor::StepPosition(uint32_t offset, int direction) const
{
	if (editMode == EMarkdownEditMode::Rendered) return NextPosition(offset, direction);
	if (direction < 0) return offset == 0 ? UINT32_MAX : doc.PrevCodepoint(offset);
	return offset >= doc.Size() ? UINT32_MAX : doc.NextCodepoint(offset);
}

CMarkdownLiveEditor::ECharClass CMarkdownLiveEditor::ClassBetween(uint32_t a, uint32_t b) const
{
	const std::string &src = doc.GetSource();
	const bool rendered = editMode == EMarkdownEditMode::Rendered;
	if (rendered) RebuildStops();
	for (uint32_t o = a; o < b && o < (uint32_t)src.size(); o++)
	{
		const unsigned char c = (unsigned char)src[o];
		// Rendered: syntax is skipped; a newline is a break between blocks,
		// a blank inside one (a soft or hard line break).
		if (rendered && !contentMask[o])
		{
			if (c == '\n') return ParagraphKey(a) != ParagraphKey(b) ? ECharClass::Break : ECharClass::Space;
			continue;
		}
		if (c == '\n') return rendered ? ECharClass::Space : ECharClass::Break;
		if (c == ' ' || c == '\t' || c == '\r') return ECharClass::Space;
		if (c < 0x80) return (isalnum(c) || c == '_') ? ECharClass::Word : ECharClass::Punct;
		unsigned int cp = 0;
		ImTextCharFromUtf8(&cp, src.c_str() + o, src.c_str() + src.size());
		if (cp == 0xA0 || cp == 0x202F || (cp >= 0x2000 && cp <= 0x200B) || cp == 0x3000) return ECharClass::Space;
		// Latin-1 and general punctuation (quotes, dashes, the ellipsis) and symbols.
		if ((cp >= 0xA1 && cp <= 0xBF) || cp == 0xD7 || cp == 0xF7 || (cp >= 0x2010 && cp <= 0x205E) || (cp >= 0x20A0 && cp <= 0x20CF))
			return ECharClass::Punct;
		return ECharClass::Word;
	}
	return rendered ? ECharClass::Break : ECharClass::Space;
}

uint32_t CMarkdownLiveEditor::WordTarget(uint32_t from, int direction) const
{
	const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
	ECharClass c = ECharClass::Break;
	auto step = [&](uint32_t p) {
		const uint32_t q = StepPosition(p, direction);
		if (q != UINT32_MAX) c = direction > 0 ? ClassBetween(p, q) : ClassBetween(q, p);
		return q;
	};
	uint32_t p = from, q = step(p);
	if (q == UINT32_MAX) return from;
	if (c == ECharClass::Break) return q;   // a paragraph break is a step of its own
	auto skip = [&](auto keep) {
		while (q != UINT32_MAX && c != ECharClass::Break && keep(c)) { p = q; q = step(p); }
	};
	if (direction < 0 || mac)
	{
		// Over the blanks (a Mac: and punctuation) before the word, then the word.
		skip([&](ECharClass k) { return k == ECharClass::Space || (mac && k == ECharClass::Punct); });
		const ECharClass run = c;
		skip([&](ECharClass k) { return k == run; });
		return p;
	}
	// Windows / Linux, rightwards: past this word (or run of punctuation),
	// then past the blanks after it.
	const ECharClass run = c;
	if (run != ECharClass::Space) skip([&](ECharClass k) { return k == run; });
	skip([&](ECharClass k) { return k == ECharClass::Space; });
	return p;
}

void CMarkdownLiveEditor::MoveWord(int direction, bool extend)
{
	MoveCaretTo(WordTarget(caret, direction), extend);
}

int64_t CMarkdownLiveEditor::ParagraphKey(uint32_t offset) const
{
	if (editMode == EMarkdownEditMode::Rendered)
	{
		const int32_t gi = GapIndexOfLine(offset);
		if (gi >= 0) return -2 - (int64_t)gi;
		const int32_t i = StopIndexAtOrBefore(offset);
		return i >= 0 ? (int64_t)stops[i].block : -1;
	}
	// Source: a run of non-blank lines; each blank line is one of its own.
	const std::string &src = doc.GetSource();
	auto blank = [&](uint32_t ls) {
		for (uint32_t o = ls; o < (uint32_t)src.size() && src[o] != '\n'; o++)
			if (src[o] != ' ' && src[o] != '\t' && src[o] != '\r') return false;
		return true;
	};
	uint32_t ls = doc.LineStart(offset);
	if (blank(ls)) return -2 - (int64_t)ls;
	while (ls > 0)
	{
		const uint32_t prev = doc.LineStart(ls - 1);
		if (blank(prev)) break;
		ls = prev;
	}
	return ls;
}

uint32_t CMarkdownLiveEditor::ParagraphEdge(uint32_t from, int direction) const
{
	const int64_t key = ParagraphKey(from);
	uint32_t p = from;
	for (;;)
	{
		const uint32_t q = StepPosition(p, direction);
		if (q == UINT32_MAX || ParagraphKey(q) != key) return p;
		p = q;
	}
}

uint32_t CMarkdownLiveEditor::ParagraphTarget(uint32_t from, int direction) const
{
	const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
	if (direction < 0 || mac)
	{
		// This paragraph's edge; already there: the neighbour's.
		const uint32_t e = ParagraphEdge(from, direction);
		if (e != from) return e;
		const uint32_t q = StepPosition(from, direction);
		return q == UINT32_MAX ? from : ParagraphEdge(q, direction);
	}
	// Windows / Linux, down: the next paragraph's start, or the end.
	const uint32_t e = ParagraphEdge(from, 1);
	const uint32_t q = StepPosition(e, 1);
	return q == UINT32_MAX ? e : q;
}

void CMarkdownLiveEditor::MoveParagraph(int direction, bool extend)
{
	MoveCaretTo(ParagraphTarget(caret, direction), extend);
}

void CMarkdownLiveEditor::DeleteTo(uint32_t target)
{
	if (target == caret) return;
	SetSelection(caret, target);
	Backspace();   // a selection goes as Backspace takes it (Rendered: keeping the syntax sound)
}

void CMarkdownLiveEditor::DeleteWord(int direction)
{
	if (readOnly) return;
	if (HasSelection()) { Backspace(); return; }
	const uint32_t target = WordTarget(caret, direction);
	// One step (a paragraph break, a single character): what Backspace /
	// Delete do there, joining blocks the Word-like way.
	if (target == StepPosition(caret, direction))
	{
		if (direction < 0) Backspace();
		else DeleteForward();
		return;
	}
	DeleteTo(target);
}

void CMarkdownLiveEditor::DeleteToLineEdge(bool toEnd)
{
	if (readOnly) return;
	if (HasSelection()) { Backspace(); return; }
	const uint32_t o = LineEdgeOffset(toEnd);
	if (o == UINT32_MAX || o == caret)
	{
		if (toEnd) DeleteForward();
		else Backspace();
		return;
	}
	DeleteTo(o);
}

void CMarkdownLiveEditor::SelectWordAt(uint32_t offset)
{
	uint32_t p = editMode == EMarkdownEditMode::Rendered ? Snap(offset) : std::min(offset, doc.Size());
	// The class of what follows (at the end: what precedes).
	uint32_t q = StepPosition(p, 1);
	ECharClass cls = q != UINT32_MAX ? ClassBetween(p, q) : ECharClass::Break;
	if (cls == ECharClass::Break)
	{
		q = StepPosition(p, -1);
		cls = q != UINT32_MAX ? ClassBetween(q, p) : ECharClass::Break;
	}
	if (cls == ECharClass::Break) { SetSelection(p, p); return; }
	uint32_t a = p, b = p;
	while ((q = StepPosition(a, -1)) != UINT32_MAX && ClassBetween(q, a) == cls) a = q;
	while ((q = StepPosition(b, 1)) != UINT32_MAX && ClassBetween(b, q) == cls) b = q;
	SetSelection(a, b);
	preferredX = -1;
}

void CMarkdownLiveEditor::SelectParagraphAt(uint32_t offset)
{
	const uint32_t p = editMode == EMarkdownEditMode::Rendered ? Snap(offset) : std::min(offset, doc.Size());
	SetSelection(ParagraphEdge(p, -1), ParagraphEdge(p, 1));
	preferredX = -1;
}

void CMarkdownLiveEditor::MovePage(int direction, bool extend)
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return;
	const Line &line = layout.lines[li];
	const float x = preferredX >= 0 ? preferredX : XOfOffset(line, caret);
	// A page is the visible height less one line, so a line stays in view.
	const float page = std::max(line.height, (pageHeight > 0 ? pageHeight : 20.0f * FontSizeFor(0)) - line.height);
	const float targetY = line.y + direction * page;
	// The furthest line in that direction that does not pass the target.
	int target = -1;
	for (int i = 0; i < (int)layout.lines.size(); i++)
	{
		const Line &t = layout.lines[i];
		if (direction > 0 ? (t.y > line.y && t.y <= targetY) : (t.y < line.y && t.y >= targetY))
		{
			if (target < 0 || (direction > 0 ? t.y > layout.lines[target].y : t.y < layout.lines[target].y)) target = i;
		}
	}
	uint32_t o;
	if (target < 0)
	{
		// Less than a page left: the first or last line.
		for (int i = 0; i < (int)layout.lines.size(); i++)
		{
			const Line &t = layout.lines[i];
			if (direction > 0 ? t.y > line.y : t.y < line.y)
				if (target < 0 || (direction > 0 ? t.y >= layout.lines[target].y : t.y < layout.lines[target].y)) target = i;
		}
	}
	if (target < 0)
	{
		o = direction < 0 ? 0 : doc.Size();   // already on the first / last line
		if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
	}
	else
	{
		// Among lines on the target's band (table cells), the one under x.
		int bestLine = target;
		float bestDist = 1e30f;
		for (int i = 0; i < (int)layout.lines.size(); i++)
		{
			const Line &t = layout.lines[i];
			if (t.y != layout.lines[target].y) continue;
			const float lx0 = t.indent, lx1 = t.indent + t.width;
			const float dx = x < lx0 ? lx0 - x : (x > lx1 ? x - lx1 : 0);
			if (dx < bestDist) { bestDist = dx; bestLine = i; }
		}
		o = OffsetAtX(layout.lines[bestLine], x);
		if (editMode == EMarkdownEditMode::Rendered) o = Snap(o);
		pendingScroll += target >= 0 ? layout.lines[target].y - line.y : 0.0f;
	}
	SetSelection(extend ? anchor : o, o);
	preferredX = x;
	scrollToCaret = true;
}

void CMarkdownLiveEditor::EnsureCaretVisible(float viewTop)
{
	const int li = FindLineOfOffset(caret);
	if (li < 0) return;
	const Line &line = layout.lines[li];
	// SetScrollY() takes effect next frame, so a scroll requested this frame
	// is passed in.
	if (viewTop < 0) viewTop = ImGui::GetScrollY();
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
	if (editMode == EMarkdownEditMode::Rendered) offset = Snap(offset);
	SetSelection(extendSelection ? anchor : offset, offset);
	preferredX = -1;
	focused = true;
}

void CMarkdownLiveEditor::HandleMouse(ImVec2)
{
	const ImGuiIO &io = ImGui::GetIO();
	const bool hovered = ImGui::IsItemHovered();
	const Glyph *under = hovered ? GlyphAtScreen(io.MousePos) : nullptr;
	const bool overBox = under != nullptr && (under->deco == kDecoBox || under->deco == kDecoBoxChecked);
	std::string href;
	const bool overLink = under != nullptr && LinkOfGlyph(*under, &href);
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (overBox)
		{
			// The task box toggles in place; the caret does not move into it.
			ToggleTaskAt(under->offset);
			focused = true;
			return;
		}
		if (overLink && io.KeyCtrl)
		{
			OpenLink(href);
			focused = true;
			return;
		}
		ClickAt(io.MousePos, io.KeyShift);
		dragging = true;
		// Double click: the word under the pointer; triple click: its paragraph.
		const int clicks = ImGui::GetMouseClickedCount(ImGuiMouseButton_Left);
		if (clicks >= 2 && !io.KeyShift)
		{
			const uint32_t at = under != nullptr && !under->synthetic ? under->offset : caret;
			if (clicks == 2) SelectWordAt(at);
			else SelectParagraphAt(at);
			dragging = false;
		}
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
	{
		// Right-click in a table: caret into the cell, then the table menu.
		if (!(HasSelection() && GlyphAtScreen(io.MousePos) != nullptr)) ClickAt(io.MousePos, false);
		if (CaretInCell(nullptr)) ImGui::OpenPopup("##md_table_menu");
	}
	if (overLink && !dragging)
	{
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay) && ImGui::BeginTooltip())
		{
			ImGui::TextUnformatted(href.c_str());
#if defined(__APPLE__)
			ImGui::TextDisabled("%s", L("markdown.editor.link_hint", "Cmd+click to open"));
#else
			ImGui::TextDisabled("%s", L("markdown.editor.link_hint", "Ctrl+click to open"));
#endif
			ImGui::EndTooltip();
		}
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
				const bool ok = HitTestContent(p, &offset, &hit);
				if (ok && editMode == EMarkdownEditMode::Rendered) offset = Snap(offset);
				if (ok && offset != caret)
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
	if (hovered)
		ImGui::SetMouseCursor(overBox || (overLink && io.KeyCtrl) ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_TextInput);
}

const CMarkdownLiveEditor::Glyph *CMarkdownLiveEditor::GlyphAtScreen(ImVec2 screenPos) const
{
	const ImVec2 p(screenPos.x - contentOrigin.x, screenPos.y - contentOrigin.y);
	for (const Line &line : layout.lines)
	{
		if (p.y < line.y || p.y >= line.y + line.height) continue;
		for (const Glyph &g : line.glyphs)
			if (p.x >= g.x0 && p.x < g.x1) return &g;
	}
	return nullptr;
}

bool CMarkdownLiveEditor::LinkOfGlyph(const Glyph &g, std::string *href) const
{
	if (g.deco != kDecoNone || (g.synthetic && g.image < 0)) return false;
	const auto &spans = doc.GetSpans();
	// The innermost link around the glyph wins; an image outside any link
	// opens its own source.
	const uint32_t at = g.offset;
	int32_t best = -1;
	for (int32_t i = 0; i < (int32_t)spans.size(); i++)
	{
		const CMarkdownSpan &sp = spans[i];
		if (sp.type != EMarkdownSpanType::Link && sp.type != EMarkdownSpanType::WikiLink) continue;
		if (at >= sp.beg && at < sp.end) best = i;
	}
	if (best < 0 && g.image >= 0)
		best = doc.FindSpanAt(g.offset, EMarkdownSpanType::Image);
	if (best < 0) return false;
	// A revealed link's markers are being edited, not followed: only its text
	// and pictures are live.
	const CMarkdownSpan &sp = spans[best];
	if (g.image < 0 && !(at >= sp.contentBeg && at < sp.contentEnd)) return false;
	if (sp.href.empty()) return false;
	*href = sp.href;
	return true;
}

void CMarkdownLiveEditor::OpenLink(const std::string &href)
{
	if (onLinkActivated) { onLinkActivated(href); return; }
	ImGuiPlatformIO &pio = ImGui::GetPlatformIO();
	if (pio.Platform_OpenInShellFn) pio.Platform_OpenInShellFn(ImGui::GetCurrentContext(), href.c_str());
}

bool CMarkdownLiveEditor::ActivateLinkAt(uint32_t offset)
{
	const auto &spans = doc.GetSpans();
	int32_t best = -1;
	for (int32_t i = 0; i < (int32_t)spans.size(); i++)
	{
		const CMarkdownSpan &sp = spans[i];
		if (sp.type != EMarkdownSpanType::Link && sp.type != EMarkdownSpanType::WikiLink && sp.type != EMarkdownSpanType::Image) continue;
		if (sp.href.empty() || offset < sp.beg || offset >= sp.end) continue;
		// A link wins over an image inside it; the inner of two links wins.
		if (best < 0 || sp.type != EMarkdownSpanType::Image) best = i;
	}
	if (best < 0) return false;
	OpenLink(spans[best].href);
	return true;
}

// The fixed keys of a word processor. Each key with and without Shift (the
// selection grows). io.ConfigMacOSXBehaviors picks the Mac set; there ImGui
// has already swapped the modifiers, so ImGuiMod_Ctrl is Cmd and
// ImGuiMod_Super is Ctrl.
//
//                      Windows / Linux              Mac
//   character          Left / Right                 Left / Right
//   word               Ctrl+Left / Right            Alt+Left / Right
//   line               Up / Down                    Up / Down
//   paragraph          Ctrl+Up / Down               Alt+Up / Down
//   line start / end   Home / End                   Cmd+Left / Right, Home / End, Ctrl+A / E
//   page               Page Up / Down               Page Up / Down
//   document           Ctrl+Home / End              Cmd+Up / Down, Cmd+Home / End
//   delete             Backspace, Delete            Backspace, Delete, Ctrl+H / D
//   delete a word      Ctrl+Backspace / Delete      Alt+Backspace / Delete
//   delete to the line edge                         Cmd+Backspace / Delete
//   clipboard          Ctrl+X / C / V, Shift+Delete (cut), Ctrl+Insert (copy), Shift+Insert (paste)
bool CMarkdownLiveEditor::ProcessMovementAndDeletionKeys()
{
	const bool mac = ImGui::GetIO().ConfigMacOSXBehaviors;
	const ImGuiInputFlags rep = ImGuiInputFlags_Repeat;
	const ImGuiKeyChord wordMod = mac ? ImGuiMod_Alt : ImGuiMod_Ctrl;
	// Key, or Shift+key to extend the selection.
	auto key = [&](ImGuiKeyChord chord, bool *extend) {
		if (ImGui::Shortcut(chord, rep)) { *extend = false; return true; }
		if (ImGui::Shortcut(chord | ImGuiMod_Shift, rep)) { *extend = true; return true; }
		return false;
	};
	bool ext = false;
	if (key(ImGuiKey_LeftArrow, &ext)) MoveHorizontal(-1, ext);
	else if (key(ImGuiKey_RightArrow, &ext)) MoveHorizontal(1, ext);
	else if (key(ImGuiKey_UpArrow, &ext)) MoveVertical(-1, ext);
	else if (key(ImGuiKey_DownArrow, &ext)) MoveVertical(1, ext);
	else if (key(wordMod | ImGuiKey_LeftArrow, &ext)) MoveWord(-1, ext);
	else if (key(wordMod | ImGuiKey_RightArrow, &ext)) MoveWord(1, ext);
	else if (key(wordMod | ImGuiKey_UpArrow, &ext)) MoveParagraph(-1, ext);
	else if (key(wordMod | ImGuiKey_DownArrow, &ext)) MoveParagraph(1, ext);
	else if (key(ImGuiKey_PageUp, &ext)) MovePage(-1, ext);
	else if (key(ImGuiKey_PageDown, &ext)) MovePage(1, ext);
	else if (key(ImGuiKey_Home, &ext)) MoveLineEdge(false, ext);
	else if (key(ImGuiKey_End, &ext)) MoveLineEdge(true, ext);
	else if (key(ImGuiMod_Ctrl | ImGuiKey_Home, &ext)) MoveDocumentEdge(false, ext);
	else if (key(ImGuiMod_Ctrl | ImGuiKey_End, &ext)) MoveDocumentEdge(true, ext);
	else if (mac && key(ImGuiMod_Ctrl | ImGuiKey_LeftArrow, &ext)) MoveLineEdge(false, ext);
	else if (mac && key(ImGuiMod_Ctrl | ImGuiKey_RightArrow, &ext)) MoveLineEdge(true, ext);
	else if (mac && key(ImGuiMod_Ctrl | ImGuiKey_UpArrow, &ext)) MoveDocumentEdge(false, ext);
	else if (mac && key(ImGuiMod_Ctrl | ImGuiKey_DownArrow, &ext)) MoveDocumentEdge(true, ext);
	else if (mac && key(ImGuiMod_Super | ImGuiKey_A, &ext)) MoveLineEdge(false, ext);
	else if (mac && key(ImGuiMod_Super | ImGuiKey_E, &ext)) MoveLineEdge(true, ext);
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A)) SelectAll();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C) || ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Insert)) Copy();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X)) Cut();
	else if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, rep) || ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Insert, rep)) Paste();
	// Shift+Backspace is Backspace (Shift is often still down while typing).
	else if (ImGui::Shortcut(ImGuiKey_Backspace, rep) || ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Backspace, rep)) Backspace();
	else if (ImGui::Shortcut(ImGuiKey_Delete, rep)) DeleteForward();
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Delete, rep))
	{
		if (!mac && HasSelection()) Cut();
		else DeleteForward();
	}
	else if (ImGui::Shortcut(wordMod | ImGuiKey_Backspace, rep)) DeleteWord(-1);
	else if (ImGui::Shortcut(wordMod | ImGuiKey_Delete, rep)) DeleteWord(1);
	else if (mac && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Backspace, rep)) DeleteToLineEdge(false);
	else if (mac && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Delete, rep)) DeleteToLineEdge(true);
	else if (mac && ImGui::Shortcut(ImGuiMod_Super | ImGuiKey_H, rep)) Backspace();
	else if (mac && ImGui::Shortcut(ImGuiMod_Super | ImGuiKey_D, rep)) DeleteForward();
	else return false;
	return true;
}

void CMarkdownLiveEditor::ProcessKeyboard()
{
	ImGuiIO &io = ImGui::GetIO();
	io.WantCaptureKeyboard = true;
	io.WantTextInput = true;
	const ImGuiInputFlags rep = ImGuiInputFlags_Repeat;

	// Remappable commands first (CMarkdownEditorBindings), so a binding the
	// user chose wins over a fixed key with the same chord.
	CMarkdownEditorBindings *kb = GetBindings();
	auto pressed = [&](EMarkdownEditorCommand c, bool repeat) {
		const ImGuiKeyChord chord = kb->GetChord(c);
		return chord != 0 && ImGui::Shortcut(chord, repeat ? rep : ImGuiInputFlags_None);
	};
	const bool inCell = CaretInCell(nullptr);
	bool handled = true;
	if (inCell && pressed(EMarkdownEditorCommand::TableMoveRowUp, true)) EditTable(EMarkdownTableEdit::MoveRowUp);
	else if (inCell && pressed(EMarkdownEditorCommand::TableMoveRowDown, true)) EditTable(EMarkdownTableEdit::MoveRowDown);
	else if (inCell && pressed(EMarkdownEditorCommand::TableMoveColumnLeft, true)) EditTable(EMarkdownTableEdit::MoveColumnLeft);
	else if (inCell && pressed(EMarkdownEditorCommand::TableMoveColumnRight, true)) EditTable(EMarkdownTableEdit::MoveColumnRight);
	else if (pressed(EMarkdownEditorCommand::Bold, false)) ToggleStrong();
	else if (pressed(EMarkdownEditorCommand::Italic, false)) ToggleEmphasis();
	else if (pressed(EMarkdownEditorCommand::Underline, false)) ToggleInlineStyle(EMarkdownInlineStyle::Underline);
	else if (pressed(EMarkdownEditorCommand::Strikethrough, false)) ToggleInlineStyle(EMarkdownInlineStyle::Strikethrough);
	else if (pressed(EMarkdownEditorCommand::InlineCode, false)) ToggleInlineStyle(EMarkdownInlineStyle::Code);
	else if (pressed(EMarkdownEditorCommand::BulletList, false)) ToggleList(EMarkdownListKind::Bullet);
	else if (pressed(EMarkdownEditorCommand::NumberedList, false)) ToggleList(EMarkdownListKind::Numbered);
	else if (pressed(EMarkdownEditorCommand::TaskList, false)) ToggleList(EMarkdownListKind::Task);
	else if (pressed(EMarkdownEditorCommand::Quote, false)) ToggleQuote();
	else if (pressed(EMarkdownEditorCommand::InsertLink, false))
	{
		if (editMode == EMarkdownEditMode::Rendered) OpenLinkDialog();
		else InsertLink();
	}
	else if (pressed(EMarkdownEditorCommand::Heading1, false)) SetHeadingLevel(1);
	else if (pressed(EMarkdownEditorCommand::Heading2, false)) SetHeadingLevel(2);
	else if (pressed(EMarkdownEditorCommand::Heading3, false)) SetHeadingLevel(3);
	else if (pressed(EMarkdownEditorCommand::Heading4, false)) SetHeadingLevel(4);
	else if (pressed(EMarkdownEditorCommand::Heading5, false)) SetHeadingLevel(5);
	else if (pressed(EMarkdownEditorCommand::Heading6, false)) SetHeadingLevel(6);
	else if (pressed(EMarkdownEditorCommand::Paragraph, false)) SetHeadingLevel(0);
	else if (pressed(EMarkdownEditorCommand::Undo, true)) Undo();
	else if (pressed(EMarkdownEditorCommand::Redo, true) || pressed(EMarkdownEditorCommand::RedoAlternate, true)) Redo();
	else if (pressed(EMarkdownEditorCommand::Find, false)) OpenFind();
	else if (pressed(EMarkdownEditorCommand::Replace, false)) OpenReplace();
	else if (pressed(EMarkdownEditorCommand::FindNext, true)) { if (findQuery.empty()) OpenFind(); else FindNext(); }
	else if (pressed(EMarkdownEditorCommand::FindPrevious, true)) { if (findQuery.empty()) OpenFind(); else FindPrevious(); }
	else if (findOpen && ImGui::Shortcut(ImGuiKey_Escape)) CloseFind();
	else if (pressed(EMarkdownEditorCommand::ToggleTaskOrInsertRow, false))
	{
		if (inCell) EditTable(EMarkdownTableEdit::InsertRowBelow);
		else ToggleTask();
	}
	else handled = false;
	if (handled) {}
	// Table navigation: Tab is otherwise a character. Tab in the last cell
	// appends a row and goes to its first cell.
	else if (ImGui::Shortcut(ImGuiKey_Tab, rep))
	{
		if (inCell) { if (!MoveToNextCell(false) && EditTable(EMarkdownTableEdit::InsertRowBelow)) SelectFirstCellOfRow(); }
		else if (IsInList(EMarkdownListKind::Bullet) || IsInList(EMarkdownListKind::Numbered) || IsInList(EMarkdownListKind::Task)) IndentListItem(false);
	}
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Tab, rep))
	{
		if (inCell) MoveToNextCell(true);
		else IndentListItem(true);
	}
	else if (ProcessMovementAndDeletionKeys()) {}
	else if (ImGui::Shortcut(ImGuiKey_Enter, rep) || ImGui::Shortcut(ImGuiKey_KeypadEnter, rep))
	{
		if (!MoveToCellBelow()) InsertNewline();
	}
	else if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Enter, rep)) InsertLineBreak();

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
		if (!text.empty() && editMode == EMarkdownEditMode::Rendered)
		{
			// Several characters in one frame: one word at a time, so "- " at a
			// paragraph's start autoformats as if typed slowly.
			size_t p = 0;
			while (p < text.size())
			{
				size_t e = text.find(' ', p);
				e = e == std::string::npos ? text.size() : e + 1;
				InsertText(std::string_view(text).substr(p, e - p));
				p = e;
			}
		}
		else if (!text.empty()) InsertText(text);
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
	if (findOpen) UpdateFindMatches();
	bool selectionIsMatch = false;
	if (findOpen)
		for (const auto &m : findMatches)
			if (m.first == selA && m.second == selB) { selectionIsMatch = true; break; }
	// Amber on either theme: pale for every match, strong for the current one.
	const ImVec4 bgv = ImGui::GetStyleColorVec4(ImGuiCol_WindowBg);
	const bool darkTheme = 0.2126f * bgv.x + 0.7152f * bgv.y + 0.0722f * bgv.z < 0.5f;
	const ImU32 matchFill = darkTheme ? IM_COL32(0xFD, 0xE6, 0x8A, 0x48) : IM_COL32(0xFD, 0xE6, 0x8A, 0x99);
	const ImU32 currentMatchFill = darkTheme ? IM_COL32(0xF5, 0x9E, 0x0B, 0xA6) : IM_COL32(0xF5, 0x9E, 0x0B, 0xD9);
	const ImU32 currentMatchEdge = darkTheme ? IM_COL32(0xFB, 0xBF, 0x24, 0xFF) : IM_COL32(0xB4, 0x53, 0x09, 0xFF);
	const int caretLine = FindLineOfOffset(caret);
	// Any move or edit restarts the blink with the caret shown, so it never
	// hides while the user types or moves.
	if (caret != caretSeenOffset || anchor != caretSeenAnchor || doc.GetParseCount() != caretSeenParse)
	{
		caretSeenOffset = caret;
		caretSeenAnchor = anchor;
		caretSeenParse = doc.GetParseCount();
		caretBlinkStart = ImGui::GetTime();
	}
	const bool showCaret = focused;
	const bool caretOn = IsCaretShownNow();

	std::string segment;
	for (size_t li = 0; li < layout.lines.size(); li++)
	{
		const Line &line = layout.lines[li];
		const float baseY = origin.y + line.y + line.ascent;

		// Find matches: every match a pale amber, the current one (the
		// selection) a strong amber with an outline, drawn instead of the
		// selection so it is the most salient of all.
		if (findOpen && !findMatches.empty())
		{
			const float rx0 = origin.y + line.y, rx1 = origin.y + line.y + line.height;
			float cx0 = -1, cx1 = -1;
			for (const Glyph &g : line.glyphs)
			{
				if (g.synthetic) continue;
				auto it = std::upper_bound(findMatches.begin(), findMatches.end(), g.offset,
					[](uint32_t o, const std::pair<uint32_t, uint32_t> &m) { return o < m.first; });
				if (it == findMatches.begin()) continue;
				--it;
				if (!(g.offset >= it->first && g.offset < it->second)) continue;
				const bool current = it->first == selA && it->second == selB;
				dl->AddRectFilled(ImVec2(origin.x + g.x0, rx0), ImVec2(origin.x + g.x1, rx1), current ? currentMatchFill : matchFill);
				if (current) { if (cx0 < 0) cx0 = g.x0; cx1 = g.x1; }
			}
			if (cx0 >= 0) dl->AddRect(ImVec2(origin.x + cx0, rx0), ImVec2(origin.x + cx1, rx1), currentMatchEdge, 2.0f, 0, 1.5f);
		}

		// Selection (unless it is the current match, drawn above).
		if (selA != selB && !selectionIsMatch)
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

		// Text, in segments of equal style; pictures and shapes on their own.
		size_t i = 0;
		while (i < line.glyphs.size())
		{
			const Glyph &first = line.glyphs[i];
			if (first.image >= 0 || first.deco != kDecoNone)
			{
				const Style &st = layout.styles[first.style];
				const float gx0 = origin.x + first.x0, gx1 = origin.x + first.x1;
				// Shapes sit on the glyphs' baseline, half the line gap above baseY.
				const float shapeBase = baseY - (LineHeightFor(st) - st.size) * 0.5f;
				if (first.image >= 0 && first.image < (int)layout.images.size())
				{
					const float h = first.ascent - st.size * 0.1f;
					dl->AddImage(layout.images[first.image], ImVec2(gx0, baseY - h), ImVec2(gx1, baseY));
				}
				else if (first.deco == kDecoBullet)
				{
					dl->AddCircleFilled(ImVec2((gx0 + gx1) * 0.5f, shapeBase - st.size * 0.3f), st.size * 0.14f, st.color);
				}
				else
				{
					const float side = st.size * 0.8f;
					const ImVec2 bmin(gx0, shapeBase - side * 0.92f), bmax(gx0 + side, shapeBase + side * 0.08f);
					if (first.deco == kDecoBoxChecked)
					{
						dl->AddRectFilled(bmin, bmax, ImGui::GetColorU32(ImGuiCol_CheckMark), side * 0.18f);
						const ImU32 tick = ImGui::GetColorU32(ImGuiCol_WindowBg);
						dl->AddLine(ImVec2(bmin.x + side * 0.22f, bmin.y + side * 0.52f), ImVec2(bmin.x + side * 0.42f, bmin.y + side * 0.74f), tick, 2.0f);
						dl->AddLine(ImVec2(bmin.x + side * 0.42f, bmin.y + side * 0.74f), ImVec2(bmin.x + side * 0.80f, bmin.y + side * 0.28f), tick, 2.0f);
					}
					else
						dl->AddRect(bmin, bmax, st.color, side * 0.18f, 0, 1.5f);
				}
				i++;
				continue;
			}
			const uint16_t style = line.glyphs[i].style;
			const float x0 = line.glyphs[i].x0;
			segment.clear();
			size_t j = i;
			float x1 = x0;
			// One AddText() per run of touching glyphs: AddText lays a string out
			// contiguously, so a gap (a list marker's, an aligned cell's) ends it.
			while (j < line.glyphs.size() && line.glyphs[j].style == style && std::fabs(line.glyphs[j].x0 - x1) < 0.5f
				&& line.glyphs[j].image < 0 && line.glyphs[j].deco == kDecoNone)
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
			// The glyphs' own baseline: baseY sits half the line gap lower.
			const float glyphBaseline = pos.y + (LineHeightFor(st) - st.size) * 0.5f + glyphAscent;
			if (st.strike)
				dl->AddLine(ImVec2(origin.x + x0, glyphBaseline - st.size * 0.3f), ImVec2(origin.x + x1, glyphBaseline - st.size * 0.3f), st.color, 1.0f);
			if (st.underline)
				dl->AddLine(ImVec2(origin.x + x0, glyphBaseline + 1.5f), ImVec2(origin.x + x1, glyphBaseline + 1.5f), st.color, 1.0f);
			i = j;
		}

		// Caret.
		if (showCaret && (int)li == caretLine)
		{
			const float cx = origin.x + XOfOffset(line, caret);
			caretMin = ImVec2(cx, origin.y + line.y);
			caretMax = ImVec2(cx + std::max(2.0f, FontSizeFor(0) * 0.11f), origin.y + line.y + line.height);
			caretRectValid = true;   // the IME and the tests need it in the off phase too
			if (caretOn) dl->AddRectFilled(caretMin, caretMax, caretColor != 0 ? caretColor : ImGui::GetColorU32(ImGuiCol_Text));
		}
	}
}

bool CMarkdownLiveEditor::IsCaretShownNow() const
{
	// From the last move or edit: 0.53 s on, 0.53 s off (the common
	// platform rate), so the caret is always visible right after typing.
	if (!caretBlink || HasSelection()) return true;
	const double since = ImGui::GetTime() - caretBlinkStart;
	return std::fmod(std::max(0.0, since), 1.06) < 0.53;
}

void CMarkdownLiveEditor::Render(const char *id, ImVec2 size)
{
	ImGui::PushID(id);
	if (toolbarVisible)
	{
		// The toolbar sits above the find bar and the text.
		const float y0 = ImGui::GetCursorPosY();
		RenderToolbar();
		if (size.y > 0) size.y = std::max(1.0f, size.y - (ImGui::GetCursorPosY() - y0));
	}
	if (findOpen)
	{
		// The bar sits above the text, outside the scrolling area.
		RenderFindBar();
		if (size.y > 0) size.y = std::max(1.0f, size.y - ImGui::GetFrameHeightWithSpacing() * (replaceOpen ? 2 : 1));
	}
	const ImGuiWindowFlags flags = ImGuiWindowFlags_NoMove;
	ImGui::BeginChild("##md_live_editor", size, ImGuiChildFlags_None, flags);
	if (returnFocus)
	{
		ImGui::SetWindowFocus();
		focused = true;
		returnFocus = false;
	}

	const float pad = ImGui::GetStyle().FramePadding.x;
	const float width = std::max(10.0f, ImGui::GetContentRegionAvail().x - 2 * pad);
	EnsureLayout(width);
	pageHeight = ImGui::GetWindowHeight();

	const ImVec2 cursor = ImGui::GetCursorScreenPos();
	contentOrigin = ImVec2(cursor.x + pad, cursor.y + pad);
	const float itemHeight = std::max(layout.height + 2 * pad, ImGui::GetContentRegionAvail().y);
	ImGui::InvisibleButton("##md_live_editor_area", ImVec2(width + 2 * pad, itemHeight), ImGuiButtonFlags_MouseButtonLeft);
	itemMin = ImGui::GetItemRectMin();
	itemMax = ImGui::GetItemRectMax();

	GetBindings()->RegisterWithGuiMain();
	HandleMouse(contentOrigin);
	RenderTableMenu();
	RenderLinkDialog();

	const bool windowFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_ChildWindows);
	if (windowFocused && (ImGui::IsItemActive() || ImGui::IsItemHovered() || focused)) focused = true;
	if (!windowFocused) focused = false;
	if (focused) ProcessKeyboard();

	// Edits and caret moves may have invalidated the layout: redo it before
	// drawing so the frame shows the new state.
	EnsureLayout(width);
	float scrolledTo = -1;
	if (pendingScroll != 0)
	{
		// A page move scrolls the view by as much as the caret moved, so the
		// caret keeps its place on screen.
		const float maxY = std::max(0.0f, layout.height + 2 * pad - ImGui::GetWindowHeight());
		scrolledTo = std::clamp(ImGui::GetScrollY() + pendingScroll, 0.0f, maxY);
		ImGui::SetScrollY(scrolledTo);
		pendingScroll = 0;
	}
	if (scrollToCaret)
	{
		EnsureCaretVisible(scrolledTo);
		scrollToCaret = false;
	}

	Draw(ImGui::GetWindowDrawList(), contentOrigin);

	// IME and text input. The SDL3 backend starts SDL text input -- without
	// which no characters arrive at all -- only for a frame whose platform
	// IME data asks for it (ImGui's InputText does the same), and it places
	// the OS composition and candidate windows at InputPos. So: every focused
	// frame, at the caret.
	if (focused && !readOnly)
	{
		ImGuiPlatformImeData &ime = GImGui->PlatformImeData;
		ime.WantVisible = true;
		ime.WantTextInput = true;
		if (caretRectValid)
		{
			ime.InputPos = ImVec2(caretMin.x - 1.0f, caretMin.y);
			ime.InputLineHeight = caretMax.y - caretMin.y;
		}
		else
		{
			ime.InputPos = itemMin;
			ime.InputLineHeight = ImGui::GetFontSize();
		}
		ime.ViewportId = ImGui::GetWindowViewport()->ID;
	}

	ImGui::EndChild();
	ImGui::PopID();
}
