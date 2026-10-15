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

	TestCompleted(true, "Markdown live editor: commands and undo");
}
