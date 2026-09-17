#pragma once

#include "CTest.h"

class CTestTextEditorMutation : public CTest
{
public:
	virtual const char *GetName() override { return "TextEditorMutation"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
