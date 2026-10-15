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
	if (!(incrementalParse && TryIncrementalReplace(beg, end, text)))
	{
		source.replace(beg, end - beg, text.data(), text.size());
		Parse();
	}
	return beg + (uint32_t)text.size();
}

// A line that could be a link reference definition (`[label]: dest`), after
// container markers. Conservative: a false positive only costs a full parse.
bool CMarkdownDocument::MayHaveLinkReferenceDefinitions(std::string_view text)
{
	size_t i = 0;
	const size_t n = text.size();
	while (i < n)
	{
		size_t j = i;
		while (j < n && (text[j] == ' ' || text[j] == '\t' || text[j] == '>')) j++;
		if (j < n && text[j] == '[')
		{
			const size_t lineEnd = text.find('\n', j);
			const std::string_view line = text.substr(j, (lineEnd == std::string_view::npos ? n : lineEnd) - j);
			if (line.find("]:") != std::string_view::npos) return true;
		}
		const size_t nl = text.find('\n', i);
		if (nl == std::string_view::npos) break;
		i = nl + 1;
	}
	return false;
}

bool CMarkdownDocument::TryIncrementalReplace(uint32_t beg, uint32_t end, std::string_view text)
{
	lastParseIncremental = false;
	if (hasRefDefs || blocks.size() <= 1) return false;
	const std::vector<int32_t> tops = GetChildren(0);
	const int n = (int)tops.size();
	if (n < 3) return false;

	// The top-level blocks the edit touches: the last starting at or before
	// each end of the edit (a gap belongs to the block before it).
	auto topAt = [&](uint32_t offset) {
		int k = 0;
		for (int i = 0; i < n; i++) if (blocks[tops[i]].beg <= offset) k = i;
		return k;
	};
	const int iA = topAt(beg), iZ = topAt(end);
	const int ia = std::max(0, iA - 1), iz = std::min(n - 1, iZ + 1);
	if (ia == 0 && iz == n - 1) return false;   // no smaller than the whole document

	// The old region: from the previous neighbour (or the document start) to
	// the next neighbour's end (or the document end).
	// The gaps to the blocks outside belong to the region too: they hold no
	// block's content, but they may hold syntax a block range does not name.
	const uint32_t r0 = ia < iA ? (ia > 0 ? std::min(blocks[tops[ia]].beg, LineEnd(blocks[tops[ia - 1]].end) + 1) : 0) : 0;
	const uint32_t r1 = iz > iZ ? (iz + 1 < n ? std::max(blocks[tops[iz]].end, LineStart(blocks[tops[iz + 1]].beg) - 1) : Size()) : Size();
	if (beg < r0 || end > r1) return false;
	const int64_t delta = (int64_t)text.size() - (int64_t)(end - beg);

	std::string newSource = source;
	newSource.replace(beg, end - beg, text.data(), text.size());
	const uint32_t nr1 = (uint32_t)((int64_t)r1 + delta);
	const std::string_view region(newSource.data() + r0, nr1 - r0);
	if (MayHaveLinkReferenceDefinitions(region)) return false;

	CMarkdownDocument sub;
	sub.incrementalParse = false;
	sub.parserFlags = parserFlags;
	sub.SetSource(region);
	const std::vector<int32_t> subTops = sub.GetChildren(0);
	if (subTops.empty()) return false;

	// The neighbours must parse exactly as before: same type, same range.
	if (ia < iA)
	{
		const CMarkdownBlock &o = blocks[tops[ia]];
		const CMarkdownBlock &f = sub.blocks[subTops.front()];
		if (f.type != o.type || f.beg + r0 != o.beg || f.end + r0 != o.end) return false;
		if (subTops.size() < 2) return false;
	}
	if (iz > iZ)
	{
		const CMarkdownBlock &o = blocks[tops[iz]];
		const CMarkdownBlock &l = sub.blocks[subTops.back()];
		if (l.type != o.type || (int64_t)l.beg + r0 != (int64_t)o.beg + delta || (int64_t)l.end + r0 != (int64_t)o.end + delta) return false;
	}

	// Splice. The old region is a contiguous run of blocks (whole top-level
	// subtrees in pre-order), of runs and of spans.
	const int32_t blockA = tops[ia];
	const int32_t blockZ = iz + 1 < n ? tops[iz + 1] : (int32_t)blocks.size();   // one past
	const uint32_t runA = blocks[blockA].firstRun;
	const uint32_t runZ = blockZ < (int32_t)blocks.size() ? blocks[blockZ].firstRun : (uint32_t)runs.size();
	uint32_t spanA = (uint32_t)spans.size(), spanZ = (uint32_t)spans.size();
	for (uint32_t i = 0; i < spans.size(); i++)
		if (spans[i].block >= blockA) { spanA = i; break; }
	for (uint32_t i = spanA; i < spans.size(); i++)
		if (spans[i].block >= blockZ) { spanZ = i; break; }

	const int32_t newBlocks = (int32_t)sub.blocks.size() - 1;
	const int32_t dBlocks = newBlocks - (blockZ - blockA);
	const int32_t dRuns = (int32_t)sub.runs.size() - (int32_t)(runZ - runA);
	const int32_t dSpans = (int32_t)sub.spans.size() - (int32_t)(spanZ - spanA);
	auto shift = [](uint32_t &v, int64_t d) { if (v != UINT32_MAX) v = (uint32_t)((int64_t)v + d); };
	auto shiftBlock = [&](CMarkdownBlock &b, int64_t d) {
		shift(b.beg, d);
		shift(b.end, d);
		if (b.isTask) shift(b.taskMarkOffset, d);
		if (b.infoEnd > b.infoBeg) { shift(b.infoBeg, d); shift(b.infoEnd, d); }
		if (b.langEnd > b.langBeg) { shift(b.langBeg, d); shift(b.langEnd, d); }
	};

	// The region's replacements, mapped into this document.
	std::vector<CMarkdownBlock> inBlocks(sub.blocks.begin() + 1, sub.blocks.end());
	for (CMarkdownBlock &b : inBlocks)
	{
		shiftBlock(b, r0);
		b.parent = b.parent <= 0 ? 0 : b.parent - 1 + blockA;
		b.firstRun += runA;
	}
	std::vector<CMarkdownRun> inRuns = sub.runs;
	for (CMarkdownRun &r : inRuns)
	{
		shift(r.beg, r0);
		shift(r.end, r0);
		if (r.block > 0) r.block = r.block - 1 + blockA;
		if (r.span >= 0) r.span += (int32_t)spanA;
	}
	std::vector<CMarkdownSpan> inSpans = sub.spans;
	for (CMarkdownSpan &sp : inSpans)
	{
		shift(sp.beg, r0); shift(sp.end, r0); shift(sp.contentBeg, r0); shift(sp.contentEnd, r0);
		if (sp.block > 0) sp.block = sp.block - 1 + blockA;
		if (sp.parent >= 0) sp.parent += (int32_t)spanA;
		sp.firstRun += runA;
	}

	// Everything after the region moves by the edit and by the count changes.
	for (int32_t i = blockZ; i < (int32_t)blocks.size(); i++)
	{
		CMarkdownBlock &b = blocks[i];
		shiftBlock(b, delta);
		if (b.parent >= blockZ) b.parent += dBlocks;
		b.firstRun = (uint32_t)((int32_t)b.firstRun + dRuns);
	}
	for (uint32_t i = runZ; i < runs.size(); i++)
	{
		CMarkdownRun &r = runs[i];
		shift(r.beg, delta);
		shift(r.end, delta);
		if (r.block >= blockZ) r.block += dBlocks;
		if (r.span >= (int32_t)spanZ) r.span += dSpans;
	}
	for (uint32_t i = spanZ; i < spans.size(); i++)
	{
		CMarkdownSpan &sp = spans[i];
		shift(sp.beg, delta); shift(sp.end, delta); shift(sp.contentBeg, delta); shift(sp.contentEnd, delta);
		if (sp.block >= blockZ) sp.block += dBlocks;
		if (sp.parent >= (int32_t)spanZ) sp.parent += dSpans;
		sp.firstRun = (uint32_t)((int32_t)sp.firstRun + dRuns);
	}

	blocks.erase(blocks.begin() + blockA, blocks.begin() + blockZ);
	blocks.insert(blocks.begin() + blockA, inBlocks.begin(), inBlocks.end());
	runs.erase(runs.begin() + runA, runs.begin() + runZ);
	runs.insert(runs.begin() + runA, inRuns.begin(), inRuns.end());
	spans.erase(spans.begin() + spanA, spans.begin() + spanZ);
	spans.insert(spans.begin() + spanA, inSpans.begin(), inSpans.end());

	source.swap(newSource);
	blocks[0].beg = 0;
	blocks[0].end = Size();
	blocks[0].runCount = (uint32_t)runs.size();
	parseCount++;
	incrementalParseCount++;
	lastParseIncremental = true;
	return true;
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
	strayLeaves = 0;
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
	lastParseIncremental = false;
	hasRefDefs = MayHaveLinkReferenceDefinitions(source);

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

	// Spans end with their block. md4c can resolve a delimiter against one it
	// later consumes elsewhere (`~a [x](~b)` pairs the two tildes) and then
	// emits that span's leave_span in a LATER block. Closing it here keeps a
	// span inside its block -- and keeps the model a function of the block's
	// own text, which incremental reparse depends on. The late leave_span is
	// recognised and dropped in LeaveSpan().
	while (!doc->openSpans.empty() && doc->spans[doc->openSpans.back()].block == index && !doc->spanStack.empty())
	{
		CMarkdownSpan &span = doc->spans[doc->openSpans.back()];
		span.runCount = (uint32_t)doc->runs.size() - span.firstRun;
		doc->currentStyles = doc->spanStack.back().style;
		doc->currentHref = doc->spanStack.back().href;
		doc->spanStack.pop_back();
		doc->openSpans.pop_back();
		doc->strayLeaves++;
	}

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

static EMarkdownSpanType SpanTypeFromMd(int type)
{
	switch ((MD_SPANTYPE)type)
	{
		case MD_SPAN_EM: return EMarkdownSpanType::Emphasis;
		case MD_SPAN_STRONG: return EMarkdownSpanType::Strong;
		case MD_SPAN_CODE: return EMarkdownSpanType::Code;
		case MD_SPAN_A: return EMarkdownSpanType::Link;
		case MD_SPAN_IMG: return EMarkdownSpanType::Image;
		case MD_SPAN_DEL: return EMarkdownSpanType::Strikethrough;
		case MD_SPAN_U: return EMarkdownSpanType::Underline;
		case MD_SPAN_LATEXMATH:
		case MD_SPAN_LATEXMATH_DISPLAY: return EMarkdownSpanType::Math;
		case MD_SPAN_WIKILINK: return EMarkdownSpanType::WikiLink;
	}
	return EMarkdownSpanType::Emphasis;
}

int CMarkdownDocument::LeaveSpan(int type, void *, void *userdata)
{
	CMarkdownDocument *doc = (CMarkdownDocument *)userdata;
	if (doc->spanStack.empty())
	{
		if (doc->strayLeaves > 0) doc->strayLeaves--;
		return 0;
	}
	// A leave_span for a span already closed with its block (see LeaveBlock):
	// the innermost open span is of another type or from another block.
	if (doc->strayLeaves > 0 && !doc->openSpans.empty())
	{
		const CMarkdownSpan &top = doc->spans[doc->openSpans.back()];
		const int32_t current = doc->blockStack.empty() ? -1 : doc->blockStack.back();
		if (top.type != SpanTypeFromMd(type) || top.block != current)
		{
			doc->strayLeaves--;
			return 0;
		}
	}
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

// The end of the line before the next blank line after `from` (or the
// source end): as far as a paragraph's text can run.
uint32_t CMarkdownDocument::NextBlankLine(uint32_t from) const
{
	const uint32_t size = Size();
	uint32_t lb = LineEnd(std::min(from, size));
	while (lb < size)
	{
		const uint32_t next = lb + 1, ne = LineEnd(next);
		bool blank = true;
		for (uint32_t k = next; k < ne; k++) if (!IsBlank(source[k])) { blank = false; break; }
		if (blank) return lb;
		lb = ne;
	}
	return size;
}

// Where a code span's closing backticks, or a link's / image's `](dest)`,
// end -- from the runs alone, before any block range is known. 0 when the
// span is of another kind or the closing cannot be found. These are the two
// spans whose closing syntax can sit on a line with no content at all
// ("`code\n`", "[a](\ndest)"), a line the content-based leaf range misses.
uint32_t CMarkdownDocument::SpanCloseEnd(const CMarkdownSpan &span) const
{
	const uint32_t size = Size();
	if (span.type != EMarkdownSpanType::Code && span.type != EMarkdownSpanType::Link && span.type != EMarkdownSpanType::Image)
	{
		// Any other span whose last run is a line break closes on a later
		// line: its delimiter is the first non-blank text after the break.
		if (span.runCount == 0) return 0;
		const CMarkdownRun &last = runs[span.firstRun + span.runCount - 1];
		if (!(last.IsSynthesized() && (last.kind == EMarkdownRunKind::SoftBreak || last.kind == EMarkdownRunKind::HardBreak))) return 0;
		uint32_t j = last.beg;
		const uint32_t limit = NextBlankLine(j);
		while (j < limit && (IsBlank(source[j]) || source[j] == '\n' || source[j] == '>')) j++;
		uint32_t k = j;
		while (k < limit && (source[k] == '*' || source[k] == '_' || source[k] == '~')) k++;
		return k > j ? k : 0;
	}
	uint32_t prevEnd = 0, cBeg = UINT32_MAX, cEnd = 0;
	for (uint32_t r = span.firstRun; r > 0; r--)
	{
		const CMarkdownRun &run = runs[r - 1];
		if (run.block != span.block) break;
		if (!run.IsSynthesized() && run.beg != UINT32_MAX) { prevEnd = run.end; break; }
	}
	for (uint32_t r = span.firstRun; r < span.firstRun + span.runCount && r < runs.size(); r++)
	{
		const CMarkdownRun &run = runs[r];
		if (run.IsSynthesized() || run.beg == UINT32_MAX) continue;
		cBeg = std::min(cBeg, run.beg);
		cEnd = std::max(cEnd, run.end);
	}
	const uint32_t limit = NextBlankLine(cBeg != UINT32_MAX ? cBeg : prevEnd);
	if (span.type == EMarkdownSpanType::Code)
	{
		// The opener: the backticks before the content (blanks between), or
		// the first backticks after the previous content.
		uint32_t open = UINT32_MAX;
		if (cBeg != UINT32_MAX)
		{
			uint32_t k = cBeg;
			while (k > prevEnd && (IsBlank(source[k - 1]) || source[k - 1] == '\n')) k--;
			while (k > prevEnd && source[k - 1] == '`') k--;
			if (k < cBeg && source[k] == '`') open = k;
		}
		else
		{
			uint32_t k = prevEnd;
			while (k < limit && source[k] != '`') k++;
			if (k < limit) open = k;
		}
		if (open == UINT32_MAX) return 0;
		uint32_t n = 0;
		while (open + n < size && source[open + n] == '`') n++;
		uint32_t j = std::max(open + n, cEnd);
		while (j < limit)
		{
			if (source[j] != '`') { j++; continue; }
			uint32_t m = 0;
			while (j + m < size && source[j + m] == '`') m++;
			if (m == n) return j + m;
			j += m;
		}
		return 0;
	}
	if (cEnd == 0) return 0;
	const uint32_t tail = MatchLinkTail(cEnd, limit);
	return tail != cEnd ? tail : 0;
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

		// Lines with no content at all -- a lone closing "`", "~", "](dest)",
		// a lazy line of markers -- are still lines of the leaf, and md4c says
		// how many: one line break per line boundary. Breaks before the first
		// content move the start up, breaks after the last content move the
		// end down. (Not for code, whose blank lines are content.)
		if (block.type != EMarkdownBlockType::Code)
		{
			uint32_t before = 0, after = 0;
			bool seen = false;
			for (uint32_t r = block.firstRun; r < block.firstRun + block.runCount; r++)
			{
				const CMarkdownRun &run = runs[r];
				if (run.block != index) continue;
				const bool lineBreak = (run.kind == EMarkdownRunKind::SoftBreak || run.kind == EMarkdownRunKind::HardBreak)
					&& run.synthesized == "\n";
				if (lineBreak) { if (seen) after++; else before++; }
				else if (!run.IsSynthesized() && run.beg != UINT32_MAX) { seen = true; after = 0; }
			}
			for (uint32_t k = 0; k < before && block.beg > 0; k++) block.beg = LineStart(block.beg - 1);
			for (uint32_t k = 0; k < after && block.end < Size(); k++) block.end = LineEnd(block.end + 1);
		}

		// A code span or link closing on a line of its own: the leaf runs to
		// that line. Known here, before the next empty block is placed after
		// this one's end.
		for (const CMarkdownSpan &sp : spans)
		{
			if (sp.block != index) continue;
			const uint32_t closeEnd = SpanCloseEnd(sp);
			if (closeEnd > block.end) block.end = LineEnd(closeEnd - 1);
		}
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
		// Closing fence: the next fence line after the last content line.
		// Blank lines inside the block come as synthesized line breaks, not as
		// content, so they are skipped on the way.
		uint32_t probe = block.end;
		while (probe < Size())
		{
			const uint32_t nextBeg = probe + 1;
			const uint32_t nextEnd = LineEnd(nextBeg);
			if (LineIsFence(nextBeg, nextEnd, block.fenceChar)) { block.end = nextEnd; break; }
			if (SkipContainerMarkers(nextBeg, nextEnd) < nextEnd) break;   // not blank: no closing fence here
			probe = nextEnd;
		}
	}
	else if (block.type == EMarkdownBlockType::Heading && block.end < Size())
	{
		// Setext underline: a `===` / `---` line right after the text.
		const uint32_t nextBeg = block.end + 1;
		const uint32_t nextEnd = LineEnd(nextBeg);
		// ATX is 1-6 '#' then a blank or the line end; "#b" is setext text.
		const uint32_t firstLineEnd = LineEnd(block.beg);
		uint32_t h = SkipContainerMarkers(block.beg, firstLineEnd), hashes = 0;
		while (h < firstLineEnd && source[h] == '#') { h++; hashes++; }
		const bool isAtx = hashes >= 1 && hashes <= 6 && (h == firstLineEnd || IsBlank(source[h]));
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
	std::vector<uint32_t> quoteTail(blocks.size(), 0);
	uint32_t cursor = 0;
	// Lines that hold nothing but quote markers (`>`) after a leaf inside a
	// quote belong to that quote; there is no content on them to say so.
	auto takeQuoteTail = [&](int32_t leaf) {
		int32_t q = blocks[leaf].parent;
		while (q > 0 && blocks[q].type != EMarkdownBlockType::Quote) q = blocks[q].parent;
		if (q <= 0) return;
		// The quote's own marker column: a `>` further left is another quote
		// (one inside a list item cannot be continued from column 0).
		const uint32_t qLine = LineStart(blocks[leaf].beg);
		uint32_t qcol = 0;
		while (qLine + qcol < Size() && source[qLine + qcol] != '>' && source[qLine + qcol] != '\n') qcol++;
		uint32_t probe = cursor;
		while (probe < Size())
		{
			const uint32_t nb = probe + 1, ne = LineEnd(nb);
			uint32_t k = nb;
			while (k < ne && IsBlank(source[k])) k++;
			if (k >= ne || source[k] != '>' || k - nb < qcol || SkipContainerMarkers(nb, ne) < ne) break;
			probe = ne;
		}
		if (probe > cursor) { cursor = probe; quoteTail[q] = std::max(quoteTail[q], probe); }
	};
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
			takeQuoteTail(i);
		}
		else if ((block.type == EMarkdownBlockType::ListItem || block.type == EMarkdownBlockType::Quote)
			&& !(i + 1 < (int32_t)blocks.size() && blocks[i + 1].parent == i))
		{
			// An empty list item (`-` alone) or quote (`>` alone): on the first
			// non-blank line after the previous block. Placed here, in document
			// order, so a run of empty ones lands on consecutive lines.
			const uint32_t lineBeg = FirstNonBlankLineStart(cursor);
			block.beg = lineBeg;
			block.end = LineEnd(lineBeg);
			cursor = std::max(cursor, block.end);
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
			// Place it on the first non-blank line after the block placed just
			// before it in document order -- a previous sibling's content, or
			// the text of an enclosing item. (Not "after the previous sibling":
			// the first item of a list has none, and its parent is not placed
			// yet in this pass; that used to put it on the document's first line.)
			uint32_t from = 0;
			for (int32_t p = i - 1; p >= 1; p--)
				if (placed[p]) { from = blocks[p].end; break; }
			const uint32_t lineBeg = FirstNonBlankLineStart(from);
			block.beg = lineBeg;
			block.end = LineEnd(lineBeg);
			placed[i] = true;
			continue;
		}

		block.beg = minBeg;
		block.end = std::max(maxEnd, quoteTail[i]);

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
	const uint32_t size = Size();
	// Where a paragraph's text can still run: up to the next blank line.
	auto paragraphLimit = [&](uint32_t from) {
		uint32_t lb = LineEnd(from);
		while (lb < size)
		{
			const uint32_t next = lb + 1, ne = LineEnd(next);
			bool blank = true;
			for (uint32_t k = next; k < ne; k++) if (!IsBlank(source[k])) { blank = false; break; }
			if (blank) return lb;
			lb = ne;
		}
		return size;
	};
	// A code span: the run of N backticks at or before `from`, and the next
	// run of exactly N after it (CommonMark: a code span may cross lines).
	auto matchCode = [&](uint32_t open, uint32_t searchFrom, uint32_t limit, uint32_t *end) {
		uint32_t n = 0;
		while (open + n < size && source[open + n] == '`') n++;
		if (n == 0) return false;
		uint32_t j = std::max(searchFrom, open + n);
		while (j < limit)
		{
			if (source[j] != '`') { j++; continue; }
			uint32_t m = 0;
			while (j + m < size && source[j + m] == '`') m++;
			if (m == n) { *end = j + m; return true; }
			j += m;
		}
		return false;
	};
	for (CMarkdownSpan &span : spans)
	{
		// A span never runs into the next block: the block after its own in
		// document order (a leaf has no children) starts where it must stop.
		uint32_t blockLimit = size;
		if (span.block >= 0)
			for (int32_t j = span.block + 1; j < (int32_t)blocks.size(); j++)
				if (blocks[j].beg > 0 || blocks[j].end > 0) { blockLimit = std::max(blocks[j].beg, LineStart(blocks[j].beg)); break; }
		auto limitFrom = [&](uint32_t from) { return std::min(paragraphLimit(from), std::max(blockLimit, from)); };

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
			if (span.type == EMarkdownSpanType::Code)
			{
				// Only synthesized content (a line break): match the backticks.
				uint32_t open = prevEnd != UINT32_MAX ? prevEnd : a;
				while (open < size && source[open] != '`' && source[open] != '\n') open++;
				uint32_t close = 0;
				const uint32_t limit = nextBeg != UINT32_MAX ? nextBeg : limitFrom(open);
				if (open < size && source[open] == '`' && matchCode(open, open, limit, &close))
				{
					span.beg = open;
					span.contentBeg = span.contentEnd = open + 1;
					span.end = close;
					continue;
				}
			}
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
		if (span.type == EMarkdownSpanType::Code)
		{
			// The opening backticks, possibly with blanks or a line break
			// before the content (`` ` x ` ``, "`\ncode`").
			const uint32_t floor = prevEnd != UINT32_MAX ? prevEnd : 0;
			uint32_t k = cBeg;
			while (k > floor && isWs(source[k - 1])) k--;
			if (k > floor && source[k - 1] == '`')
			{
				while (k > floor && source[k - 1] == '`') k--;
				a = k;
			}
			else while (a > lower && !isWs(source[a - 1])) a--;
		}
		else while (a > lower && !isWs(source[a - 1])) a--;
		span.beg = a;

		// Closing: forward over syntax bytes to the next content or a blank;
		// a link or image through its destination, a code span through its
		// matching backticks -- the only two that may close past a blank.
		const uint32_t upper = nextBeg != UINT32_MAX ? nextBeg : LineEnd(cEnd > 0 ? cEnd - 1 : cEnd);
		uint32_t b = cEnd;
		uint32_t codeEnd = 0;
		if (span.type == EMarkdownSpanType::Code && a < cBeg && source[a] == '`'
			&& matchCode(a, cEnd, nextBeg != UINT32_MAX ? nextBeg : limitFrom(cEnd), &codeEnd))
		{
			b = codeEnd;
		}
		else if (span.type == EMarkdownSpanType::Link || span.type == EMarkdownSpanType::Image)
		{
			const uint32_t linkUpper = nextBeg != UINT32_MAX ? nextBeg : limitFrom(cEnd > 0 ? cEnd - 1 : cEnd);
			const uint32_t tail = MatchLinkTail(b, std::max(linkUpper, b));
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

	// A block covers its spans. Content alone can miss a line: a code span
	// whose closing backtick sits alone on a lazy continuation line ("`a\n`")
	// has no in-buffer content there, so the leaf (and its containers) would
	// end a line early -- and a reparse of the block's range would lose the span.
	for (const CMarkdownSpan &sp : spans)
	{
		if (sp.block <= 0 || sp.block >= (int32_t)blocks.size() || sp.end == 0) continue;
		CMarkdownBlock &b = blocks[sp.block];
		if (b.IsCell() || (sp.end <= b.end && sp.beg >= b.beg)) continue;
		const uint32_t newEnd = sp.end > b.end ? LineEnd(sp.end - 1) : b.end;
		const uint32_t newBeg = sp.beg < b.beg ? LineStart(sp.beg) : b.beg;
		for (int32_t k = sp.block; k > 0; k = blocks[k].parent)
		{
			blocks[k].end = std::max(blocks[k].end, newEnd);
			blocks[k].beg = std::min(blocks[k].beg, newBeg);
		}
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

// ---- Table edits ------------------------------------------------------------------

CMarkdownDocument::TableLine CMarkdownDocument::SplitTableLine(uint32_t lineBeg, uint32_t lineEnd) const
{
	TableLine t;
	uint32_t i = SkipContainerMarkers(lineBeg, lineEnd);
	t.prefix = source.substr(lineBeg, i - lineBeg);
	if (i < lineEnd && source[i] == '|') { t.lead = true; i++; }
	uint32_t segBeg = i;
	while (i <= lineEnd)
	{
		if (i < lineEnd && source[i] == '\\') { i += 2; continue; }
		if (i == lineEnd || source[i] == '|')
		{
			const uint32_t segEnd = std::min(i, lineEnd);
			const std::string seg = source.substr(segBeg, segEnd - segBeg);
			if (i == lineEnd)
			{
				// After a trailing pipe only blanks may remain; then there is no
				// last cell, the line simply ends with `|`.
				bool blank = true;
				for (char c : seg) if (!IsBlank(c)) { blank = false; break; }
				if (blank && !t.cells.empty() && segBeg > lineBeg && source[segBeg - 1] == '|') { t.trail = true; break; }
				t.cells.push_back(seg);
				break;
			}
			t.cells.push_back(seg);
			i++;
			segBeg = i;
			continue;
		}
		i++;
	}
	return t;
}

bool CMarkdownDocument::EditTable(int32_t cell, EMarkdownTableEdit edit, uint32_t *caretOut)
{
	if (cell <= 0 || cell >= (int32_t)blocks.size() || !blocks[cell].IsCell()) return false;
	const int32_t table = TableOf(cell);
	if (table < 0) return false;
	const std::vector<int32_t> rows = RowsOf(table);
	const int32_t rowBlock = RowOf(cell);
	int row = -1;
	for (size_t r = 0; r < rows.size(); r++) if (rows[r] == rowBlock) row = (int)r;
	int col = ColumnOf(cell);
	if (row < 0 || col < 0) return false;
	const int headRows = std::max<int>(1, blocks[table].headRowCount);
	const int rowCount = (int)rows.size();
	const int colCount = std::max<int>(1, blocks[table].columnCount);

	// The table's physical lines: head rows, the delimiter row, body rows.
	const uint32_t tableBeg = blocks[table].beg, tableEnd = blocks[table].end;
	std::vector<TableLine> lines;
	int delimLine = -1;
	for (uint32_t lb = tableBeg; lb <= tableEnd; )
	{
		const uint32_t le = LineEnd(lb);
		if (delimLine < 0 && LineIsTableDelimiter(lb, le)) delimLine = (int)lines.size();
		lines.push_back(SplitTableLine(lb, le));
		if (le >= tableEnd || le >= Size()) break;
		lb = le + 1;
	}
	if (delimLine < 0) return false;
	auto lineOfRow = [&](int r) { return r < headRows ? r : r + 1; };   // the delimiter sits after the head rows

	int targetRow = row, targetCol = col;
	const std::string blanks = "   ";
	switch (edit)
	{
		case EMarkdownTableEdit::InsertRowAbove:
		case EMarkdownTableEdit::InsertRowBelow:
		{
			const bool below = edit == EMarkdownTableEdit::InsertRowBelow;
			if (!below && row < headRows) return false;
			TableLine fresh;
			const TableLine &ref = lines[delimLine];
			fresh.prefix = lines[lineOfRow(row)].prefix;
			fresh.lead = ref.lead;
			fresh.trail = ref.trail;
			for (int c = 0; c < (int)ref.cells.size(); c++)
				fresh.cells.push_back(std::string(std::max<size_t>(3, ref.cells[c].size()), ' '));
			const int at = below ? (row < headRows ? delimLine + 1 : lineOfRow(row) + 1) : lineOfRow(row);
			lines.insert(lines.begin() + at, fresh);
			targetRow = below ? (row < headRows ? headRows : row + 1) : row;
			break;
		}
		case EMarkdownTableEdit::DeleteRow:
			if (row < headRows) return false;
			lines.erase(lines.begin() + lineOfRow(row));
			targetRow = row + 1 < rowCount ? row : row - 1;
			break;
		case EMarkdownTableEdit::MoveRowUp:
		case EMarkdownTableEdit::MoveRowDown:
		{
			const int other = edit == EMarkdownTableEdit::MoveRowUp ? row - 1 : row + 1;
			if (row < headRows || other < headRows || other >= rowCount) return false;
			std::swap(lines[lineOfRow(row)], lines[lineOfRow(other)]);
			targetRow = other;
			break;
		}
		case EMarkdownTableEdit::InsertColumnLeft:
		case EMarkdownTableEdit::InsertColumnRight:
		{
			const int at = edit == EMarkdownTableEdit::InsertColumnRight ? col + 1 : col;
			for (int l = 0; l < (int)lines.size(); l++)
			{
				TableLine &tl = lines[l];
				const std::string seg = l == delimLine ? std::string("---") : blanks;
				const bool padded = !tl.cells.empty() && !tl.cells[0].empty() && tl.cells[0][0] == ' ';
				const std::string cellText = l == delimLine && padded ? " " + seg + " " : seg;
				if (at <= (int)tl.cells.size()) tl.cells.insert(tl.cells.begin() + at, cellText);
				else tl.cells.push_back(cellText);
			}
			targetCol = at;
			break;
		}
		case EMarkdownTableEdit::DeleteColumn:
			if (colCount <= 1) return false;
			for (TableLine &tl : lines)
				if (col < (int)tl.cells.size()) tl.cells.erase(tl.cells.begin() + col);
			targetCol = col + 1 < colCount ? col : col - 1;
			break;
		case EMarkdownTableEdit::MoveColumnLeft:
		case EMarkdownTableEdit::MoveColumnRight:
		{
			const int other = edit == EMarkdownTableEdit::MoveColumnLeft ? col - 1 : col + 1;
			if (other < 0 || other >= colCount) return false;
			for (TableLine &tl : lines)
				if (col < (int)tl.cells.size() && other < (int)tl.cells.size()) std::swap(tl.cells[col], tl.cells[other]);
			targetCol = other;
			break;
		}
		case EMarkdownTableEdit::AlignDefault:
		case EMarkdownTableEdit::AlignLeft:
		case EMarkdownTableEdit::AlignCenter:
		case EMarkdownTableEdit::AlignRight:
		{
			TableLine &dl = lines[delimLine];
			if (col >= (int)dl.cells.size()) return false;
			std::string &seg = dl.cells[col];
			size_t a = 0, b = seg.size();
			while (a < b && IsBlank(seg[a])) a++;
			while (b > a && IsBlank(seg[b - 1])) b--;
			size_t dashes = 0;
			for (size_t k = a; k < b; k++) if (seg[k] == '-') dashes++;
			dashes = std::max<size_t>(dashes, 3);
			const bool left = edit == EMarkdownTableEdit::AlignLeft || edit == EMarkdownTableEdit::AlignCenter;
			const bool right = edit == EMarkdownTableEdit::AlignRight || edit == EMarkdownTableEdit::AlignCenter;
			// Keep the segment's width when the colons fit into its dashes.
			const size_t colons = (left ? 1 : 0) + (right ? 1 : 0);
			size_t oldColons = 0;
			for (size_t k = a; k < b; k++) if (seg[k] == ':') oldColons++;
			if (dashes + oldColons >= colons + 3) dashes = dashes + oldColons - colons;
			const std::string core = std::string(left ? ":" : "") + std::string(dashes, '-') + std::string(right ? ":" : "");
			seg = seg.substr(0, a) + core + seg.substr(b);
			break;
		}
	}

	std::string text;
	for (size_t l = 0; l < lines.size(); l++)
	{
		const TableLine &tl = lines[l];
		if (l > 0) text += '\n';
		text += tl.prefix;
		if (tl.lead) text += '|';
		for (size_t c = 0; c < tl.cells.size(); c++)
		{
			if (c > 0) text += '|';
			text += tl.cells[c];
		}
		if (tl.trail) text += '|';
	}
	if (text == source.substr(tableBeg, tableEnd - tableBeg)) return false;
	Replace(tableBeg, tableEnd, text);

	if (caretOut)
	{
		*caretOut = tableBeg;
		const int32_t t = TableOf(FindBlockAt(tableBeg + (uint32_t)lines[0].prefix.size() + 1));
		if (t > 0)
		{
			const std::vector<int32_t> newRows = RowsOf(t);
			if (targetRow >= 0 && targetRow < (int)newRows.size())
			{
				const std::vector<int32_t> cells = CellsOf(newRows[targetRow]);
				if (targetCol >= 0 && targetCol < (int)cells.size()) *caretOut = blocks[cells[targetCol]].beg;
			}
		}
	}
	return true;
}
