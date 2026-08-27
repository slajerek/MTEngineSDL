#pragma once
#include "CTest.h"

// TS-1 Task 1: the theme system's numeric substrate. OKLab reference values
// (Ottosson), round trip, gamut clipping, the WCAG contrast definition, and
// the achromatic closed form (linear luminance of a neutral == L^3) that the
// contrast solver relies on. Pure maths, no ImGui -- runs headlessly.
class CTestThemeMath : public CTest
{
public:
	CTestThemeMath() = default;
	~CTestThemeMath() = default;
	virtual const char *GetName() override { return "ThemeMath"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
};
