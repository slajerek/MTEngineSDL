#include "CTestMarkdownLiveEditor.h"
#include "CMarkdownLiveEditor.h"
#include "imgui.h"
#include "CMarkdownEditorBindings.h"
#include "CSlrKeyboardShortcuts.h"
#include "SYS_KeyCodes.h"
#include "DBG_Log.h"

#if MT_ENABLE_UNDO
#include "CUndoManager.h"
#include "CUndoFieldChange.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

#define MDE_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[768]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", (msg)); \
			LOGD("CTestMarkdownLiveEditor: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

void CTestMarkdownLiveEditor::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

#if MT_ENABLE_UNDO
	// ---- Undo through Tools/Undo ------------------------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		ed.SetSource("Hello\n\nWorld\n");
		MDE_ASSERT(ed.GetUndoManager() != nullptr && !ed.CanUndo() && !ed.CanRedo(), "a fresh editor has its own empty undo manager");

		// Typing: one burst, one step (the calls run within the grouping window).
		ed.SetCaret(5);
		ed.InsertText(" ");
		ed.InsertText("w");
		ed.InsertText("o");
		MDE_ASSERT(ed.GetSource() == "Hello wo\n\nWorld\n" && ed.GetCaret() == 8, "typing inserts at the caret");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello\n\nWorld\n" && ed.GetCaret() == 5, "one Undo removes the whole typing burst and restores the caret");
		MDE_ASSERT(!ed.CanUndo() && ed.CanRedo(), "the burst was a single action");
		MDE_ASSERT(ed.Redo() && ed.GetSource() == "Hello wo\n\nWorld\n" && ed.GetCaret() == 8, "Redo re-applies it with the caret after it");

		// Backspace burst, then a different kind: two steps.
		ed.Backspace();
		ed.Backspace();
		ed.InsertText("!");
		MDE_ASSERT(ed.GetSource() == "Hello !\n\nWorld\n", "backspaces then typing");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello \n\nWorld\n", "Undo removes the typing only");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello wo\n\nWorld\n" && ed.GetCaret() == 8, "the next Undo restores both deleted characters at once");

		// Forward delete groups with forward delete, not with Backspace.
		ed.SetCaret(0);
		ed.DeleteForward();
		ed.DeleteForward();
		MDE_ASSERT(ed.GetSource() == "llo wo\n\nWorld\n", "forward deletes");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello wo\n\nWorld\n" && ed.GetCaret() == 0, "one Undo restores the forward-delete burst");

		// A caret move ends a burst.
		ed.SetCaret(8);
		ed.InsertText("r");
		ed.SetCaret(9);
		ed.InsertText("l");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello wor\n\nWorld\n", "typing after a caret move is a new step");
		ed.Undo();

		// A compound command (two replacements) is one step, and restores the selection.
		ed.SetSelection(0, 5);
		ed.ToggleStrong();
		MDE_ASSERT(ed.GetSource() == "**Hello** wo\n\nWorld\n", "Ctrl+B wraps the selection");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello wo\n\nWorld\n" && ed.GetAnchor() == 0 && ed.GetCaret() == 5, "one Undo unwraps it and restores the selection");

		// A heading change is one step.
		ed.SetCaret(12);
		MDE_ASSERT(ed.SetHeadingLevel(2) && ed.GetSource() == "Hello wo\n\n## World\n", "Ctrl+2 on the second paragraph");
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "Hello wo\n\nWorld\n", "Undo restores the paragraph");
		MDE_ASSERT(!ed.SetHeadingLevel(0) || ed.GetSource() == "Hello wo\n\nWorld\n", "a no-op heading command changes nothing");

		// Read-only blocks undo too.
		ed.SetReadOnly(true);
		MDE_ASSERT(!ed.Undo() && ed.GetSource() == "Hello wo\n\nWorld\n", "a read-only editor does not undo");
		ed.SetReadOnly(false);

		// A new document starts a new history.
		ed.SetSource("New\n");
		MDE_ASSERT(!ed.CanUndo() && !ed.CanRedo(), "SetSource clears the editor's own history");
	}

	// ---- A host-supplied manager: one history for the app and the editor --------
	{
		CUndoManager host;
		int setting = 1;
		host.Push(std::make_unique<CUndoFieldChange<int>>(&setting, 1, 2));
		setting = 2;

		auto ed = std::make_unique<CMarkdownLiveEditor>();
		ed->SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed->SetSource("abc\n");
		ed->SetUndoManager(&host);
		MDE_ASSERT(ed->GetUndoManager() == &host, "the editor records into the host's manager");
		ed->SetCaret(3);
		ed->InsertText("d");
		MDE_ASSERT(ed->GetSource() == "abcd\n", "edit recorded in the shared history");
		host.PerformUndo();
		MDE_ASSERT(ed->GetSource() == "abc\n" && setting == 2, "the host's Undo undoes the editor edit first");
		host.PerformUndo();
		MDE_ASSERT(setting == 1 && ed->GetSource() == "abc\n", "then the host's own action");
		host.PerformRedo();
		host.PerformRedo();
		MDE_ASSERT(setting == 2 && ed->GetSource() == "abcd\n", "Redo replays both in order");

		// Loading a new document detaches the old actions.
		ed->SetSource("xyz\n");
		host.PerformUndo();
		MDE_ASSERT(ed->GetSource() == "xyz\n", "an action from the previous document does nothing to the new one");
		host.PerformRedo();

		// The editor goes away while its actions stay in the host's manager.
		ed->SetCaret(0);
		ed->InsertText("q");
		ed.reset();
		host.PerformUndo();
		host.PerformRedo();
		MDE_ASSERT(host.CanUndo(), "actions of a destroyed editor are inert, the host's history still works");

		CMarkdownLiveEditor back;
		back.SetUndoManager(&host);
		back.SetUndoManager(nullptr);
		MDE_ASSERT(back.GetUndoManager() != &host && back.GetUndoManager() != nullptr, "nullptr goes back to the editor's own manager");
	}
#else
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		ed.SetSource("abc\n");
		ed.SetCaret(3);
		ed.InsertText("d");
		MDE_ASSERT(!ed.CanUndo() && !ed.Undo() && ed.GetSource() == "abcd\n", "without MT_CAP_UNDO edits work and nothing is recorded");
	}
#endif

	// ---- Reveal units partition the source ---------------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		const std::string src = "\n# H\n\npara one\nline two\n\n- a\n- b\n  - c\n\n> q1\n>\n> q2\n\n| x | y |\n|---|---|\n| 1 |   |\n\n```\ncode\n```\n\n---\n";
		ed.SetSource(src);
		bool covered = true, monotonic = true, cells = true;
		uint32_t prevBeg = 0;
		int32_t prevBlock = -1;
		for (uint32_t o = 0; o <= ed.GetDocument().Size(); o++)
		{
			ed.SetCaret(o);
			uint32_t b = 0, e = 0;
			ed.GetRevealedRange(&b, &e);
			const int32_t blk = ed.GetRevealedBlock();
			if (blk < 0 || o < b || (o > e) || (o == e && o != ed.GetDocument().Size() && !ed.GetDocument().GetBlocks()[blk].IsCell())) covered = false;
			if (!ed.GetDocument().GetBlocks()[blk].IsCell())
			{
				if (b < prevBeg) monotonic = false;
				prevBeg = b;
			}
			else if (ed.GetDocument().TableOf(blk) < 0) cells = false;
			prevBlock = blk;
		}
		(void)prevBlock;
		MDE_ASSERT(covered, "every byte, blank lines and container syntax included, is inside the revealed range for a caret on it");
		MDE_ASSERT(monotonic, "reveal ranges follow document order");
		MDE_ASSERT(cells, "a cell is revealed only inside its table");
		ed.SetCaret((uint32_t)src.find("q2"));
		MDE_ASSERT(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].type == EMarkdownBlockType::Paragraph, "inside a quote the unit is the paragraph");
		ed.SetCaret((uint32_t)src.find("c\n"));
		uint32_t b = 0, e = 0;
		ed.GetRevealedRange(&b, &e);
		MDE_ASSERT(src.substr(b, e - b) == "  - c\n\n", "a nested item is its own unit and owns the blank line after it");
		ed.SetCaret((uint32_t)src.find("| 1"));
		MDE_ASSERT(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].type == EMarkdownBlockType::Table, "a caret on a table's pipe reveals the table");
		ed.SetCaret((uint32_t)src.find("| 1") + 2);
		MDE_ASSERT(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].IsCell(), "a caret in a cell reveals the cell");
	}

	// ---- Tasks and links -------------------------------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		ed.SetSource("- [ ] buy milk\n- [x] done\n- plain\n");
		ed.SetCaret(8);
		MDE_ASSERT(ed.ToggleTask() && ed.GetSource() == "- [x] buy milk\n- [x] done\n- plain\n" && ed.GetCaret() == 8,
			"ToggleTask checks the box at the caret, one byte, caret in place");
		MDE_ASSERT(ed.ToggleTaskAt((uint32_t)ed.GetSource().find("x] done")) && ed.GetSource() == "- [x] buy milk\n- [ ] done\n- plain\n",
			"ToggleTaskAt flips the box whose mark is at the offset");
		ed.SetCaret((uint32_t)ed.GetSource().find("plain"));
		MDE_ASSERT(!ed.ToggleTask() && ed.GetSource() == "- [x] buy milk\n- [ ] done\n- plain\n", "a plain item has no box to toggle");
#if MT_ENABLE_UNDO
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "- [x] buy milk\n- [x] done\n- plain\n", "a toggle is one undo step");
#endif

		ed.SetSource("see docs here\n");
		ed.SetSelection(4, 8);
		ed.InsertLink();
		MDE_ASSERT(ed.GetSource() == "see [docs]() here\n" && ed.GetCaret() == 11, "Ctrl+K wraps the selection and puts the caret in the destination");
		ed.InsertText("http://d");
		MDE_ASSERT(ed.GetSource() == "see [docs](http://d) here\n", "typing fills the destination");
#if MT_ENABLE_UNDO
		ed.Undo();
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "see docs here\n", "the link insertion is one undo step");
#endif
		ed.SetSource("go https://x.y now\n");
		ed.SetSelection(3, 14);
		ed.InsertLink();
		MDE_ASSERT(ed.GetSource() == "go [](https://x.y) now\n" && ed.GetCaret() == 4, "a selected URL becomes the destination, the caret goes to the text");
		ed.SetSource("x\n");
		ed.SetCaret(1);
		ed.InsertLink();
		MDE_ASSERT(ed.GetSource() == "x[]()\n" && ed.GetCaret() == 2, "with no selection the caret goes between the brackets");

		std::string opened;
		ed.onLinkActivated = [&opened](const std::string &href) { opened = href; };
		ed.SetSource("a [t](http://t.t) b ![i](p.png)\n");
		MDE_ASSERT(ed.ActivateLinkAt(3) && opened == "http://t.t", "ActivateLinkAt opens the link under the offset");
		MDE_ASSERT(ed.ActivateLinkAt((uint32_t)ed.GetSource().find("i]")) && opened == "p.png", "an image opens its source");
		MDE_ASSERT(!ed.ActivateLinkAt(0), "plain text opens nothing");
	}

	// ---- Table commands ------------------------------------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		const std::string tsrc = "| A | B |\n|---|---|\n| 1 | 2 |\n";
		ed.SetSource(tsrc);
		ed.SetCaret(2);
		MDE_ASSERT(!ed.EditTable(EMarkdownTableEdit::DeleteRow) && ed.GetSource() == tsrc, "a refused table command changes nothing");
		ed.SetCaret((uint32_t)tsrc.find("2 |"));
		MDE_ASSERT(ed.EditTable(EMarkdownTableEdit::InsertColumnRight) && ed.GetSource() == "| A | B |   |\n|---|---|---|\n| 1 | 2 |   |\n",
			"a column to the right of the caret's cell");
		MDE_ASSERT(ed.GetDocument().GetBlocks()[ed.GetDocument().FindBlockAt(ed.GetCaret())].IsCell()
			&& ed.GetDocument().ColumnOf(ed.GetDocument().FindBlockAt(ed.GetCaret())) == 2, "the caret is in the new column");
#if MT_ENABLE_UNDO
		MDE_ASSERT(ed.Undo() && ed.GetSource() == tsrc, "one undo step restores the table");
#endif
		ed.SetCaret(2);
		MDE_ASSERT(!ed.MoveToNextCell(true), "Shift+Tab from the first cell goes nowhere");
	}

	// ---- Bindings: CSlrKeyboardShortcut objects -> ImGui chords -------------------
	{
		CMarkdownEditorBindings kb;   // a private set: the default one is shared with the UI
		MDE_ASSERT(kb.GetChord(EMarkdownEditorCommand::Bold) == (ImGuiMod_Ctrl | ImGuiKey_B)
			&& kb.GetChord(EMarkdownEditorCommand::Heading2) == (ImGuiMod_Ctrl | ImGuiKey_2)
			&& kb.GetChord(EMarkdownEditorCommand::Paragraph) == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_0)
			&& kb.GetChord(EMarkdownEditorCommand::Redo) == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)
			&& kb.GetChord(EMarkdownEditorCommand::ToggleTaskOrInsertRow) == (ImGuiMod_Ctrl | ImGuiKey_Enter)
			&& kb.GetChord(EMarkdownEditorCommand::FindPrevious) == (ImGuiMod_Shift | ImGuiKey_F3)
			&& kb.GetChord(EMarkdownEditorCommand::TableMoveRowUp) == (ImGuiMod_Alt | ImGuiMod_Shift | ImGuiKey_UpArrow),
			"the default bindings are the product spec's, as ImGui chords (Ctrl is Cmd on macOS on both sides)");
		bool distinct = true, named = true;
		for (int i = 0; i < (int)EMarkdownEditorCommand::Count; i++)
		{
			const EMarkdownEditorCommand c = (EMarkdownEditorCommand)i;
			const CSlrKeyboardShortcut *sc = kb.GetShortcut(c);
			if (sc == nullptr || sc->zone != CMarkdownEditorBindings::kZone || strlen(CMarkdownEditorBindings::GetCommandKey(c)) == 0) named = false;
			for (int j = 0; j < i; j++)
				if (kb.GetChord((EMarkdownEditorCommand)j) == kb.GetChord(c) || kb.GetShortcut((EMarkdownEditorCommand)j)->hashCode == sc->hashCode) distinct = false;
		}
		MDE_ASSERT(named && distinct, "every command has its own shortcut in the editor's zone, a translation key, and a chord no other command has");
		kb.GetShortcut(EMarkdownEditorCommand::Bold)->SetKeyCode('b', true, false, true, false);
		MDE_ASSERT(kb.GetChord(EMarkdownEditorCommand::Bold) == (ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B), "a rebinding shows in the chord at once");
		kb.GetShortcut(EMarkdownEditorCommand::Italic)->SetKeyCode(0, false, false, false, false);
		MDE_ASSERT(kb.GetChord(EMarkdownEditorCommand::Italic) == 0, "key code 0 unbinds");
		kb.ResetToDefaults();
		MDE_ASSERT(kb.GetChord(EMarkdownEditorCommand::Bold) == (ImGuiMod_Ctrl | ImGuiKey_B) && kb.GetChord(EMarkdownEditorCommand::Italic) == (ImGuiMod_Ctrl | ImGuiKey_I),
			"ResetToDefaults restores them");
		MDE_ASSERT(CMarkdownEditorBindings::KeyCodeToImGuiKey('q') == ImGuiKey_Q && CMarkdownEditorBindings::KeyCodeToImGuiKey(MTKEY_F12) == ImGuiKey_F12
			&& CMarkdownEditorBindings::KeyCodeToImGuiKey(MTKEY_ARROW_LEFT) == ImGuiKey_LeftArrow && CMarkdownEditorBindings::KeyCodeToImGuiKey(']') == ImGuiKey_RightBracket,
			"engine key codes map to ImGui keys");
	}

	// ---- Find -------------------------------------------------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);   // these steps test the M2 reveal behaviour
		ed.SetSource("Plan **beta** now\n\nBeta test, then beta.\n");
		ed.SetFindQuery("beta");
		MDE_ASSERT(ed.GetFindMatches().size() == 3, "matching is case-insensitive by default and runs over the source");
		ed.SetFindCaseSensitive(true);
		MDE_ASSERT(ed.GetFindMatches().size() == 2, "Match case drops 'Beta'");
		ed.SetFindCaseSensitive(false);
		ed.SetCaret(0);
		MDE_ASSERT(ed.FindNext() && ed.GetSelectedText() == "beta" && ed.GetCurrentFindMatch() == 0, "FindNext selects the first match after the caret");
		MDE_ASSERT(ed.FindNext() && ed.GetSelectedText() == "Beta" && ed.GetCurrentFindMatch() == 1, "then the next");
		ed.FindNext();
		MDE_ASSERT(ed.FindNext() && ed.GetCurrentFindMatch() == 0, "and wraps around");
		MDE_ASSERT(ed.FindPrevious() && ed.GetCurrentFindMatch() == 2, "FindPrevious wraps backwards");
		MDE_ASSERT(ed.GetDocument().GetBlocks()[ed.GetRevealedBlock()].type == EMarkdownBlockType::Paragraph
			&& ed.GetRevealedBlock() == ed.GetDocument().FindBlockAt(ed.GetCaret()), "the selected match's unit is revealed");
		ed.SetFindQuery("**be");
		MDE_ASSERT(ed.GetFindMatches().size() == 1, "a query with markers finds the markers");
		ed.SetFindQuery("beta");
		const uint32_t firstBefore = ed.GetFindMatches()[0].first;
		ed.SetCaret(0);
		ed.InsertText("X ");
		MDE_ASSERT(ed.GetFindMatches().size() == 3 && ed.GetFindMatches()[0].first == firstBefore + 2, "matches follow edits");
		ed.SetFindQuery("");
		MDE_ASSERT(ed.GetFindMatches().empty() && !ed.FindNext(), "an empty query finds nothing");
	}

	// ---- Word-like editing (the default mode) ---------------------------------------
	{
		CMarkdownLiveEditor ed;
		MDE_ASSERT(ed.GetEditMode() == EMarkdownEditMode::Rendered, "a new editor edits Word-like");
		ed.SetSource("Hello **bold** end\n");
		ed.SetCaret(7);
		MDE_ASSERT(ed.GetCaret() == 6, "the caret never stands inside syntax: inside the opener it snaps to the stop before");
		MDE_ASSERT(ed.GetRevealedBlock() < 0, "nothing is revealed");
		ed.MoveHorizontal(1, false);
		MDE_ASSERT(ed.GetCaret() == 9, "one Right moves over one glyph, skipping the markers");
		ed.SetCaret(12);
		ed.InsertText("er");
		MDE_ASSERT(ed.GetSource() == "Hello **bolder** end\n", "typing after a bold word continues the bold");
		ed.SetCaret(6);
		ed.InsertText("x");
		MDE_ASSERT(ed.GetSource() == "Hello x**bolder** end\n", "typing before it does not");

		ed.SetSource("Hi\n");
		ed.SetCaret(2);
		ed.InsertText("*a* [b]");
		MDE_ASSERT(ed.GetSource() == "Hi\\*a\\* \\[b\\]\n", "typed Markdown specials are literal");
		ed.SetSource("x\n");
		ed.SetCaret(0);
		ed.InsertText("#");
		MDE_ASSERT(ed.GetSource() == "\\#x\n", "a '#' at a block start is escaped");
		ed.SetSource("Run `a*b` now\n");
		ed.SetCaret(7);
		ed.InsertText("*");
		MDE_ASSERT(ed.GetSource() == "Run `a**b` now\n", "inside code nothing is escaped");

		// Autoformat at a paragraph's start.
		ed.SetSource("");
		ed.InsertText("-");
		ed.InsertText(" ");
		ed.InsertText("item");
		MDE_ASSERT(ed.GetSource() == "- item", "'- ' starts a bulleted list");
		ed.SetSource("");
		ed.InsertText("#");
		ed.InsertText("#");
		ed.InsertText(" ");
		ed.InsertText("T");
		MDE_ASSERT(ed.GetSource() == "## T" && ed.GetHeadingLevelAtCaret() == 2, "'## ' starts a heading");
		ed.SetSource("");
		ed.InsertText("1");
		ed.InsertText(".");
		ed.InsertText(" ");
		ed.InsertText("a");
		MDE_ASSERT(ed.GetSource() == "1. a" && ed.IsInList(EMarkdownListKind::Numbered), "'1. ' starts a numbered list");
		ed.SetSource("");
		ed.InsertText(">");
		ed.InsertText(" ");
		ed.InsertText("q");
		MDE_ASSERT(ed.GetSource() == "> q" && ed.IsInQuote(), "'> ' starts a quote");
		ed.SetSource("- a\n");
		ed.SetCaret(3);
		ed.InsertNewline();
		ed.InsertText("[");
		ed.InsertText(" ");
		ed.InsertText("]");
		ed.InsertText(" ");
		ed.InsertText("t");
		MDE_ASSERT(ed.GetSource() == "- a\n- [ ] t\n" && ed.IsInList(EMarkdownListKind::Task), "'[ ] ' in an item makes it a task");

		// Deletion keeps the syntax whole.
		ed.SetSource("a **b** c\n");
		ed.SetCaret(5);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "a  c\n" && ed.GetCaret() == 2, "a span left empty goes with its markers");
		ed.SetSource("x **bold** y\n");
		ed.SetSelection(1, 6);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "x**ld** y\n", "a selection cut through a span gives the rest its opener back");
		ed.SetSource("a \\* b\n");
		ed.SetCaret(4);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "a  b\n", "an escaped character goes with its backslash");
		ed.SetSource("x **bold** y\n");
		ed.SetCaret(1);
		ed.DeleteForward();
		MDE_ASSERT(ed.GetSource() == "x**bold** y\n", "Delete removes the next glyph");

		// Enter and Backspace on blocks.
		ed.SetSource("Hello world\n");
		ed.SetCaret(6);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "Hello \n\nworld\n" && ed.GetCaret() == 8, "Enter starts a new paragraph");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "Hello world\n" && ed.GetCaret() == 6, "Backspace at a paragraph's start merges it with the one before");
		ed.SetSource("a **b**\n\nc\n");
		ed.SetCaret(9);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "a **b**c\n" && ed.GetCaret() == 5, "a merge keeps the closing markers of the block before");
		ed.SetCaret(5);
		ed.InsertNewline();
		ed.SetCaret(5);
		ed.DeleteForward();
		MDE_ASSERT(ed.GetSource() == "a **b**c\n", "Delete at a block's end merges the next one");
		ed.SetCaret(ed.GetDocument().Size());
		MDE_ASSERT(ed.GetCaret() == 8, "the line after the final newline is not a caret position");
		ed.SetSource("Hello **bold** x\n");
		ed.SetCaret(10);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "Hello **bo**\n\n**ld** x\n", "Enter inside a span splits it in two");
		ed.SetSource("- one\n");
		ed.SetCaret(5);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "- one\n- \n" && ed.IsInList(EMarkdownListKind::Bullet), "Enter in a list item continues the list");
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "- one\n\n" && !ed.IsInList(EMarkdownListKind::Bullet), "Enter on an empty item ends the list");
		ed.InsertText("p");
		MDE_ASSERT(ed.GetSource() == "- one\n\np\n", "text typed on the new line is a paragraph of its own");
		ed.SetSource("Hello\n\nWorld\n");
		ed.SetCaret(5);
		ed.InsertNewline();
		ed.InsertText("x");
		MDE_ASSERT(ed.GetSource() == "Hello\n\nx\n\nWorld\n", "an empty paragraph made by Enter takes text");
		ed.SetSource("ab\n");
		ed.SetCaret(1);
		ed.InsertLineBreak();
		MDE_ASSERT(ed.GetSource() == "a\\\nb\n" && ed.GetCaret() == 3, "Shift+Enter is a line break in the paragraph");
		ed.SetCaret(4);
		ed.InsertLineBreak();
		MDE_ASSERT(ed.GetSource() == "a\\\nb\n\n" && ed.IsGapPosition(ed.GetCaret()), "at a paragraph's end, where Markdown has no empty last line, it starts a new paragraph");

		// Empty paragraphs: a blank line beyond the one between two blocks.
		ed.SetSource("abc\n\ndef\n");
		ed.SetCaret(4);
		MDE_ASSERT(ed.GetCaret() == 3, "the blank line between two paragraphs is not a caret position");
		ed.SetCaret(5);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "abc\n\n\ndef\n" && ed.GetCaret() == 6 && ed.IsGapPosition(5), "Enter at a block's start opens an empty paragraph above; the caret stays with the text");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "abc\n\ndef\n" && ed.GetCaret() == 5, "Backspace at the block's start takes that empty paragraph away again");
		ed.SetCaret(3);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "abc\n\n\ndef\n" && ed.GetCaret() == 5 && ed.IsGapPosition(5), "Enter at a block's end opens an empty paragraph below, with the caret on it");
		ed.MoveHorizontal(1, false);
		MDE_ASSERT(ed.GetCaret() == 6, "Right from the empty paragraph goes to the next block");
		ed.MoveHorizontal(-1, false);
		MDE_ASSERT(ed.GetCaret() == 5, "and Left comes back onto it");
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "abc\n\n\n\ndef\n" && ed.GetCaret() == 6 && ed.IsGapPosition(6), "Enter on an empty paragraph adds another below it");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "abc\n\n\ndef\n" && ed.GetCaret() == 5, "Backspace on it takes it away, the caret onto the one before");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "abc\n\ndef\n" && ed.GetCaret() == 3, "Backspace on the last empty paragraph takes it away; the two paragraphs do not merge");
		ed.InsertNewline();
		ed.SetCaret(3);
		ed.DeleteForward();
		MDE_ASSERT(ed.GetSource() == "abc\n\ndef\n" && ed.GetCaret() == 3, "Delete at a block's end takes the empty paragraph below away first");
		ed.DeleteForward();
		MDE_ASSERT(ed.GetSource() == "abcdef\n", "and then merges the next block");
		ed.SetSource("abc\n");
		ed.SetCaret(0);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "\nabc\n" && ed.GetCaret() == 1 && ed.IsGapPosition(0), "at the document's start one blank line is an empty paragraph");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "abc\n" && ed.GetCaret() == 0, "and Backspace takes it away");
		ed.SetSource("abc def\n");
		ed.SetCaret(3);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "abc \n\ndef\n" && ed.GetCaret() == 6, "Enter before a blank: the blank stays with the first part, the caret lands on the text");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "abc def\n" && ed.GetCaret() == 4, "and the merge gives the text back");
		ed.SetSource("abc\ndef\n");
		ed.SetCaret(3);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "abc\n\ndef\n" && ed.GetCaret() == 5, "Enter at a soft line break makes two paragraphs, no empty one");
		ed.SetSource("**bold**\n");
		ed.SetCaret(4);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "**bo**\n\n**ld**\n" && ed.GetCaret() == 10, "Enter inside bold text: both halves are bold");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "**bold**\n" && ed.GetCaret() == 4, "and the merge makes one bold span of them again");

		// Empty list items.
		ed.SetSource("2. a\n\n   - x\n   - y\n\n3. b\n");
		ed.SetCaret(19);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "2. a\n\n   - x\n   - y\n   - \n\n3. b\n" && ed.GetCaret() == 25, "Enter at the end of a nested item adds an empty item, the caret on it");
		ed.InsertText("z");
		MDE_ASSERT(ed.GetSource() == "2. a\n\n   - x\n   - y\n   - z\n\n3. b\n", "typing fills it");
		ed.Backspace();
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "2. a\n\n   - x\n   - y\n\n\n3. b\n" && ed.IsGapPosition(ed.GetCaret()) && ed.GetCaret() == 21, "Backspace on the empty item takes the bullet: an empty paragraph stays");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "2. a\n\n   - x\n   - y\n\n3. b\n" && ed.GetCaret() == 19, "and once more takes that away");
		ed.SetSource("- one\n- \n- three\n");
		ed.SetCaret(8);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "- one\n\n\n- three\n" && ed.GetCaret() == 7 && ed.IsGapPosition(7), "an empty item between two items becomes an empty paragraph, with its separator line");
		ed.SetSource("- a\n- b\n");
		ed.SetCaret(6);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "- a\n- \n- b\n" && ed.GetCaret() == 9, "Enter at an item's start opens an empty item above; the caret stays with the text");
		ed.SetSource("abc\n\n\ndef\n");
		ed.SetCaret(5);
		MDE_ASSERT(ed.ToggleList(EMarkdownListKind::Bullet) && ed.GetSource() == "abc\n\n- \n\ndef\n" && ed.GetCaret() == 7, "Bullets on an empty paragraph make it an empty item");
		ed.SetSource("abc\n\n\ndef\n");
		ed.SetCaret(5);
		MDE_ASSERT(ed.SetHeadingLevel(2) && ed.GetSource() == "abc\n\n## \n\ndef\n" && ed.GetHeadingLevelAtCaret() == 2, "Heading 2 on an empty paragraph makes it an empty heading");

		// Quotes: a second paragraph merges within the quote; the first leaves it.
		ed.SetSource("> ab\n");
		ed.SetCaret(3);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "> a\n>\n> b\n" && ed.GetCaret() == 8, "Enter inside a quote's paragraph splits it within the quote");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "> ab\n" && ed.GetCaret() == 3, "Backspace at the second paragraph's start merges them again");
		ed.SetCaret(4);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "> ab\n>\n" && ed.IsGapPosition(ed.GetCaret()) && ed.GetCaret() == 6, "Enter at the quote's end: an empty paragraph inside the quote");
		ed.InsertText("c");
		MDE_ASSERT(ed.GetSource() == "> ab\n>\n> c\n", "typed there, after the quote marker");
		ed.SetSource("> a\n>\n> b\n\nc\n");
		ed.SetCaret(8);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "> a\n>\n>\n> b\n\nc\n" && ed.GetCaret() == 10, "Enter at a quoted paragraph's start: an empty paragraph above, inside the quote");
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "> a\n>\n> b\n\nc\n" && ed.GetCaret() == 8, "and Backspace takes it away");
		ed.SetCaret(11);
		MDE_ASSERT(ed.GetCaret() == 11 && !ed.IsGapPosition(10), "the blank line after the quote is the separator, not a position");

		// Code blocks: every line is a position, an empty one too.
		ed.SetSource("> q\n\n```\nmake release\n```\n");
		ed.SetCaret(9);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "> q\n\n```\n\nmake release\n```\n" && ed.GetCaret() == 10, "Enter at a code block's first line opens an empty code line above");
		ed.MoveHorizontal(-1, false);
		MDE_ASSERT(ed.GetCaret() == 9, "which is a caret position");
		ed.MoveHorizontal(1, false);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "> q\n\n```\nmake release\n```\n" && ed.GetCaret() == 9, "Enter at a code block's start then Backspace is identity");
		ed.SetSource("```\nmake\n```\n");
		ed.SetCaret(8);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource() == "```\nmake\n\n```\n" && ed.GetCaret() == 9, "Enter at a code line's end opens an empty code line");
		ed.InsertText("x");
		MDE_ASSERT(ed.GetSource() == "```\nmake\nx\n```\n", "which takes text as it is");
		ed.Backspace();
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "```\nmake\n```\n" && ed.GetCaret() == 8, "and Backspace closes it again");

		// Enter in a table: the last row gets a row below.
		ed.SetSource("| a | b |\n|---|---|\n| c | d |\n");
		ed.SetCaret(22);
		ed.InsertNewline();
		MDE_ASSERT(ed.GetSource().find("| c | d |\n|") != std::string::npos && ed.GetDocument().GetBlocks().size() > 8, "Enter in the last row adds a row");
		ed.SetSource("# Title\n");
		ed.SetCaret(2);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "Title\n" && ed.GetCaret() == 0, "Backspace at a heading's start makes it a paragraph");
		ed.SetSource("- one\n");
		ed.SetCaret(2);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "one\n" && ed.GetCaret() == 0, "Backspace at an item's start takes the bullet off");
		ed.SetSource("> q\n");
		ed.SetCaret(2);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "q\n", "Backspace at a quote's start leaves the quote");

		// Inline styles.
		ed.SetSource("a\n");
		ed.SetCaret(1);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "a\n" && ed.IsInlineStyleActive(EMarkdownInlineStyle::Strong), "Bold with no selection changes nothing yet");
		ed.InsertText("b");
		ed.InsertText("c");
		MDE_ASSERT(ed.GetSource() == "a**bc**\n", "the next typed text is bold");
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		ed.InsertText("d");
		MDE_ASSERT(ed.GetSource() == "a**bc**d\n", "Bold off again: typing continues outside the span");
		ed.SetSource("**bold**\n");
		ed.SetCaret(6);
		ed.InsertText(" ");
		MDE_ASSERT(ed.GetSource() == "**bold** \n" && ed.GetCaret() == 9 && ed.IsInlineStyleActive(EMarkdownInlineStyle::Strong),
			"a blank typed at a bold word's end goes past the closer ('**bold **' is not bold) and bold carries on");
		ed.InsertText("x");
		MDE_ASSERT(ed.GetSource() == "**bold** **x**\n", "so the next word is bold too");
		ed.SetSource("**bold x**\n");
		ed.SetCaret(8);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "**bold** \n" && ed.GetCaret() == 9, "deleting the last word keeps the span valid the same way");
		ed.InsertText("y");
		MDE_ASSERT(ed.GetSource() == "**bold** **y**\n", "and typing goes on in bold");
		ed.SetSource("a\n");
		ed.SetCaret(1);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Emphasis);
		ed.InsertText(" ");
		ed.InsertText("b");
		MDE_ASSERT(ed.GetSource() == "a *b*\n", "a pending style leaves blanks outside its markers");
		ed.SetSource("make it bold\n");
		ed.SetSelection(8, 12);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "make it **bold**\n" && ed.GetSelectedText() == "bold", "Bold on a selection wraps it");
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "make it bold\n" && ed.GetSelectedText() == "bold", "and again unwraps it");
		ed.SetSource("**abc**\n");
		ed.SetSelection(3, 4);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "**a**b**c**\n", "unbolding the middle splits the span");
		ed.SetSource("one\n\ntwo\n");
		ed.SelectAll();
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Emphasis);
		MDE_ASSERT(ed.GetSource() == "*one*\n\n*two*\n", "a style over two paragraphs is one span in each");
		ed.SetSource("say hi\n");
		ed.SetSelection(4, 6);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Code);
		MDE_ASSERT(ed.GetSource() == "say `hi`\n" && ed.IsInlineStyleActive(EMarkdownInlineStyle::Code), "inline code");

		// Block commands.
		ed.SetSource("one\n\ntwo\n");
		ed.SelectAll();
		ed.ToggleList(EMarkdownListKind::Bullet);
		MDE_ASSERT(ed.GetSource() == "- one\n- two\n", "two paragraphs become a bulleted list");
		ed.SelectAll();
		ed.ToggleList(EMarkdownListKind::Numbered);
		MDE_ASSERT(ed.GetSource() == "1. one\n2. two\n", "a bulleted list becomes a numbered one");
		ed.SelectAll();
		ed.ToggleList(EMarkdownListKind::Numbered);
		MDE_ASSERT(ed.GetSource() == "one\n\ntwo\n", "and back to paragraphs");
		ed.SetSource("- a\n- b\n");
		ed.SetCaret(7);
		MDE_ASSERT(ed.IndentListItem(false) && ed.GetSource() == "- a\n  - b\n", "Indent nests an item under the one before");
		MDE_ASSERT(ed.IndentListItem(true) && ed.GetSource() == "- a\n- b\n", "Outdent takes it back");
		MDE_ASSERT(!ed.IndentListItem(true), "an item at the top level cannot be outdented");
		ed.SetSource("q\n");
		ed.SetCaret(1);
		MDE_ASSERT(ed.ToggleQuote() && ed.GetSource() == "> q\n" && ed.IsInQuote(), "Quote");
		MDE_ASSERT(ed.ToggleQuote() && ed.GetSource() == "q\n" && !ed.IsInQuote(), "and back");
		ed.SetSource("Title\n");
		ed.SetCaret(2);
		MDE_ASSERT(ed.SetHeadingLevel(1) && ed.GetSource() == "# Title\n" && ed.GetHeadingLevelAtCaret() == 1, "paragraph style: Heading 1");
		ed.SetSource("x\n");
		ed.SetCaret(1);
		MDE_ASSERT(ed.InsertTable(2, 2) && ed.GetDocument().GetBlocks().size() > 3, "Insert table");
		{
			bool table = false;
			for (const CMarkdownBlock &b : ed.GetDocument().GetBlocks()) if (b.type == EMarkdownBlockType::Table) table = true;
			MDE_ASSERT(table && ed.GetSource().rfind("x\n\n|", 0) == 0, "the table is a table block after the paragraph");
		}
		ed.SetSource("see docs\n");
		ed.SetSelection(4, 8);
		ed.OpenLinkDialog();
		MDE_ASSERT(ed.IsLinkDialogOpen(), "Ctrl+K opens the link dialog");
		MDE_ASSERT(ed.ApplyLink("docs", "https://example.com") && ed.GetSource() == "see [docs](https://example.com)\n" && !ed.IsLinkDialogOpen(),
			"the dialog writes a link");

		// Undo of a Word-like command is one step.
		ed.SetSource("one\n\ntwo\n");
		ed.SelectAll();
		ed.ToggleList(EMarkdownListKind::Bullet);
#if MT_ENABLE_UNDO
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "one\n\ntwo\n", "a list toggle undoes in one step");
#endif

		// The other modes.
		ed.SetSource("Hello **bold** end\n");
		ed.SetEditMode(EMarkdownEditMode::Source);
		ed.SetCaret(7);
		MDE_ASSERT(ed.GetCaret() == 7, "Source mode: the caret goes anywhere");
		ed.InsertText("*");
		MDE_ASSERT(ed.GetSource() == "Hello ***bold** end\n", "and typing is raw");
		ed.SetEditMode(EMarkdownEditMode::RevealNearCaret);
		ed.SetCaret(8);
		MDE_ASSERT(ed.GetRevealedBlock() > 0, "RevealNearCaret shows the caret's unit");
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		MDE_ASSERT(ed.GetRevealedBlock() < 0, "back to Word-like: nothing revealed");
	}

	// ---- Word processor keys: words, paragraphs, Cut, double / triple click ------
	{
		ImGuiIO &io = ImGui::GetIO();
		const bool wasMac = io.ConfigMacOSXBehaviors;
		CMarkdownLiveEditor ed;
		const std::string plain = "Hello big world, again\n\nNext para\n";
		ed.SetSource(plain);
		io.ConfigMacOSXBehaviors = false;
		std::string seen;
		auto walk = [&](int direction, uint32_t from, int steps) {
			ed.SetCaret(from);
			seen.clear();
			for (int i = 0; i < steps; i++) { ed.MoveWord(direction, false); seen += std::to_string(ed.GetCaret()) + " "; }
		};
		walk(1, 0, 8);
		MDE_ASSERT(seen == "6 10 15 17 22 24 29 33 ", "Ctrl+Right: word starts, a comma on its own, the paragraph's end, the next one");
		walk(-1, 33, 8);
		MDE_ASSERT(seen == "29 24 22 17 15 10 6 0 ", "Ctrl+Left: word starts back");
		io.ConfigMacOSXBehaviors = true;
		walk(1, 0, 6);
		MDE_ASSERT(seen == "5 9 15 22 24 28 ", "Mac Alt+Right: word ends, punctuation skipped");
		walk(-1, 22, 4);
		MDE_ASSERT(seen == "17 10 6 0 ", "Mac Alt+Left: word starts, punctuation skipped");
		io.ConfigMacOSXBehaviors = false;
		ed.SetCaret(0);
		ed.MoveWord(1, true);
		ed.MoveWord(1, true);
		MDE_ASSERT(ed.GetSelectedText() == "Hello big ", "Ctrl+Shift+Right selects word by word");

		ed.SetCaret(12);
		ed.MoveParagraph(-1, false);
		MDE_ASSERT(ed.GetCaret() == 0, "Ctrl+Up: the paragraph's start");
		ed.MoveParagraph(1, false);
		MDE_ASSERT(ed.GetCaret() == 24, "Ctrl+Down: the next paragraph's start");
		ed.MoveParagraph(1, false);
		MDE_ASSERT(ed.GetCaret() == 33, "and at the last one, its end");
		ed.MoveParagraph(-1, false);
		ed.MoveParagraph(-1, false);
		MDE_ASSERT(ed.GetCaret() == 0, "Ctrl+Up twice: back over the paragraph's start to the previous one");
		io.ConfigMacOSXBehaviors = true;
		ed.SetCaret(12);
		ed.MoveParagraph(1, false);
		MDE_ASSERT(ed.GetCaret() == 22, "Mac Alt+Down: the paragraph's end");
		ed.MoveParagraph(1, false);
		MDE_ASSERT(ed.GetCaret() == 33, "then the next one's");
		io.ConfigMacOSXBehaviors = false;
		ed.SetCaret(12);
		ed.MoveDocumentEdge(true, true);
		MDE_ASSERT(ed.GetSelectedText() == "rld, again\n\nNext para\n" || ed.GetSelectedText() == "rld, again\n\nNext para",
			"Ctrl+Shift+End selects to the document's end");

		ed.SetCaret(15);
		ed.DeleteWord(-1);
		MDE_ASSERT(ed.GetSource() == "Hello big , again\n\nNext para\n" && ed.GetCaret() == 10, "Ctrl+Backspace deletes the word before the caret");
		ed.SetSource(plain);
		ed.SetCaret(10);
		ed.DeleteWord(1);
		MDE_ASSERT(ed.GetSource() == "Hello big , again\n\nNext para\n" && ed.GetCaret() == 10, "Ctrl+Delete deletes the word after it");
		ed.SetSource(plain);
		ed.SetCaret(24);
		ed.DeleteWord(-1);
		MDE_ASSERT(ed.GetSource() == "Hello big world, againNext para\n" && ed.GetCaret() == 22, "Ctrl+Backspace at a paragraph's start joins it to the one before, as Backspace");
#if MT_ENABLE_UNDO
		MDE_ASSERT(ed.Undo() && ed.GetSource() == plain, "and undoes in one step");
#endif

		// Syntax is skipped and stays sound.
		ed.SetSource("Hello **big** world\n");
		ed.SetCaret(0);
		ed.MoveWord(1, false);
		MDE_ASSERT(ed.GetCaret() == 6 || ed.GetCaret() == 8, "Ctrl+Right stops before the bold word, not inside its markers");
		ed.MoveWord(1, false);
		MDE_ASSERT(ed.GetCaret() == 14, "and over it to the next word");
		ed.DeleteWord(-1);
		MDE_ASSERT(ed.GetSource() == "Hello world\n", "Ctrl+Backspace takes the bold word and its markers");
		ed.SetSource("Hello **big** world\n");
		ed.SelectWordAt(9);
		{
			// The selection runs between caret stops, which may lie either side of a marker.
			std::string t = ed.GetSelectedText();
			t.erase(std::remove(t.begin(), t.end(), '*'), t.end());
			MDE_ASSERT(t == "big", "double click selects the word under the pointer");
		}
		ed.SetSource(plain);
		ed.SelectWordAt(15);
		MDE_ASSERT(ed.GetSelectedText() == ",", "on punctuation, the punctuation");
		ed.SelectParagraphAt(12);
		MDE_ASSERT(ed.GetSelectedText() == "Hello big world, again", "triple click selects the paragraph");

		// Cut in Word-like mode (it used to do nothing) and in Source mode.
		ed.SetSource(plain);
		ed.SetSelection(6, 10);
		ed.Cut();
		const char *clip = ImGui::GetClipboardText();
		MDE_ASSERT(ed.GetSource() == "Hello world, again\n\nNext para\n" && clip != nullptr && std::string(clip) == "big ", "Cut takes the selection to the clipboard");
		ed.SetEditMode(EMarkdownEditMode::Source);
		ed.SetSelection(0, 6);
		ed.Cut();
		MDE_ASSERT(ed.GetSource() == "world, again\n\nNext para\n", "and in Source mode");

		// Source mode: code points, a line break ends a paragraph.
		ed.SetSource("Hello big\n\nNext\n");
		walk(1, 0, 5);
		MDE_ASSERT(seen == "6 9 10 11 15 ", "Source mode: Ctrl+Right, with every line break a step");
		ed.SetCaret(13);
		ed.MoveParagraph(-1, false);
		MDE_ASSERT(ed.GetCaret() == 11, "Source mode: Ctrl+Up to the paragraph's first line");
		ed.SetEditMode(EMarkdownEditMode::Rendered);
		io.ConfigMacOSXBehaviors = wasMac;
	}

	// ---- Underline (`<u>...</u>`) and find / replace ------------------------------
	{
		CMarkdownLiveEditor ed;
		ed.SetSource("make it plain\n");
		ed.SetSelection(8, 13);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Underline);
		MDE_ASSERT(ed.GetSource() == "make it <u>plain</u>\n" && ed.GetSelectedText() == "plain" && ed.IsInlineStyleActive(EMarkdownInlineStyle::Underline),
			"Underline on a selection wraps it in <u></u>");
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Underline);
		MDE_ASSERT(ed.GetSource() == "make it plain\n", "and again takes it off");
		ed.SetSource("a\n");
		ed.SetCaret(1);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Underline);
		MDE_ASSERT(ed.IsInlineStyleActive(EMarkdownInlineStyle::Underline), "Underline with no selection is pending");
		ed.InsertText(" ");
		ed.InsertText("b");
		MDE_ASSERT(ed.GetSource() == "a <u>b</u>\n", "and what is typed next is underlined");
		ed.SetSource("<u>ab</u>\n");
		ed.SetCaret(5);
		ed.InsertText("c");
		MDE_ASSERT(ed.GetSource() == "<u>abc</u>\n", "typing at the end of underlined text goes on underlined");
		ed.SetSource("<u>**x**</u>\n");
		ed.SetSelection(5, 6);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "<u>x</u>\n", "unbolding inside underline keeps the underline");
		ed.SetSource("**<u>x</u>**\n");
		ed.SetSelection(5, 6);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Strong);
		MDE_ASSERT(ed.GetSource() == "<u>x</u>\n", "unbolding around underline keeps the tags paired");
		ed.SetSource("a <u>b</u> c\n");
		ed.SetSelection(5, 6);
		ed.Backspace();
		MDE_ASSERT(ed.GetSource() == "a  c\n", "deleting all the underlined text takes the tags too");
		ed.SetSource("x <u>under</u> y\n");
		ed.SetEditMode(EMarkdownEditMode::Source);
		ed.SetCaret(1);
		ed.ToggleInlineStyle(EMarkdownInlineStyle::Underline);
		MDE_ASSERT(ed.GetSource() == "x<u></u> <u>under</u> y\n" && ed.GetCaret() == 4, "Source mode: Ctrl+U inserts the tag pair, the caret between");
		ed.SetEditMode(EMarkdownEditMode::Rendered);

		ed.SetSource("cat and cat and Cat\n");
		ed.OpenReplace();
		MDE_ASSERT(ed.IsFindOpen() && ed.IsReplaceOpen(), "Ctrl+H opens find with the replace row");
		ed.SetFindQuery("cat");
		ed.SetReplaceText("dog");
		ed.SetCaret(0);
		MDE_ASSERT(!ed.ReplaceCurrent() && ed.GetCurrentFindMatch() == 0, "Replace with no match selected only selects the first");
		MDE_ASSERT(ed.ReplaceCurrent() && ed.GetSource() == "dog and cat and Cat\n" && ed.GetCurrentFindMatch() == 0 && ed.GetSelectedText() == "cat",
			"Replace replaces the selected match and selects the next");
		MDE_ASSERT(ed.ReplaceAll() == 2 && ed.GetSource() == "dog and dog and dog\n", "Replace all replaces every match (case-insensitive)");
#if MT_ENABLE_UNDO
		MDE_ASSERT(ed.Undo() && ed.GetSource() == "dog and cat and Cat\n", "and undoes in one step");
#endif
		ed.SetFindCaseSensitive(true);
		MDE_ASSERT(ed.ReplaceAll() == 1 && ed.GetSource() == "dog and dog and Cat\n", "matching case leaves Cat");
		ed.SetFindCaseSensitive(false);
		ed.SetSource("**bold** word\n");
		ed.SetFindQuery("bold");
		ed.SetReplaceText("strong");
		MDE_ASSERT(ed.ReplaceAll() == 1 && ed.GetSource() == "**strong** word\n", "replacing styled text keeps its style");
		ed.SetReadOnly(true);
		ed.SetFindQuery("word");
		MDE_ASSERT(ed.ReplaceAll() == 0 && ed.GetSource() == "**strong** word\n", "a read-only editor replaces nothing");
		ed.SetReadOnly(false);
		ed.OpenFind();
		MDE_ASSERT(ed.IsFindOpen() && !ed.IsReplaceOpen(), "Ctrl+F opens find alone");
		ed.CloseFind();
	}

	TestCompleted(true, "Markdown live editor: commands and undo");
}
