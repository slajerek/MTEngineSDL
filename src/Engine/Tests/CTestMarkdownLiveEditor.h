#pragma once

#include "CTest.h"

// The live Markdown editor's command layer, driven through its public API
// without a window: undo through Tools/Undo (grouping, compound commands, a
// shared host manager), and the editing commands whose effect is a precise
// change to the source. Rendering and real input are covered by the
// imgui_test_engine tests in CImGuiMarkdownEditorTests.
class CTestMarkdownLiveEditor : public CTest
{
public:
	virtual const char *GetName() override { return "MarkdownLiveEditor"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
