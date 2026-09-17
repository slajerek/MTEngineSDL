#include "CTestTextEditorMutation.h"
#include "DBG_Log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "imgui.h"

// TextEditor names this class a friend, so the internals below are reachable
// without a `#define protected public` at the include site: under the MSVC ABI
// member access is part of the mangled name, so that trick emits references to
// `public:` manglings of out-of-line members that nothing defines.
#include "TextEditor.h"

#define TE_ASSERT(cond, msg) \
	do { \
		if (!(cond)) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", (msg)); \
			LOGD("CTestTextEditorMutation: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, (msg)); \
	} while (0)

void CTestTextEditorMutation::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	TextEditor editor;
	std::string longText;
	for (int i = 0; i < 64; i++)
	{
		longText += "line ";
		longText += std::to_string(i);
		longText += "\n";
	}

	editor.SetText(longText);
	TE_ASSERT(editor.typeSetter.update(editor.config, editor.document, editor.lineFold),
		"initial layout builds rows for the long document");

	const size_t staleRow = editor.typeSetter.getRowCount() - 1;
	TE_ASSERT(staleRow > 0, "the long document has a scrollable row table");
	const size_t staleLine = editor.typeSetter[staleRow].line;
	TE_ASSERT(staleLine > 0, "the chosen stale row maps below the first line");

	editor.SelectAll();
	editor.ReplaceSectionText(editor.GetCurrentCursorSelection(), "");
	TE_ASSERT(editor.document.size() == 1 && editor.document[0].empty(),
		"select-all replacement shrinks the document to one empty line");
	TE_ASSERT(editor.document.isUpdated(), "the replacement leaves the document pending layout");
	TE_ASSERT(editor.typeSetter[staleRow].line == staleLine,
		"the row table is still the previous layout before updateState rebuilds it");

	TextEditor::DocPos mapped = editor.typeSetter.visPos2DocPos(editor.document, TextEditor::VisPos(staleRow, 0));
	TE_ASSERT(mapped.line == 0 && mapped.index == 0,
		"a stale visual row maps safely into the current document");

	TE_ASSERT(editor.typeSetter.update(editor.config, editor.document, editor.lineFold),
		"the pending layout rebuild still runs after the safe mapping");
	TE_ASSERT(editor.typeSetter.getRowCount() == 1,
		"the rebuilt row table matches the shrunken document");

	TextEditor pasteEditor;
	pasteEditor.SetText(longText);
	TE_ASSERT(pasteEditor.typeSetter.update(pasteEditor.config, pasteEditor.document, pasteEditor.lineFold),
		"initial layout builds rows for the paste document");

	const size_t pasteStaleRow = pasteEditor.typeSetter.getRowCount() - 1;
	pasteEditor.SelectAll();
	pasteEditor.ReplaceSectionText(pasteEditor.GetCurrentCursorSelection(), "pasted line 0\npasted line 1");
	TE_ASSERT(pasteEditor.document.size() == 2,
		"select-all paste replacement leaves the pasted line count");
	TE_ASSERT(pasteEditor.typeSetter[pasteStaleRow].line >= pasteEditor.document.size(),
		"the paste case also leaves stale rows past the new document");

	mapped = pasteEditor.typeSetter.visPos2DocPos(pasteEditor.document, TextEditor::VisPos(pasteStaleRow, 0));
	TE_ASSERT(mapped.line == pasteEditor.document.size() - 1,
		"a stale visual row after paste maps safely into the pasted document");

	TestCompleted(true, "TextEditor stale row mapping survives select-all delete and paste");
}
