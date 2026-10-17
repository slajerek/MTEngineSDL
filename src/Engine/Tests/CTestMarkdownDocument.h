#pragma once

#include "CTest.h"

// The Markdown document model: md4c structure mapped to source byte ranges,
// content/syntax classification, and the structural edits the live editor
// makes. Pure model, no window: it runs in the suite like any CTest.
class CTestMarkdownDocument : public CTest
{
public:
	virtual const char *GetName() override { return "MarkdownDocument"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
