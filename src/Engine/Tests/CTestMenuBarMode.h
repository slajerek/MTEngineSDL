#ifndef _CTESTMENUBARMODE_H_
#define _CTESTMENUBARMODE_H_

#include "CTest.h"

// Verifies that the menu bar the engine created matches SYS_GetMenuBarMode()
// on this platform. Valid in any host and any mode, so every app can register it.
class CTestMenuBarMode : public CTest
{
public:
	virtual const char *GetName() override { return "MenuBarMode"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override {}
};

#endif
