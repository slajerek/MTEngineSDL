#pragma once

#include "CTest.h"

// S-5 Task 3: the resident-format funnel -- one decision, two answers.
//
// A CTest rather than an ImGui test because SlrResidentFormatFor is a PURE
// function: the caller passes the backend's capability rather than the function
// reaching for a backend. That parameter is exactly what keeps this data-only;
// without it the test would need a live GPU and would belong in the ImGui suite
// (Task 2's rule).
class CTestResidentFunnel : public CTest
{
public:
	virtual const char *GetName() override { return "ResidentFunnel"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
