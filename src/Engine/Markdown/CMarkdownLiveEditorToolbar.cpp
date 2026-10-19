// CMarkdownLiveEditor, the formatting toolbar.
//
// A row above the text: paragraph style, inline styles, link, lists, indent,
// quote, table, undo/redo, find. Buttons show the state at the caret; the
// style box names the caret's block (a list, a quote, a table, code) where
// no paragraph style applies. A narrow toolbar wraps between groups, never
// inside one. In a table the table button is a menu of its rows, columns
// and alignment. Only
// what Markdown stores is offered (no colour, size, alignment); underline is
// `<u>text</u>`.
// Icons are drawn shapes: the engine has no icon font. Every command goes
// through the same public API a host would call, then gives the keyboard
// focus back to the text.

#include "CMarkdownLiveEditor.h"
#include "CMarkdownEditorBindings.h"
#include "imgui.h"
#include "imgui_internal.h"   // ImGui::GetKeyChordName

#include <algorithm>
#include <array>
#include <cfloat>
#include <string>

namespace
{
	ImU32 IconColor(bool enabled)
	{
		return ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled);
	}

	// Horizontal lines of a paragraph, from x0 to x1, n lines in the box.
	void DrawLines(ImDrawList *dl, ImVec2 a, ImVec2 b, float x0, int n, ImU32 col, float t)
	{
		const float h = b.y - a.y;
		for (int i = 0; i < n; i++)
		{
			const float y = a.y + h * (i + 0.5f) / n;
			dl->AddLine(ImVec2(x0, y), ImVec2(b.x, y), col, t);
		}
	}

	// Undo / redo: an arc over the top with the head down at its left / right end.
	void DrawUndoArrow(ImDrawList *dl, ImVec2 a, ImVec2 b, bool redo, ImU32 col, float t)
	{
		const float w = b.x - a.x, h = b.y - a.y;
		const ImVec2 c(a.x + w * 0.5f, a.y + h * 0.6f);
		const float r = std::min(w, h) * 0.38f;
		dl->PathArcTo(c, r, 0.0f, -3.14159265f, 12);
		dl->PathStroke(col, ImDrawFlags_None, t);
		const float hx = redo ? c.x + r : c.x - r;
		const float s = r * 0.5f;
		dl->AddTriangleFilled(ImVec2(hx - s, c.y), ImVec2(hx + s, c.y), ImVec2(hx, c.y + s * 1.2f), col);
	}
}

void CMarkdownLiveEditor::RenderToolbar()
{
	ImGui::PushID("##md_toolbar");
	const ImGuiStyle &style = ImGui::GetStyle();
	const float h = ImGui::GetFrameHeight();
	const float bw = h * 1.15f;
	const float lineRight = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
	const bool editable = !readOnly;
	bool first = true;
	CMarkdownEditorBindings *kb = GetBindings();

	// The next item on this row, or the next row when it would not fit. A
	// group's first item moves to the next row when the whole group would not
	// fit, so a group is split only when it is wider than the toolbar. A group
	// separator is drawn only between items on one row.
	bool separatorDue = false;
	float groupWidth = 0;
	auto place = [&](float w) {
		const float need = std::max(w, groupWidth);
		groupWidth = 0;
		if (!first)
		{
			const float sepW = separatorDue ? style.ItemSpacing.x * 2 : 0.0f;
			ImGui::SameLine();
			if (ImGui::GetCursorPosX() + sepW + need > lineRight) ImGui::NewLine();
			else if (separatorDue)
			{
				const ImVec2 p = ImGui::GetCursorScreenPos();
				ImGui::Dummy(ImVec2(style.ItemSpacing.x, h));
				const float x = p.x + style.ItemSpacing.x * 0.5f;
				ImGui::GetWindowDrawList()->AddLine(ImVec2(x, p.y + h * 0.2f), ImVec2(x, p.y + h * 0.8f), ImGui::GetColorU32(ImGuiCol_Separator), 1.0f);
				ImGui::SameLine();
			}
		}
		separatorDue = false;
		first = false;
	};
	// A new group of `buttons` square buttons (plus `extra` width).
	auto separator = [&](int buttons, float extra = 0.0f) {
		separatorDue = true;
		groupWidth = buttons * bw + std::max(0, buttons - 1) * style.ItemSpacing.x + extra;
	};
	auto tooltip = [&](const char *key, const char *fallback, ImGuiKeyChord chord) {
		if (!ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) return;
		std::string tip = L(key, fallback);
		if (chord != 0) tip += std::string(" (") + ImGui::GetKeyChordName(chord) + ")";
		ImGui::SetTooltip("%s", tip.c_str());
	};
	// A square button; draw() paints its icon into the item's rectangle.
	auto button = [&](const char *id, bool active, bool enabled, auto draw) {
		place(bw);
		if (!enabled) ImGui::BeginDisabled();
		// Flat buttons, as in a word processor; the active state is filled.
		ImGui::PushStyleColor(ImGuiCol_Button, active ? ImGui::GetColorU32(ImGuiCol_ButtonActive) : 0u);
		const bool clicked = ImGui::Button(id, ImVec2(bw, h));
		ImGui::PopStyleColor();
		if (!enabled) ImGui::EndDisabled();
		const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
		const float m = h * 0.24f;
		draw(ImGui::GetWindowDrawList(), ImVec2(a.x + m, a.y + m), ImVec2(b.x - m, b.y - m), IconColor(enabled));
		if (clicked) returnFocus = true;
		return clicked;
	};
	auto letter = [&](ImFont *font, const char *text) {
		return [font, text](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			ImFont *f = font != nullptr ? font : ImGui::GetFont();
			const float size = (b.y - a.y) * 1.45f;
			const ImVec2 ts = f->CalcTextSizeA(size, FLT_MAX, 0, text);
			dl->AddText(f, size, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), col, text);
		};
	};
	const float t = std::max(1.0f, h * 0.07f);

	// Paragraph style.
	const int level = GetHeadingLevelAtCaret();
	const char *styleKeys[] = { "markdown.editor.toolbar.normal", "markdown.editor.toolbar.heading1", "markdown.editor.toolbar.heading2",
		"markdown.editor.toolbar.heading3", "markdown.editor.toolbar.heading4" };
	const char *styleFallbacks[] = { "Normal", "Heading 1", "Heading 2", "Heading 3", "Heading 4" };
	const EMarkdownEditorCommand styleCommands[] = { EMarkdownEditorCommand::Paragraph, EMarkdownEditorCommand::Heading1,
		EMarkdownEditorCommand::Heading2, EMarkdownEditorCommand::Heading3, EMarkdownEditorCommand::Heading4 };
	// Where no paragraph style applies, the box names the block instead.
	struct BlockName { EMarkdownCaretBlock kind; const char *key; const char *fallback; };
	static const BlockName blockNames[] = {
		{ EMarkdownCaretBlock::BulletList, "markdown.editor.toolbar.style_bullets", "Bulleted list" },
		{ EMarkdownCaretBlock::NumberedList, "markdown.editor.toolbar.style_numbers", "Numbered list" },
		{ EMarkdownCaretBlock::TaskList, "markdown.editor.toolbar.style_tasks", "Task list" },
		{ EMarkdownCaretBlock::Quote, "markdown.editor.toolbar.style_quote", "Quote" },
		{ EMarkdownCaretBlock::Table, "markdown.editor.toolbar.style_table", "Table" },
		{ EMarkdownCaretBlock::Code, "markdown.editor.toolbar.style_code", "Code" },
		{ EMarkdownCaretBlock::FrontMatter, "markdown.editor.toolbar.style_front_matter", "Metadata" },
	};
	float comboW = 0;
	for (int i = 0; i < 5; i++) comboW = std::max(comboW, ImGui::CalcTextSize(L(styleKeys[i], styleFallbacks[i])).x);
	for (const BlockName &n : blockNames) comboW = std::max(comboW, ImGui::CalcTextSize(L(n.key, n.fallback)).x);
	comboW += h + style.FramePadding.x * 2;
	place(comboW);
	ImGui::SetNextItemWidth(comboW);
	if (!editable) ImGui::BeginDisabled();
	const EMarkdownCaretBlock kind = GetBlockKindAtCaret();
	const int shown = level >= 0 && level <= 4 ? level : -1;
	std::string preview = shown >= 0 ? L(styleKeys[shown], styleFallbacks[shown]) : (level > 4 ? "H" + std::to_string(level) : "");
	for (const BlockName &n : blockNames)
		if (n.kind == kind) preview = L(n.key, n.fallback);
	if (ImGui::BeginCombo("###md_tb_style", preview.c_str()))
	{
		for (int i = 0; i < 5; i++)
		{
			const std::string label = std::string(L(styleKeys[i], styleFallbacks[i])) + "###md_tb_style_" + std::to_string(i);
			if (ImGui::Selectable(label.c_str(), i == level))
			{
				SetHeadingLevel(i);
				returnFocus = true;
			}
			const ImGuiKeyChord chord = kb->GetChord(styleCommands[i]);
			if (chord != 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", ImGui::GetKeyChordName(chord));
		}
		ImGui::EndCombo();
	}
	if (!editable) ImGui::EndDisabled();
	tooltip("markdown.editor.toolbar.style", "Paragraph style", 0);

	separator(6);
	if (button("###md_tb_bold", IsInlineStyleActive(EMarkdownInlineStyle::Strong), editable, letter(fonts.bold, "B")))
		ToggleInlineStyle(EMarkdownInlineStyle::Strong);
	tooltip("markdown.editor.toolbar.bold", "Bold", kb->GetChord(EMarkdownEditorCommand::Bold));
	if (button("###md_tb_italic", IsInlineStyleActive(EMarkdownInlineStyle::Emphasis), editable, letter(fonts.italic, "I")))
		ToggleInlineStyle(EMarkdownInlineStyle::Emphasis);
	tooltip("markdown.editor.toolbar.italic", "Italic", kb->GetChord(EMarkdownEditorCommand::Italic));
	if (button("###md_tb_underline", IsInlineStyleActive(EMarkdownInlineStyle::Underline), editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			letter(fonts.regular, "U")(dl, ImVec2(a.x, a.y - (b.y - a.y) * 0.08f), ImVec2(b.x, b.y - (b.y - a.y) * 0.08f), col);
			const float y = b.y + (b.y - a.y) * 0.16f;
			dl->AddLine(ImVec2(a.x + (b.x - a.x) * 0.1f, y), ImVec2(b.x - (b.x - a.x) * 0.1f, y), col, t);
		}))
		ToggleInlineStyle(EMarkdownInlineStyle::Underline);
	tooltip("markdown.editor.toolbar.underline", "Underline", kb->GetChord(EMarkdownEditorCommand::Underline));
	if (button("###md_tb_strike", IsInlineStyleActive(EMarkdownInlineStyle::Strikethrough), editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			letter(fonts.regular, "S")(dl, a, b, col);
			const float y = (a.y + b.y) * 0.5f;
			dl->AddLine(ImVec2(a.x, y), ImVec2(b.x, y), col, t);
		}))
		ToggleInlineStyle(EMarkdownInlineStyle::Strikethrough);
	tooltip("markdown.editor.toolbar.strikethrough", "Strikethrough", kb->GetChord(EMarkdownEditorCommand::Strikethrough));
	if (button("###md_tb_code", IsInlineStyleActive(EMarkdownInlineStyle::Code), editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			// "< >" drawn as two chevrons.
			const float w = b.x - a.x, cy = (a.y + b.y) * 0.5f, s = (b.y - a.y) * 0.4f;
			dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(a.x + w * 0.35f, cy - s), ImVec2(a.x, cy), ImVec2(a.x + w * 0.35f, cy + s) }.data(), 3, col, 0, t);
			dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(b.x - w * 0.35f, cy - s), ImVec2(b.x, cy), ImVec2(b.x - w * 0.35f, cy + s) }.data(), 3, col, 0, t);
		}))
		ToggleInlineStyle(EMarkdownInlineStyle::Code);
	tooltip("markdown.editor.toolbar.code", "Inline code", kb->GetChord(EMarkdownEditorCommand::InlineCode));
	if (button("###md_tb_link", false, editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			// Two links of a chain, at 45 degrees drawn as overlapping rounded boxes.
			const float w = b.x - a.x, hh = (b.y - a.y);
			const float r = hh * 0.22f;
			dl->AddRect(ImVec2(a.x, a.y + hh * 0.28f), ImVec2(a.x + w * 0.6f, a.y + hh * 0.72f), col, r, 0, t);
			dl->AddRect(ImVec2(a.x + w * 0.4f, a.y + hh * 0.28f), ImVec2(b.x, a.y + hh * 0.72f), col, r, 0, t);
		}))
	{
		if (editMode == EMarkdownEditMode::Rendered) OpenLinkDialog();
		else InsertLink();
		returnFocus = false;   // the dialog takes the keyboard
	}
	tooltip("markdown.editor.toolbar.link", "Link", kb->GetChord(EMarkdownEditorCommand::InsertLink));

	separator(6);
	const bool inBullet = IsInList(EMarkdownListKind::Bullet), inNumbered = IsInList(EMarkdownListKind::Numbered), inTask = IsInList(EMarkdownListKind::Task);
	if (button("###md_tb_bullets", inBullet, editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			const float hh = b.y - a.y, r = hh * 0.09f;
			for (int i = 0; i < 3; i++) dl->AddCircleFilled(ImVec2(a.x + r, a.y + hh * (i + 0.5f) / 3), r * 1.3f, col);
			DrawLines(dl, a, b, a.x + hh * 0.38f, 3, col, t);
		}))
		ToggleList(EMarkdownListKind::Bullet);
	tooltip("markdown.editor.toolbar.bullets", "Bulleted list", kb->GetChord(EMarkdownEditorCommand::BulletList));
	if (button("###md_tb_numbers", inNumbered, editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			const float hh = b.y - a.y;
			ImFont *f = ImGui::GetFont();
			const float size = hh * 0.5f;
			const char *n[3] = { "1", "2", "3" };
			for (int i = 0; i < 3; i++)
				dl->AddText(f, size, ImVec2(a.x - hh * 0.05f, a.y + hh * (i + 0.5f) / 3 - size * 0.5f), col, n[i]);
			DrawLines(dl, a, b, a.x + hh * 0.38f, 3, col, t);
		}))
		ToggleList(EMarkdownListKind::Numbered);
	tooltip("markdown.editor.toolbar.numbers", "Numbered list", kb->GetChord(EMarkdownEditorCommand::NumberedList));
	if (button("###md_tb_tasks", inTask, editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			const float hh = b.y - a.y, s = hh * 0.2f;
			for (int i = 0; i < 2; i++)
			{
				const float cy = a.y + hh * (i + 0.5f) / 2;
				dl->AddRect(ImVec2(a.x, cy - s), ImVec2(a.x + 2 * s, cy + s), col, 1.0f, 0, t);
			}
			const float cy = a.y + hh * 0.25f;
			dl->AddPolyline(std::array<ImVec2, 3>{ ImVec2(a.x + s * 0.4f, cy), ImVec2(a.x + s * 0.9f, cy + s * 0.5f), ImVec2(a.x + s * 1.7f, cy - s * 0.6f) }.data(), 3, col, 0, t);
			DrawLines(dl, a, b, a.x + hh * 0.45f, 2, col, t);
		}))
		ToggleList(EMarkdownListKind::Task);
	tooltip("markdown.editor.toolbar.tasks", "Task list", kb->GetChord(EMarkdownEditorCommand::TaskList));
	const bool inList = inBullet || inNumbered || inTask;
	auto indentIcon = [&](bool out) {
		return [&, out](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			const float hh = b.y - a.y;
			const float cy = (a.y + b.y) * 0.5f, s = hh * 0.2f;
			if (out) dl->AddTriangleFilled(ImVec2(a.x, cy), ImVec2(a.x + s * 1.3f, cy - s), ImVec2(a.x + s * 1.3f, cy + s), col);
			else dl->AddTriangleFilled(ImVec2(a.x + s * 1.3f, cy), ImVec2(a.x, cy - s), ImVec2(a.x, cy + s), col);
			DrawLines(dl, a, b, a.x + hh * 0.45f, 3, col, t);
		};
	};
	if (button("###md_tb_outdent", false, editable && inList, indentIcon(true))) IndentListItem(true);
	tooltip("markdown.editor.toolbar.outdent", "Decrease indent", ImGuiMod_Shift | ImGuiKey_Tab);
	if (button("###md_tb_indent", false, editable && inList, indentIcon(false))) IndentListItem(false);
	tooltip("markdown.editor.toolbar.indent", "Increase indent", ImGuiKey_Tab);
	if (button("###md_tb_quote", IsInQuote(), editable,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			const float hh = b.y - a.y;
			dl->AddRectFilled(ImVec2(a.x, a.y), ImVec2(a.x + hh * 0.14f, b.y), col);
			DrawLines(dl, a, b, a.x + hh * 0.38f, 3, col, t);
		}))
		ToggleQuote();
	tooltip("markdown.editor.toolbar.quote", "Quote", kb->GetChord(EMarkdownEditorCommand::Quote));

	// Table: inserts one, or in a table opens the table menu (a chevron says so).
	const bool inTable = IsInTable();
	const float tableW = inTable ? bw * 1.5f : bw;
	separator(0, tableW);
	{
		place(tableW);
		if (!editable) ImGui::BeginDisabled();
		ImGui::PushStyleColor(ImGuiCol_Button, 0u);
		const bool clicked = ImGui::Button("###md_tb_table", ImVec2(tableW, h));
		ImGui::PopStyleColor();
		if (!editable) ImGui::EndDisabled();
		const ImVec2 ia = ImGui::GetItemRectMin(), ib = ImGui::GetItemRectMax();
		const float m = h * 0.24f;
		const ImVec2 a(ia.x + m, ia.y + m), b(ia.x + bw - m, ib.y - m);
		ImDrawList *dl = ImGui::GetWindowDrawList();
		const ImU32 col = IconColor(editable);
		dl->AddRect(a, b, col, 0, 0, t);
		const float w = b.x - a.x, hh = b.y - a.y;
		dl->AddLine(ImVec2(a.x, a.y + hh / 3), ImVec2(b.x, a.y + hh / 3), col, t);
		dl->AddLine(ImVec2(a.x, a.y + hh * 2 / 3), ImVec2(b.x, a.y + hh * 2 / 3), col, t);
		dl->AddLine(ImVec2(a.x + w / 2, a.y), ImVec2(a.x + w / 2, b.y), col, t);
		if (inTable)
		{
			const float cx = (b.x + ib.x) * 0.5f - m * 0.15f, cy = (ia.y + ib.y) * 0.5f, s = h * 0.1f;
			dl->AddTriangleFilled(ImVec2(cx - s, cy - s * 0.5f), ImVec2(cx + s, cy - s * 0.5f), ImVec2(cx, cy + s * 0.6f), col);
		}
		if (clicked)
		{
			if (inTable)
			{
				// Under the button, opening toward the middle of the window.
				ImGui::OpenPopup("##md_tb_table_menu");
				const ImGuiViewport *vp = ImGui::GetWindowViewport();
				const bool rightHalf = ia.x > vp->Pos.x + vp->Size.x * 0.5f;
				tableMenuPos = ImVec2(rightHalf ? ib.x : ia.x, ib.y);
				tableMenuPivot = ImVec2(rightHalf ? 1.0f : 0.0f, 0.0f);
			}
			else
			{
				InsertTable(2, 3);
				returnFocus = true;
			}
		}
		if (inTable) tooltip("markdown.editor.toolbar.table_menu", "Table: rows, columns, alignment", 0);
		else tooltip("markdown.editor.toolbar.table", "Insert table", 0);
		ImGui::SetNextWindowPos(tableMenuPos, ImGuiCond_Always, tableMenuPivot);
		if (ImGui::BeginPopup("##md_tb_table_menu"))
		{
			RenderTableMenuItems();
			ImGui::EndPopup();
		}
	}

	separator(2);
	if (button("###md_tb_undo", false, editable && CanUndo(),
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) { DrawUndoArrow(dl, a, b, false, col, t); }))
		Undo();
	tooltip("markdown.editor.toolbar.undo", "Undo", kb->GetChord(EMarkdownEditorCommand::Undo));
	if (button("###md_tb_redo", false, editable && CanRedo(),
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) { DrawUndoArrow(dl, a, b, true, col, t); }))
		Redo();
	tooltip("markdown.editor.toolbar.redo", "Redo", kb->GetChord(EMarkdownEditorCommand::Redo));

	separator(1);
	if (button("###md_tb_find", findOpen, true,
		[&](ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 col) {
			// A magnifier: a round lens of 0.8 of the icon's height, a short
			// thick handle at 45 degrees (it may reach into the margin).
			const float hh = b.y - a.y, r = hh * 0.4f;
			const ImVec2 c((a.x + b.x) * 0.5f - hh * 0.2f, a.y + hh * 0.4f);
			dl->AddCircle(c, r, col, 24, t * 1.3f);
			const float d0 = r + t * 0.5f, d1 = r + hh * 0.55f;
			dl->AddLine(ImVec2(c.x + d0 * 0.7071f, c.y + d0 * 0.7071f), ImVec2(c.x + d1 * 0.7071f, c.y + d1 * 0.7071f), col, t * 2.2f);
		}))
	{
		if (findOpen) CloseFind();
		else { OpenFind(); returnFocus = false; }
	}
	tooltip("markdown.editor.toolbar.find", "Find", kb->GetChord(EMarkdownEditorCommand::Find));
	ImGui::PopID();
}
