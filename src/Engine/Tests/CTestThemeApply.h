#pragma once
#include "CTest.h"
#include "imgui.h"

// TS-1 Task 5: geometry, scale-once idempotence, border snapping, role
// bindings and the per-theme override. Needs a live ImGui context.
class CTestThemeScale : public CTest
{
public:
	virtual const char *GetName() override { return "ThemeScale"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
	// Restores what Run() disturbed. MEMBER state, not locals: CTest calls
	// Teardown() before the callback on FAILURE as well as success, so the
	// restore has to survive a failed assertion -- a local in Run() would not.
	virtual void Teardown() override;

private:
	ImGuiStyle savedStyle;
	bool       hasSavedStyle = false;
};

// TS-1 Task 6: registration, enumeration, activation, callbacks.
class CTestThemeRegistry : public CTest
{
public:
	virtual const char *GetName() override { return "ThemeRegistry"; }
	virtual void Run(ITestCallback *callback) override;
	virtual void Cancel() override { isRunning = false; }
	virtual void Teardown() override;

private:
	bool        hadTheme = false;
	char        savedId[64] = {0};
	int         savedMode = 0;
	float       savedScale = 1.0f;
	bool        needsRestore = false;
};
