#include "CImGuiMarkdownEditorTests.h"
#include "imgui.h"
#include "imgui_te_engine.h"
#include "imgui_te_context.h"
#include "CGuiMain.h"
#include "CGuiViewMarkdownEditor.h"

#include <string>

namespace
{
	CGuiViewMarkdownEditor *s_view = nullptr;

	CGuiViewMarkdownEditor *EnsureView(ImGuiTestContext *ctx)
	{
		if (s_view == nullptr)
		{
			s_view = new CGuiViewMarkdownEditor("MarkdownEditorTest", 40, 40, -1, 640, 480);
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
}
