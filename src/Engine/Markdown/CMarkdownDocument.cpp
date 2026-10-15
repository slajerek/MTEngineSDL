#include "CMarkdownDocument.h"
#include "md4c.h"

#include <algorithm>
#include <cstring>

// ---- Range derivation --------------------------------------------------------
//
// md4c reports CONTENT (text runs) with pointers into the input buffer and
// reports BLOCKS without any offset at all (upstream issue mity/md4c#91, open
// since 2019; the maintainer declined). So block ranges are derived here:
//
//   leaf block    = the lines from the first content byte to the last content
//                   byte, widened to full physical lines so the markers on
//                   those lines (`#`, `-`, `>`, `1.`, `|`) are inside; then
//                   widened once more for the syntax lines md4c swallows and
//                   never names: the opening/closing fence of a fenced code
//                   block, the `===`/`---` underline of a setext heading, the
//                   `|---|` delimiter row of a table.
//   empty leaf    = (a lone `#`, a `---` rule, an empty `-` item, an empty
//                   cell): no content, so the block is placed on the first
//                   non-blank line after the previous block. Blocks arrive in
//                   document order, so that line is the right one.
//   container     = the union of its children.
//   table cell    = the content range; an empty cell is the point just after
//                   its opening pipe.
//
// The exceptions in the md4c text() callback -- the literals it passes that
// are NOT in the input: " " for a soft break, "\n" for a hard break, the
// 16-space indent chunk for code-block indentation, "" for a NUL -- are
// detected by a pointer range check and recorded as synthesized runs at the
// position of the previous content byte.

namespace
{
	bool IsBlank(char c) { return c == ' ' || c == '\t' || c == '\r'; }
}

CMarkdownDocument::CMarkdownDocument()
	: parserFlags(MD_DIALECT_GITHUB)
{
	Parse();
}

CMarkdownDocument::CMarkdownDocument(std::string_view src)
	: parserFlags(MD_DIALECT_GITHUB)
{
	source.assign(src.data(), src.size());
	Parse();
}

void CMarkdownDocument::SetSource(std::string_view src)
{
	source.assign(src.data(), src.size());
	Parse();
}

void CMarkdownDocument::SetParserFlags(unsigned flags)
{
	parserFlags = flags;
	Parse();
}

uint32_t CMarkdownDocument::Replace(uint32_t beg, uint32_t end, std::string_view text)
{
	const uint32_t size = Size();
	if (beg > size) beg = size;
	if (end > size) end = size;
	if (end < beg) std::swap(beg, end);
	source.replace(beg, end - beg, text.data(), text.size());
	Parse();
	return beg + (uint32_t)text.size();
}

// ---- Parsing -----------------------------------------------------------------

void CMarkdownDocument::Parse()
{
	blocks.clear();
	runs.clear();
	spans.clear();
	blockStack.clear();
	spanStack.clear();
	openSpans.clear();
	currentStyles = 0;
	currentHref.clear();
	currentLeaf = -1;
	parseCount++;

	MD_PARSER parser;
	memset(&parser, 0, sizeof(parser));
	parser.abi_version = 0;
	parser.flags = parserFlags;
	parser.enter_block = [](MD_BLOCKTYPE t, void *d, void *u) { return EnterBlock((int)t, d, u); };
	parser.leave_block = [](MD_BLOCKTYPE t, void *d, void *u) { return LeaveBlock((int)t, d, u); };
	parser.enter_span = [](MD_SPANTYPE t, void *d, void *u) { return EnterSpan((int)t, d, u); };
	parser.leave_span = [](MD_SPANTYPE t, void *d, void *u) { return LeaveSpan((int)t, d, u); };
	parser.text = [](MD_TEXTTYPE t, const MD_CHAR *text, MD_SIZE size, void *u) { return Text((int)t, text, size, u); };

	md_parse(source.data(), (MD_SIZE)source.size(), &parser, this);

	// md4c always brackets the document in MD_BLOCK_DOC, so blocks[0] is the
	// Document. Guard anyway: a failed parse must still leave a usable model.
	if (blocks.empty())
	{
		CMarkdownBlock doc;
		doc.type = EMarkdownBlockType::Document;
		blocks.push_back(doc);
	}

	ResolveRanges();
	ResolveSpans();
}

static EMarkdownBlockType BlockTypeFromMd(int type)
{
	switch ((MD_BLOCKTYPE)type)
	{
		case MD_BLOCK_DOC: return EMarkdownBlockType::Document;
		case MD_BLOCK_QUOTE: return EMarkdownBlockType::Quote;
		case MD_BLOCK_UL: return EMarkdownBlockType::UnorderedList;
		case MD_BLOCK_OL: return EMarkdownBlockType::OrderedList;
		case MD_BLOCK_LI: return EMarkdownBlockType::ListItem;
		case MD_BLOCK_HR: return EMarkdownBlockType::HorizontalRule;
		case MD_BLOCK_H: return EMarkdownBlockType::Heading;
		case MD_BLOCK_CODE: return EMarkdownBlockType::Code;
		case MD_BLOCK_HTML: return EMarkdownBlockType::Html;
		case MD_BLOCK_P: return EMarkdownBlockType::Paragraph;
		case MD_BLOCK_TABLE: return EMarkdownBlockType::Table;
		case MD_BLOCK_THEAD: return EMarkdownBlockType::TableHead;
		case MD_BLOCK_TBODY: return EMarkdownBlockType::TableBody;
		case MD_BLOCK_TR: return EMarkdownBlockType::TableRow;
		case MD_BLOCK_TH: return EMarkdownBlockType::TableHeaderCell;
		case MD_BLOCK_TD: return EMarkdownBlockType::TableCell;
	}
	return EMarkdownBlockType::Paragraph;
}

int CMarkdownDocument::EnterBlock(int type, void *detail, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;
	CMarkdownBlock block;
	block.type = BlockTypeFromMd(type);
	block.parent = doc->blockStack.empty() ? -1 : doc->blockStack.back();
	block.depth = (uint16_t)doc->blockStack.size();

	// Nesting level of lists: how many list containers are on the stack.
	uint16_t listDepth = 0;
	for (int32_t idx : doc->blockStack)
	{
		const EMarkdownBlockType t = doc->blocks[idx].type;
		if (t == EMarkdownBlockType::UnorderedList || t == EMarkdownBlockType::OrderedList) listDepth++;
	}

	const char *base = doc->source.data();
	auto offsetOf = [&](const MD_CHAR *p) -> uint32_t {
		return (p >= base && p <= base + doc->source.size()) ? (uint32_t)(p - base) : 0;
	};

	switch ((MD_BLOCKTYPE)type)
	{
		case MD_BLOCK_UL:
		{
			const MD_BLOCK_UL_DETAIL *d = (const MD_BLOCK_UL_DETAIL *)detail;
			block.listIsTight = d->is_tight != 0;
			block.listMark = d->mark;
			block.level = (uint16_t)(listDepth + 1);
			break;
		}
		case MD_BLOCK_OL:
		{
			const MD_BLOCK_OL_DETAIL *d = (const MD_BLOCK_OL_DETAIL *)detail;
			block.listIsTight = d->is_tight != 0;
			block.listStart = d->start;
			block.listMark = d->mark_delimiter;
			block.level = (uint16_t)(listDepth + 1);
			break;
		}
		case MD_BLOCK_LI:
		{
			const MD_BLOCK_LI_DETAIL *d = (const MD_BLOCK_LI_DETAIL *)detail;
			block.isTask = d->is_task != 0;
			block.taskMark = d->task_mark;
			block.taskMarkOffset = d->task_mark_offset;
			block.level = listDepth;
			break;
		}
		case MD_BLOCK_H:
			block.level = (uint16_t)((const MD_BLOCK_H_DETAIL *)detail)->level;
			break;
		case MD_BLOCK_CODE:
		{
			const MD_BLOCK_CODE_DETAIL *d = (const MD_BLOCK_CODE_DETAIL *)detail;
			block.isFenced = d->fence_char != 0;
			block.fenceChar = d->fence_char;
			if (d->info.text != nullptr && d->info.size > 0)
			{
				block.infoBeg = offsetOf(d->info.text);
				block.infoEnd = block.infoBeg + d->info.size;
			}
			if (d->lang.text != nullptr && d->lang.size > 0)
			{
				block.langBeg = offsetOf(d->lang.text);
				block.langEnd = block.langBeg + d->lang.size;
			}
			break;
		}
		case MD_BLOCK_TABLE:
		{
			const MD_BLOCK_TABLE_DETAIL *d = (const MD_BLOCK_TABLE_DETAIL *)detail;
			block.columnCount = (uint16_t)d->col_count;
			block.headRowCount = (uint16_t)d->head_row_count;
			block.bodyRowCount = (uint16_t)d->body_row_count;
			break;
		}
		case MD_BLOCK_TH:
		case MD_BLOCK_TD:
		{
			const MD_BLOCK_TD_DETAIL *d = (const MD_BLOCK_TD_DETAIL *)detail;
			switch (d->align)
			{
				case MD_ALIGN_LEFT: block.align = EMarkdownCellAlign::Left; break;
				case MD_ALIGN_CENTER: block.align = EMarkdownCellAlign::Center; break;
				case MD_ALIGN_RIGHT: block.align = EMarkdownCellAlign::Right; break;
				default: block.align = EMarkdownCellAlign::Default; break;
			}
			break;
		}
		default:
			break;
	}

	block.firstRun = (uint32_t)doc->runs.size();
	block.runCount = 0;

	const int32_t index = (int32_t)doc->blocks.size();
	doc->blocks.push_back(block);
	doc->blockStack.push_back(index);
	if (block.IsLeaf())
		doc->currentLeaf = index;
	return 0;
}

int CMarkdownDocument::LeaveBlock(int, void *, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;
	if (doc->blockStack.empty())
		return 0;
	const int32_t index = doc->blockStack.back();
	doc->blockStack.pop_back();
	CMarkdownBlock &block = doc->blocks[index];
	block.runCount = (uint32_t)doc->runs.size() - block.firstRun;
	if (doc->currentLeaf == index)
		doc->currentLeaf = -1;
	return 0;
}

int CMarkdownDocument::EnterSpan(int type, void *detail, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;
	doc->spanStack.push_back({doc->currentStyles, doc->currentHref});

	auto attr = [](const MD_ATTRIBUTE &a) { return std::string(a.text ? a.text : "", a.text ? a.size : 0); };

	CMarkdownSpan span;
	span.block = doc->blockStack.empty() ? -1 : doc->blockStack.back();
	span.parent = doc->openSpans.empty() ? -1 : doc->openSpans.back();
	span.firstRun = (uint32_t)doc->runs.size();
	switch ((MD_SPANTYPE)type)
	{
		case MD_SPAN_EM: doc->currentStyles |= MD_STYLE_EMPHASIS; span.type = EMarkdownSpanType::Emphasis; break;
		case MD_SPAN_STRONG: doc->currentStyles |= MD_STYLE_STRONG; span.type = EMarkdownSpanType::Strong; break;
		case MD_SPAN_CODE: doc->currentStyles |= MD_STYLE_CODE; span.type = EMarkdownSpanType::Code; break;
		case MD_SPAN_DEL: doc->currentStyles |= MD_STYLE_STRIKETHROUGH; span.type = EMarkdownSpanType::Strikethrough; break;
		case MD_SPAN_U: doc->currentStyles |= MD_STYLE_UNDERLINE; span.type = EMarkdownSpanType::Underline; break;
		case MD_SPAN_LATEXMATH:
		case MD_SPAN_LATEXMATH_DISPLAY: doc->currentStyles |= MD_STYLE_MATH; span.type = EMarkdownSpanType::Math; break;
		case MD_SPAN_A:
			doc->currentStyles |= MD_STYLE_LINK;
			doc->currentHref = attr(((const MD_SPAN_A_DETAIL *)detail)->href);
			span.type = EMarkdownSpanType::Link;
			break;
		case MD_SPAN_IMG:
			doc->currentStyles |= MD_STYLE_IMAGE;
			doc->currentHref = attr(((const MD_SPAN_IMG_DETAIL *)detail)->src);
			span.type = EMarkdownSpanType::Image;
			break;
		case MD_SPAN_WIKILINK:
			doc->currentStyles |= MD_STYLE_WIKILINK;
			doc->currentHref = attr(((const MD_SPAN_WIKILINK_DETAIL *)detail)->target);
			span.type = EMarkdownSpanType::WikiLink;
			break;
		default: break;
	}
	span.href = doc->currentHref;
	doc->openSpans.push_back((int32_t)doc->spans.size());
	doc->spans.push_back(std::move(span));
	return 0;
}

int CMarkdownDocument::LeaveSpan(int, void *, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;
	if (doc->spanStack.empty())
		return 0;
	doc->currentStyles = doc->spanStack.back().style;
	doc->currentHref = doc->spanStack.back().href;
	doc->spanStack.pop_back();
	if (!doc->openSpans.empty())
	{
		CMarkdownSpan &span = doc->spans[doc->openSpans.back()];
		span.runCount = (uint32_t)doc->runs.size() - span.firstRun;
		doc->openSpans.pop_back();
	}
	return 0;
}

int CMarkdownDocument::Text(int type, const char *text, unsigned size, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;

	CMarkdownRun run;
	// The innermost open block owns the text. That is a leaf in most cases, but
	// a TIGHT list item owns its text directly (md4c emits no paragraph), and a
	// range derivation that only looked at leaves would place such an item on
	// the wrong line.
	run.block = doc->blockStack.empty() ? -1 : doc->blockStack.back();
	run.styles = doc->currentStyles;
	run.href = doc->currentHref;
	run.span = doc->openSpans.empty() ? -1 : doc->openSpans.back();

	const char *base = doc->source.data();
	const bool inSource = text >= base && text + size <= base + doc->source.size();

	switch ((MD_TEXTTYPE)type)
	{
		case MD_TEXT_NORMAL: run.kind = EMarkdownRunKind::Normal; break;
		case MD_TEXT_CODE: run.kind = EMarkdownRunKind::Code; break;
		case MD_TEXT_ENTITY: run.kind = EMarkdownRunKind::Entity; break;
		case MD_TEXT_NULLCHAR: run.kind = EMarkdownRunKind::NullChar; break;
		case MD_TEXT_BR: run.kind = EMarkdownRunKind::HardBreak; break;
		case MD_TEXT_SOFTBR: run.kind = EMarkdownRunKind::SoftBreak; break;
		case MD_TEXT_HTML: run.kind = EMarkdownRunKind::Html; break;
		case MD_TEXT_LATEXMATH: run.kind = EMarkdownRunKind::Normal; break;
	}

	// Where a synthesized run sits: right after the previous content byte of
	// this leaf, else at the leaf's (still unknown) start, patched up in
	// ResolveRanges() when the leaf is placed.
	uint32_t anchor = 0;
	bool anchored = false;
	for (size_t i = doc->runs.size(); i > 0; i--)
	{
		const CMarkdownRun &prev = doc->runs[i - 1];
		if (prev.block != run.block) break;
		if (!prev.IsSynthesized()) { anchor = prev.end; anchored = true; break; }
	}

	if (inSource && (run.kind == EMarkdownRunKind::Normal || run.kind == EMarkdownRunKind::Code
		|| run.kind == EMarkdownRunKind::Entity || run.kind == EMarkdownRunKind::Html))
	{
		run.beg = (uint32_t)(text - base);
		run.end = run.beg + size;
	}
	else
	{
		// A literal md4c owns (" ", "\n", indent chunk, "") or a NUL marker.
		run.beg = run.end = anchored ? anchor : UINT32_MAX;   // UINT32_MAX: "place me when the leaf is placed"
		run.synthesized.assign(text, size);
		if (run.kind == EMarkdownRunKind::Normal || run.kind == EMarkdownRunKind::Code
			|| run.kind == EMarkdownRunKind::Entity || run.kind == EMarkdownRunKind::Html)
		{
			// Out-of-buffer text of a content kind: md4c's indent chunk or a
			// collapsed space. Keep the kind, mark synthesized through beg == end.
			if (run.kind == EMarkdownRunKind::Normal) run.kind = EMarkdownRunKind::SoftBreak;
		}
	}

	doc->runs.push_back(std::move(run));
	return 0;
}

// ---- Range resolution --------------------------------------------------------

uint32_t CMarkdownDocument::LineStart(uint32_t offset) const
{
	if (offset > Size()) offset = Size();
	while (offset > 0 && source[offset - 1] != '\n') offset--;
	return offset;
}

uint32_t CMarkdownDocument::LineEnd(uint32_t offset) const
{
	const uint32_t size = Size();
	if (offset > size) offset = size;
	while (offset < size && source[offset] != '\n') offset++;
	return offset;
}

uint32_t CMarkdownDocument::FirstNonBlankLineStart(uint32_t from) const
{
	const uint32_t size = Size();
	uint32_t lineBeg = from > size ? size : from;
	while (lineBeg < size)
	{
		const uint32_t lineEnd = LineEnd(lineBeg);
		for (uint32_t i = lineBeg; i < lineEnd; i++)
			if (!IsBlank(source[i])) return lineBeg;
		lineBeg = lineEnd < size ? lineEnd + 1 : size;
	}
	return size;
}

// Skip the container prefix of a line: indentation, `>` quote markers and
// their spaces. List markers are NOT skipped (a list item's own line starts
// with one, and a fence or setext underline cannot follow one on the same line
// in a way that matters here).
uint32_t CMarkdownDocument::SkipContainerMarkers(uint32_t lineBeg, uint32_t lineEnd) const
{
	uint32_t i = lineBeg;
	while (i < lineEnd)
	{
		if (IsBlank(source[i]) || source[i] == '>') i++;
		else break;
	}
	return i;
}

bool CMarkdownDocument::LineIsSetextUnderline(uint32_t lineBeg, uint32_t lineEnd) const
{
	uint32_t i = SkipContainerMarkers(lineBeg, lineEnd);
	if (i >= lineEnd) return false;
	const char c = source[i];
	if (c != '=' && c != '-') return false;
	while (i < lineEnd && source[i] == c) i++;
	while (i < lineEnd && IsBlank(source[i])) i++;
	return i == lineEnd;
}

bool CMarkdownDocument::LineIsFence(uint32_t lineBeg, uint32_t lineEnd, char fenceChar) const
{
	uint32_t i = SkipContainerMarkers(lineBeg, lineEnd);
	uint32_t n = 0;
	while (i < lineEnd && source[i] == fenceChar) { i++; n++; }
	return n >= 3;
}

bool CMarkdownDocument::LineIsTableDelimiter(uint32_t lineBeg, uint32_t lineEnd) const
{
	uint32_t i = SkipContainerMarkers(lineBeg, lineEnd);
	bool sawDash = false;
	for (; i < lineEnd; i++)
	{
		const char c = source[i];
		if (c == '-') sawDash = true;
		else if (c != '|' && c != ':' && !IsBlank(c)) return false;
	}
	return sawDash;
}

// True when a block has at least one non-synthesized run of its own.
bool CMarkdownDocument::OwnsContent(int32_t index) const
{
	const CMarkdownBlock &block = blocks[index];
	for (uint32_t r = block.firstRun; r < block.firstRun + block.runCount; r++)
	{
		const CMarkdownRun &run = runs[r];
		if (run.block == index && !run.IsSynthesized() && run.beg != UINT32_MAX) return true;
	}
	return false;
}

void CMarkdownDocument::ResolveLeafRange(int32_t index, uint32_t &cursor)
{
	CMarkdownBlock &block = blocks[index];
	uint32_t minBeg = UINT32_MAX, maxEnd = 0;
	for (uint32_t r = block.firstRun; r < block.firstRun + block.runCount; r++)
	{
		const CMarkdownRun &run = runs[r];
		if (run.block != index || run.IsSynthesized() || run.beg == UINT32_MAX) continue;
		minBeg = std::min(minBeg, run.beg);
		maxEnd = std::max(maxEnd, run.end);
	}

	if (minBeg == UINT32_MAX)
	{
		// No content: the first non-blank line after the previous block.
		const uint32_t lineBeg = FirstNonBlankLineStart(cursor);
		block.beg = lineBeg;
		block.end = LineEnd(lineBeg);
	}
	else
	{
		block.beg = LineStart(minBeg);
		block.end = LineEnd(maxEnd > 0 ? maxEnd - 1 : maxEnd);
		if (maxEnd > block.end) block.end = maxEnd;   // a run that ends exactly at a '\n' (verbatim lines)
	}

	// Lines md4c swallows and never names.
	if (block.type == EMarkdownBlockType::Code && block.isFenced)
	{
		// Opening fence: the line above the first content line -- or the
		// placed line itself when the block has no content.
		if (minBeg != UINT32_MAX && block.beg > 0)
		{
			const uint32_t prevEnd = block.beg - 1;
			const uint32_t prevBeg = LineStart(prevEnd);
			if (LineIsFence(prevBeg, prevEnd, block.fenceChar)) block.beg = prevBeg;
		}
		// Closing fence: the line after the last content line, if it is one.
		if (block.end < Size())
		{
			const uint32_t nextBeg = block.end + 1;
			const uint32_t nextEnd = LineEnd(nextBeg);
			if (nextBeg <= Size() && LineIsFence(nextBeg, nextEnd, block.fenceChar)) block.end = nextEnd;
		}
	}
	else if (block.type == EMarkdownBlockType::Heading && block.end < Size())
	{
		// Setext underline: a `===` / `---` line right after the text.
		const uint32_t nextBeg = block.end + 1;
		const uint32_t nextEnd = LineEnd(nextBeg);
		const uint32_t firstContent = SkipContainerMarkers(block.beg, LineEnd(block.beg));
		const bool isAtx = firstContent < Size() && source[firstContent] == '#';
		if (!isAtx && LineIsSetextUnderline(nextBeg, nextEnd)) block.end = nextEnd;
	}

	// Synthesized runs that had nothing before them sit at the block's first content.
	for (uint32_t r = block.firstRun; r < block.firstRun + block.runCount; r++)
	{
		CMarkdownRun &run = runs[r];
		if (run.block == index && run.beg == UINT32_MAX) run.beg = run.end = (minBeg == UINT32_MAX ? block.beg : minBeg);
	}

	cursor = std::max(cursor, block.end);
}

void CMarkdownDocument::ResolveCellRange(int32_t index, uint32_t rowLineBeg, uint32_t &cursor)
{
	CMarkdownBlock &cell = blocks[index];
	uint32_t minBeg = UINT32_MAX, maxEnd = 0;
	for (uint32_t r = cell.firstRun; r < cell.firstRun + cell.runCount; r++)
	{
		const CMarkdownRun &run = runs[r];
		if (run.IsSynthesized() || run.beg == UINT32_MAX) continue;
		minBeg = std::min(minBeg, run.beg);
		maxEnd = std::max(maxEnd, run.end);
	}

	const uint32_t lineEnd = LineEnd(rowLineBeg);
	if (minBeg != UINT32_MAX)
	{
		cell.beg = minBeg;
		cell.end = maxEnd;
	}
	else
	{
		// Empty cell: just after the next pipe at or after the cursor, and
		// after ONE blank when there is one (`| |` style padding), so typing
		// into the cell produces `| text   |`. The cursor is the end of the
		// previous cell (or the row's line start).
		uint32_t i = std::max(cursor, rowLineBeg);
		while (i < lineEnd && source[i] != '|') i++;
		if (i < lineEnd) i++;
		if (i < lineEnd && IsBlank(source[i]) && i + 1 < lineEnd && source[i + 1] != '|') i++;
		cell.beg = cell.end = i;
	}

	for (uint32_t r = cell.firstRun; r < cell.firstRun + cell.runCount; r++)
	{
		CMarkdownRun &run = runs[r];
		if (run.beg == UINT32_MAX) run.beg = run.end = cell.beg;
	}

	// Past this cell's content and its closing pipe, so the next empty cell
	// searches from the right place.
	uint32_t next = cell.end;
	while (next < lineEnd && source[next] != '|') next++;
	cursor = next;
}

void CMarkdownDocument::ResolveRanges()
{
	// Pass 1, document order: leaves, cells, and any container that owns text
	// itself (a tight list item).
	std::vector<bool> placed(blocks.size(), false);
	uint32_t cursor = 0;
	uint32_t rowLineBeg = 0;
	uint32_t cellCursor = 0;
	for (int32_t i = 0; i < (int32_t)blocks.size(); i++)
	{
		CMarkdownBlock &block = blocks[i];
		if (block.type == EMarkdownBlockType::Document) continue;
		if (block.type == EMarkdownBlockType::TableRow)
		{
			// The row's line is the first non-blank line after the cursor.
			rowLineBeg = FirstNonBlankLineStart(cursor);
			// The delimiter row sits between the head row and the first body
			// row; it is not a block, so skip it when it is next.
			if (LineIsTableDelimiter(rowLineBeg, LineEnd(rowLineBeg)))
			{
				const uint32_t afterDelim = LineEnd(rowLineBeg);
				rowLineBeg = FirstNonBlankLineStart(afterDelim < Size() ? afterDelim + 1 : Size());
			}
			cellCursor = rowLineBeg;
			block.beg = rowLineBeg;
			block.end = LineEnd(rowLineBeg);
			cursor = std::max(cursor, block.end);
			placed[i] = true;
		}
		else if (block.IsCell())
		{
			ResolveCellRange(i, rowLineBeg, cellCursor);
			placed[i] = true;
		}
		else if (block.IsLeaf() || OwnsContent(i))
		{
			ResolveLeafRange(i, cursor);
			placed[i] = true;
		}
	}

	// Pass 2, reverse order: containers are the union of their children (and
	// of their own text, when they have some). Children have larger indices
	// than their parent, so walking backwards sees every child resolved before
	// its parent.
	for (int32_t i = (int32_t)blocks.size() - 1; i >= 0; i--)
	{
		CMarkdownBlock &block = blocks[i];
		if (block.IsLeaf() || block.IsCell() || block.type == EMarkdownBlockType::TableRow) continue;

		uint32_t minBeg = UINT32_MAX, maxEnd = 0;
		if (placed[i])   // own text, placed in pass 1
		{
			minBeg = block.beg;
			maxEnd = block.end;
		}
		for (int32_t c = i + 1; c < (int32_t)blocks.size(); c++)
		{
			if (blocks[c].parent != i) continue;
			if (!placed[c]) continue;
			minBeg = std::min(minBeg, blocks[c].beg);
			maxEnd = std::max(maxEnd, blocks[c].end);
		}

		if (block.type == EMarkdownBlockType::Document)
		{
			block.beg = 0;
			block.end = Size();
			placed[i] = true;
			continue;
		}

		if (minBeg == UINT32_MAX)
		{
			// A container with no placed child: an empty list item (`-` alone).
			// Place it on the first non-blank line after the previous sibling
			// (or the parent's start).
			uint32_t from = 0;
			for (int32_t p = i - 1; p >= 0; p--)
			{
				if (placed[p] && blocks[p].depth >= block.depth) { from = blocks[p].end; break; }
				if (blocks[p].depth < block.depth) break;
			}
			const uint32_t lineBeg = FirstNonBlankLineStart(from);
			block.beg = lineBeg;
			block.end = LineEnd(lineBeg);
			placed[i] = true;
			continue;
		}

		block.beg = minBeg;
		block.end = maxEnd;

		// A list item / quote begins at its marker, which is on the first
		// child's first line: widen to the line start. (A cell is intra-line
		// and never a container, so this is safe.)
		block.beg = LineStart(block.beg);

		if (block.type == EMarkdownBlockType::Table)
		{
			// Head-only table: the delimiter row follows the head row.
			if (block.end < Size())
			{
				const uint32_t nextBeg = block.end + 1;
				const uint32_t nextEnd = LineEnd(nextBeg);
				if (LineIsTableDelimiter(nextBeg, nextEnd)) block.end = nextEnd;
			}
		}
		placed[i] = true;
	}

	// Pass 3: a container's children that could not be placed from content and
	// fell before their parent (never happens with the rules above, but keep
	// the invariant parent.beg <= child.beg <= child.end <= parent.end true).
	for (int32_t i = 1; i < (int32_t)blocks.size(); i++)
	{
		CMarkdownBlock &block = blocks[i];
		const CMarkdownBlock &parent = blocks[block.parent];
		if (block.parent <= 0) continue;
		if (block.beg < parent.beg) block.beg = parent.beg;
		if (block.end > parent.end) block.end = parent.end;
		if (block.end < block.beg) block.end = block.beg;
	}
}

// ---- Inline spans --------------------------------------------------------------

// From just after a link's text (`from`, at or before its `]`), the end of the
// link's tail: `](dest "title")`, `][ref]`, `[]`, or just `]`. Returns `from`
// when there is no `]` before `limit` (an autolink).
uint32_t CMarkdownDocument::MatchLinkTail(uint32_t from, uint32_t limit) const
{
	uint32_t k = from;
	while (k < limit && source[k] != ']') k++;
	if (k >= limit) return from;
	uint32_t j = k + 1;
	if (j < limit && source[j] == '(')
	{
		int depth = 0;
		char quote = 0;
		bool angle = false;
		for (; j < limit; j++)
		{
			const char c = source[j];
			if (c == '\\' && j + 1 < limit) { j++; continue; }
			if (quote) { if (c == quote) quote = 0; continue; }
			if (angle) { if (c == '>') angle = false; continue; }
			if (c == '<') angle = true;
			else if (c == '"' || c == '\'') quote = c;
			else if (c == '(') depth++;
			else if (c == ')') { if (--depth == 0) return j + 1; }
		}
		return limit;
	}
	if (j < limit && source[j] == '[')
	{
		while (j < limit && source[j] != ']') j++;
		return j < limit ? j + 1 : limit;
	}
	return j;
}

void CMarkdownDocument::ResolveSpans()
{
	auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
	for (CMarkdownSpan &span : spans)
	{
		// Content: the in-buffer runs inside.
		uint32_t cBeg = UINT32_MAX, cEnd = 0;
		for (uint32_t r = span.firstRun; r < span.firstRun + span.runCount && r < runs.size(); r++)
		{
			const CMarkdownRun &run = runs[r];
			if (run.IsSynthesized() || run.beg == UINT32_MAX) continue;
			cBeg = std::min(cBeg, run.beg);
			cEnd = std::max(cEnd, run.end);
		}

		// Neighbouring content of the same block bounds the markers.
		uint32_t prevEnd = UINT32_MAX, nextBeg = UINT32_MAX;
		for (uint32_t r = span.firstRun; r > 0; r--)
		{
			const CMarkdownRun &run = runs[r - 1];
			if (run.block != span.block) break;
			if (!run.IsSynthesized() && run.beg != UINT32_MAX) { prevEnd = run.end; break; }
		}
		for (uint32_t r = span.firstRun + span.runCount; r < runs.size(); r++)
		{
			const CMarkdownRun &run = runs[r];
			if (run.block != span.block) break;
			if (!run.IsSynthesized() && run.beg != UINT32_MAX) { nextBeg = run.beg; break; }
		}

		if (cBeg == UINT32_MAX)
		{
			// No content (`![](x)`, `[](x)`): the span is the syntax between its
			// neighbours, blanks trimmed. Without neighbours it cannot be placed.
			uint32_t a = prevEnd != UINT32_MAX ? prevEnd : (nextBeg != UINT32_MAX ? LineStart(nextBeg) : 0);
			uint32_t b = nextBeg != UINT32_MAX ? nextBeg : LineEnd(a);
			if (prevEnd == UINT32_MAX && span.block >= 0 && span.block < (int32_t)blocks.size() && nextBeg == UINT32_MAX)
			{
				a = blocks[span.block].beg;
				b = LineEnd(a);
			}
			if (prevEnd == UINT32_MAX) a = SkipContainerMarkers(LineStart(a), LineEnd(a));
			while (a < b && isWs(source[a])) a++;
			while (b > a && isWs(source[b - 1])) b--;
			// Within that, the image/link syntax starts at its `!` or `[`.
			uint32_t open = a;
			while (open < b && source[open] != '[' && source[open] != '!') open++;
			if (open < b) a = open;
			span.beg = a;
			span.contentBeg = span.contentEnd = std::min(b, a + (source[a] == '!' ? 2u : 1u));
			span.end = (span.type == EMarkdownSpanType::Link || span.type == EMarkdownSpanType::Image)
				? std::max(span.contentEnd, MatchLinkTail(span.contentEnd, std::max(b, span.contentEnd))) : b;
			continue;
		}

		span.contentBeg = cBeg;
		span.contentEnd = cEnd;

		// Opening: back over syntax bytes to the previous content or a blank.
		const uint32_t lower = prevEnd != UINT32_MAX ? prevEnd : LineStart(cBeg);
		uint32_t a = cBeg;
		if (span.type == EMarkdownSpanType::Code && a > lower && source[a - 1] == ' ') a--;   // `` ` x ` ``
		while (a > lower && !isWs(source[a - 1])) a--;
		span.beg = a;

		// Closing: forward over syntax bytes to the next content or a blank;
		// a link or image through its destination.
		const uint32_t upper = nextBeg != UINT32_MAX ? nextBeg : LineEnd(cEnd > 0 ? cEnd - 1 : cEnd);
		uint32_t b = cEnd;
		if (span.type == EMarkdownSpanType::Link || span.type == EMarkdownSpanType::Image)
		{
			const uint32_t tail = MatchLinkTail(b, std::max(upper, b));
			if (tail != b) b = tail;
			else while (b < upper && !isWs(source[b])) b++;   // autolink `<url>`
		}
		else
		{
			if (span.type == EMarkdownSpanType::Code && b < upper && source[b] == ' ') b++;
			while (b < upper && !isWs(source[b])) b++;
		}
		span.end = std::max(b, cEnd);
	}

	// An enclosing span covers its children.
	for (int32_t i = (int32_t)spans.size() - 1; i >= 0; i--)
	{
		const int32_t p = spans[i].parent;
		if (p < 0) continue;
		spans[p].beg = std::min(spans[p].beg, spans[i].beg);
		spans[p].end = std::max(spans[p].end, spans[i].end);
	}
}

int32_t CMarkdownDocument::FindSpanAt(uint32_t offset, EMarkdownSpanType type) const
{
	int32_t best = -1;
	for (int32_t i = 0; i < (int32_t)spans.size(); i++)
		if (spans[i].type == type && spans[i].Touches(offset)) best = i;   // later = inner
	return best;
}

std::vector<int32_t> CMarkdownDocument::SpansTouching(uint32_t offset) const
{
	std::vector<int32_t> out;
	for (int32_t i = 0; i < (int32_t)spans.size(); i++)
		if (spans[i].Touches(offset)) out.push_back(i);
	return out;
}

// ---- Queries -----------------------------------------------------------------

std::vector<int32_t> CMarkdownDocument::GetChildren(int32_t block) const
{
	std::vector<int32_t> out;
	for (int32_t i = block + 1; i < (int32_t)blocks.size(); i++)
	{
		if (blocks[i].parent == block) out.push_back(i);
		else if (blocks[i].depth <= blocks[block].depth) break;
	}
	return out;
}

int32_t CMarkdownDocument::FindBlockAt(uint32_t offset, bool topLevelOnly) const
{
	if (blocks.size() <= 1) return 0;
	int32_t best = -1;
	int32_t before = -1;   // the last top-level block ending at or before offset
	int32_t after = -1;    // the first top-level block starting after offset
	for (int32_t i = 1; i < (int32_t)blocks.size(); i++)
	{
		const CMarkdownBlock &b = blocks[i];
		if (b.parent == 0)
		{
			if (b.end <= offset) before = i;
			if (b.beg > offset && after == -1) after = i;
		}
		// Containment: offset in [beg, end]; `end` inclusive so a caret at the
		// end of a block still belongs to it. Deeper blocks win because they
		// come later in document order.
		if (offset >= b.beg && offset <= b.end)
		{
			if (topLevelOnly && b.parent != 0) continue;
			if (best == -1 || b.depth >= blocks[best].depth) best = i;
		}
	}
	if (best != -1)
		return topLevelOnly ? TopLevelOf(best) : best;
	if (before != -1) return before;
	if (after != -1) return after;
	return 0;
}

int32_t CMarkdownDocument::TopLevelOf(int32_t block) const
{
	if (block <= 0 || block >= (int32_t)blocks.size()) return 0;
	while (blocks[block].parent > 0) block = blocks[block].parent;
	return block;
}

int32_t CMarkdownDocument::NextTopLevel(int32_t block) const
{
	const int32_t top = TopLevelOf(block);
	for (int32_t i = top + 1; i < (int32_t)blocks.size(); i++)
		if (blocks[i].parent == 0) return i;
	return -1;
}

int32_t CMarkdownDocument::PrevTopLevel(int32_t block) const
{
	const int32_t top = TopLevelOf(block);
	for (int32_t i = top - 1; i >= 1; i--)
		if (blocks[i].parent == 0) return i;
	return -1;
}

int32_t CMarkdownDocument::RowOf(int32_t cell) const
{
	if (cell <= 0 || cell >= (int32_t)blocks.size() || !blocks[cell].IsCell()) return -1;
	return blocks[cell].parent;
}

int32_t CMarkdownDocument::TableOf(int32_t block) const
{
	while (block > 0 && block < (int32_t)blocks.size())
	{
		if (blocks[block].type == EMarkdownBlockType::Table) return block;
		block = blocks[block].parent;
	}
	return -1;
}

int32_t CMarkdownDocument::ColumnOf(int32_t cell) const
{
	const int32_t row = RowOf(cell);
	if (row < 0) return -1;
	int32_t col = 0;
	for (int32_t i = row + 1; i < cell; i++)
		if (blocks[i].parent == row) col++;
	return col;
}

std::vector<int32_t> CMarkdownDocument::CellsOf(int32_t row) const
{
	return GetChildren(row);
}

std::vector<int32_t> CMarkdownDocument::RowsOf(int32_t table) const
{
	std::vector<int32_t> rows;
	if (table <= 0 || table >= (int32_t)blocks.size() || blocks[table].type != EMarkdownBlockType::Table) return rows;
	for (int32_t i = table + 1; i < (int32_t)blocks.size(); i++)
	{
		if (blocks[i].depth <= blocks[table].depth) break;
		if (blocks[i].type == EMarkdownBlockType::TableRow) rows.push_back(i);
	}
	return rows;
}

int32_t CMarkdownDocument::FindRunAt(uint32_t offset) const
{
	// Runs are in document order and content runs do not overlap. Linear is
	// fine for the spike's document sizes; a binary search is the obvious
	// upgrade.
	for (int32_t i = 0; i < (int32_t)runs.size(); i++)
	{
		const CMarkdownRun &r = runs[i];
		if (r.IsSynthesized()) continue;
		if (offset >= r.beg && offset < r.end) return i;
	}
	return -1;
}

bool CMarkdownDocument::IsContentByte(uint32_t offset) const
{
	return FindRunAt(offset) != -1;
}

uint32_t CMarkdownDocument::NextCodepoint(uint32_t offset) const
{
	const uint32_t size = Size();
	if (offset >= size) return size;
	offset++;
	while (offset < size && ((unsigned char)source[offset] & 0xC0) == 0x80) offset++;
	return offset;
}

uint32_t CMarkdownDocument::PrevCodepoint(uint32_t offset) const
{
	if (offset == 0) return 0;
	if (offset > Size()) offset = Size();
	offset--;
	while (offset > 0 && ((unsigned char)source[offset] & 0xC0) == 0x80) offset--;
	return offset;
}

// ---- Structural edits --------------------------------------------------------

bool CMarkdownDocument::GetAtxMarkerRange(int32_t block, uint32_t *beg, uint32_t *end) const
{
	if (block <= 0 || block >= (int32_t)blocks.size()) return false;
	const CMarkdownBlock &b = blocks[block];
	if (b.type != EMarkdownBlockType::Heading && b.type != EMarkdownBlockType::Paragraph) return false;

	const uint32_t lineEnd = LineEnd(b.beg);
	uint32_t i = SkipContainerMarkers(b.beg, lineEnd);
	const uint32_t markerBeg = i;
	uint32_t hashes = 0;
	while (i < lineEnd && source[i] == '#') { i++; hashes++; }
	if (hashes == 0 || hashes > 6 || (i < lineEnd && !IsBlank(source[i])))
	{
		// Not an ATX marker (a paragraph, or `#foo`): empty range at content start.
		*beg = *end = markerBeg;
		return true;
	}
	while (i < lineEnd && IsBlank(source[i])) i++;
	*beg = markerBeg;
	*end = i;
	return true;
}

bool CMarkdownDocument::SetHeadingLevel(uint32_t offset, int level, uint32_t *caretInOut)
{
	if (level < 0 || level > 6) return false;
	const int32_t block = FindBlockAt(offset);
	if (block <= 0) return false;
	const CMarkdownBlock &b = blocks[block];
	if (b.type != EMarkdownBlockType::Heading && b.type != EMarkdownBlockType::Paragraph) return false;

	uint32_t markerBeg = 0, markerEnd = 0;
	if (!GetAtxMarkerRange(block, &markerBeg, &markerEnd)) return false;

	// A setext heading has no ATX marker but is a Heading: leave it alone
	// rather than produce `# Title\n====`.
	if (b.type == EMarkdownBlockType::Heading && markerBeg == markerEnd) return false;

	std::string marker;
	if (level > 0) marker.assign((size_t)level, '#').append(" ");

	const uint32_t caret = caretInOut ? *caretInOut : offset;
	Replace(markerBeg, markerEnd, marker);

	if (caretInOut)
	{
		if (caret >= markerEnd) *caretInOut = caret - (markerEnd - markerBeg) + (uint32_t)marker.size();
		else *caretInOut = markerBeg + (uint32_t)marker.size();
	}
	return true;
}

std::string CMarkdownDocument::ListContinuationPrefix(uint32_t offset) const
{
	const uint32_t lineBeg = LineStart(offset);
	const uint32_t lineEnd = LineEnd(offset);
	uint32_t i = lineBeg;
	while (i < lineEnd && IsBlank(source[i])) i++;
	const std::string indent = source.substr(lineBeg, i - lineBeg);
	if (i >= lineEnd) return "";

	std::string marker;
	if (source[i] == '-' || source[i] == '*' || source[i] == '+')
	{
		marker = source.substr(i, 1);
		i++;
	}
	else if (source[i] >= '0' && source[i] <= '9')
	{
		uint32_t j = i;
		unsigned long n = 0;
		while (j < lineEnd && source[j] >= '0' && source[j] <= '9' && j - i < 9) { n = n * 10 + (unsigned long)(source[j] - '0'); j++; }
		if (j >= lineEnd || (source[j] != '.' && source[j] != ')')) return "";
		marker = std::to_string(n + 1) + source[j];
		i = j + 1;
	}
	else return "";

	if (i >= lineEnd || !IsBlank(source[i])) return "";
	while (i < lineEnd && IsBlank(source[i])) i++;

	std::string prefix = indent + marker + " ";
	// Task box: `[ ]`, `[x]`, `[X]` followed by a space.
	if (i + 3 <= lineEnd && source[i] == '[' && (source[i + 1] == ' ' || source[i + 1] == 'x' || source[i + 1] == 'X')
		&& source[i + 2] == ']' && (i + 3 == lineEnd || IsBlank(source[i + 3])))
		prefix += "[ ] ";
	return prefix;
}

// ---- Task items ----------------------------------------------------------------

int32_t CMarkdownDocument::FindTaskItemAt(uint32_t offset) const
{
	int32_t block = FindBlockAt(offset);
	while (block > 0)
	{
		const CMarkdownBlock &b = blocks[block];
		if (b.type == EMarkdownBlockType::ListItem && b.isTask) return block;
		block = b.parent;
	}
	return -1;
}

int32_t CMarkdownDocument::FindTaskItemByMark(uint32_t markOffset) const
{
	for (int32_t i = 1; i < (int32_t)blocks.size(); i++)
		if (blocks[i].type == EMarkdownBlockType::ListItem && blocks[i].isTask && blocks[i].taskMarkOffset == markOffset) return i;
	return -1;
}

bool CMarkdownDocument::ToggleTask(int32_t item)
{
	if (item <= 0 || item >= (int32_t)blocks.size()) return false;
	const CMarkdownBlock &b = blocks[item];
	if (b.type != EMarkdownBlockType::ListItem || !b.isTask || b.taskMarkOffset >= Size()) return false;
	const bool checked = b.taskMark == 'x' || b.taskMark == 'X';
	Replace(b.taskMarkOffset, b.taskMarkOffset + 1, checked ? " " : "x");
	return true;
}
