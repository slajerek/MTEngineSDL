#include "CTestMarkdownDocument.h"
#include "CMarkdownDocument.h"
#include "DBG_Log.h"

#include <chrono>
#include <cstdio>
#include <string>

#define MD_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[512]; \
			snprintf(buf, sizeof(buf), "FAIL: %.500s", (msg)); \
			LOGD("CTestMarkdownDocument: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

namespace
{
	// The block's source text, with newlines shown as '~' for a one-line message.
	std::string Slice(const CMarkdownDocument &d, int32_t block)
	{
		const CMarkdownBlock &b = d.GetBlocks()[block];
		std::string s = d.GetSource().substr(b.beg, b.end - b.beg);
		for (char &c : s) if (c == '\n') c = '~';
		return s;
	}

	int32_t FirstOfType(const CMarkdownDocument &d, EMarkdownBlockType t, int skip = 0)
	{
		const auto &blocks = d.GetBlocks();
		for (int32_t i = 0; i < (int32_t)blocks.size(); i++)
			if (blocks[i].type == t) { if (skip == 0) return i; skip--; }
		return -1;
	}

	// The source text of the nth span of a type, "" when there is none.
	std::string SpanText(const CMarkdownDocument &d, EMarkdownSpanType t, int skip = 0)
	{
		for (const CMarkdownSpan &sp : d.GetSpans())
			if (sp.type == t) { if (skip == 0) return d.GetSource().substr(sp.beg, sp.end - sp.beg); skip--; }
		return "";
	}

	// Field-by-field equality of two parses; the first difference in `why`.
	bool SameModel(const CMarkdownDocument &a, const CMarkdownDocument &b, std::string *why)
	{
		char buf[256];
		if (a.GetSource() != b.GetSource()) { *why = "source"; return false; }
		const auto &ba = a.GetBlocks(), &bb = b.GetBlocks();
		if (ba.size() != bb.size()) { snprintf(buf, sizeof(buf), "block count %zu vs %zu", ba.size(), bb.size()); *why = buf; return false; }
		for (size_t i = 0; i < ba.size(); i++)
		{
			const CMarkdownBlock &x = ba[i], &y = bb[i];
			const bool same = x.type == y.type && x.beg == y.beg && x.end == y.end && x.parent == y.parent && x.depth == y.depth
				&& x.level == y.level && x.listStart == y.listStart && x.listMark == y.listMark && x.listIsTight == y.listIsTight
				&& x.isTask == y.isTask && x.taskMark == y.taskMark && (!x.isTask || x.taskMarkOffset == y.taskMarkOffset)
				&& x.isFenced == y.isFenced && x.fenceChar == y.fenceChar && x.infoBeg == y.infoBeg && x.infoEnd == y.infoEnd
				&& x.langBeg == y.langBeg && x.langEnd == y.langEnd && x.columnCount == y.columnCount
				&& x.headRowCount == y.headRowCount && x.bodyRowCount == y.bodyRowCount && x.align == y.align
				&& x.firstRun == y.firstRun && x.runCount == y.runCount;
			if (!same) { snprintf(buf, sizeof(buf), "block %zu (type %d [%u,%u) vs type %d [%u,%u))", i, (int)x.type, x.beg, x.end, (int)y.type, y.beg, y.end); *why = buf; return false; }
		}
		const auto &ra = a.GetRuns(), &rb = b.GetRuns();
		if (ra.size() != rb.size()) { snprintf(buf, sizeof(buf), "run count %zu vs %zu", ra.size(), rb.size()); *why = buf; return false; }
		for (size_t i = 0; i < ra.size(); i++)
		{
			const CMarkdownRun &x = ra[i], &y = rb[i];
			if (!(x.beg == y.beg && x.end == y.end && x.styles == y.styles && x.block == y.block && x.kind == y.kind
				&& x.synthesized == y.synthesized && x.href == y.href && x.span == y.span))
			{ snprintf(buf, sizeof(buf), "run %zu ([%u,%u) blk %d vs [%u,%u) blk %d)", i, x.beg, x.end, x.block, y.beg, y.end, y.block); *why = buf; return false; }
		}
		const auto &sa = a.GetSpans(), &sb = b.GetSpans();
		if (sa.size() != sb.size()) { snprintf(buf, sizeof(buf), "span count %zu vs %zu", sa.size(), sb.size()); *why = buf; return false; }
		for (size_t i = 0; i < sa.size(); i++)
		{
			const CMarkdownSpan &x = sa[i], &y = sb[i];
			if (!(x.type == y.type && x.beg == y.beg && x.end == y.end && x.contentBeg == y.contentBeg && x.contentEnd == y.contentEnd
				&& x.block == y.block && x.parent == y.parent && x.firstRun == y.firstRun && x.runCount == y.runCount && x.href == y.href))
			{ snprintf(buf, sizeof(buf), "span %zu", i); *why = buf; return false; }
		}
		return true;
	}

	// '_' where a byte is syntax, '~' for newline, the byte itself when content.
	std::string ContentMask(const CMarkdownDocument &d)
	{
		const std::string &s = d.GetSource();
		std::string m(s.size(), '.');
		for (size_t i = 0; i < s.size(); i++)
			m[i] = s[i] == '\n' ? '~' : (d.IsContentByte((uint32_t)i) ? s[i] : '_');
		return m;
	}
}

void CTestMarkdownDocument::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// The agenda shape from the product spec, plus every construct whose range
	// md4c does not report: a setext underline, fences, a delimiter row, an
	// empty cell, a rule, a tight nested list with a task item, a quote.
	const std::string src =
		"AGENDA: Internal AI meeting 2026/10/24\n"
		"\n"
		"# Title **bold** and *em*\n"
		"\n"
		"1. Wprowadzenie (Marcin S)\n"
		"2. Podzia\xC5\x82 projektu\n"
		"\n"
		"   Opis akapitu.\n"
		"\n"
		"   - Autoryzacja\n"
		"   - [ ] Zadanie\n"
		"\n"
		"3. Analiza\n"
		"\n"
		"Setext\n"
		"======\n"
		"\n"
		"```cpp\n"
		"int x;\n"
		"```\n"
		"\n"
		"| A | B |\n"
		"|---|:-:|\n"
		"| 1 |   |\n"
		"| x | y |\n"
		"\n"
		"---\n"
		"\n"
		"> quote `code` [link](http://x.y)\n"
		"line two\n";

	CMarkdownDocument d(src);
	const auto &blocks = d.GetBlocks();

	MD_ASSERT(blocks.size() == 31, "the sample parses into the 31 blocks md4c reports (document included)");
	MD_ASSERT(blocks[0].type == EMarkdownBlockType::Document && blocks[0].beg == 0 && blocks[0].end == src.size(),
		"block 0 is the Document spanning the whole source");

	// Leaf ranges include their markers.
	const int32_t h1 = FirstOfType(d, EMarkdownBlockType::Heading);
	MD_ASSERT(h1 > 0 && Slice(d, h1) == "# Title **bold** and *em*", "an ATX heading's range starts at its '#'");
	MD_ASSERT(blocks[h1].level == 1, "the heading level is reported");

	const int32_t setext = FirstOfType(d, EMarkdownBlockType::Heading, 1);
	MD_ASSERT(setext > 0 && Slice(d, setext) == "Setext~======", "a setext heading's range includes its underline");

	const int32_t code = FirstOfType(d, EMarkdownBlockType::Code);
	MD_ASSERT(code > 0 && Slice(d, code) == "```cpp~int x;~```", "a fenced code block's range includes both fences");
	MD_ASSERT(blocks[code].isFenced && blocks[code].fenceChar == '`', "fence char is reported");
	MD_ASSERT(src.substr(blocks[code].langBeg, blocks[code].langEnd - blocks[code].langBeg) == "cpp", "the code language maps to its source bytes");
	{
		// Empty code lines right after the opening fence are synthesized line
		// breaks, not content: the fence is still found past them.
		CMarkdownDocument e("a\n\n```\n\n\nint y;\n```\n");
		const int32_t c = FirstOfType(e, EMarkdownBlockType::Code);
		MD_ASSERT(c > 0 && Slice(e, c) == "```~~~int y;~```", "a fenced block starting with empty lines still includes its opening fence");
	}

	const int32_t hr = FirstOfType(d, EMarkdownBlockType::HorizontalRule);
	MD_ASSERT(hr > 0 && Slice(d, hr) == "---", "a rule (no content at all) is placed on its line");

	// Containers are unions of their children, from the first marker.
	const int32_t ol = FirstOfType(d, EMarkdownBlockType::OrderedList);
	MD_ASSERT(ol > 0 && src.compare(blocks[ol].beg, 2, "1.") == 0 && Slice(d, ol).size() == std::string("1. Wprowadzenie (Marcin S)~2. Podzia\xC5\x82 projektu~~   Opis akapitu.~~   - Autoryzacja~   - [ ] Zadanie~~3. Analiza").size(),
		"an ordered list spans from its first marker to its last item");
	const int32_t ul = FirstOfType(d, EMarkdownBlockType::UnorderedList);
	MD_ASSERT(ul > 0 && Slice(d, ul) == "   - Autoryzacja~   - [ ] Zadanie", "a nested tight list spans exactly its two item lines");
	MD_ASSERT(blocks[ul].level == 2 && blocks[ol].level == 1, "list nesting levels are 1 for the outer and 2 for the inner list");

	const std::vector<int32_t> items = d.GetChildren(ul);
	MD_ASSERT(items.size() == 2 && Slice(d, items[0]) == "   - Autoryzacja" && Slice(d, items[1]) == "   - [ ] Zadanie",
		"tight list items (which own their text directly) get their own line as range");
	MD_ASSERT(blocks[items[1]].isTask && blocks[items[1]].taskMark == ' ' && src[blocks[items[1]].taskMarkOffset] == ' ',
		"a task item reports its mark and the mark's source offset");

	const int32_t quote = FirstOfType(d, EMarkdownBlockType::Quote);
	MD_ASSERT(quote > 0 && Slice(d, quote) == "> quote `code` [link](http://x.y)~line two", "a quote spans its lazy continuation line too");

	// Tables: rows, cells, the delimiter row, an empty cell.
	const int32_t table = FirstOfType(d, EMarkdownBlockType::Table);
	MD_ASSERT(table > 0 && Slice(d, table) == "| A | B |~|---|:-:|~| 1 |   |~| x | y |", "a table's range includes its delimiter row");
	MD_ASSERT(blocks[table].columnCount == 2 && blocks[table].headRowCount == 1 && blocks[table].bodyRowCount == 2, "table dimensions are reported");
	const std::vector<int32_t> rows = d.RowsOf(table);
	MD_ASSERT(rows.size() == 3, "three rows (the delimiter row is not a block)");
	const std::vector<int32_t> row2 = d.CellsOf(rows[1]);
	MD_ASSERT(row2.size() == 2 && Slice(d, row2[0]) == "1" && blocks[row2[1]].beg == blocks[row2[1]].end,
		"a cell's range is its content; an empty cell is a point");
	MD_ASSERT(src[blocks[row2[1]].beg - 1] == ' ' && src[blocks[row2[1]].beg - 2] == '|',
		"the empty cell's point is one blank inside its opening pipe");
	MD_ASSERT(blocks[row2[1]].align == EMarkdownCellAlign::Center && blocks[row2[0]].align == EMarkdownCellAlign::Default, "cell alignment comes from the delimiter row");
	MD_ASSERT(d.ColumnOf(row2[1]) == 1 && d.RowOf(row2[1]) == rows[1] && d.TableOf(row2[1]) == table, "cell -> column, row, table navigation");

	// Content versus syntax. Every byte the parser never named is syntax.
	const std::string mask = ContentMask(d);
	MD_ASSERT(mask.compare(0, 38, "AGENDA: Internal AI meeting 2026/10/24") == 0, "plain paragraph bytes are content");
	MD_ASSERT(mask.find("__Title __bold__ and _em_") != std::string::npos, "'# ', '**' and '*' are syntax; the words are content");
	MD_ASSERT(mask.find("___Wprowadzenie") != std::string::npos, "the list marker '1. ' is syntax");
	MD_ASSERT(mask.find("_________Zadanie") != std::string::npos, "indent, bullet and task box are syntax");
	MD_ASSERT(mask.find("______~int x;~___") != std::string::npos, "fences are syntax, code lines are content");
	MD_ASSERT(mask.find("__A___B__~_________~") != std::string::npos, "pipes and the delimiter row are syntax");
	MD_ASSERT(mask.find("_link_____________") != std::string::npos, "link text is content, its URL is syntax");

	// Lookups.
	MD_ASSERT(d.FindBlockAt(3) == 1 && d.FindBlockAt(3, true) == 1, "an offset in the first paragraph finds it");
	MD_ASSERT(d.FindBlockAt((uint32_t)src.find("Autoryzacja")) == items[0], "an offset in a nested item finds the item (deepest block)");
	MD_ASSERT(d.FindBlockAt((uint32_t)src.find("Autoryzacja"), true) == ol, "...and its top-level ancestor when asked");
	MD_ASSERT(d.TopLevelOf(items[0]) == ol && d.NextTopLevel(ol) == setext && d.PrevTopLevel(ol) == h1, "top-level navigation");
	const uint32_t gap = 39;   // the blank line between the first paragraph and the heading
	MD_ASSERT(src[gap] == '\n' && d.FindBlockAt(gap) == 1, "an offset in the gap after a block belongs to that block");

	// UTF-8 stepping.
	const uint32_t l = (uint32_t)src.find("\xC5\x82");
	MD_ASSERT(d.NextCodepoint(l) == l + 2 && d.PrevCodepoint(l + 2) == l, "code point stepping skips both bytes of a two-byte character");

	// ---- Edits are byte replacements. Nothing else moves.
	uint32_t parses = d.GetParseCount();
	const int32_t emptyCell = row2[1];
	const uint32_t cellPos = blocks[emptyCell].beg;
	std::string expected = src;
	expected.insert(cellPos, "Z");
	d.Replace(cellPos, cellPos, "Z");
	MD_ASSERT(d.GetSource() == expected, "editing an empty cell changes exactly one byte of the source");
	MD_ASSERT(d.GetParseCount() == parses + 1, "one edit is one reparse");
	MD_ASSERT(Slice(d, table) == "| A | B |~|---|:-:|~| 1 | Z  |~| x | y |",
		"the table is still a table after the edit");
	MD_ASSERT(d.GetBlocks()[emptyCell].runCount == 1 && Slice(d, emptyCell) == "Z", "the cell now has content");

	// Heading level on a paragraph, and back.
	uint32_t caret = 3;
	MD_ASSERT(d.SetHeadingLevel(caret, 2, &caret), "a paragraph can become a heading");
	MD_ASSERT(d.GetSource().compare(0, 10, "## AGENDA:") == 0 && caret == 6, "'## ' is prepended and the caret moves with the text");
	MD_ASSERT(d.GetBlocks()[1].type == EMarkdownBlockType::Heading && d.GetBlocks()[1].level == 2, "the block is a level-2 heading");
	MD_ASSERT(d.SetHeadingLevel(caret, 3, &caret) && d.GetSource().compare(0, 11, "### AGENDA:") == 0 && caret == 7, "changing the level rewrites only the marker");
	MD_ASSERT(d.SetHeadingLevel(caret, 0, &caret) && d.GetSource().compare(0, 7, "AGENDA:") == 0 && caret == 3, "level 0 removes the marker");
	MD_ASSERT(d.GetSource() == expected, "after heading round trip the source is byte-identical");
	uint32_t c2 = (uint32_t)d.GetSource().find("Setext") + 1;
	MD_ASSERT(!d.SetHeadingLevel(c2, 1, &c2), "a setext heading is refused rather than mangled");
	uint32_t c3 = (uint32_t)d.GetSource().find("int x;");
	MD_ASSERT(!d.SetHeadingLevel(c3, 1, &c3), "a code block is refused");
	MD_ASSERT(d.GetSource() == expected, "refused commands leave the source untouched");

	// List continuation.
	MD_ASSERT(d.ListContinuationPrefix((uint32_t)d.GetSource().find("Podzia")) == "3. ", "an ordered item continues with the next number");
	MD_ASSERT(d.ListContinuationPrefix((uint32_t)d.GetSource().find("Zadanie")) == "   - [ ] ", "a task item continues with an unchecked box at the same indent");
	MD_ASSERT(d.ListContinuationPrefix((uint32_t)d.GetSource().find("Opis")) == "", "a paragraph inside an item is not a list line");
	MD_ASSERT(d.ListContinuationPrefix((uint32_t)d.GetSource().find("---")) == "", "a rule is not a list line");

	// Inline spans: ranges with their markers, content inside.
	{
		CMarkdownDocument sp(src);
		MD_ASSERT(SpanText(sp, EMarkdownSpanType::Strong) == "**bold**", "a strong span's range includes both `**`");
		MD_ASSERT(SpanText(sp, EMarkdownSpanType::Emphasis) == "*em*", "an emphasis span's range includes both `*`");
		MD_ASSERT(SpanText(sp, EMarkdownSpanType::Code) == "`code`", "a code span's range includes its backticks");
		MD_ASSERT(SpanText(sp, EMarkdownSpanType::Link) == "[link](http://x.y)", "a link's range runs through its destination");
		const CMarkdownSpan *link = nullptr;
		for (const CMarkdownSpan &x : sp.GetSpans()) if (x.type == EMarkdownSpanType::Link) link = &x;
		MD_ASSERT(link && link->href == "http://x.y" && sp.GetSource().substr(link->contentBeg, link->contentEnd - link->contentBeg) == "link",
			"a link carries its destination and its text range");
		const uint32_t boldAt = (uint32_t)sp.GetSource().find("**bold**");
		MD_ASSERT(sp.SpansTouching(boldAt).size() == 1 && sp.SpansTouching(boldAt + 8).size() == 1 && sp.SpansTouching(boldAt - 1).empty(),
			"a span is touched from its first marker through just after its last");
		const int32_t r = sp.FindRunAt(boldAt + 2);
		MD_ASSERT(r >= 0 && sp.GetRuns()[r].span >= 0 && sp.GetSpans()[sp.GetRuns()[r].span].type == EMarkdownSpanType::Strong,
			"a run knows its innermost span");

		CMarkdownDocument hard("a ***x*** b [t](u \"q r\") c ![](i.png) d <http://a.b> e ` x ` f ~~s~~ g [**k**](v)\n");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Emphasis) == "***x***" && SpanText(hard, EMarkdownSpanType::Strong) == "***x***",
			"nested spans sharing a delimiter run both claim it");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Link) == "[t](u \"q r\")", "a link title with blanks stays inside the link");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Image) == "![](i.png)", "an image with no alt text is placed between its neighbours");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Link, 1) == "<http://a.b>", "an autolink's range includes its angle brackets");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Code) == "` x `", "a padded code span keeps its padding inside");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Strikethrough) == "~~s~~", "strikethrough");
		MD_ASSERT(SpanText(hard, EMarkdownSpanType::Link, 2) == "[**k**](v)", "a link around strong text spans the whole link");
	}

	// Table edits: whole-line rewrites that keep pipes, padding and escapes.
	{
		const std::string tsrc = "Intro\n\n| Name | Age |\n|:-----|----:|\n| Ann  | 31  |\n| Bob  |     |\n| a\\|b | 2 |\n\nAfter\n";
		auto cellAt = [](const CMarkdownDocument &d, int row, int col) -> int32_t {
			const int32_t t = FirstOfType(d, EMarkdownBlockType::Table);
			return d.CellsOf(d.RowsOf(t)[row])[col];
		};
		auto tableText = [](const CMarkdownDocument &d) {
			const int32_t t = FirstOfType(d, EMarkdownBlockType::Table);
			return d.GetSource().substr(d.GetBlocks()[t].beg, d.GetBlocks()[t].end - d.GetBlocks()[t].beg);
		};
		CMarkdownDocument t1(tsrc);
		uint32_t c = 0;
		MD_ASSERT(t1.EditTable(cellAt(t1, 1, 0), EMarkdownTableEdit::InsertRowBelow, &c)
			&& tableText(t1) == "| Name | Age |\n|:-----|----:|\n| Ann  | 31  |\n|      |     |\n| Bob  |     |\n| a\\|b | 2 |",
			"a new row is as wide as the delimiter row");
		MD_ASSERT(t1.CellsOf(t1.RowOf(t1.FindBlockAt(c))).size() == 2 && t1.RowsOf(FirstOfType(t1, EMarkdownBlockType::Table)).size() == 5
			&& t1.GetBlocks()[t1.FindBlockAt(c)].IsCell(), "the caret lands in the new row's cell");
		MD_ASSERT(t1.GetSource().find("\n\nAfter\n") != std::string::npos && t1.GetSource().rfind("Intro\n\n", 0) == 0, "only the table's lines change");
		CMarkdownDocument t2(tsrc);
		MD_ASSERT(!t2.EditTable(cellAt(t2, 0, 0), EMarkdownTableEdit::DeleteRow, nullptr) && !t2.EditTable(cellAt(t2, 0, 0), EMarkdownTableEdit::InsertRowAbove, nullptr)
			&& !t2.EditTable(cellAt(t2, 1, 0), EMarkdownTableEdit::MoveRowUp, nullptr) && t2.GetSource() == tsrc,
			"the header row cannot be deleted, preceded or swapped with");
		MD_ASSERT(t2.EditTable(cellAt(t2, 1, 0), EMarkdownTableEdit::MoveRowDown, nullptr)
			&& tableText(t2) == "| Name | Age |\n|:-----|----:|\n| Bob  |     |\n| Ann  | 31  |\n| a\\|b | 2 |", "a body row moves down");
		CMarkdownDocument t3(tsrc);
		MD_ASSERT(t3.EditTable(cellAt(t3, 1, 0), EMarkdownTableEdit::InsertColumnRight, nullptr)
			&& tableText(t3) == "| Name |   | Age |\n|:-----|---|----:|\n| Ann  |   | 31  |\n| Bob  |   |     |\n| a\\|b |   | 2 |",
			"a column is inserted on every line, the delimiter row included; an escaped pipe is not a column");
		CMarkdownDocument t4(tsrc);
		MD_ASSERT(t4.EditTable(cellAt(t4, 1, 1), EMarkdownTableEdit::DeleteColumn, nullptr)
			&& tableText(t4) == "| Name |\n|:-----|\n| Ann  |\n| Bob  |\n| a\\|b |", "a column is deleted from every line");
		MD_ASSERT(!t4.EditTable(cellAt(t4, 1, 0), EMarkdownTableEdit::DeleteColumn, nullptr), "the last column cannot be deleted");
		CMarkdownDocument t5(tsrc);
		MD_ASSERT(t5.EditTable(cellAt(t5, 1, 1), EMarkdownTableEdit::AlignCenter, nullptr)
			&& tableText(t5).find("|:-----|:---:|") != std::string::npos, "alignment rewrites the delimiter cell and keeps its width");
		MD_ASSERT(t5.GetBlocks()[cellAt(t5, 1, 1)].align == EMarkdownCellAlign::Center, "and md4c reads it back");
		CMarkdownDocument t6(tsrc);
		MD_ASSERT(t6.EditTable(cellAt(t6, 2, 0), EMarkdownTableEdit::MoveColumnRight, &c)
			&& tableText(t6).rfind("| Age | Name |\n|----:|:-----|", 0) == 0 && t6.GetSource().compare(c, 3, "Bob") == 0,
			"a column moves with its alignment and the caret follows the cell");
		CMarkdownDocument quoted("> | a | b |\n> |---|---|\n> | 1 | 2 |\n");
		const int32_t qt = FirstOfType(quoted, EMarkdownBlockType::Table);
		MD_ASSERT(qt > 0 && quoted.EditTable(quoted.CellsOf(quoted.RowsOf(qt)[1])[0], EMarkdownTableEdit::InsertRowBelow, nullptr)
			&& quoted.GetSource() == "> | a | b |\n> |---|---|\n> | 1 | 2 |\n> |   |   |\n", "a table in a quote keeps the quote prefix on a new row");
	}

	// Incremental reparse: every random edit must give exactly what a full
	// parse of the same source gives. The alphabet is Markdown syntax, so
	// edits open fences, make setext underlines, start and end lists, tables,
	// quotes and spans -- the cases where a local reparse would be wrong.
	{
		CMarkdownDocument inc(src + "\n## Second\n\nMore text *here* and `code`.\n\n- x\n- y\n\n> q\n\nEnd.\n");
		const char alphabet[] = " \n\n\n#-*_`|>[]():x1.=~ab";
		uint32_t seed = 20261015u;
		auto rnd = [&seed](uint32_t n) { seed = seed * 1664525u + 1013904223u; return n ? (seed >> 8) % n : 0; };
		const int kEdits = 3000;
		int incremental = 0;
		std::string why;
		bool ok = true;
		int failedAt = -1;
		for (int i = 0; i < kEdits && ok; i++)
		{
			const uint32_t size = inc.Size();
			const uint32_t pos = rnd(size + 1);
			if (rnd(3) == 0 || size > 2400)
			{
				const uint32_t len = 1 + rnd(3);
				inc.Replace(pos, std::min(size, pos + len), "");
			}
			else
			{
				std::string ins;
				const uint32_t len = 1 + rnd(3);
				for (uint32_t k = 0; k < len; k++) ins += alphabet[rnd(sizeof(alphabet) - 1)];
				inc.Replace(pos, pos, ins);
			}
			if (inc.WasLastParseIncremental()) incremental++;
			CMarkdownDocument full(inc.GetSource());
			if (!SameModel(inc, full, &why)) { ok = false; failedAt = i; }
		}
		char msg[512];
		snprintf(msg, sizeof(msg), "%d random edits: incremental reparse equals a full parse (%d took the incremental path)%s%s%s",
			kEdits, incremental, ok ? "" : "; FIRST DIFFERENCE at edit ", ok ? "" : std::to_string(failedAt).c_str(), ok ? "" : (": " + why).c_str());
		MD_ASSERT(ok, msg);
		MD_ASSERT(incremental > kEdits / 4, "the incremental path is taken for most local edits");

		// A long agenda: typing stays local.
		std::string big;
		for (int k = 0; k < 600; k++)
			big += "## Topic " + std::to_string(k) + "\n\nSome **notes** for the topic, with a [link](http://x).\n\n- point one\n- point two\n\n";
		CMarkdownDocument bigInc(big), bigFull(big);
		bigFull.SetIncrementalParse(false);
		const uint32_t at = (uint32_t)big.find("Some", big.size() / 2);
		auto t0 = std::chrono::steady_clock::now();
		for (int k = 0; k < 100; k++) bigInc.Replace(at + (uint32_t)k, at + (uint32_t)k, "z");
		auto t1 = std::chrono::steady_clock::now();
		for (int k = 0; k < 100; k++) bigFull.Replace(at + (uint32_t)k, at + (uint32_t)k, "z");
		auto t2 = std::chrono::steady_clock::now();
		const double incMs = std::chrono::duration<double, std::milli>(t1 - t0).count();
		const double fullMs = std::chrono::duration<double, std::milli>(t2 - t1).count();
		std::string why2;
		MD_ASSERT(SameModel(bigInc, bigFull, &why2), "100 keystrokes in a 600-topic agenda: incremental and full parses agree");
		snprintf(msg, sizeof(msg), "typing in a %zu-byte document: %.3f ms per keystroke incremental, %.3f ms full",
			big.size(), incMs / 100.0, fullMs / 100.0);
		MD_ASSERT(bigInc.GetIncrementalParseCount() == 100, msg);
	}

	// Degenerate inputs.
	CMarkdownDocument empty("");
	MD_ASSERT(empty.GetBlocks().size() == 1 && empty.FindBlockAt(0) == 0, "an empty source is a Document with nothing in it");
	CMarkdownDocument blank("\n\n  \n");
	MD_ASSERT(blank.GetBlocks().size() == 1, "a blank source has no blocks");
	CMarkdownDocument lone("#");
	MD_ASSERT(lone.GetBlocks().size() == 2 && lone.GetBlocks()[1].type == EMarkdownBlockType::Heading && Slice(lone, 1) == "#", "a lone '#' is an empty heading on its line");
	CMarkdownDocument emptyItem("- a\n-\n- c\n");
	const int32_t eul = FirstOfType(emptyItem, EMarkdownBlockType::UnorderedList);
	const std::vector<int32_t> eitems = emptyItem.GetChildren(eul);
	MD_ASSERT(eitems.size() == 3 && Slice(emptyItem, eitems[1]) == "-", "an empty list item is placed on its own line");
	CMarkdownDocument noClose("```\ncode\n");
	MD_ASSERT(Slice(noClose, 1) == "```~code~" || Slice(noClose, 1) == "```~code", "an unterminated fence still starts at the fence");
	CMarkdownDocument crlf("# A\r\n\r\nB\r\n");
	MD_ASSERT(crlf.GetBlocks().size() == 3 && crlf.GetBlocks()[1].type == EMarkdownBlockType::Heading, "CRLF sources parse");

	// Underline: `<u>text</u>`, inline HTML, as a span of its own.
	{
		CMarkdownDocument u("a <u>bc</u> d\n");
		MD_ASSERT(SpanText(u, EMarkdownSpanType::Underline) == "<u>bc</u>", "<u>...</u> is an Underline span");
		MD_ASSERT(!u.IsContentByte(2) && !u.IsContentByte(4) && u.IsContentByte(5) && !u.IsContentByte(7) && u.IsContentByte(11),
			"its tags are syntax, the text between is content");
		const int32_t r = u.FindRunAt(5);
		MD_ASSERT(r >= 0 && (u.GetRuns()[r].styles & MD_STYLE_UNDERLINE) != 0 && u.GetRuns()[r].span >= 0
			&& u.GetSpans()[u.GetRuns()[r].span].type == EMarkdownSpanType::Underline, "the text is underlined and inside the span");
		MD_ASSERT(u.GetBlocks()[0].runCount == u.GetRuns().size() && Slice(u, 1) == "a <u>bc</u> d", "the paragraph keeps its range and run count");
		CMarkdownDocument outer("<u>**b** c</u>\n");
		const auto &os = outer.GetSpans();
		MD_ASSERT(os.size() == 2 && os[0].type == EMarkdownSpanType::Underline && os[1].type == EMarkdownSpanType::Strong && os[1].parent == 0 && os[0].parent == -1,
			"bold inside underline: the underline is the parent, in pre-order");
		CMarkdownDocument inner("**<u>b</u>**\n");
		const auto &is = inner.GetSpans();
		MD_ASSERT(is.size() == 2 && is[0].type == EMarkdownSpanType::Strong && is[1].type == EMarkdownSpanType::Underline && is[1].parent == 0,
			"underline inside bold: bold is the parent");
		const int32_t ri = inner.FindRunAt(5);
		MD_ASSERT(ri >= 0 && (inner.GetRuns()[ri].styles & (MD_STYLE_UNDERLINE | MD_STYLE_STRONG)) == (MD_STYLE_UNDERLINE | MD_STYLE_STRONG), "and the text is both");
		CMarkdownDocument upper("<U>x</U>\n");
		MD_ASSERT(SpanText(upper, EMarkdownSpanType::Underline) == "<U>x</U>", "the tags in any case");
		CMarkdownDocument nested("<u>a <u>b</u> c</u>\n");
		MD_ASSERT(SpanText(nested, EMarkdownSpanType::Underline) == "<u>a <u>b</u> c</u>" && SpanText(nested, EMarkdownSpanType::Underline, 1) == "<u>b</u>",
			"nested pairs match by depth");
		CMarkdownDocument empty("<u></u> x\n"), open("<u>x\n"), crossed("**a <u>b** c</u>\n");
		MD_ASSERT(SpanText(empty, EMarkdownSpanType::Underline).empty() && empty.IsContentByte(0), "an empty pair stays raw HTML");
		MD_ASSERT(SpanText(open, EMarkdownSpanType::Underline).empty(), "an unclosed tag stays raw HTML");
		MD_ASSERT(SpanText(crossed, EMarkdownSpanType::Underline).empty(), "tags across another span stay raw HTML");
		CMarkdownDocument two("<u>a</u>\n\nb <u>c</u>\n");
		MD_ASSERT(SpanText(two, EMarkdownSpanType::Underline, 1) == "<u>c</u>" && two.GetSpans()[1].block > two.GetSpans()[0].block, "one span per paragraph, in block order");

		// Incremental reparse with the tags as tokens.
		CMarkdownDocument inc("Intro <u>under</u> line.\n\n- <u>item</u>\n- **b**\n\n## H <u>x</u>\n\nEnd.\n");
		const char *tokens[] = { "<u>", "</u>", "**", "x", " ", "\n\n", "- ", "`" };
		uint32_t seed = 20261017u;
		auto rnd = [&seed](uint32_t n) { seed = seed * 1664525u + 1013904223u; return n ? (seed >> 8) % n : 0; };
		std::string why;
		bool ok = true;
		int failedAt = -1;
		for (int i = 0; i < 1500 && ok; i++)
		{
			const uint32_t size = inc.Size();
			const uint32_t pos = rnd(size + 1);
			if (rnd(3) == 0 || size > 1200) inc.Replace(pos, std::min(size, pos + 1 + rnd(4)), "");
			else inc.Replace(pos, pos, tokens[rnd(sizeof(tokens) / sizeof(tokens[0]))]);
			CMarkdownDocument full;
			full.SetIncrementalParse(false);
			full.SetSource(inc.GetSource());
			if (!SameModel(inc, full, &why)) { ok = false; failedAt = i; }
		}
		char m[900];
		snprintf(m, sizeof(m), "1500 random edits with <u> tags: incremental equals full%s%s%s", ok ? "" : "; FIRST DIFFERENCE at edit ",
			ok ? "" : std::to_string(failedAt).c_str(), ok ? "" : (": " + why).substr(0, 600).c_str());
		MD_ASSERT(ok, m);
	}

	TestCompleted(true, "Markdown document: ranges, content/syntax, edits, lists, degenerate inputs");
}
