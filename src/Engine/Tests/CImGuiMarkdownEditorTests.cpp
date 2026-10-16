#include "CImGuiMarkdownEditorTests.h"
#include "imgui.h"
#include "imgui_internal.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "CGuiMain.h"
#include "CGuiViewMarkdownEditor.h"
#include "CMarkdownEditorBindings.h"
#include "CSlrKeyboardShortcuts.h"
#include "MT_CaptureHelpers.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace
{
	CGuiViewMarkdownEditor *s_view = nullptr;

	CGuiViewMarkdownEditor *EnsureView(ImGuiTestContext *ctx)
	{
		if (s_view == nullptr)
		{
			// Inside the headless app window, so captures see all of it.
			s_view = new CGuiViewMarkdownEditor("MarkdownEditorTest", 20, 30, -1, 600, 420);
			guiMain->AddView(s_view);
		}
		s_view->SetVisible(true);
		ctx->Yield(2);
		return s_view;
	}

	// Click just inside the glyph at `offset`, so the caret lands before it.
	bool ClickOffset(ImGuiTestContext *ctx, CMarkdownLiveEditor &ed, uint32_t offset, bool shift = false)
	{
		ImVec2 pos;
		float h = 0;
		if (!ed.GetOffsetScreenPos(offset, &pos, &h)) return false;
		ctx->MouseMoveToPos(ImVec2(pos.x + 1.0f, pos.y + h * 0.5f));
		if (shift) ctx->KeyDown(ImGuiMod_Shift);
		ctx->MouseClick(ImGuiMouseButton_Left);
		if (shift) ctx->KeyUp(ImGuiMod_Shift);
		ctx->Yield(2);
		return true;
	}

	// Is the byte at `offset` drawn (has a glyph) in the current layout?
	bool OffsetHasGlyph(const CMarkdownLiveEditor &ed, uint32_t offset)
	{
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (!g.synthetic && g.offset == offset) return true;
		return false;
	}

	float GlyphSize(const CMarkdownLiveEditor &ed, uint32_t offset)
	{
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (!g.synthetic && g.offset == offset) return line.height;
		return 0;
	}
}

void RegisterMarkdownEditorTests(ImGuiTestEngine *engine)
{
	ImGuiTest *t = IM_REGISTER_TEST(engine, "markdown", "click_places_caret_and_reveals_block");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetSource("Hello **bold** world\n\nSecond paragraph here\n");
		ctx->Yield(2);

		// Rendered: the '**' of the first paragraph are hidden while the caret
		// (at 0, so the first paragraph IS revealed) -- move it away first.
		const uint32_t second = (uint32_t)ed.GetSource().find("Second");
		IM_CHECK(ClickOffset(ctx, ed, second));
		IM_CHECK_EQ(ed.GetCaret(), second);
		IM_CHECK(ed.IsFocused());
		IM_CHECK_EQ(ed.GetRevealedBlock(), ed.GetDocument().FindBlockAt(second, true));
		IM_CHECK(!OffsetHasGlyph(ed, 6));          // the first '*' of '**bold**' is hidden
		IM_CHECK(OffsetHasGlyph(ed, 8));           // 'b' of bold is drawn

		// Click into "world" of the first paragraph: it is revealed, syntax visible.
		const uint32_t world = (uint32_t)ed.GetSource().find("world");
		IM_CHECK(ClickOffset(ctx, ed, world));
		IM_CHECK_EQ(ed.GetCaret(), world);
		IM_CHECK_EQ(ed.GetRevealedBlock(), 1);
		// Inline markers show only for the span the caret touches: not yet.
		IM_CHECK(!OffsetHasGlyph(ed, 6));
		// ...and the second paragraph is still drawn as content.
		IM_CHECK(OffsetHasGlyph(ed, second));
		// Into "bold": its '**' are drawn (dimmed).
		IM_CHECK(ClickOffset(ctx, ed, 9));
		IM_CHECK_EQ(ed.GetCaret(), 9u);
		IM_CHECK(OffsetHasGlyph(ed, 6));
		IM_CHECK(OffsetHasGlyph(ed, 12));
		IM_CHECK(ClickOffset(ctx, ed, world));

		// The caret rectangle sits where the glyph is.
		ImVec2 cmin, cmax, gpos;
		float gh = 0;
		IM_CHECK(ed.GetCaretScreenRect(&cmin, &cmax));
		IM_CHECK(ed.GetOffsetScreenPos(world, &gpos, &gh));
		IM_CHECK_LT(ImFabs(cmin.x - gpos.x), 2.0f);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "typing_backspace_and_round_trip");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string original = "First line *kept* exactly\n\n- item one\n- item two\n\n```\ncode block\n```\n";
		ed.SetSource(original);
		ctx->Yield(2);

		const uint32_t at = (uint32_t)original.find(" *kept*");
		IM_CHECK(ClickOffset(ctx, ed, at));
		IM_CHECK_EQ(ed.GetCaret(), at);
		ctx->KeyChars(" typed");
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "First line typed *kept* exactly\n\n- item one\n- item two\n\n```\ncode block\n```\n");
		IM_CHECK_EQ(ed.GetCaret(), at + 6);
		IM_CHECK(ed.IsDirty());

		ctx->KeyPress(ImGuiKey_Backspace);
		ctx->Yield();
		IM_CHECK_EQ(ed.GetCaret(), at + 5);
		ctx->KeyPress(ImGuiKey_Backspace, 5);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), original.c_str());   // byte-identical round trip
		IM_CHECK_EQ(ed.GetCaret(), at);

		// Delete forward eats the space before '*kept*'.
		ctx->KeyPress(ImGuiKey_Delete);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "First line*kept* exactly\n\n- item one\n- item two\n\n```\ncode block\n```\n");

		// Undo restores it; redo re-applies it.
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), original.c_str());
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "First line*kept* exactly\n\n- item one\n- item two\n\n```\ncode block\n```\n");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), original.c_str());
	};

	t = IM_REGISTER_TEST(engine, "markdown", "heading_shortcuts_change_level_and_size");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetSource("Plain title\n\nBody text.\n");
		ctx->Yield(2);

		IM_CHECK(ClickOffset(ctx, ed, 2));
		const float plainHeight = GlyphSize(ed, 2);
		IM_CHECK_GT(plainHeight, 0.0f);

		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_2);
		ctx->Yield(2);
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "## Plain title\n\nBody text.\n");
		IM_CHECK_EQ(ed.GetCaret(), 5u);
		IM_CHECK_GT(GlyphSize(ed, 5), plainHeight * 1.3f);   // the revealed heading is drawn large
		IM_CHECK(OffsetHasGlyph(ed, 0));                      // and its '## ' is visible (revealed)

		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_1);
		ctx->Yield(2);
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "# Plain title\n\nBody text.\n");
		IM_CHECK_GT(GlyphSize(ed, 4), GlyphSize(ed, (uint32_t)ed.GetSource().find("Body")));

		// Move the caret to the body: the heading renders without its marker.
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)ed.GetSource().find("Body")));
		IM_CHECK(!OffsetHasGlyph(ed, 0));
		IM_CHECK(OffsetHasGlyph(ed, 2));

		// Back to a paragraph.
		IM_CHECK(ClickOffset(ctx, ed, 4));
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_0);
		ctx->Yield(2);
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "Plain title\n\nBody text.\n");

		// Bold toggles around a selection.
		IM_CHECK(ClickOffset(ctx, ed, 0));
		IM_CHECK(ClickOffset(ctx, ed, 5, true));
		IM_CHECK(ed.HasSelection());
		IM_CHECK_STR_EQ(ed.GetSelectedText().c_str(), "Plain");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_B);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "**Plain** title\n\nBody text.\n");
		IM_CHECK_STR_EQ(ed.GetSelectedText().c_str(), "Plain");
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_B);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "Plain title\n\nBody text.\n");
	};

	t = IM_REGISTER_TEST(engine, "markdown", "enter_continues_and_ends_lists");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetSource("1. one\n2. two\n");
		ctx->Yield(2);

		// Caret at the end of "two" (before its '\n').
		const uint32_t endTwo = (uint32_t)ed.GetSource().find("two") + 3;
		IM_CHECK(ClickOffset(ctx, ed, endTwo - 1));
		ctx->KeyPress(ImGuiKey_RightArrow);
		ctx->Yield();
		IM_CHECK_EQ(ed.GetCaret(), endTwo);
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "1. one\n2. two\n3. \n");
		ctx->KeyChars("three");
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "1. one\n2. two\n3. three\n");
		// Enter on a fresh empty item ends the list.
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "1. one\n2. two\n3. three\n4. \n");
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), "1. one\n2. two\n3. three\n\n");
		// The list renders with its own numbers, markers hidden when not revealed.
		IM_CHECK_EQ(ed.GetDocument().GetBlocks()[1].type, EMarkdownBlockType::OrderedList);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "table_cell_edit_keeps_grid_and_source");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string original =
			"Intro paragraph.\n"
			"\n"
			"| Topic | Owner |\n"
			"|-------|:-----:|\n"
			"| Intro | Ann   |\n"
			"| Plan  |       |\n"
			"\n"
			"Outro.\n";
		ed.SetSource(original);
		ctx->Yield(2);

		const CMarkdownDocument &doc = ed.GetDocument();
		int32_t table = -1;
		for (int32_t i = 1; i < (int32_t)doc.GetBlocks().size(); i++)
			if (doc.GetBlocks()[i].type == EMarkdownBlockType::Table) { table = i; break; }
		IM_CHECK(table > 0);

		// Click into "Ann": the cell is revealed, the table stays a grid.
		const uint32_t ann = (uint32_t)original.find("Ann");
		IM_CHECK(ClickOffset(ctx, ed, ann));
		IM_CHECK_EQ(ed.GetCaret(), ann);
		IM_CHECK(doc.GetBlocks()[ed.GetRevealedBlock()].IsCell());
		IM_CHECK(!OffsetHasGlyph(ed, (uint32_t)original.find("|---")));   // delimiter row stays hidden

		ctx->KeyPress(ImGuiKey_End);
		ctx->Yield();
		ctx->KeyChars("a");
		ctx->Yield();
		std::string expected = original;
		expected.insert(ann + 3, "a");
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), expected.c_str());   // one byte changed, nothing else

		// Tab selects the next cell (the empty one on the next row is reached
		// after the "Plan" cell); its content gets replaced by typing.
		ctx->KeyPress(ImGuiKey_Tab);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSelectedText().c_str(), "Plan");
		ctx->KeyPress(ImGuiKey_Tab);
		ctx->Yield();
		IM_CHECK(!ed.HasSelection());   // the empty cell
		ctx->KeyChars("Bob");
		ctx->Yield();
		expected.insert(expected.find("| Plan  |") + 10, "Bob");   // one blank inside the pipe
		IM_CHECK_STR_EQ(ed.GetSource().c_str(), expected.c_str());

		// Shift+Tab goes back and selects "Plan"; Enter moves down a row.
		ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_Tab);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSelectedText().c_str(), "Plan");
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)ed.GetSource().find("Intro |") ));
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		IM_CHECK_STR_EQ(ed.GetSelectedText().c_str(), "Plan");
		// Still one table, still three rows, Outro untouched.
		IM_CHECK_EQ((int)doc.RowsOf(table).size(), 3);
		IM_CHECK(ed.GetSource().find("\nOutro.\n") != std::string::npos);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "arrows_cross_blocks_and_wrap_lines");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		std::string longPara = "# Head\n\n";
		for (int i = 0; i < 40; i++) longPara += "word" + std::to_string(i) + " ";
		longPara += "\n\n> quoted\n";
		ed.SetSource(longPara);
		ctx->Yield(2);

		// The long paragraph wraps into several visual lines.
		int paraLines = 0;
		for (const auto &line : ed.GetLayoutLines())
			if (line.block == 2) paraLines++;
		IM_CHECK_GT(paraLines, 2);

		// Down from the heading crosses the blank line and lands in the
		// paragraph (the revealed heading shows its gap line too, so it can
		// take two presses); keep going to the quote.
		IM_CHECK(ClickOffset(ctx, ed, 2));
		for (int i = 0; i < 3 && ed.GetRevealedBlock() != 2; i++) { ctx->KeyPress(ImGuiKey_DownArrow); ctx->Yield(); }
		IM_CHECK(ed.GetCaret() >= 8 && ed.GetCaret() < longPara.find("> quoted"));
		IM_CHECK_EQ(ed.GetRevealedBlock(), 2);
		for (int i = 0; i < paraLines + 3 && ed.GetCaret() < longPara.find("> quoted"); i++) { ctx->KeyPress(ImGuiKey_DownArrow); ctx->Yield(); }
		IM_CHECK(ed.GetCaret() >= longPara.find("> quoted"));
		// The quote's paragraph is the unit, not the whole quote.
		IM_CHECK_EQ(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].type, EMarkdownBlockType::Paragraph);
		IM_CHECK_EQ(ed.GetDocument().GetBlocks()[ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].parent].type, EMarkdownBlockType::Quote);
		// Up goes back into the paragraph (across the gap line).
		for (int i = 0; i < 3 && ed.GetRevealedBlock() != 2; i++) { ctx->KeyPress(ImGuiKey_UpArrow); ctx->Yield(); }
		IM_CHECK_EQ(ed.GetRevealedBlock(), 2);
		// Home / End move within the visual line.
		const uint32_t before = ed.GetCaret();
		ctx->KeyPress(ImGuiKey_Home);
		ctx->Yield();
		IM_CHECK(ed.GetCaret() <= before);
		ctx->KeyPress(ImGuiKey_End);
		ctx->Yield();
		IM_CHECK(ed.GetCaret() >= before);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "reveal_unit_is_one_list_item_and_inline_span");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string src = "- one\n- two **b** x\n- three\n";
		ed.SetSource(src);
		ctx->Yield(2);

		const uint32_t two = (uint32_t)src.find("two");
		IM_CHECK(ClickOffset(ctx, ed, two));
		IM_CHECK_EQ(ed.GetCaret(), two);
		const CMarkdownDocument &doc = ed.GetDocument();
		IM_CHECK_EQ(doc.GetBlocks()[ed.GetRevealedBlock()].type, EMarkdownBlockType::ListItem);
		uint32_t rb = 0, re = 0;
		ed.GetRevealedRange(&rb, &re);
		IM_CHECK_EQ(rb, (uint32_t)src.find("- two"));
		IM_CHECK_EQ(re, (uint32_t)src.find("- three"));
		IM_CHECK(OffsetHasGlyph(ed, rb));                       // this item's '-' shows as source
		IM_CHECK(!OffsetHasGlyph(ed, 0));                       // the first item keeps its rendered bullet
		IM_CHECK(OffsetHasGlyph(ed, 2));                        // ...and its text
		IM_CHECK(!OffsetHasGlyph(ed, (uint32_t)src.find("- three")));
		const uint32_t stars = (uint32_t)src.find("**b**");
		IM_CHECK(!OffsetHasGlyph(ed, stars));                   // untouched span: markers hidden
		IM_CHECK(OffsetHasGlyph(ed, stars + 2));                // its text drawn

		// Touch the span: its markers appear; the caret stays on the same byte.
		IM_CHECK(ClickOffset(ctx, ed, stars + 2));
		IM_CHECK_EQ(ed.GetCaret(), stars + 2);
		IM_CHECK(OffsetHasGlyph(ed, stars));
		IM_CHECK(OffsetHasGlyph(ed, stars + 3));

		// The whole-unit option shows every marker wherever the caret is.
		ed.SetRevealAllInlineSyntax(true);
		IM_CHECK(ClickOffset(ctx, ed, two));
		IM_CHECK(OffsetHasGlyph(ed, stars));
		ed.SetRevealAllInlineSyntax(false);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "task_box_link_and_image_interactions");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string src = "Intro [site](http://s.t) end\n\n- [ ] task one\n- next\n\nPic ![alt](a.png) after\n";
		std::string opened;
		ed.onLinkActivated = [&opened](const std::string &href) { opened = href; };
		ed.imageResolver = [](const std::string &srcPath, ImTextureRef *tex, ImVec2 *size) {
			if (srcPath != "a.png") return false;
			*tex = ImGui::GetIO().Fonts->TexRef;
			*size = ImVec2(48, 24);
			return true;
		};
		ed.SetSource(src);
		ctx->Yield(2);
		const uint32_t next = (uint32_t)src.find("next");
		IM_CHECK(ClickOffset(ctx, ed, next));      // caret away from the box and the link

		// The task box is a drawn shape; a click toggles it and leaves the caret.
		const CMarkdownLiveEditor::Glyph *box = nullptr;
		float boxY = 0, boxH = 0;
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (g.deco == CMarkdownLiveEditor::kDecoBox) { box = &g; boxY = line.y; boxH = line.height; }
		IM_CHECK(box != nullptr);
		const ImVec2 origin(ed.GetItemMin().x + ImGui::GetStyle().FramePadding.x, ed.GetItemMin().y + ImGui::GetStyle().FramePadding.x);
		ctx->MouseMoveToPos(ImVec2(origin.x + (box->x0 + box->x1) * 0.5f, origin.y + boxY + boxH * 0.5f));
		ctx->MouseClick(ImGuiMouseButton_Left);
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("- [x] task one") != std::string::npos);
		IM_CHECK_EQ(ed.GetCaret(), next);
		bool checkedShown = false;
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (g.deco == CMarkdownLiveEditor::kDecoBoxChecked) checkedShown = true;
		IM_CHECK(checkedShown);
		// The unordered marker is a drawn bullet, not a font glyph.
		bool bullet = false;
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (g.deco == CMarkdownLiveEditor::kDecoBullet) bullet = true;
		IM_CHECK(bullet);

		// A resolved image is one picture box; its alt text is not drawn.
		const uint32_t pic = (uint32_t)ed.GetSource().find("![alt]");
		const CMarkdownLiveEditor::Glyph *img = nullptr;
		for (const auto &line : ed.GetLayoutLines())
			for (const auto &g : line.glyphs)
				if (g.image >= 0) img = &g;
		IM_CHECK(img != nullptr);
		IM_CHECK_EQ(img->offset, pic);
		IM_CHECK(img->x1 - img->x0 > 47.0f && img->x1 - img->x0 < 49.0f);
		IM_CHECK(!OffsetHasGlyph(ed, pic + 2));    // 'a' of alt

		// Ctrl+click on the rendered link opens it; the caret stays put.
		const uint32_t site = (uint32_t)ed.GetSource().find("site");
		ImVec2 lp;
		float lh = 0;
		IM_CHECK(ed.GetOffsetScreenPos(site + 1, &lp, &lh));
		ctx->MouseMoveToPos(ImVec2(lp.x + 1.0f, lp.y + lh * 0.5f));
		ctx->KeyDown(ImGuiMod_Ctrl);
		ctx->MouseClick(ImGuiMouseButton_Left);
		ctx->KeyUp(ImGuiMod_Ctrl);
		ctx->Yield(2);
		IM_CHECK_EQ(opened, std::string("http://s.t"));
		IM_CHECK_EQ(ed.GetCaret(), next);
		// A plain click edits: the caret lands in the link text.
		IM_CHECK(ClickOffset(ctx, ed, site + 1));
		IM_CHECK_EQ(ed.GetCaret(), site + 1);
		IM_CHECK(OffsetHasGlyph(ed, site - 1));    // the touched link shows its '['

		// Clicking the picture puts the caret on it, which reveals its source.
		IM_CHECK(ClickOffset(ctx, ed, pic));
		IM_CHECK_EQ(ed.GetCaret(), pic);
		IM_CHECK(OffsetHasGlyph(ed, pic));         // '!' drawn as source now
		IM_CHECK(OffsetHasGlyph(ed, pic + 2));

		ed.onLinkActivated = nullptr;
		ed.imageResolver = nullptr;
	};

	t = IM_REGISTER_TEST(engine, "markdown", "table_menu_tab_and_row_moves");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string src = "Top\n\n| Item | Who |\n|------|-----|\n| Plan | Ann |\n| Ship | Bob |\n";
		ed.SetSource(src);
		ctx->Yield(2);
		auto rowsNow = [&ed]() {
			const CMarkdownDocument &d = ed.GetDocument();
			for (int32_t i = 0; i < (int32_t)d.GetBlocks().size(); i++)
				if (d.GetBlocks()[i].type == EMarkdownBlockType::Table) return (int)d.RowsOf(i).size();
			return 0;
		};
		IM_CHECK_EQ(rowsNow(), 3);

		// Right-click a cell, then "Insert row below" from the table menu.
		const uint32_t plan = (uint32_t)src.find("Plan");
		ImVec2 pos;
		float h = 0;
		IM_CHECK(ed.GetOffsetScreenPos(plan + 1, &pos, &h));
		ctx->MouseMoveToPos(ImVec2(pos.x + 1.0f, pos.y + h * 0.5f));
		ctx->MouseClick(ImGuiMouseButton_Right);
		ctx->Yield(2);
		ctx->ItemClick("//$FOCUSED/###md_table_row_below");
		ctx->Yield(2);
		IM_CHECK_EQ(rowsNow(), 4);
		IM_CHECK(ed.GetSource().find("| Plan | Ann |\n|      |     |\n| Ship | Bob |") != std::string::npos);
		IM_CHECK(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].IsCell());   // the caret is in the new row

		// Tab from the very last cell appends a row and selects its first cell.
		const uint32_t bob = (uint32_t)ed.GetSource().find("Bob");
		IM_CHECK(ClickOffset(ctx, ed, bob + 1));
		ctx->KeyPress(ImGuiKey_Tab);
		ctx->Yield(2);
		IM_CHECK_EQ(rowsNow(), 5);
		const CMarkdownDocument &d = ed.GetDocument();
		const int32_t cell = d.FindBlockAt(ed.GetCaret());
		IM_CHECK(d.GetBlocks()[cell].IsCell() && d.ColumnOf(cell) == 0);
		ctx->KeyChars("Test");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("| Ship | Bob |\n| Test") != std::string::npos);

		// Alt+Shift+Up moves that row above "Ship".
		ctx->KeyPress(ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_UpArrow);
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("| Test") < ed.GetSource().find("| Ship"));
		IM_CHECK_EQ(rowsNow(), 5);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "remapped_binding_replaces_default_chord");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetSource("make me bold\n");
		ctx->Yield(2);
		CMarkdownEditorBindings *kb = ed.GetBindings();
		IM_CHECK(kb->IsRegistered());
		// The host's shortcut registry sees the editor's shortcuts in their zone.
		std::list<u32> zones;
		zones.push_back(CMarkdownEditorBindings::kZone);
		IM_CHECK(guiMain->keyboardShortcuts->FindShortcut(zones, 'b', false, false, true, false) == kb->GetShortcut(EMarkdownEditorCommand::Bold));

		CSlrKeyboardShortcut *bold = kb->GetShortcut(EMarkdownEditorCommand::Bold);
		guiMain->RemoveKeyboardShortcut(bold);
		bold->SetKeyCode('b', true, false, true, false);    // Ctrl/Cmd+Shift+B
		guiMain->AddKeyboardShortcut(bold);

		IM_CHECK(ClickOffset(ctx, ed, 0));
		ed.SetSelection(0, 4);
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_B);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("make me bold\n"));   // the old chord does nothing now
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("**make** me bold\n"));

		// A binding takes effect from the next frame: ImGui routes a chord for
		// the frame after the one that asked for it.
		kb->ResetToDefaults();
		ctx->Yield(2);
		ed.SetSelection(9, 11);
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_B);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("**make** **me** bold\n"));
	};

	t = IM_REGISTER_TEST(engine, "markdown", "ime_data_follows_the_caret");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetSource("First line\n\nSecond paragraph\n");
		ctx->Yield(2);
		IM_CHECK(ClickOffset(ctx, ed, 3));
		ctx->Yield(2);
		// What the editor published last frame (ImGui keeps it as Prev).
		ImVec2 cmin, cmax;
		IM_CHECK(ed.GetCaretScreenRect(&cmin, &cmax));
		const ImGuiPlatformImeData &ime = GImGui->PlatformImeDataPrev;
		IM_CHECK(ime.WantTextInput);      // what makes SDL3 deliver characters
		IM_CHECK(ime.WantVisible);
		IM_CHECK(std::fabs(ime.InputPos.x - (cmin.x - 1.0f)) < 0.5f);
		IM_CHECK(std::fabs(ime.InputPos.y - cmin.y) < 0.5f);
		IM_CHECK(std::fabs(ime.InputLineHeight - (cmax.y - cmin.y)) < 0.5f);

		// The caret moves to the next paragraph: so does the IME position.
		const float firstY = ime.InputPos.y;
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)ed.GetSource().find("paragraph")));
		ctx->Yield(2);
		IM_CHECK_GT(GImGui->PlatformImeDataPrev.InputPos.y, firstY + 1.0f);

		// Read-only: the editor asks for no text input (nothing else in the
		// test window does either).
		ed.SetReadOnly(true);
		ctx->Yield(2);
		IM_CHECK(!GImGui->PlatformImeDataPrev.WantTextInput);
		ed.SetReadOnly(false);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "find_bar_steps_through_matches");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetFindQuery("");
		ed.SetSource("alpha beta\n\nBeta gamma beta\n");
		ctx->Yield(2);
		IM_CHECK(ClickOffset(ctx, ed, 0));
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
		ctx->Yield(3);
		IM_CHECK(ed.IsFindOpen());
		ctx->KeyChars("beta");          // into the bar, which took the focus
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetFindQuery(), std::string("beta"));
		IM_CHECK_EQ((int)ed.GetFindMatches().size(), 3);
		IM_CHECK_EQ(ed.GetCurrentFindMatch(), 0);
		IM_CHECK_EQ(ed.GetSelectedText(), std::string("beta"));
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCurrentFindMatch(), 1);
		IM_CHECK_EQ(ed.GetSelectedText(), std::string("Beta"));
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield(2);
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCurrentFindMatch(), 0);   // wrapped
		ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCurrentFindMatch(), 2);
		IM_CHECK_EQ(ed.GetDocument().FindBlockAt(ed.GetCaret()), ed.GetRevealedBlock());

		// Esc closes the bar and gives the text the keyboard back.
		ctx->KeyPress(ImGuiKey_Escape);
		ctx->Yield(3);
		IM_CHECK(!ed.IsFindOpen());
		IM_CHECK(ed.IsFocused());
		// F3 in the text keeps stepping with the last query.
		ctx->KeyPress(ImGuiKey_F3);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCurrentFindMatch(), 0);
		ctx->KeyChars("Z");             // typing replaces the selected match: the text has the keyboard
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("alpha Z\n\nBeta gamma beta\n"));
	};

	t = IM_REGISTER_TEST(engine, "markdown", "word_like_typing_never_shows_syntax");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		ed.SetFindQuery("");
		ed.SetSource("Hello **bold** world\n\nSecond\n");
		ctx->Yield(2);
		const uint32_t world = (uint32_t)ed.GetSource().find("world");
		IM_CHECK(ClickOffset(ctx, ed, world));
		IM_CHECK_EQ(ed.GetCaret(), world);
		IM_CHECK_EQ(ed.GetRevealedBlock(), -1);
		IM_CHECK(!OffsetHasGlyph(ed, 6));          // the paragraph with the caret still hides '**'
		IM_CHECK(ClickOffset(ctx, ed, 9));         // into "bold"
		IM_CHECK_EQ(ed.GetCaret(), 9u);
		IM_CHECK(!OffsetHasGlyph(ed, 6));
		IM_CHECK(!OffsetHasGlyph(ed, 12));
		ctx->KeyChars("*X");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("Hello **b\\*Xold** world\n\nSecond\n"));
		// Left over the closer-free text: one press, one glyph.
		const uint32_t before = ed.GetCaret();
		ctx->KeyPress(ImGuiKey_LeftArrow);
		ctx->Yield();
		IM_CHECK_EQ(ed.GetCaret(), before - 1);
		ctx->KeyPress(ImGuiKey_LeftArrow);
		ctx->Yield();
		IM_CHECK_EQ(ed.GetCaret(), before - 3);    // over '*' and its backslash at once

		// End, Enter, a list typed as text.
		ctx->KeyPress(ImGuiKey_End);
		ctx->Yield();
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		ctx->KeyChars("- one");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("Hello **b\\*Xold** world\n\n- one\n\nSecond\n"));
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		ctx->KeyChars("two");
		ctx->KeyPress(ImGuiKey_Tab);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("Hello **b\\*Xold** world\n\n- one\n  - two\n\nSecond\n"));
		ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_Tab);
		ctx->Yield();
		// Ctrl+B with no selection: the next typed text is bold.
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_B);
		ctx->Yield();
		ctx->KeyChars(" big");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("Hello **b\\*Xold** world\n\n- one\n- two **big**\n\nSecond\n"));
		// Enter at the end of bold text: the next item goes on in bold.
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield();
		ctx->KeyChars("xy");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("- two **big**\n- **xy**\n") != std::string::npos);
		// Shift+Enter inside the text: a line break in the same item.
		ctx->KeyPress(ImGuiKey_LeftArrow);
		ctx->Yield();
		ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("- **x**\\\n  **y**\n") != std::string::npos);
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_Z);
		ctx->Yield();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
	};

	t = IM_REGISTER_TEST(engine, "markdown", "toolbar_buttons_format_the_text");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		ed.SetToolbarVisible(true);
		ed.SetFindQuery("");
		ed.SetSource("make it bold\n\nsecond line\n");
		ctx->Yield(2);
		ctx->SetRef("//MarkdownEditorTest/##markdowneditor/##md_toolbar");
		IM_CHECK(ClickOffset(ctx, ed, 8));
		ed.SetSelection(8, 12);
		ctx->ItemClick("###md_tb_bold");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("make it **bold**\n\nsecond line\n"));
		IM_CHECK(ed.IsFocused());                   // the text has the keyboard back
		IM_CHECK(ed.IsInlineStyleActive(EMarkdownInlineStyle::Strong));
		ctx->ItemClick("###md_tb_italic");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("make it ***bold***\n\nsecond line\n"));
		ctx->ItemClick("###md_tb_undo");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("make it **bold**\n\nsecond line\n"));
		ctx->ItemClick("###md_tb_redo");
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetSource(), std::string("make it ***bold***\n\nsecond line\n"));

		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)ed.GetSource().find("line")));
		ctx->ItemClick("###md_tb_bullets");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("- second line") != std::string::npos);
		IM_CHECK(ed.IsInList(EMarkdownListKind::Bullet));
		ctx->ItemClick("###md_tb_numbers");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("1. second line") != std::string::npos);
		ctx->ItemClick("###md_tb_numbers");
		ctx->Yield(2);
		IM_CHECK(!ed.IsInList(EMarkdownListKind::Numbered));
		ctx->ItemClick("###md_tb_quote");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("> second line") != std::string::npos);
		ctx->ItemClick("###md_tb_quote");
		ctx->Yield(2);

		// The link button opens the dialog; OK writes the link.
		const uint32_t s = (uint32_t)ed.GetSource().find("second");
		ed.SetSelection(s, s + 6);
		ctx->ItemClick("###md_tb_link");
		ctx->Yield(2);
		IM_CHECK(ed.IsLinkDialogOpen());
		ctx->KeyChars("https://example.com");
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK(!ed.IsLinkDialogOpen());
		IM_CHECK(ed.GetSource().find("[second](https://example.com) line") != std::string::npos);

		// Hidden toolbar: nothing drawn, the text starts higher.
		ImVec2 withBar, withoutBar;
		float gh = 0;
		IM_CHECK(ed.GetOffsetScreenPos(0, &withBar, &gh));
		ed.SetToolbarVisible(false);
		ctx->Yield(2);
		IM_CHECK(ed.GetOffsetScreenPos(0, &withoutBar, &gh));
		IM_CHECK_LT(withoutBar.y, withBar.y - 4.0f);
		ed.SetToolbarVisible(true);
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ctx->SetRef("");
	};

	// Page Up / Page Down: the caret moves about one visible page and the view
	// scrolls with it; Shift extends the selection; at the ends they stop.
	t = IM_REGISTER_TEST(engine, "markdown", "page_up_down_move_by_a_page");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		ed.SetFindQuery("");
		std::string src;
		for (int i = 1; i <= 80; i++) src += "Paragraph number " + std::to_string(i) + " of the long document.\n\n";
		ed.SetSource(src);
		ctx->Yield(2);
		IM_CHECK(ClickOffset(ctx, ed, 0));
		ImGuiWindow *child = nullptr;
		for (ImGuiWindow *w : GImGui->Windows)
			if (strstr(w->Name, "##md_live_editor") != nullptr && strstr(w->Name, "MarkdownEditorTest") != nullptr) child = w;
		IM_CHECK(child != nullptr);
		auto caretY = [&]() { ImVec2 a, b; ed.GetCaretScreenRect(&a, &b); return a.y; };
		const float y0 = caretY();
		ctx->KeyPress(ImGuiKey_PageDown);
		ctx->Yield(2);
		const uint32_t afterDown = ed.GetCaret();
		IM_CHECK_GT(afterDown, 0u);
		IM_CHECK_GT(child->Scroll.y, 0.0f);                          // the view scrolled
		const float onScreen = caretY();
		IM_CHECK_GE(onScreen, child->Pos.y);                          // and the caret is in view
		IM_CHECK_LE(onScreen, child->Pos.y + child->Size.y);
		IM_CHECK_LT(ImFabs(onScreen - y0), child->Size.y * 0.5f);     // about where it was on screen
		// The caret moved about a page: the first paragraphs are now above.
		const uint32_t pageChars = afterDown;
		IM_CHECK(ed.GetSource().substr(0, afterDown).find("number 5 ") != std::string::npos);
		ctx->KeyPress(ImGuiKey_PageUp);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCaret(), 0u);                               // back on the first line, same column
		IM_CHECK_EQ(child->Scroll.y, 0.0f);
		ctx->KeyPress(ImGuiKey_PageUp);
		ctx->Yield(2);
		IM_CHECK_EQ(ed.GetCaret(), 0u);                               // nothing above
		ctx->KeyPress(ImGuiMod_Shift | ImGuiKey_PageDown);
		ctx->Yield(2);
		IM_CHECK(ed.HasSelection());
		IM_CHECK_EQ(ed.GetCaret(), pageChars);
		for (int i = 0; i < 40; i++) { ctx->KeyPress(ImGuiKey_PageDown); ctx->Yield(); }
		ctx->Yield(2);
		IM_CHECK_GE(ed.GetCaret(), (uint32_t)ed.GetSource().rfind("Paragraph number 80"));   // on the last paragraph
		// Leave the view at the top for the next tests.
		ed.SetSource("");
		ctx->Yield(3);
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
	};

	// Word-like caret invariants over a real agenda, at every caret position:
	// the caret has a line; Right then Left comes back; Down goes down and Up
	// comes back; Enter opens the line below with the caret on it, and
	// Backspace undoes it (in a list the bullet goes first, as in Word, so
	// there the text is compared without markers); a typed character lands
	// right after the caret on its line and Backspace removes it.
	t = IM_REGISTER_TEST(engine, "markdown", "word_like_caret_invariants");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		ed.SetFindQuery("");
		const std::string src =
			"AGENDA: Internal AI meeting 2026/10/24\n\n"
			"# Internal AI meeting\n\n"
			"1. Wprowadzenie i kwestie organizacyjne (Marcin S)\n"
			"2. Podzial projektu na moduly i kamienie milowe (Marcin N)\n\n"
			"   Przedstawienie proponowanego podzialu aplikacji **WWW** i *mobilnej*.\n\n"
			"   - Autoryzacja\n"
			"   - Uzytkownicy\n"
			"   - Dashboard\n"
			"   - Urzadzenia\n\n"
			"3. Analiza biznesowa - Autoryzacja i Uzytkownicy (Michal K)\n\n"
			"   Przeglad wymagan i acceptance criteria, zob. [notatki](https://example.com/notes).\n\n"
			"## Action items\n\n"
			"- [x] Rozeslac agende\n"
			"- [ ] Przygotowac demo\n"
			"- [ ] Zebrac uwagi do `release-plan.md`\n\n"
			"## Timing\n\n"
			"| Topic        | Owner    | Minutes |\n"
			"|:-------------|----------|--------:|\n"
			"| Introduction | Marcin S |       5 |\n"
			"| Modules      | Marcin N |      20 |\n\n"
			"> Decyzje zapisujemy w protokole.\n\n"
			"```\nmake release\n```\n";
		ed.SetSource(src);
		ctx->Yield(2);
		auto lineOf = [&](uint32_t o) {
			const auto &lines = ed.GetLayoutLines();
			int fallback = -1;
			for (size_t i = 0; i < lines.size(); i++)
			{
				for (const auto &g : lines[i].glyphs) if (!g.synthetic && g.offset == o) return (int)i;
				if (o >= lines[i].begOffset && o <= lines[i].endOffset) fallback = (int)i;
			}
			return fallback;
		};
		auto lineY = [&](int li) { return li >= 0 ? ed.GetLayoutLines()[(size_t)li].y : -1.0f; };
		auto isPosition = [&](uint32_t o) {
			const uint32_t c = ed.GetCaret();
			ed.SetCaret(o);
			const bool ok = ed.GetCaret() == o;
			ed.SetCaret(c);
			return ok;
		};
		auto show = [&](uint32_t o) {
			const std::string &t = ed.GetSource();
			std::string r = t.substr(o > 12 ? o - 12 : 0, o > 12 ? 12 : o) + "|" + t.substr(std::min<size_t>(o, t.size()), 10);
			for (char &ch : r) if (ch == '\n') ch = '/';
			return r;
		};
		// The text without any Markdown block syntax or blank lines.
		auto plain = [](const std::string &t) {
			std::string out;
			size_t p = 0;
			while (p <= t.size())
			{
				size_t e = t.find('\n', p);
				if (e == std::string::npos) e = t.size();
				std::string line = t.substr(p, e - p);
				size_t k = 0;
				while (k < line.size() && (line[k] == ' ' || line[k] == '>' || line[k] == '#')) k++;
				if (k < line.size() && (line[k] == '-' || line[k] == '*' || line[k] == '+') && (k + 1 == line.size() || line[k + 1] == ' ')) k++;
				else { size_t d = k; while (d < line.size() && line[d] >= '0' && line[d] <= '9') d++; if (d > k && d < line.size() && line[d] == '.') k = d + 1; }
				while (k < line.size() && line[k] == ' ') k++;
				if (k + 3 <= line.size() && line[k] == '[' && line[k + 2] == ']') { k += 3; while (k < line.size() && line[k] == ' ') k++; }
				std::string body = line.substr(k);
				while (!body.empty() && body.back() == ' ') body.pop_back();
				if (!body.empty()) { if (!out.empty()) out += ' '; out += body; }
				if (e >= t.size()) break;
				p = e + 1;
			}
			return out;
		};
		const std::string plainSrc = plain(src);
		std::vector<uint32_t> positions;
		ed.SetCaret(0);
		positions.push_back(ed.GetCaret());
		for (;;)
		{
			ed.MoveHorizontal(1, false);
			if (ed.GetCaret() == positions.back()) break;
			positions.push_back(ed.GetCaret());
		}
		ctx->LogInfo("%d caret positions", (int)positions.size());
		IM_CHECK(ClickOffset(ctx, ed, 0));   // the keyboard goes to the editor
		int failures = 0;
		auto fail = [&](const char *what, uint32_t at, const std::string &detail) {
			failures++;
			ctx->LogWarning("INV %s at %u [%s]: %s", what, at, show(at).c_str(), detail.c_str());
		};
		auto reset = [&]() {
			ed.SetSource(src);
			// Back to the top (the view may have scrolled), then the keyboard back.
			ed.SetCaret(0);
			ed.MoveHorizontal(1, false);
			ed.MoveHorizontal(-1, false);
			ctx->Yield(2);
			IM_CHECK_NO_RET(ClickOffset(ctx, ed, 0));
		};
		// After a split before a blank and a merge, the caret stands after the blank.
		auto caretBack = [&](uint32_t s) {
			const uint32_t c = ed.GetCaret();
			if (c == s) return true;
			if (c < s || ed.GetSource() != src) return false;
			for (uint32_t o = s; o < c; o++) if (src[o] != ' ' && src[o] != '*' && src[o] != '`' && src[o] != '~') return false;
			return true;
		};
		// Every position gets Right/Left; the costlier checks go to every third
		// one and to every first and last position of a layout line.
		std::vector<bool> full(positions.size(), false);
		for (size_t k = 0; k < positions.size(); k++)
		{
			const int l = lineOf(positions[k]);
			const bool edge = k == 0 || k + 1 == positions.size() || lineOf(positions[k - 1]) != l || lineOf(positions[k + 1]) != l;
			full[k] = edge || k % 3 == 0;
		}
		ImGuiTestEngineIO &eio = ImGuiTestEngine_GetIO(ctx->Engine);
		const float watchdog = eio.ConfigWatchdogKillTest;
		eio.ConfigWatchdogKillTest = 300.0f;
		auto inCode = [&](uint32_t o) {
			const int32_t b = ed.GetDocument().FindBlockAt(o);
			return b > 0 && ed.GetDocument().GetBlocks()[b].type == EMarkdownBlockType::Code;
		};
		for (size_t pi = 0; pi < positions.size(); pi++)
		{
			const uint32_t s = positions[pi];
			ed.SetCaret(s);
			ctx->Yield(1);
			const int li = lineOf(s);
			ImVec2 cmin, cmax;
			if (li < 0) fail("no line for caret", s, "");
			if (!ed.GetCaretScreenRect(&cmin, &cmax)) fail("caret not drawn", s, "");

			// Right then Left.
			ctx->KeyPress(ImGuiKey_RightArrow);
			ctx->Yield(1);
			const uint32_t right = ed.GetCaret();
			if (right != s)
			{
				if (right < s || !isPosition(right)) fail("Right", s, "to " + std::to_string(right));
				ctx->KeyPress(ImGuiKey_LeftArrow);
				ctx->Yield(1);
				if (ed.GetCaret() != s) fail("Left does not come back", s, "right " + std::to_string(right) + " left " + std::to_string(ed.GetCaret()));
			}

			if (!full[pi]) continue;

			// Down then Up.
			ed.SetCaret(s);
			ctx->Yield(1);
			ctx->KeyPress(ImGuiKey_DownArrow);
			ctx->Yield(1);
			const uint32_t down = ed.GetCaret();
			const int ld = lineOf(down);
			if (down != s)
			{
				const bool lastLine = li >= 0 && (size_t)li + 1 == ed.GetLayoutLines().size();
				if (!isPosition(down)) fail("Down lands off a position", s, "to " + std::to_string(down));
				if (ld < 0 || li < 0 || (lineY(ld) <= lineY(li) && !(lastLine && ld == li))) fail("Down did not go down", s, "line " + std::to_string(li) + " -> " + std::to_string(ld));
				if (ld != li)
				{
					ctx->KeyPress(ImGuiKey_UpArrow);
					ctx->Yield(1);
					const uint32_t up = ed.GetCaret();
					if (lineOf(up) != li) fail("Up did not come back to the line", s, "down " + std::to_string(down) + " up " + std::to_string(up));
				}
			}

			// Enter, then Backspace undoes it.
			const bool inList = ed.IsInList(EMarkdownListKind::Bullet) || ed.IsInList(EMarkdownListKind::Numbered) || ed.IsInList(EMarkdownListKind::Task);
			const bool inCell = ed.GetDocument().GetBlocks()[std::max(0, ed.GetDocument().FindBlockAt(s))].IsCell();
			ed.SetCaret(s);
			ctx->Yield(1);
			ctx->KeyPress(ImGuiKey_Enter);
			ctx->Yield(1);
			const uint32_t afterEnter = ed.GetCaret();
			const int le = lineOf(afterEnter);
			if (!isPosition(afterEnter)) fail("Enter lands off a position", s, "to " + std::to_string(afterEnter) + " [" + show(afterEnter) + "]");
			if (le < 0) fail("no line after Enter", s, "caret " + std::to_string(afterEnter));
			else if (li >= 0 && !inCell && le != li + 1) fail("Enter did not open the next line", s, "line " + std::to_string(li) + " -> " + std::to_string(le) + " caret [" + show(afterEnter) + "]");
			if (inCell)
			{
				// Enter in the last row adds a row; Enter elsewhere moves down a row.
				if (plain(ed.GetSource()) != plainSrc && ed.GetSource().find("|  ") == std::string::npos) fail("Enter in a cell", s, "");
				reset();
			}
			else
			{
				ctx->KeyPress(ImGuiKey_Backspace);
				ctx->Yield(1);
				if (ed.GetSource() != src && inList) { ctx->KeyPress(ImGuiKey_Backspace); ctx->Yield(1); }
				const bool same = ed.GetSource() == src;
				const bool samePlain = plain(ed.GetSource()) == plainSrc;
				if (!(same || (inList && samePlain)) || (same && !caretBack(s)))
				{
					std::string d = "caret " + std::to_string(ed.GetCaret());
					if (!same) d += " source differs: [" + show(afterEnter) + "] plain " + (samePlain ? "same" : "DIFFERS");
					fail("Enter+Backspace is not identity", s, d);
					reset();
				}
				else if (!same) reset();
			}

			// A typed character.
			ed.SetCaret(s);
			ctx->Yield(1);
			ctx->KeyChars("q");
			ctx->Yield(1);
			const uint32_t afterType = ed.GetCaret();
			const std::string &now = ed.GetSource();
			const size_t q = now.find('q');
			if (q == std::string::npos || afterType != (uint32_t)q + 1 || lineOf(afterType) != li)
				fail("typing", s, "caret " + std::to_string(afterType) + " line " + std::to_string(lineOf(afterType)) + " q at " + std::to_string((long)q));
			ctx->KeyPress(ImGuiKey_Backspace);
			ctx->Yield(1);
			if (ed.GetSource() != src || ed.GetCaret() != s)
			{
				fail("type+Backspace is not identity", s, "caret " + std::to_string(ed.GetCaret()) + (ed.GetSource() != src ? " source differs" : ""));
				reset();
			}
			(void)inCode;
		}
		eio.ConfigWatchdogKillTest = watchdog;
		ctx->LogInfo("%d invariant failures", failures);
		IM_CHECK_EQ(failures, 0);
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
	};

	// Pixels, not geometry: the editor's window is captured in four states and
	// each must have drawn real text. With MT_MD_CAPTURE_DIR set, the captures
	// are also written there (raw RGBA artifacts) for a human to look at.
	t = IM_REGISTER_TEST(engine, "markdown", "captures_show_rendered_text");
	t->TestFunc = [](ImGuiTestContext *ctx)
	{
		CGuiViewMarkdownEditor *view = EnsureView(ctx);
		CMarkdownLiveEditor &ed = view->GetEditor();
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		const std::string src =
			"# Internal AI meeting\n\n"
			"Agenda for **24 October**, see [the notes](http://example.com) and `build.sh`.\n\n"
			"1. Introduction (Anna)\n"
			"2. Project split\n"
			"   - Authorisation\n"
			"   - [x] Data model\n"
			"   - [ ] Release plan\n\n"
			"> Decisions are recorded in the minutes.\n\n"
			"| Topic | Owner | Minutes |\n"
			"|:------|-------|--------:|\n"
			"| Intro | Anna  | 5 |\n"
			"| Split | Bob   | 20 |\n\n"
			"```\nmake release\n```\n\n"
			"Closing remarks with ~~old~~ *new* items.\n";
		ed.SetFindQuery("");
		ed.SetSource(src);
		ctx->Yield(3);
		const char *dir = getenv("MT_MD_CAPTURE_DIR");
		auto shot = [&](const char *name) {
			ctx->Yield(3);
			std::vector<unsigned int> px;
			int w = 0, h = 0;
			IM_CHECK(MT_CaptureWindowRGBA(ctx, "//MarkdownEditorTest", &px, &w, &h));
			const unsigned int bg = px.empty() ? 0 : px[(size_t)(h / 2) * (size_t)w + (size_t)(w - 3)];
			int distinct = 0;
			const float ink = MT_NonBackgroundFraction(px, bg, 24, &distinct);
			ctx->LogInfo("capture %s: %dx%d, %.3f non-background, %d colours", name, w, h, ink, distinct);
			IM_CHECK_GT(ink, 0.02f);
			IM_CHECK_GT(distinct, 8);
			if (dir != nullptr)
			{
				const std::string path = std::string(dir) + "/" + name + ".rgba";
				MT_WriteCaptureArtifact(path.c_str(), px, w, h);
			}
		};
		// 1. Everything rendered (caret at the end, in the last paragraph).
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)src.find("items")));
		shot("1_rendered");
		// 2. A list item revealed, its strong span untouched.
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)src.find("Data model")));
		shot("2_list_item_revealed");
		// 3. A table cell revealed: the grid stays a grid.
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)src.find("Bob") + 1));
		shot("3_table_cell");
		// 5. Word-like (the default): no syntax anywhere, the toolbar above.
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)src.find("October")));
		shot("5_word_like");
		// 7. Word-like editing: Enter at the end of the last nested item and a
		// word typed on the new item; Enter at the start of the last paragraph
		// (an empty paragraph opens above it, drawn as an empty line).
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)src.find("plan") + 3));
		ctx->KeyPress(ImGuiKey_End);
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->KeyChars("Budget");
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("   - [ ] Release plan\n   - [ ] Budget\n") != std::string::npos);
		IM_CHECK(ClickOffset(ctx, ed, (uint32_t)ed.GetSource().find("Closing")));
		ctx->KeyPress(ImGuiKey_Enter);
		ctx->Yield(2);
		IM_CHECK(ed.GetSource().find("```\n\n\nClosing") != std::string::npos);
		shot("7_word_like_edits");
		ed.SetSource(src);
		ctx->Yield(2);
		// 6. The whole source.
		ed.SetEditMode(EMarkdownEditMode::Source);
		shot("6_source");
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		// 4. The find bar with three matches.
		ctx->KeyPress(ImGuiMod_Ctrl | ImGuiKey_F);
		ctx->Yield(2);
		ctx->KeyChars("an");
		shot("4_find");
		ctx->KeyPress(ImGuiKey_Escape);
		ctx->Yield(2);
	};
}
