#pragma once

// CMarkdownDocument: a Markdown source string plus the block/run structure md4c
// finds in it, with every element mapped back to a BYTE RANGE of the source.
//
// WHY RANGES. This is the model behind rendered ("live") Markdown editing in
// the engine: the source text is the single truth, and the editor shows it
// rendered everywhere except where the caret is. For that the renderer must
// know, for every block, which bytes of the source it covers, and which of
// those bytes are CONTENT (the words) and which are SYNTAX (the `#`, the
// `**`, the pipes, the fences). md4c reports content through its text()
// callback with pointers INTO the input buffer, so content offsets are exact;
// block ranges are derived from them and from the line structure (see the
// .cpp), because md4c does not report them. The derivation is documented and
// tested, and a byte the parser never named is, by definition, syntax.
//
// WHAT THIS IS NOT. Not an AST with a serializer: nothing here regenerates
// Markdown. An edit is a byte replacement on the source followed by a reparse,
// so untouched formatting survives by construction. That is the round-trip
// guarantee the spike was asked to demonstrate, and it is free.
//
// Pure C++ over md4c, no ImGui: compiled and tested without a window
// (CTestMarkdownDocument), used by CMarkdownLiveEditor.

#include <string>
#include <string_view>
#include <vector>
#include <cstdint>

enum class EMarkdownBlockType : uint8_t
{
	Document,
	Quote,
	UnorderedList,
	OrderedList,
	ListItem,
	HorizontalRule,
	Heading,
	Code,
	Html,
	Paragraph,
	Table,
	TableHead,
	TableBody,
	TableRow,
	TableHeaderCell,
	TableCell
};

// Inline style flags on a run. A run inherits every span it is nested in.
enum EMarkdownRunStyle : uint32_t
{
	MD_STYLE_NONE          = 0,
	MD_STYLE_EMPHASIS      = 1u << 0,
	MD_STYLE_STRONG        = 1u << 1,
	MD_STYLE_CODE          = 1u << 2,   // inline code span
	MD_STYLE_LINK          = 1u << 3,
	MD_STYLE_IMAGE         = 1u << 4,
	MD_STYLE_STRIKETHROUGH = 1u << 5,
	MD_STYLE_UNDERLINE     = 1u << 6,
	MD_STYLE_MATH          = 1u << 7,
	MD_STYLE_WIKILINK      = 1u << 8,
};

// What kind of text a run carries, after md4c's MD_TEXTTYPE.
enum class EMarkdownRunKind : uint8_t
{
	Normal,      // ordinary text, bytes in source
	Code,        // verbatim code (inline or block), bytes in source
	Entity,      // `&amp;` style entity, bytes in source are the entity text
	NullChar,    // a NUL in the source; render U+FFFD
	HardBreak,   // explicit line break; synthesized "\n"
	SoftBreak,   // a newline inside a paragraph; synthesized " " (rendered) -- the real '\n' is syntax
	Html         // raw inline HTML, bytes in source
};

struct CMarkdownRun
{
	uint32_t beg = 0;             // source byte range of the CONTENT; beg == end for a synthesized run
	uint32_t end = 0;
	uint32_t styles = MD_STYLE_NONE;
	int32_t block = -1;           // index of the leaf block (Heading/Paragraph/Code/cell/...) this run belongs to
	EMarkdownRunKind kind = EMarkdownRunKind::Normal;

	// Non-empty only for a synthesized run: the text md4c produced that is not
	// in the source (" " for a soft break, "\n" for a hard break, "" for NUL).
	// Positioned at `beg` (== end).
	std::string synthesized;

	// The link/image destination for a run inside MD_STYLE_LINK/IMAGE, as
	// written in the source (not resolved, not escaped).
	std::string href;

	// The innermost inline span this run is inside (index into
	// CMarkdownDocument::GetSpans()), -1 when none.
	int32_t span = -1;

	bool IsSynthesized() const { return beg == end && kind != EMarkdownRunKind::Normal; }
};

enum class EMarkdownSpanType : uint8_t
{
	Emphasis,
	Strong,
	Code,
	Link,
	Image,
	Strikethrough,
	Underline,
	Math,
	WikiLink
};

// An inline span (`**strong**`, `*em*`, `` `code` ``, `~~del~~`, `[link](url)`,
// `![image](src)`, `<autolink>`) with its source range. md4c reports spans
// without offsets, like blocks; the derivation is in the .cpp:
//
//   content = the runs inside the span;
//   opening = the syntax bytes right before the first content byte, back to
//             the previous content of the block or a blank;
//   closing = the syntax bytes right after the last content byte; for a link
//             or image, through the destination: `](url "title")`, `][ref]`.
//
// Nested spans share marker bytes where their delimiters touch (`***x***`):
// each claims the whole run of delimiter characters. The editor only uses
// the ranges to decide which markers to show, and over-claiming there
// reveals a little more, never less.
struct CMarkdownSpan
{
	EMarkdownSpanType type = EMarkdownSpanType::Emphasis;
	uint32_t beg = 0, end = 0;               // markers included
	uint32_t contentBeg = 0, contentEnd = 0; // inner content; empty for `![](x)`
	int32_t block = -1;                      // the block whose text holds the span
	int32_t parent = -1;                     // enclosing span, -1 none
	uint32_t firstRun = 0, runCount = 0;     // runs inside, contiguous
	std::string href;                        // link / image / wikilink destination

	bool Touches(uint32_t offset) const { return offset >= beg && offset <= end; }
};

enum class EMarkdownCellAlign : uint8_t { Default, Left, Center, Right };

// Structural table edits (CMarkdownDocument::EditTable).
enum class EMarkdownTableEdit : uint8_t
{
	InsertRowAbove,     // not above the header row
	InsertRowBelow,     // below the header: the first body row
	DeleteRow,          // body rows only
	MoveRowUp,          // body rows, within the body
	MoveRowDown,
	InsertColumnLeft,
	InsertColumnRight,
	DeleteColumn,       // never the last column
	MoveColumnLeft,
	MoveColumnRight,
	AlignDefault,       // the delimiter row: `---`
	AlignLeft,          // `:---`
	AlignCenter,        // `:---:`
	AlignRight          // `---:`
};

struct CMarkdownBlock
{
	EMarkdownBlockType type = EMarkdownBlockType::Paragraph;

	// Source byte range of the whole block, markers and nested blocks included.
	// For a container it is the union of its children (plus the lines the
	// container itself owns: a table's delimiter row). beg <= end; beg == end
	// only for a block the derivation could not place (see ResolveRanges()).
	uint32_t beg = 0;
	uint32_t end = 0;

	int32_t parent = -1;          // index into blocks; -1 for Document
	uint16_t depth = 0;           // nesting depth, Document == 0

	// Heading: 1..6. List item / list: nesting level of lists (1 for a top-level list).
	uint16_t level = 0;

	// Lists
	uint32_t listStart = 1;       // OrderedList: first number
	char listMark = 0;            // UnorderedList: '-', '*', '+'; OrderedList: '.' or ')'
	bool listIsTight = true;
	// List item
	bool isTask = false;
	char taskMark = 0;            // ' ', 'x', 'X' when isTask
	uint32_t taskMarkOffset = 0;  // offset of the char between '[' and ']' when isTask

	// Code
	bool isFenced = false;
	char fenceChar = 0;
	uint32_t infoBeg = 0, infoEnd = 0;   // the info string (language) range, if any
	uint32_t langBeg = 0, langEnd = 0;

	// Table / cells
	uint16_t columnCount = 0;     // Table
	uint16_t headRowCount = 0, bodyRowCount = 0;
	EMarkdownCellAlign align = EMarkdownCellAlign::Default;  // cells

	// Leaf blocks: the runs, contiguous in CMarkdownDocument::runs.
	uint32_t firstRun = 0;
	uint32_t runCount = 0;

	bool IsLeaf() const
	{
		return type == EMarkdownBlockType::Heading || type == EMarkdownBlockType::Paragraph
			|| type == EMarkdownBlockType::Code || type == EMarkdownBlockType::Html
			|| type == EMarkdownBlockType::HorizontalRule
			|| type == EMarkdownBlockType::TableHeaderCell || type == EMarkdownBlockType::TableCell;
	}
	bool IsCell() const { return type == EMarkdownBlockType::TableHeaderCell || type == EMarkdownBlockType::TableCell; }
	bool Contains(uint32_t offset) const { return offset >= beg && offset < end; }
};

class CMarkdownDocument
{
public:
	CMarkdownDocument();
	explicit CMarkdownDocument(std::string_view source);

	// Replace the whole source and reparse.
	void SetSource(std::string_view source);
	const std::string &GetSource() const { return source; }

	// Replace bytes [beg, end) with `text` and reparse. The ONLY mutation
	// primitive: every edit the editor makes is one of these, which is what
	// keeps unrelated bytes untouched. Returns the offset just after the
	// inserted text. Out-of-range arguments are clamped.
	//
	// The reparse is INCREMENTAL when that is provably equivalent: only the
	// top-level blocks the edit touches plus one neighbour on each side are
	// parsed, and the result is spliced in. It falls back to a full parse when
	// the neighbours do not come back with the same type and range (the edit
	// changed how they parse: an opened fence, a setext underline, a lazy
	// continuation) or when the document has link reference definitions
	// (document-global in CommonMark). CTestMarkdownDocument compares
	// thousands of random incremental edits with full parses.
	uint32_t Replace(uint32_t beg, uint32_t end, std::string_view text);
	void SetIncrementalParse(bool value) { incrementalParse = value; }
	bool WasLastParseIncremental() const { return lastParseIncremental; }
	uint32_t GetIncrementalParseCount() const { return incrementalParseCount; }

	// md4c parser flags. Default: MD_DIALECT_GITHUB (tables, strikethrough,
	// task lists, permissive autolinks). Set before SetSource(); changing it
	// reparses.
	void SetParserFlags(unsigned flags);
	unsigned GetParserFlags() const { return parserFlags; }

	const std::vector<CMarkdownBlock> &GetBlocks() const { return blocks; }
	const std::vector<CMarkdownRun> &GetRuns() const { return runs; }
	const std::vector<CMarkdownSpan> &GetSpans() const { return spans; }

	// Spans whose range [beg, end] contains `offset` (the end inclusive: a
	// caret just after `**bold**` touches it), outermost first.
	std::vector<int32_t> SpansTouching(uint32_t offset) const;
	// The innermost span of `type` touching `offset`, -1 none.
	int32_t FindSpanAt(uint32_t offset, EMarkdownSpanType type) const;

	// Children of a block, in document order (indices into GetBlocks()).
	std::vector<int32_t> GetChildren(int32_t block) const;

	// The deepest block whose range contains `offset`, or the nearest one for an
	// offset in a gap between blocks (the preceding block if any, else the
	// following). Document (0) when the source is empty. `topLevelOnly` stops
	// at the Document's direct children.
	int32_t FindBlockAt(uint32_t offset, bool topLevelOnly = false) const;

	// The top-level ancestor of a block (a direct child of Document), or 0 for Document.
	int32_t TopLevelOf(int32_t block) const;

	// The next / previous top-level block after `block`'s top-level ancestor; -1 at the ends.
	int32_t NextTopLevel(int32_t block) const;
	int32_t PrevTopLevel(int32_t block) const;

	// For a cell: the row, the table and the cell's column index.
	int32_t RowOf(int32_t cell) const;
	int32_t TableOf(int32_t block) const;
	int32_t ColumnOf(int32_t cell) const;
	// Cells of a row in column order; a row with fewer cells than the table
	// has columns is returned as it is (md4c pads to columnCount, so this is
	// the normal case only for malformed rows).
	std::vector<int32_t> CellsOf(int32_t row) const;
	// Rows of a table in document order (head rows first). Includes only rows
	// with cells; the delimiter row is not a block.
	std::vector<int32_t> RowsOf(int32_t table) const;

	// True when the byte at `offset` is CONTENT: inside a non-synthesized run.
	// Everything else in a block's range is SYNTAX. O(log n).
	bool IsContentByte(uint32_t offset) const;
	// The run covering offset, or -1 (syntax, or outside every block).
	int32_t FindRunAt(uint32_t offset) const;

	// Line helpers on the source. End of line is the offset of the '\n' (or
	// size()); start of line is the offset after the previous '\n'.
	uint32_t LineStart(uint32_t offset) const;
	uint32_t LineEnd(uint32_t offset) const;
	uint32_t Size() const { return (uint32_t)source.size(); }

	// UTF-8 code point stepping on the source. Clamped to [0, Size()].
	uint32_t NextCodepoint(uint32_t offset) const;
	uint32_t PrevCodepoint(uint32_t offset) const;

	// ---- Structural edits. Each is one Replace() and documents its exact
	// ---- byte effect, so a test can assert the source.

	// Make the top-level block at `offset` an ATX heading of `level` (1..6), or
	// a paragraph for level 0. Only a Paragraph or an ATX Heading can be
	// changed; anything else (a list, a code block, a setext heading) is left
	// alone and false is returned. The rewrite touches only the leading
	// `#...# ` of the block's first line. Returns the new caret offset for a
	// caret that was at `offset`.
	bool SetHeadingLevel(uint32_t offset, int level, uint32_t *caretInOut);

	// The ATX heading marker range on `block`'s first line: [beg, end) covering
	// `#`s and the following spaces; beg == end == first-line content start
	// when there is none. Returns false when the block is not a Heading or
	// Paragraph.
	bool GetAtxMarkerRange(int32_t block, uint32_t *beg, uint32_t *end) const;

	// For the physical line at `offset`: if it is a list item line ("- ", "* ",
	// "+ ", "1. ", "1) ", optionally "[ ] "), the prefix that a new item below
	// it should carry (same indent, same mark, number + 1, an unchecked task
	// box when the item is a task). Empty string when it is not a list line.
	std::string ListContinuationPrefix(uint32_t offset) const;

	// Edit the table that `cell` belongs to, as one Replace() of the table's
	// lines. Lines are split on unescaped pipes, so the delimiter row is
	// edited like any row and leading/trailing pipes, container prefixes
	// (`> `, indentation) and the padding of untouched cells are kept. A new
	// row's cells are as wide as the delimiter row's. Returns false, changing
	// nothing, when the edit does not apply (no cell, the header row for a
	// row deletion, the last column, a move off the end). `caretOut`, when
	// given, receives the content start of the cell the caret belongs in
	// afterwards: the new, moved or neighbouring cell.
	bool EditTable(int32_t cell, EMarkdownTableEdit edit, uint32_t *caretOut);

	// The task list item (`- [ ] ...`) whose own line holds `offset`, or whose
	// subtree does when nothing nearer is a task; -1 none.
	int32_t FindTaskItemAt(uint32_t offset) const;
	// The task item whose box mark is at `markOffset`; -1 none.
	int32_t FindTaskItemByMark(uint32_t markOffset) const;
	// Flip a task item's box: the one byte between `[` and `]` becomes `x` or
	// ` `. Nothing else changes, so every offset stays valid. False when the
	// block is not a task item.
	bool ToggleTask(int32_t item);

	// The number of parse passes so far; a test uses it to assert that an edit
	// reparsed exactly once.
	uint32_t GetParseCount() const { return parseCount; }

	// True when the block has text of its own (a tight list item does; a
	// loose one has it in a child paragraph).
	bool OwnsContentPublic(int32_t block) const { return OwnsContent(block); }

private:
	void Parse();
	bool TryIncrementalReplace(uint32_t beg, uint32_t end, std::string_view text);
	static bool MayHaveLinkReferenceDefinitions(std::string_view text);
	void ResolveRanges();
	bool OwnsContent(int32_t index) const;
	void ResolveLeafRange(int32_t index, uint32_t &cursor);
	void ResolveCellRange(int32_t index, uint32_t rowLineBeg, uint32_t &cursor);
	void ResolveSpans();
	uint32_t NextBlankLine(uint32_t from) const;
	uint32_t SpanCloseEnd(const CMarkdownSpan &span) const;
	uint32_t MatchLinkTail(uint32_t from, uint32_t limit) const;
	struct TableLine { std::string prefix; bool lead = false, trail = false; std::vector<std::string> cells; };
	TableLine SplitTableLine(uint32_t lineBeg, uint32_t lineEnd) const;
	bool LineIsSetextUnderline(uint32_t lineBeg, uint32_t lineEnd) const;
	bool LineIsFence(uint32_t lineBeg, uint32_t lineEnd, char fenceChar) const;
	bool LineIsTableDelimiter(uint32_t lineBeg, uint32_t lineEnd) const;
	uint32_t SkipContainerMarkers(uint32_t lineBeg, uint32_t lineEnd) const;
	uint32_t FirstNonBlankLineStart(uint32_t from) const;

	// md4c callbacks
	static int EnterBlock(int type, void *detail, void *userdata);
	static int LeaveBlock(int type, void *detail, void *userdata);
	static int EnterSpan(int type, void *detail, void *userdata);
	static int LeaveSpan(int type, void *detail, void *userdata);
	static int Text(int type, const char *text, unsigned size, void *userdata);

	std::string source;
	std::vector<CMarkdownBlock> blocks;
	std::vector<CMarkdownRun> runs;
	std::vector<CMarkdownSpan> spans;
	unsigned parserFlags;
	uint32_t parseCount = 0;
	bool incrementalParse = true;
	bool lastParseIncremental = false;
	uint32_t incrementalParseCount = 0;
	bool hasRefDefs = false;

	// Parse-time state
	std::vector<int32_t> blockStack;
	struct SpanState { uint32_t style; std::string href; };
	std::vector<SpanState> spanStack;
	std::vector<int32_t> openSpans;
	uint32_t strayLeaves = 0;
	uint32_t currentStyles = 0;
	std::string currentHref;
	int32_t currentLeaf = -1;
};
