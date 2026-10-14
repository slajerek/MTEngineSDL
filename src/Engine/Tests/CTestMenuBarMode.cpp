#include "CTestMenuBarMode.h"
#include "CMTNativeMenuBar.h"
#include "CGuiMain.h"
#include "VID_Main.h"
#include "DBG_Log.h"

void CTestMenuBarMode::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

	using M = EMTMenuBarMode;
	using K = EMTMenuBarKind;

	// Step 1: the mapping PLATFORM_UpdateMenus() follows, for EVERY mode and both
	// headless states -- None and ImGui are covered although no shipped host runs None.
#if defined(LINUX)
	const K nativeGui = K::ImGui, nativeHeadless = K::ImGui;
#elif defined(MACOS)
	const K nativeGui = K::Native, nativeHeadless = K::None;
#else
	const K nativeGui = K::Native, nativeHeadless = K::Native;
#endif
	const bool table =
		MT_MenuBarKindFor(M::None,   false) == K::None  && MT_MenuBarKindFor(M::None,   true) == K::None  &&
		MT_MenuBarKindFor(M::ImGui,  false) == K::ImGui && MT_MenuBarKindFor(M::ImGui,  true) == K::ImGui &&
		MT_MenuBarKindFor(M::Native, false) == nativeGui && MT_MenuBarKindFor(M::Native, true) == nativeHeadless;
	StepCompleted(1, table, "mode -> backend mapping for None, ImGui and Native");

	// Step 2: the bar this host actually got matches the mapping for its mode.
	const K kind = MT_MenuBarKindFor(SYS_GetMenuBarMode(), gHeadlessMode);
	CMTNativeMenuBar *bar = guiMain->nativeMenuBar;
	const bool created = (kind == K::None) ? (bar == nullptr)
	                                       : (bar != nullptr && bar->IsNative() == (kind == K::Native));
	LOGM("CTestMenuBarMode: mode=%d kind=%d bar=%p native=%d -> %s", (int)SYS_GetMenuBarMode(), (int)kind,
	     bar, bar ? (int)bar->IsNative() : -1, created ? "ok" : "MISMATCH");
	StepCompleted(2, created, "created bar matches the mode");

	const bool ok = table && created;
	TestCompleted(ok, ok ? "menu bar matches the mode" : (table ? "created bar does not match the mode"
	                                                             : "mode -> backend mapping is wrong"));
}
