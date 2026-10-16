// CMarkdownLiveEditor, Word-like editing (EMarkdownEditMode::Rendered).
//
// The source stays the only truth and every edit a byte replacement; what
// this file adds is how a caret that never sees syntax maps onto the source,
// and what keys do to it.
//
// CARET STOPS. The caret stands only on content boundaries: every code point
// boundary inside a content run, plus one point for a block with no content
// (an empty list item, an empty cell). Two stops with nothing but syntax
// between them on the same line are ONE visual position; the left one is
// kept, so typing after "**bold**" inserts before the closing "**" and
// continues the bold -- what Word does with the formatting of the character
// left of the caret. A caret on a blank line (a new, still empty paragraph)
// is allowed and drawn as an empty line.
//
// TYPED TEXT IS LITERAL. Markdown specials are escaped as they are typed,
// except in code; "- ", "1. ", "# ", "> " typed at the start of a paragraph
// are autoformatted into the block they spell.
//
// DELETION NEVER BREAKS SYNTAX. A deleted range that cuts through a span gets
// the span's markers put back at the cut; a span left with no content goes
// with its markers; an escaped character goes with its backslash.

#include "CMarkdownLiveEditor.h"
#include "imgui.h"

#include <algorithm>
#include <cstring>

namespace
{
	bool IsBlankChar(char c) { return c == ' ' || c == '\t' || c == '\r'; }

	const char *DelimFor(EMarkdownInlineStyle s)
	{
		switch (s)
		{
			case EMarkdownInlineStyle::Strong: return "**";
			case EMarkdownInlineStyle::Emphasis: return "*";
			case EMarkdownInlineStyle::Strikethrough: return "~~";
			case EMarkdownInlineStyle::Code: return "`";
		}
		return "**";
	}

	EMarkdownSpanType SpanTypeFor(EMarkdownInlineStyle s)
	{
		switch (s)
		{
			case EMarkdownInlineStyle::Strong: return EMarkdownSpanType::Strong;
			case EMarkdownInlineStyle::Emphasis: return EMarkdownSpanType::Emphasis;
			case EMarkdownInlineStyle::Strikethrough: return EMarkdownSpanType::Strikethrough;
			case EMarkdownInlineStyle::Code: return EMarkdownSpanType::Code;
		}
		return EMarkdownSpanType::Strong;
	}

	uint32_t StyleFlagFor(EMarkdownInlineStyle s)
	{
		switch (s)
		{
			case EMarkdownInlineStyle::Strong: return MD_STYLE_STRONG;
			case EMarkdownInlineStyle::Emphasis: return MD_STYLE_EMPHASIS;
			case EMarkdownInlineStyle::Strikethrough: return MD_STYLE_STRIKETHROUGH;
			case EMarkdownInlineStyle::Code: return MD_STYLE_CODE;
		}
		return 0;
	}

	// Every Markdown special made literal (for text going into ordinary text).
	std::string EscapeAll(std::string_view s)
	{
		std::string out;
		out.reserve(s.size() + 8);
		for (char c : s)
		{
			if (strchr("\\*_`[]<>~|&#", c) && c != 0) out += '\\';
			out += c;
		}
		return out;
	}

	// Backslash escapes removed (for text going into a code span).
	std::string Unescape(std::string_view s)
	{
		std::string out;
		out.reserve(s.size());
		for (size_t i = 0; i < s.size(); i++)
		{
			if (s[i] == '\\' && i + 1 < s.size() && strchr("\\*_`[]<>~|&#!-+.()=", s[i + 1]) && s[i + 1] != 0) { out += s[++i]; continue; }
			out += s[i];
		}
		return out;
	}
}

// ---- Stops ------------------------------------------------------------------------

void CMarkdownLiveEditor::RebuildStops() const
{
	if (stopsParseCount == doc.GetParseCount()) return;
	stopsParseCount = doc.GetParseCount();
	const std::string &src = doc.GetSource();
	const auto &runs = doc.GetRuns();
	const auto &blocks = doc.GetBlocks();
	contentMask.assign(src.size(), 0);

	std::vector<Stop> raw;
	raw.reserve(src.size() / 2 + 16);
	std::vector<bool> hasContent(blocks.size(), false);
	for (const CMarkdownRun &r : runs)
	{
		if (r.IsSynthesized() || r.beg == UINT32_MAX || r.end > src.size() || r.beg >= r.end) continue;
		if (r.block >= 0 && r.block < (int32_t)blocks.size()) hasContent[r.block] = true;
		for (uint32_t o = r.beg; o < r.end; o++) contentMask[o] = 1;
		if (r.kind == EMarkdownRunKind::Entity)
		{
			// An entity is one character on screen.
			raw.push_back({r.beg, r.block});
			raw.push_back({r.end, r.block});
			continue;
		}
		for (uint32_t o = r.beg; o < r.end; o = doc.NextCodepoint(o)) raw.push_back({o, r.block});
		raw.push_back({r.end, r.block});
	}
	// A block with no content still needs a place for the caret.
	for (int32_t i = 1; i < (int32_t)blocks.size(); i++)
	{
		const CMarkdownBlock &b = blocks[i];
		const bool hasChild = i + 1 < (int32_t)blocks.size() && blocks[i + 1].parent == i;
		const bool candidate = b.IsCell() || b.type == EMarkdownBlockType::HorizontalRule
			|| ((b.IsLeaf() || b.type == EMarkdownBlockType::ListItem) && !hasChild);
		if (!candidate || hasContent[i]) continue;
		uint32_t at = b.IsCell() || b.type == EMarkdownBlockType::HorizontalRule ? b.beg : doc.LineEnd(b.beg);
		raw.push_back({at, i});
	}
	std::stable_sort(raw.begin(), raw.end(), [](const Stop &a, const Stop &b) { return a.offset < b.offset; });

	stops.clear();
	for (const Stop &s : raw)
	{
		if (!stops.empty())
		{
			const Stop &p = stops.back();
			if (s.offset == p.offset) continue;
			// Same visual position: same block, only syntax between, no line break.
			if (s.block == p.block)
			{
				bool apart = false;
				for (uint32_t o = p.offset; o < s.offset && o < src.size(); o++)
					if (src[o] == '\n' || contentMask[o]) { apart = true; break; }
				if (!apart) continue;
			}
		}
		stops.push_back(s);
	}
}

bool CMarkdownLiveEditor::IsContentAt(uint32_t offset) const
{
	RebuildStops();
	return offset < contentMask.size() && contentMask[offset];
}

int32_t CMarkdownLiveEditor::StopIndexAtOrBefore(uint32_t offset) const
{
	RebuildStops();
	auto it = std::upper_bound(stops.begin(), stops.end(), offset, [](uint32_t o, const Stop &s) { return o < s.offset; });
	return (int32_t)(it - stops.begin()) - 1;
}

bool CMarkdownLiveEditor::IsGapPosition(uint32_t offset) const
{
	const uint32_t ls = doc.LineStart(offset), le = doc.LineEnd(offset);
	if (le > ls)
	{
		// Only blanks and quote markers on the line.
		for (uint32_t o = ls; o < le; o++)
			if (!IsBlankChar(doc.GetSource()[o]) && doc.GetSource()[o] != '>') return false;
	}
	// The line after a hard break ("\\" at a line's end) is the same block's.
	if (ls >= 2)
	{
		const std::string &src = doc.GetSource();
		uint32_t k = ls - 1, n = 0;
		while (k > 0 && src[k - 1] == '\\') { k--; n++; }
		if (n % 2 == 1) return false;
	}
	// The empty line after a document's final newline is not a paragraph of
	// its own unless a blank line comes before it (Enter at the end made it).
	if (offset == doc.Size() && le == ls && ls > 0)
	{
		const uint32_t ps = doc.LineStart(ls - 1);
		bool prevBlank = true;
		for (uint32_t o = ps; o < ls - 1; o++) if (!IsBlankChar(doc.GetSource()[o]) && doc.GetSource()[o] != '>') prevBlank = false;
		if (!prevBlank) return false;
	}
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(offset);
	return !(i >= 0 && stops[i].offset == offset);
}

uint32_t CMarkdownLiveEditor::Snap(uint32_t offset) const
{
	RebuildStops();
	const uint32_t size = doc.Size();
	if (offset > size) offset = size;
	if (stops.empty()) return offset;
	const int32_t i = StopIndexAtOrBefore(offset);
	if (i >= 0 && stops[i].offset == offset) return offset;
	if (IsGapPosition(offset)) return offset;
	const std::string &src = doc.GetSource();
	// After blanks typed at a line's end (not content: Markdown drops them).
	if (i >= 0 && offset > 0 && IsBlankChar(src[offset - 1]) && !contentMask[offset - 1])
	{
		bool tail = true;
		for (uint32_t o = stops[i].offset; o < offset && tail; o++) if (src[o] == '\n' || contentMask[o]) tail = false;
		for (uint32_t o = offset; o < size && src[o] != '\n' && tail; o++) if (contentMask[o]) tail = false;
		if (tail) return offset;
	}
	auto clean = [&](uint32_t a, uint32_t b, bool allowContent) {
		for (uint32_t o = a; o < b && o < size; o++)
		{
			if (src[o] == '\n') return false;
			if (!allowContent && contentMask[o]) return false;
		}
		return true;
	};
	if (i >= 0 && clean(stops[i].offset, offset, false)) return stops[i].offset;
	if (i + 1 < (int32_t)stops.size() && clean(offset, stops[i + 1].offset, true)) return stops[i + 1].offset;
	if (i < 0) return stops[0].offset;
	if (i + 1 >= (int32_t)stops.size()) return stops[i].offset;
	return (offset - stops[i].offset <= stops[i + 1].offset - offset) ? stops[i].offset : stops[i + 1].offset;
}

int32_t CMarkdownLiveEditor::UnitBlockOf(uint32_t offset) const
{
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(offset);
	if (i >= 0 && stops[i].offset == offset) return stops[i].block;
	return doc.FindBlockAt(offset);
}

uint32_t CMarkdownLiveEditor::BlockFirstStop(int32_t block) const
{
	RebuildStops();
	for (const Stop &s : stops) if (s.block == block) return s.offset;
	return UINT32_MAX;
}

bool CMarkdownLiveEditor::InCodeAt(uint32_t offset) const
{
	const int32_t b = doc.FindBlockAt(offset);
	if (b > 0 && (doc.GetBlocks()[b].type == EMarkdownBlockType::Code || doc.GetBlocks()[b].type == EMarkdownBlockType::Html)) return true;
	for (const CMarkdownSpan &sp : doc.GetSpans())
		if (sp.type == EMarkdownSpanType::Code && offset >= sp.contentBeg && offset <= sp.contentEnd && sp.contentEnd > sp.contentBeg) return true;
	return false;
}

// ---- Typing --------------------------------------------------------------------------

uint32_t CMarkdownLiveEditor::DelimiterChainEnd(uint32_t at, uint8_t *styles) const
{
	// Bold, italic and strikethrough spans whose content ends exactly here,
	// nested closers back to back ("***x***"): the end of the outermost one.
	*styles = 0;
	uint32_t end = at;
	bool grew = true;
	while (grew)
	{
		grew = false;
		for (const CMarkdownSpan &sp : doc.GetSpans())
		{
			if (sp.contentEnd != end || sp.end <= end || sp.contentEnd <= sp.contentBeg) continue;
			uint8_t bit = 0;
			if (sp.type == EMarkdownSpanType::Strong) bit = 1u << (int)EMarkdownInlineStyle::Strong;
			else if (sp.type == EMarkdownSpanType::Emphasis) bit = 1u << (int)EMarkdownInlineStyle::Emphasis;
			else if (sp.type == EMarkdownSpanType::Strikethrough) bit = 1u << (int)EMarkdownInlineStyle::Strikethrough;
			else continue;
			*styles |= bit;
			end = sp.end;
			grew = true;
		}
	}
	return end;
}

std::string CMarkdownLiveEditor::EscapeTyped(std::string_view text, uint32_t at) const
{
	if (InCodeAt(at)) return std::string(text);
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(at);
	const bool exact = i >= 0 && stops[i].offset == at;
	const bool blockStart = !exact || i == 0 || stops[i - 1].block != stops[i].block;
	// What the block already holds before the caret (for "1." at its start).
	bool onlyDigitsBefore = false;
	if (blockStart) onlyDigitsBefore = false;
	else
	{
		uint32_t first = at;
		for (int32_t k = i; k >= 0 && stops[k].block == stops[i].block; k--) first = stops[k].offset;
		onlyDigitsBefore = first < at;
		for (uint32_t o = first; o < at; o++) if (!(doc.GetSource()[o] >= '0' && doc.GetSource()[o] <= '9')) { onlyDigitsBefore = false; break; }
	}
	std::string out;
	for (size_t k = 0; k < text.size(); k++)
	{
		const char c = text[k];
		const bool first = k == 0;
		if (c != 0 && strchr("\\*_`[]<~|&", c)) out += '\\';
		else if (first && blockStart && c != 0 && strchr("#>+-=", c)) out += '\\';
		else if (first && onlyDigitsBefore && (c == '.' || c == ')')) out += '\\';
		out += c;
	}
	return out;
}

void CMarkdownLiveEditor::WordInsertText(std::string_view text)
{
	if (readOnly || text.empty()) return;
	if ((pendingOn || pendingOff) && caret != pendingAt) pendingOn = pendingOff = 0;
	const bool compound = HasSelection() || pendingOn || pendingOff;
	if (compound) BeginCompound();
	if (HasSelection())
	{
		const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
		const uint32_t c = DeleteRangeSafe(a, b);
		caret = anchor = c;
	}
	uint32_t at = caret;
	// An empty list item or heading whose marker has no space yet ("-", "#").
	std::string lead;
	{
		const uint32_t ls = doc.LineStart(at), le = doc.LineEnd(at);
		if (at == le && le > ls)
		{
			const std::string line = doc.GetSource().substr(ls, le - ls);
			size_t k = 0;
			while (k < line.size() && (IsBlankChar(line[k]) || line[k] == '>')) k++;
			const std::string rest = line.substr(k);
			if (rest == "-" || rest == "*" || rest == "+" || (!rest.empty() && rest.find_first_not_of('#') == std::string::npos && rest.size() <= 6))
				lead = " ";
		}
	}
	// A new paragraph on a blank line: blank lines around it, so it does not
	// join the block above or below (a lazy continuation line).
	std::string gapTail;
	if (lead.empty() && IsGapPosition(at) && !InCodeAt(at))
	{
		const std::string &src = doc.GetSource();
		const uint32_t ls = doc.LineStart(at), le = doc.LineEnd(at);
		bool quoted = false;
		for (uint32_t o = ls; o < le; o++) if (src[o] == '>') quoted = true;
		auto blank = [&](uint32_t from) {
			const uint32_t e = doc.LineEnd(from);
			for (uint32_t o = from; o < e; o++) if (!IsBlankChar(src[o])) return false;
			return true;
		};
		if (!quoted)
		{
			if (ls > 0 && !blank(doc.LineStart(ls - 1))) lead = "\n";
			if (le < src.size() && le + 1 < src.size() && !blank(le + 1)) gapTail = "\n";
		}
	}
	std::string body = EscapeTyped(text, at);
	std::string open, close;
	uint32_t insertAt = at;
	// Emphasis cannot open before or close after a blank ("**a **" is not
	// bold). Blanks typed at the end of a styled span go past its closers and
	// the style carries on for the next text; blanks typed with a pending style
	// stay outside the markers the same way.
	uint8_t rearm = 0;
	std::string blanksBefore, blanksAfter, chainClosers;
	const bool code = InCodeAt(at) || (pendingOn & (1u << (int)EMarkdownInlineStyle::Code)) != 0;
	if (!code && lead.empty())
	{
		size_t l = 0, r = body.size();
		while (l < r && IsBlankChar(body[l])) l++;
		while (r > l && IsBlankChar(body[r - 1])) r--;
		if (pendingOn)
		{
			blanksBefore = body.substr(0, l);
			blanksAfter = body.substr(r);
			body = body.substr(l, r - l);
			if (body.empty() || !blanksAfter.empty()) rearm = pendingOn;
			if (body.empty()) pendingOn = 0;   // nothing to wrap yet
		}
		else if (!body.empty() && IsBlankChar(body.back()))
		{
			r = body.size();
			while (r > 0 && IsBlankChar(body[r - 1])) r--;
			uint8_t chain = 0;
			const uint32_t chainEnd = DelimiterChainEnd(at, &chain);
			if (chainEnd != at)
			{
				blanksAfter = body.substr(r);
				body = body.substr(0, r);
				chainClosers = doc.GetSource().substr(at, chainEnd - at);
				rearm = chain;
			}
		}
	}
	for (int s = 0; s < 4; s++)
	{
		const uint8_t bit = (uint8_t)(1u << s);
		const EMarkdownInlineStyle style = (EMarkdownInlineStyle)s;
		if (pendingOn & bit)
		{
			open += DelimFor(style);
			close = std::string(DelimFor(style)) + close;
		}
		else if (pendingOff & bit)
		{
			const int32_t sp = InnermostSpanOfType(SpanTypeFor(style), at, at);
			if (sp >= 0)
			{
				const CMarkdownSpan &span = doc.GetSpans()[sp];
				const std::string opener = doc.GetSource().substr(span.beg, span.contentBeg - span.beg);
				const std::string closer = doc.GetSource().substr(span.contentEnd, span.end - span.contentEnd);
				if (at >= span.contentEnd) insertAt = std::max(insertAt, span.end);   // past the closer, nothing to reopen
				else { open = closer + open; close = close + opener; }
			}
		}
	}
	// Text typed into a pending code span is literal.
	if (pendingOn & (1u << (int)EMarkdownInlineStyle::Code)) body = std::string(text);
	std::string full = lead + blanksBefore + open + body + close + gapTail;
	uint32_t replaceEnd = insertAt;
	uint32_t caretAfter = insertAt + (uint32_t)(lead.size() + blanksBefore.size() + open.size() + body.size());
	if (!blanksAfter.empty())
	{
		// The blanks after every closer, the caret after the blanks.
		full = lead + blanksBefore + open + body + close + chainClosers + blanksAfter + gapTail;
		replaceEnd = insertAt + (uint32_t)chainClosers.size();
		caretAfter = insertAt + (uint32_t)(full.size() - gapTail.size());
	}
	pendingOn = pendingOff = 0;
	if (compound)
	{
		doc.Replace(insertAt, replaceEnd, full);
		caret = anchor = caretAfter;
		EndCompound();
		preferredX = -1;
		scrollToCaret = true;
		Changed();
	}
	else
	{
		ApplyReplace(insertAt, replaceEnd, full, EEditKind::Insert, caretAfter);
	}
	if (text.back() == ' ' && TryAutoformat()) rearm = 0;
	if (rearm != 0)
	{
		pendingOn = rearm;
		pendingAt = caret;
	}
}

bool CMarkdownLiveEditor::TryAutoformat()
{
	const std::string &src = doc.GetSource();
	const int32_t blk = doc.FindBlockAt(caret > 0 ? caret - 1 : caret);
	if (blk <= 0) return false;
	const CMarkdownBlock &b = doc.GetBlocks()[blk];
	const bool inItem = b.type == EMarkdownBlockType::ListItem || (b.parent > 0 && doc.GetBlocks()[b.parent].type == EMarkdownBlockType::ListItem);
	if (b.type != EMarkdownBlockType::Paragraph && !inItem) return false;
	const uint32_t ls = doc.LineStart(caret), le = doc.LineEnd(caret);
	if (caret != le) return false;
	uint32_t ps = ls;
	while (ps < le && (IsBlankChar(src[ps]) || src[ps] == '>')) ps++;
	if (doc.LineStart(b.beg) != ls && !inItem) return false;   // only on the paragraph's first line
	std::string typed = Unescape(src.substr(ps, caret - ps));
	std::string token;
	if (inItem)
	{
		// "[ ] " / "[x] " at a list item's start: a task item.
		uint32_t cs = ps;
		while (cs < le && src[cs] != ' ') cs++;   // past the list marker
		while (cs < le && src[cs] == ' ') cs++;
		const std::string afterMarker = Unescape(src.substr(cs, caret - cs));
		if (afterMarker == "[ ] " || afterMarker == "[] " || afterMarker == "[x] ")
		{
			BeginCompound();
			doc.Replace(cs, caret, afterMarker == "[x] " ? "[x] " : "[ ] ");
			caret = anchor = doc.LineEnd(cs);
			EndCompound();
			Changed();
			return true;
		}
		return false;
	}
	if (typed == "- " || typed == "* " || typed == "+ ") token = "- ";
	else if (typed == "> ") token = "> ";
	else if (typed.size() >= 2 && typed.size() <= 7 && typed.back() == ' ' && typed.find_first_not_of('#') == typed.size() - 1) token = typed;
	else if (typed.size() >= 3 && (typed[typed.size() - 2] == '.' || typed[typed.size() - 2] == ')') && typed.back() == ' ')
	{
		bool digits = true;
		for (size_t k = 0; k + 2 < typed.size(); k++) if (typed[k] < '0' || typed[k] > '9') digits = false;
		if (digits && typed.size() <= 11) token = typed;
	}
	else if (typed == "[ ] " || typed == "[] ") token = "- [ ] ";
	if (token.empty()) return false;
	// Only when that is all the paragraph holds.
	const uint32_t bEnd = doc.GetBlocks()[blk].end;
	if (bEnd > caret) return false;
	BeginCompound();
	doc.Replace(ps, caret, token);
	caret = anchor = ps + (uint32_t)token.size();
	EndCompound();
	Changed();
	return true;
}

// ---- Deletion --------------------------------------------------------------------------

uint32_t CMarkdownLiveEditor::DeleteRangeSafe(uint32_t a, uint32_t b)
{
	RebuildStops();
	const std::string &src = doc.GetSource();
	const auto &spans = doc.GetSpans();
	const auto &runs = doc.GetRuns();
	if (a > b) std::swap(a, b);

	// Between table cells only the cells' text can go.
	{
		const int32_t ba = doc.FindBlockAt(a), bb = doc.FindBlockAt(b);
		const bool ta = ba > 0 && doc.TableOf(ba) > 0, tb = bb > 0 && doc.TableOf(bb) > 0;
		if ((ta || tb) && ba != bb)
		{
			if (ta && doc.GetBlocks()[ba].IsCell()) b = std::min(b, doc.GetBlocks()[ba].end);
			else return a;
		}
	}

	// Deleting the end of a styled span's text must not leave it ending in a
	// blank ("**bold **" is not bold): the blanks move past its closers and
	// the style carries on for the next text.
	{
		uint8_t chain = 0;
		const uint32_t chainEnd = DelimiterChainEnd(b, &chain);
		uint32_t innerBeg = 0;
		for (const CMarkdownSpan &sp : spans) if (sp.contentEnd == b && sp.contentEnd > sp.contentBeg) innerBeg = std::max(innerBeg, sp.contentBeg);
		uint32_t k = a;
		while (k > innerBeg && IsBlankChar(src[k - 1])) k--;
		if (chainEnd != b && a < b && k < a && k > innerBeg && innerBeg <= a)
		{
			const std::string blanks = src.substr(k, a - k);
			const std::string closersMoved = src.substr(b, chainEnd - b);
			doc.Replace(k, chainEnd, closersMoved + blanks);
			pendingOn = chain;
			pendingOff = 0;
			pendingAt = k + (uint32_t)(closersMoved.size() + blanks.size());
			return pendingAt;
		}
	}

	bool changed = true;
	while (changed)
	{
		changed = false;
		for (const CMarkdownSpan &sp : spans)
		{
			// A span the deletion would empty goes with its markers.
			if (sp.contentEnd > sp.contentBeg && sp.contentBeg >= a && sp.contentEnd <= b && (sp.beg < a || sp.end > b))
			{
				a = std::min(a, sp.beg);
				b = std::max(b, sp.end);
				changed = true;
			}
		}
		// An escaped character goes with its backslash.
		if (a > 0 && a < src.size() && src[a - 1] == '\\' && !contentMask[a - 1] && contentMask[a]) { a--; changed = true; }
		// An entity is one character.
		for (const CMarkdownRun &r : runs)
		{
			if (r.kind != EMarkdownRunKind::Entity || r.IsSynthesized()) continue;
			if (a > r.beg && a < r.end) { a = r.beg; changed = true; }
			if (b > r.beg && b < r.end) { b = r.end; changed = true; }
		}
	}

	// Spans the range cuts through get their markers back at the cut:
	// closers of spans that began before it (innermost first), then openers of
	// spans that go on after it (outermost first).
	std::string closers, openers;
	// A span that starts before the range and ends inside it lost its closer.
	for (int32_t i = (int32_t)spans.size() - 1; i >= 0; i--)
	{
		const CMarkdownSpan &sp = spans[i];
		if (sp.contentBeg < a && sp.beg < a && sp.end > a && sp.end <= b)
			closers += src.substr(sp.contentEnd, sp.end - sp.contentEnd);
	}
	// A span that starts inside the range and ends after it lost its opener.
	for (const CMarkdownSpan &sp : spans)
	{
		if (sp.beg >= a && sp.beg < b && sp.end > b && sp.contentEnd > b)
			openers += src.substr(sp.beg, sp.contentBeg - sp.beg);
	}
	doc.Replace(a, b, closers + openers);
	return a + (uint32_t)closers.size();
}

void CMarkdownLiveEditor::WordBackspace()
{
	if (readOnly) return;
	if (HasSelection())
	{
		BeginCompound();
		caret = anchor = DeleteRangeSafe(std::min(anchor, caret), std::max(anchor, caret));
		EndCompound();
		caret = anchor = Snap(caret);
		Changed();
		return;
	}
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(caret);
	const bool exact = i >= 0 && stops[i].offset == caret;
	if (!exact)
	{
		// A caret on a blank line (an empty paragraph): the line goes.
		if (i < 0) return;
		BeginCompound();
		doc.Replace(stops[i].offset, caret, "");
		caret = anchor = stops[i].offset;
		EndCompound();
		caret = anchor = Snap(caret);
		Changed();
		return;
	}
	const bool blockStart = i == 0 || stops[i - 1].block != stops[i].block;
	if (!blockStart)
	{
		const uint32_t from = doc.PrevCodepoint(caret);
		if (IsContentAt(from))
		{
			// One character, merged into the Backspace burst when it is one.
			uint32_t a = from, b = caret;
			BeginCompound();
			const uint32_t c = DeleteRangeSafe(a, b);
			caret = anchor = c;
			EndCompound();
			caret = anchor = Snap(caret);
			preferredX = -1;
			scrollToCaret = true;
			Changed();
			return;
		}
	}

	// At the start of a block.
	const int32_t blk = stops[i].block;
	const auto &blocks = doc.GetBlocks();
	if (blk <= 0) return;
	const CMarkdownBlock &b = blocks[blk];
	int32_t item = -1;
	if (b.type == EMarkdownBlockType::ListItem) item = blk;
	else if (b.parent > 0 && blocks[b.parent].type == EMarkdownBlockType::ListItem && doc.LineStart(b.beg) == doc.LineStart(blocks[b.parent].beg)) item = b.parent;
	if (item > 0)
	{
		// The bullet goes; the text stays, as a paragraph of its own.
		const EMarkdownListKind kind = blocks[item].isTask ? EMarkdownListKind::Task
			: (blocks[blocks[item].parent].type == EMarkdownBlockType::OrderedList ? EMarkdownListKind::Numbered : EMarkdownListKind::Bullet);
		ToggleList(kind);
		return;
	}
	if (b.type == EMarkdownBlockType::Heading)
	{
		uint32_t mb = 0, me = 0;
		if (doc.GetAtxMarkerRange(blk, &mb, &me) && mb != me) { SetHeadingLevel(0); caret = anchor = Snap(caret); return; }
	}
	if (b.IsCell()) return;
	for (int32_t p = b.parent; p > 0; p = blocks[p].parent)
		if (blocks[p].type == EMarkdownBlockType::Quote) { ToggleQuote(); return; }
	if (i == 0) return;
	const Stop prev = stops[i - 1];
	const CMarkdownBlock &pb = blocks[prev.block];
	if (pb.type == EMarkdownBlockType::HorizontalRule)
	{
		BeginCompound();
		const uint32_t ls = doc.LineStart(pb.beg);
		doc.Replace(ls, std::min(doc.Size(), pb.end + 1), "");
		caret = anchor = Snap(ls);
		EndCompound();
		Changed();
		return;
	}
	if (pb.type == EMarkdownBlockType::Code || pb.IsCell())
	{
		SetCaret(prev.offset);
		return;
	}
	// Merge into the block before: only the break between them goes, so
	// trailing blanks and closing markers of the block before stay.
	uint32_t from = prev.offset;
	while (from < caret && doc.GetSource()[from] != '\n') from++;
	BeginCompound();
	caret = anchor = DeleteRangeSafe(from, caret);
	EndCompound();
	caret = anchor = Snap(caret);
	Changed();
}

void CMarkdownLiveEditor::WordDeleteForward()
{
	if (readOnly) return;
	if (HasSelection()) { WordBackspace(); return; }
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(caret);
	const bool exact = i >= 0 && stops[i].offset == caret;
	if (!exact)
	{
		if (i + 1 >= (int32_t)stops.size()) return;
		BeginCompound();
		doc.Replace(caret, stops[i + 1].offset, "");
		EndCompound();
		caret = anchor = Snap(caret);
		Changed();
		return;
	}
	const bool blockEnd = i + 1 >= (int32_t)stops.size() || stops[i + 1].block != stops[i].block;
	if (!blockEnd)
	{
		uint32_t k = caret;
		while (k < doc.Size() && !IsContentAt(k)) k++;
		if (k >= doc.Size()) return;
		BeginCompound();
		DeleteRangeSafe(k, doc.NextCodepoint(k));
		EndCompound();
		caret = anchor = Snap(caret);
		Changed();
		return;
	}
	if (i + 1 >= (int32_t)stops.size()) return;
	const Stop next = stops[i + 1];
	const auto &blocks = doc.GetBlocks();
	const CMarkdownBlock &cb = blocks[stops[i].block];
	const CMarkdownBlock &nb = blocks[next.block];
	if (cb.IsCell() || nb.IsCell() || nb.type == EMarkdownBlockType::Code || cb.type == EMarkdownBlockType::Code) return;
	BeginCompound();
	if (nb.type == EMarkdownBlockType::HorizontalRule)
	{
		const uint32_t ls = doc.LineStart(nb.beg);
		doc.Replace(ls, std::min(doc.Size(), nb.end + 1), "");
	}
	else
	{
		uint32_t from = caret;
		while (from < next.offset && doc.GetSource()[from] != '\n') from++;
		DeleteRangeSafe(from, next.offset);
	}
	EndCompound();
	caret = anchor = Snap(caret);
	Changed();
}

// ---- Enter -------------------------------------------------------------------------------

std::string CMarkdownLiveEditor::ContainerPrefixAt(uint32_t offset, bool forParagraph) const
{
	const std::string &src = doc.GetSource();
	const uint32_t ls = doc.LineStart(offset), le = doc.LineEnd(offset);
	uint32_t k = ls;
	while (k < le && (IsBlankChar(src[k]) || src[k] == '>')) k++;
	std::string prefix = src.substr(ls, k - ls);
	if (!forParagraph)
	{
		// A line break continues at the text's column: a list marker (and a
		// task box) on this line becomes spaces.
		uint32_t m = k;
		if (m < le && (src[m] == '-' || src[m] == '*' || src[m] == '+')) m++;
		else
		{
			while (m < le && src[m] >= '0' && src[m] <= '9') m++;
			if (m > k && m < le && (src[m] == '.' || src[m] == ')')) m++;
			else m = k;
		}
		if (m > k && m < le && src[m] == ' ')
		{
			while (m < le && src[m] == ' ') m++;
			if (m + 3 < le && src[m] == '[' && src[m + 2] == ']' && src[m + 3] == ' ') m += 4;
			prefix += std::string(m - k, ' ');
		}
	}
	return prefix;
}

void CMarkdownLiveEditor::SplitAt(uint32_t at, const std::string &separator, const std::string &)
{
	const auto &spans = doc.GetSpans();
	const std::string &src = doc.GetSource();
	// A span starting exactly here moves to the second part whole; one ending
	// exactly here stays in the first.
	bool moved = true;
	while (moved)
	{
		moved = false;
		for (const CMarkdownSpan &sp : spans)
		{
			if (sp.contentBeg == at && sp.beg < at && sp.contentEnd > sp.contentBeg) { at = sp.beg; moved = true; }
			else if (sp.contentEnd == at && sp.end > at && sp.contentEnd > sp.contentBeg) { at = sp.end; moved = true; }
		}
	}
	std::string closers, openers;
	for (int32_t i = (int32_t)spans.size() - 1; i >= 0; i--)
		if (spans[i].contentBeg < at && at < spans[i].contentEnd) closers += src.substr(spans[i].contentEnd, spans[i].end - spans[i].contentEnd);
	for (const CMarkdownSpan &sp : spans)
		if (sp.contentBeg < at && at < sp.contentEnd) openers += src.substr(sp.beg, sp.contentBeg - sp.beg);
	doc.Replace(at, at, closers + separator + openers);
	caret = anchor = at + (uint32_t)(closers.size() + separator.size());
}

bool CMarkdownLiveEditor::AtBlockTextEnd(uint32_t offset) const
{
	RebuildStops();
	const int32_t i = StopIndexAtOrBefore(offset);
	if (i < 0 || stops[i].offset != offset) return true;
	return i + 1 >= (int32_t)stops.size() || stops[i + 1].block != stops[i].block;
}

void CMarkdownLiveEditor::WordNewline(bool lineBreak)
{
	if (readOnly) return;
	BeginCompound();
	if (HasSelection()) caret = anchor = DeleteRangeSafe(std::min(anchor, caret), std::max(anchor, caret));
	// At the end of styled text the new line goes on in that style, as in Word.
	uint8_t carry = 0;
	DelimiterChainEnd(caret, &carry);
	const auto &blocks = doc.GetBlocks();
	const std::string &src = doc.GetSource();
	if (InCodeAt(caret) && doc.FindBlockAt(caret) > 0 && blocks[doc.FindBlockAt(caret)].type == EMarkdownBlockType::Code)
	{
		doc.Replace(caret, caret, "\n");
		caret = anchor = caret + 1;
	}
	else if (lineBreak && !AtBlockTextEnd(caret))
	{
		const std::string prefix = ContainerPrefixAt(caret, false);
		SplitAt(caret, "\\\n" + prefix, "");
	}
	else
	{
		// Enter, or Shift+Enter with nothing after it in the block: Markdown
		// has no empty last line ("a\\" before a blank line is a literal
		// backslash), so it starts the next paragraph or item.
		RebuildStops();
		const int32_t i = StopIndexAtOrBefore(caret);
		const bool exact = i >= 0 && stops[i].offset == caret;
		const int32_t blk = exact ? stops[i].block : -1;
		int32_t item = -1;
		if (blk > 0)
		{
			if (blocks[blk].type == EMarkdownBlockType::ListItem) item = blk;
			else if (blocks[blk].parent > 0 && blocks[blocks[blk].parent].type == EMarkdownBlockType::ListItem) item = blocks[blk].parent;
		}
		const bool blockStart = exact && (i == 0 || stops[i - 1].block != stops[i].block);
		if (item > 0)
		{
			const std::string prefix = doc.ListContinuationPrefix(blocks[item].beg);
			// An item with no text and nothing nested.
			const bool empty = blk == item && !doc.OwnsContentPublic(item) && doc.GetChildren(item).empty();
			if (empty)
			{
				// Enter on an empty item ends the list: its marker line goes.
				const uint32_t ls = doc.LineStart(caret);
				doc.Replace(ls, doc.LineEnd(caret), "");
				caret = anchor = ls;
			}
			else
			{
				const int32_t list = blocks[item].parent;
				const bool tight = list > 0 && blocks[list].listIsTight;
				SplitAt(caret, (tight ? "\n" : "\n\n") + prefix, "");
			}
		}
		else if (blockStart && blk > 0 && !blocks[blk].IsCell())
		{
			// At a block's start: an empty paragraph opens above; the caret
			// stays with the text.
			const uint32_t ls = doc.LineStart(caret);
			const std::string qp = ContainerPrefixAt(caret, true);
			std::string qpTrim = qp;
			while (!qpTrim.empty() && IsBlankChar(qpTrim.back())) qpTrim.pop_back();
			doc.Replace(ls, ls, qp.empty() ? "\n\n" : qp + "\n" + qpTrim + "\n");
			caret = anchor = Snap(caret + (uint32_t)(qp.empty() ? 2 : qp.size() + qpTrim.size() + 2));
		}
		else
		{
			const std::string qp = ContainerPrefixAt(caret, true);
			std::string qpTrim = qp;
			while (!qpTrim.empty() && IsBlankChar(qpTrim.back())) qpTrim.pop_back();
			SplitAt(caret, "\n" + qpTrim + "\n" + qp, "");
		}
		(void)src;
	}
	EndCompound();
	if (carry != 0) { pendingOn = carry; pendingOff = 0; pendingAt = caret; }
	preferredX = -1;
	scrollToCaret = true;
	Changed();
}

// ---- Movement ------------------------------------------------------------------------------

void CMarkdownLiveEditor::WordMoveHorizontal(int direction, bool extend)
{
	RebuildStops();
	if (!extend && HasSelection())
	{
		const uint32_t t = direction < 0 ? std::min(anchor, caret) : std::max(anchor, caret);
		SetSelection(t, t);
		preferredX = -1;
		scrollToCaret = true;
		return;
	}
	if (stops.empty()) return;
	const int32_t i = StopIndexAtOrBefore(caret);
	const bool exact = i >= 0 && stops[i].offset == caret;
	int32_t t = direction > 0 ? i + 1 : (exact ? i - 1 : i);
	if (t < 0 || t >= (int32_t)stops.size()) return;
	const uint32_t o = stops[t].offset;
	SetSelection(extend ? anchor : o, o);
	preferredX = -1;
	scrollToCaret = true;
}

// ---- Inline styles --------------------------------------------------------------------------

int32_t CMarkdownLiveEditor::InnermostSpanOfType(EMarkdownSpanType type, uint32_t a, uint32_t b) const
{
	int32_t best = -1;
	const auto &spans = doc.GetSpans();
	for (int32_t i = 0; i < (int32_t)spans.size(); i++)
		if (spans[i].type == type && spans[i].contentBeg <= a && b <= spans[i].contentEnd && spans[i].contentEnd > spans[i].contentBeg) best = i;
	return best;
}

bool CMarkdownLiveEditor::IsInlineStyleActive(EMarkdownInlineStyle style) const
{
	const uint8_t bit = (uint8_t)(1u << (int)style);
	if (pendingOn & bit) return true;
	if (pendingOff & bit) return false;
	uint32_t probe = caret;
	if (HasSelection()) probe = std::min(anchor, caret);
	RebuildStops();
	uint32_t k = UINT32_MAX;
	if (HasSelection()) { k = probe; while (k < doc.Size() && !IsContentAt(k)) k++; }
	else if (probe > 0 && IsContentAt(doc.PrevCodepoint(probe))) k = doc.PrevCodepoint(probe);
	else { k = probe; while (k < doc.Size() && !IsContentAt(k) && doc.GetSource()[k] != '\n') k++; }
	if (k >= doc.Size() || !IsContentAt(k)) return false;
	const int32_t r = doc.FindRunAt(k);
	return r >= 0 && (doc.GetRuns()[r].styles & StyleFlagFor(style)) != 0;
}

bool CMarkdownLiveEditor::ToggleInlineInBlock(EMarkdownSpanType type, const char *delim, uint32_t a, uint32_t b, uint32_t *newA, uint32_t *newB)
{
	const std::string &src = doc.GetSource();
	const int32_t inside = InnermostSpanOfType(type, a, b);
	if (inside >= 0)
	{
		// Take the style off [a, b): the span splits around it.
		const CMarkdownSpan sp = doc.GetSpans()[inside];
		const std::string open = src.substr(sp.beg, sp.contentBeg - sp.beg);
		const std::string close = src.substr(sp.contentEnd, sp.end - sp.contentEnd);
		const std::string left = src.substr(sp.contentBeg, a - sp.contentBeg);
		std::string mid = src.substr(a, b - a);
		const std::string right = src.substr(b, sp.contentEnd - b);
		if (type == EMarkdownSpanType::Code) mid = EscapeAll(mid);
		const std::string before = left.empty() ? "" : open + left + close;
		const std::string text = before + mid + (right.empty() ? "" : open + right + close);
		doc.Replace(sp.beg, sp.end, text);
		*newA = sp.beg + (uint32_t)before.size();
		*newB = *newA + (uint32_t)mid.size();
		return true;
	}
	// Put it on: same-type spans touching the range merge into one.
	const auto &spans = doc.GetSpans();
	bool grew = true;
	while (grew)
	{
		grew = false;
		for (const CMarkdownSpan &sp : spans)
			if (sp.type == type && sp.contentBeg < b && sp.contentEnd > a && (sp.beg < a || sp.end > b)) { a = std::min(a, sp.beg); b = std::max(b, sp.end); grew = true; }
	}
	std::vector<std::pair<uint32_t, uint32_t>> drop;
	for (const CMarkdownSpan &sp : spans)
		if (sp.type == type && sp.beg >= a && sp.end <= b)
		{
			drop.push_back({sp.beg, sp.contentBeg});
			drop.push_back({sp.contentEnd, sp.end});
		}
	std::string inner;
	for (uint32_t o = a; o < b; o++)
	{
		bool skip = false;
		for (const auto &d : drop) if (o >= d.first && o < d.second) { skip = true; break; }
		if (!skip) inner += src[o];
	}
	if (type == EMarkdownSpanType::Code) inner = Unescape(inner);
	// Emphasis cannot open or close on a blank: keep blanks outside.
	size_t l = 0, r = inner.size();
	while (l < r && IsBlankChar(inner[l])) l++;
	while (r > l && IsBlankChar(inner[r - 1])) r--;
	if (l == r) return false;
	const std::string text = inner.substr(0, l) + delim + inner.substr(l, r - l) + delim + inner.substr(r);
	doc.Replace(a, b, text);
	*newA = a + (uint32_t)(l + strlen(delim));
	*newB = *newA + (uint32_t)(r - l);
	return true;
}

std::vector<int32_t> CMarkdownLiveEditor::SelectedLeafBlocks() const
{
	RebuildStops();
	std::vector<int32_t> out;
	const uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	for (const Stop &s : stops)
	{
		if (s.offset < a || s.offset > b) continue;
		if (out.empty() || out.back() != s.block) out.push_back(s.block);
	}
	if (out.empty())
	{
		const int32_t blk = UnitBlockOf(caret);
		if (blk > 0) out.push_back(blk);
	}
	// A selection ending at a block's first stop does not take that block.
	if (out.size() > 1 && a != b && BlockFirstStop(out.back()) == b) out.pop_back();
	return out;
}

void CMarkdownLiveEditor::ToggleInlineStyle(EMarkdownInlineStyle style)
{
	if (readOnly) return;
	const char *delim = DelimFor(style);
	const EMarkdownSpanType type = SpanTypeFor(style);
	if (!HasSelection())
	{
		if (editMode != EMarkdownEditMode::Rendered)
		{
			// Source-visible modes: a pair of markers with the caret between.
			BeginCompound();
			doc.Replace(caret, caret, std::string(delim) + delim);
			caret = anchor = caret + (uint32_t)strlen(delim);
			EndCompound();
			Changed();
			return;
		}
		const uint8_t bit = (uint8_t)(1u << (int)style);
		const bool active = IsInlineStyleActive(style);
		pendingOn &= (uint8_t)~bit;
		pendingOff &= (uint8_t)~bit;
		const bool base = IsInlineStyleActive(style);
		if (active == base) { if (base) pendingOff |= bit; else pendingOn |= bit; }
		pendingAt = caret;
		return;
	}
	uint32_t a = std::min(anchor, caret), b = std::max(anchor, caret);
	std::vector<int32_t> blocksSel = SelectedLeafBlocks();
	// All of it in the style already: take it off; else put it on everywhere.
	bool allOn = true;
	struct Seg { uint32_t a, b; };
	std::vector<Seg> segs;
	RebuildStops();
	for (int32_t blk : blocksSel)
	{
		uint32_t sa = UINT32_MAX, sb = 0;
		for (const Stop &s : stops) if (s.block == blk) { sa = std::min(sa, s.offset); sb = std::max(sb, s.offset); }
		if (sa == UINT32_MAX) continue;
		const uint32_t x = std::max(a, sa), y = std::min(b, sb);
		if (x >= y) continue;
		segs.push_back({x, y});
		if (InnermostSpanOfType(type, x, y) < 0) allOn = false;
	}
	if (segs.empty()) return;
	// From the last segment to the first, so earlier offsets stay valid; the
	// new selection runs from the first segment's start to the last one's end,
	// moved by whatever the earlier segments' edits added.
	BeginCompound();
	uint32_t firstA = segs.front().a, lastB = segs.back().b;
	uint32_t sizeAfterLast = doc.Size();
	for (int32_t k = (int32_t)segs.size() - 1; k >= 0; k--)
	{
		const Seg sg = segs[k];
		uint32_t na = sg.a, nb = sg.b;
		if (allOn || InnermostSpanOfType(type, sg.a, sg.b) < 0)
			ToggleInlineInBlock(type, delim, sg.a, sg.b, &na, &nb);
		if (k == (int32_t)segs.size() - 1) { lastB = nb; sizeAfterLast = doc.Size(); }
		if (k == 0) firstA = na;
	}
	lastB = (uint32_t)((int64_t)lastB + (int64_t)doc.Size() - (int64_t)sizeAfterLast);
	anchor = firstA;
	caret = std::max(firstA, lastB);
	EndCompound();
	pendingOn = pendingOff = 0;
	Changed();
}

// ---- Block commands ------------------------------------------------------------------------

int CMarkdownLiveEditor::GetHeadingLevelAtCaret() const
{
	const int32_t blk = UnitBlockOf(caret);
	if (blk <= 0) return 0;
	const CMarkdownBlock &b = doc.GetBlocks()[blk];
	if (b.type == EMarkdownBlockType::Heading) return b.level;
	if (b.type == EMarkdownBlockType::Paragraph)
	{
		const int32_t p = b.parent;
		if (p > 0 && doc.GetBlocks()[p].type == EMarkdownBlockType::ListItem) return -1;
		return 0;
	}
	return -1;
}

bool CMarkdownLiveEditor::IsInList(EMarkdownListKind kind) const
{
	const auto &blocks = doc.GetBlocks();
	int32_t blk = UnitBlockOf(caret);
	while (blk > 0 && blocks[blk].type != EMarkdownBlockType::ListItem) blk = blocks[blk].parent;
	if (blk <= 0) return false;
	const CMarkdownBlock &item = blocks[blk];
	const bool ordered = blocks[item.parent].type == EMarkdownBlockType::OrderedList;
	switch (kind)
	{
		case EMarkdownListKind::Task: return item.isTask;
		case EMarkdownListKind::Numbered: return ordered && !item.isTask;
		case EMarkdownListKind::Bullet: return !ordered && !item.isTask;
	}
	return false;
}

bool CMarkdownLiveEditor::ToggleList(EMarkdownListKind kind)
{
	if (readOnly) return false;
	const auto &blocks = doc.GetBlocks();
	const std::string &src = doc.GetSource();
	std::vector<int32_t> sel = SelectedLeafBlocks();
	if (sel.empty()) return false;
	auto itemOf = [&](int32_t blk) {
		if (blk > 0 && blocks[blk].type == EMarkdownBlockType::ListItem) return blk;
		if (blk > 0 && blocks[blk].parent > 0 && blocks[blocks[blk].parent].type == EMarkdownBlockType::ListItem
			&& doc.LineStart(blocks[blk].beg) == doc.LineStart(blocks[blocks[blk].parent].beg)) return blocks[blk].parent;
		return -1;
	};
	// Every selected block already a list item of this kind: they stop being one.
	bool remove = true;
	for (int32_t blk : sel)
	{
		const int32_t it = itemOf(blk);
		if (it < 0) { remove = false; break; }
		const bool ordered = blocks[blocks[it].parent].type == EMarkdownBlockType::OrderedList;
		const bool match = kind == EMarkdownListKind::Task ? blocks[it].isTask
			: (kind == EMarkdownListKind::Numbered ? (ordered && !blocks[it].isTask) : (!ordered && !blocks[it].isTask));
		if (!match) { remove = false; break; }
	}

	// For each block: its container prefix (quote markers, indentation), where
	// its text starts on the first line, and where its own lines end.
	struct Piece { std::string prefix; uint32_t textBeg, end; int32_t blk; };
	std::vector<Piece> pieces;
	for (int32_t blk : sel)
	{
		const int32_t it = itemOf(blk);
		const uint32_t ls = doc.LineStart(blocks[blk].beg), le = doc.LineEnd(blocks[blk].beg);
		uint32_t k = ls;
		while (k < le && (IsBlankChar(src[k]) || src[k] == '>')) k++;
		const std::string prefix = src.substr(ls, k - ls);
		if (it > 0)
		{
			while (k < le && !IsBlankChar(src[k])) k++;          // the list marker
			while (k < le && IsBlankChar(src[k])) k++;
			if (blocks[it].isTask && k + 3 <= le && src[k] == '[' && src[k + 2] == ']')
			{
				k += 3;
				while (k < le && IsBlankChar(src[k])) k++;
			}
		}
		if (blocks[blk].type == EMarkdownBlockType::Heading)
		{
			uint32_t mb = 0, me = 0;
			if (doc.GetAtxMarkerRange(blk, &mb, &me)) k = std::max(k, me);
		}
		uint32_t end = blocks[blk].end;
		if (it > 0 && it == blk)
		{
			// A tight item: its own lines only, not its nested children.
			for (int32_t c : doc.GetChildren(it)) { end = std::min(end, doc.LineStart(blocks[c].beg) > 0 ? doc.LineStart(blocks[c].beg) - 1 : 0); break; }
		}
		pieces.push_back({prefix, k, std::max(end, k), blk});
	}

	const uint32_t regionBeg = doc.LineStart(blocks[sel.front()].beg);
	const uint32_t regionEnd = pieces.back().end;
	const int32_t caretBlk = UnitBlockOf(caret);
	std::string out;
	uint32_t caretOut = UINT32_MAX;
	int number = 1;
	for (size_t n = 0; n < pieces.size(); n++)
	{
		const Piece &pc = pieces[n];
		std::string marker;
		if (!remove)
			marker = kind == EMarkdownListKind::Bullet ? "- " : (kind == EMarkdownListKind::Task ? "- [ ] " : std::to_string(number++) + ". ");
		if (n > 0)
		{
			std::string prefTrim = pc.prefix;
			while (!prefTrim.empty() && IsBlankChar(prefTrim.back())) prefTrim.pop_back();
			out += remove ? "\n" + prefTrim + "\n" : "\n";
		}
		out += pc.prefix + marker;
		if (pc.blk == caretBlk && caret >= pc.textBeg) caretOut = (uint32_t)out.size() + (caret - pc.textBeg);
		// Continuation lines: the container prefix, then the marker's width.
		const std::string text = src.substr(pc.textBeg, pc.end - pc.textBeg);
		const std::string indent = pc.prefix + std::string(marker.size(), ' ');
		for (size_t p = 0; p < text.size(); p++)
		{
			if (text[p] != '\n') { out += text[p]; continue; }
			out += '\n';
			size_t q = p + 1;
			while (q < text.size() && (IsBlankChar(text[q]) || text[q] == '>')) q++;
			out += indent;
			p = q - 1;
		}
	}
	// Blank lines between the result and lines it must not run into.
	std::string pre, post;
	auto lineBlank = [&](uint32_t ls) {
		const uint32_t le = doc.LineEnd(ls);
		for (uint32_t o = ls; o < le; o++) if (!IsBlankChar(src[o]) && src[o] != '>') return false;
		return true;
	};
	if (remove && regionBeg > 0 && !lineBlank(doc.LineStart(regionBeg - 1))) pre = "\n";
	if (remove && regionEnd < src.size() && regionEnd + 1 < src.size() && !lineBlank(regionEnd + 1)) post = "\n";
	BeginCompound();
	doc.Replace(regionBeg, regionEnd, pre + out + post);
	const uint32_t c = caretOut != UINT32_MAX ? regionBeg + (uint32_t)pre.size() + caretOut : regionBeg + (uint32_t)(pre.size() + out.size());
	caret = anchor = Snap(c);
	EndCompound();
	Changed();
	return true;
}

bool CMarkdownLiveEditor::IsInQuote() const
{
	const auto &blocks = doc.GetBlocks();
	for (int32_t b = UnitBlockOf(caret); b > 0; b = blocks[b].parent)
		if (blocks[b].type == EMarkdownBlockType::Quote) return true;
	return false;
}

bool CMarkdownLiveEditor::ToggleQuote()
{
	if (readOnly) return false;
	const auto &blocks = doc.GetBlocks();
	const std::string &src = doc.GetSource();
	std::vector<int32_t> sel = SelectedLeafBlocks();
	if (sel.empty()) return false;
	const bool remove = IsInQuote();
	const uint32_t regionBeg = doc.LineStart(blocks[sel.front()].beg);
	const uint32_t regionEnd = doc.LineEnd(blocks[sel.back()].end > 0 ? blocks[sel.back()].end - 1 : 0);
	const std::string text = src.substr(regionBeg, regionEnd - regionBeg);
	std::string out;
	size_t p = 0;
	int64_t caretShift = 0;
	while (p <= text.size())
	{
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		std::string line = text.substr(p, e - p);
		const uint32_t lineAbs = regionBeg + (uint32_t)p;
		if (remove)
		{
			size_t k = 0;
			while (k < line.size() && k < 3 && line[k] == ' ') k++;
			if (k < line.size() && line[k] == '>')
			{
				size_t cut = k + 1 + (k + 1 < line.size() && line[k + 1] == ' ' ? 1 : 0);
				if (lineAbs < caret) caretShift -= (int64_t)std::min<size_t>(cut, caret - lineAbs);
				line = line.substr(0, k) + line.substr(cut);
			}
		}
		else
		{
			const std::string add = line.empty() ? ">" : "> ";
			if (lineAbs <= caret) caretShift += (int64_t)add.size();
			line = add + line;
		}
		out += line;
		if (e >= text.size()) break;
		out += '\n';
		p = e + 1;
	}
	BeginCompound();
	std::string pre, post;
	if (remove)
	{
		// Out of the quote: blank lines keep it from swallowing the text back.
		if (regionBeg >= 2 && src[regionBeg - 2] != '\n') pre = "\n";
		if (regionEnd + 1 < src.size() && src[regionEnd + 1] != '\n') post = "\n";
	}
	doc.Replace(regionBeg, regionEnd, pre + out + post);
	caret = anchor = Snap((uint32_t)std::max<int64_t>(0, (int64_t)caret + caretShift + (int64_t)pre.size()));
	EndCompound();
	Changed();
	return true;
}

bool CMarkdownLiveEditor::IndentListItem(bool outdent)
{
	if (readOnly) return false;
	const auto &blocks = doc.GetBlocks();
	const std::string &src = doc.GetSource();
	int32_t item = UnitBlockOf(caret);
	while (item > 0 && blocks[item].type != EMarkdownBlockType::ListItem) item = blocks[item].parent;
	if (item <= 0) return false;
	const int32_t list = blocks[item].parent;
	auto markerCol = [&](int32_t it) {
		const uint32_t ls = doc.LineStart(blocks[it].beg);
		uint32_t k = ls;
		while (k < src.size() && (src[k] == ' ' || src[k] == '>')) k++;
		return k - ls;
	};
	auto textCol = [&](int32_t it) {
		const uint32_t ls = doc.LineStart(blocks[it].beg);
		uint32_t k = ls;
		while (k < src.size() && (src[k] == ' ' || src[k] == '>')) k++;
		while (k < src.size() && src[k] != ' ' && src[k] != '\n') k++;
		while (k < src.size() && src[k] == ' ') k++;
		return k - ls;
	};
	int delta = 0;
	if (!outdent)
	{
		// Under the previous sibling: indent to its text column.
		int32_t prev = -1;
		for (int32_t c : doc.GetChildren(list)) { if (c == item) break; prev = c; }
		if (prev < 0) return false;
		delta = (int)textCol(prev) - (int)markerCol(item);
		if (delta <= 0) return false;
	}
	else
	{
		const int32_t parentItem = blocks[list].parent;
		if (parentItem <= 0 || blocks[parentItem].type != EMarkdownBlockType::ListItem) return false;
		delta = (int)markerCol(parentItem) - (int)markerCol(item);
		if (delta >= 0) return false;
	}
	const uint32_t ls = doc.LineStart(blocks[item].beg);
	const uint32_t le = doc.LineEnd(blocks[item].end > 0 ? blocks[item].end - 1 : 0);
	std::string text = src.substr(ls, le - ls);
	std::string out;
	size_t p = 0;
	int64_t caretShift = 0;
	while (p <= text.size())
	{
		size_t e = text.find('\n', p);
		if (e == std::string::npos) e = text.size();
		std::string line = text.substr(p, e - p);
		const uint32_t lineAbs = ls + (uint32_t)p;
		if (!line.empty())
		{
			if (delta > 0) { line = std::string((size_t)delta, ' ') + line; if (lineAbs <= caret) caretShift += delta; }
			else
			{
				size_t k = 0;
				while (k < line.size() && k < (size_t)-delta && line[k] == ' ') k++;
				line = line.substr(k);
				if (lineAbs < caret) caretShift -= (int64_t)k;
			}
		}
		out += line;
		if (e >= text.size()) break;
		out += '\n';
		p = e + 1;
	}
	BeginCompound();
	doc.Replace(ls, le, out);
	caret = anchor = Snap((uint32_t)std::max<int64_t>(0, (int64_t)caret + caretShift));
	EndCompound();
	Changed();
	return true;
}

bool CMarkdownLiveEditor::InsertTable(int columns, int rows)
{
	if (readOnly || columns < 1 || rows < 1) return false;
	const std::string &src = doc.GetSource();
	// After the caret's block (its last line).
	const int32_t blk = UnitBlockOf(caret);
	uint32_t at = blk > 0 ? doc.LineEnd(doc.GetBlocks()[blk].end > 0 ? doc.GetBlocks()[blk].end - 1 : 0) : doc.LineEnd(caret);
	if (IsGapPosition(caret)) at = doc.LineEnd(caret);
	std::string t;
	const bool blankBefore = at == 0 || (at >= 1 && src[at - 1] == '\n' && (at < 2 || src[at - 2] == '\n'));
	t += at == 0 ? "" : (blankBefore ? "" : "\n\n");
	if (at > 0 && src[at - 1] == '\n' && !blankBefore) t = "\n";
	const std::string header = L("markdown.editor.table.column", "Column");
	std::string headRow = "|", delimRow = "|", bodyRow = "|";
	for (int c = 0; c < columns; c++)
	{
		const std::string name = header + " " + std::to_string(c + 1);
		headRow += " " + name + " |";
		delimRow += std::string(name.size() + 2, '-') + "|";
		bodyRow += std::string(name.size() + 2, ' ') + "|";
	}
	t += headRow + "\n" + delimRow;
	for (int r = 1; r < rows; r++) t += "\n" + bodyRow;
	if (at < src.size() && !(at + 1 < src.size() && src[at] == '\n' && src[at + 1] == '\n')) t += "\n";
	BeginCompound();
	doc.Replace(at, at, t);
	const uint32_t tableAt = at + (uint32_t)(t.find('|'));
	caret = anchor = Snap(tableAt + 2);
	EndCompound();
	Changed();
	return true;
}

void CMarkdownLiveEditor::OpenLinkDialog()
{
	if (readOnly) return;
	linkA = std::min(anchor, caret);
	linkB = std::max(anchor, caret);
	linkText[0] = linkUrl[0] = 0;
	// Inside an existing link: edit that link.
	const int32_t sp = InnermostSpanOfType(EMarkdownSpanType::Link, linkA, linkB);
	std::string text;
	if (sp >= 0)
	{
		const CMarkdownSpan &l = doc.GetSpans()[sp];
		linkA = l.beg;
		linkB = l.end;
		text = Unescape(doc.GetSource().substr(l.contentBeg, l.contentEnd - l.contentBeg));
		snprintf(linkUrl, sizeof(linkUrl), "%s", l.href.c_str());
	}
	else text = Unescape(doc.GetSource().substr(linkA, linkB - linkA));
	if (text.find('\n') != std::string::npos) text.clear();
	snprintf(linkText, sizeof(linkText), "%s", text.c_str());
	linkDialogOpen = true;
	linkDialogFocus = true;
}

bool CMarkdownLiveEditor::ApplyLink(std::string_view text, std::string_view url)
{
	if (readOnly || url.empty()) return false;
	std::string t = text.empty() ? std::string(url) : std::string(text);
	std::string u(url);
	if (u.find(' ') != std::string::npos || u.find(')') != std::string::npos) u = "<" + u + ">";
	const std::string md = "[" + EscapeAll(t) + "](" + u + ")";
	BeginCompound();
	const uint32_t a = std::min(linkA, doc.Size()), b = std::min(std::max(linkB, a), doc.Size());
	doc.Replace(a, b, md);
	caret = anchor = Snap(a + (uint32_t)md.size());
	EndCompound();
	linkDialogOpen = false;
	Changed();
	return true;
}

void CMarkdownLiveEditor::RenderLinkDialog()
{
	if (linkDialogOpen && !ImGui::IsPopupOpen("##md_link_dialog")) ImGui::OpenPopup("##md_link_dialog");
	if (!ImGui::BeginPopup("##md_link_dialog")) { if (!ImGui::IsPopupOpen("##md_link_dialog")) linkDialogOpen = false; return; }
	ImGui::TextUnformatted(L("markdown.editor.link.text", "Text"));
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
	ImGui::InputText("###md_link_text", linkText, sizeof(linkText));
	ImGui::TextUnformatted(L("markdown.editor.link.address", "Address"));
	if (linkDialogFocus) { ImGui::SetKeyboardFocusHere(); linkDialogFocus = false; }
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
	const bool enter = ImGui::InputTextWithHint("###md_link_url", "https://", linkUrl, sizeof(linkUrl), ImGuiInputTextFlags_EnterReturnsTrue);
	const std::string ok = std::string(L("markdown.editor.link.ok", "OK")) + "###md_link_ok";
	const std::string cancel = std::string(L("markdown.editor.link.cancel", "Cancel")) + "###md_link_cancel";
	if (ImGui::Button(ok.c_str()) || enter)
	{
		ApplyLink(linkText, linkUrl);
		ImGui::CloseCurrentPopup();
	}
	ImGui::SameLine();
	if (ImGui::Button(cancel.c_str()) || ImGui::IsKeyPressed(ImGuiKey_Escape, false))
	{
		linkDialogOpen = false;
		ImGui::CloseCurrentPopup();
	}
	ImGui::EndPopup();
}
