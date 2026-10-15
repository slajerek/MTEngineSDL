#include "CTestMarkdownLiveEditor.h"
#include "CMarkdownLiveEditor.h"
#include "DBG_Log.h"

#if MT_ENABLE_UNDO
#include "CUndoManager.h"
#include "CUndoFieldChange.h"
#endif

#include <cstdio>
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
		ed.SetSource("abc\n");
		ed.SetCaret(3);
		ed.InsertText("d");
		MDE_ASSERT(!ed.CanUndo() && !ed.Undo() && ed.GetSource() == "abcd\n", "without MT_CAP_UNDO edits work and nothing is recorded");
	}
#endif

	// ---- Reveal units partition the source ---------------------------------------
	{
		CMarkdownLiveEditor ed;
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

	TestCompleted(true, "Markdown live editor: commands and undo");
}
