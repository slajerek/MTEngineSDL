#pragma once

#include "CTest.h"

// Display-profile discovery and the change serial a colour epoch watches.
//
// Note what is deliberately NOT asserted: which discovery path ran. Headless
// creates the SDL window but never shows it, and Cocoa's implementation reads
// [nswindow screen], which an unordered window generally reports as nil -- so
// a headless run may legitimately take either the live path or the sRGB
// fallback. What must hold either way is that a valid profile always comes
// back, and that the serial moves only for the right events on the right window.
class CTestDisplayProfile : public CTest
{
public:
	CTestDisplayProfile();
	virtual ~CTestDisplayProfile();

	virtual const char *GetName() override { return "DisplayProfile"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override;
};
