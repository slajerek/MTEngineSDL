#pragma once
#include "CTest.h"

// TS-1 Task 3: ramp generation -- anchors, monotonicity, gamma distribution,
// gamut clipping, index inversion, determinism.
class CTestThemeRamp : public CTest
{
public:
	virtual const char *GetName() override { return "ThemeRamp"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};

// TS-1 Task 4: the contrast rule table across synthetic seeds, both modes.
class CTestThemeContrast : public CTest
{
public:
	virtual const char *GetName() override { return "ThemeContrast"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
