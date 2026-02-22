#pragma once

#ifdef ENABLE_IMGUI_TEST_ENGINE

#include "imgui.h"

struct ImGuiTestEngine;

class CImGuiTestEngine
{
public:
	static void Init();
	static void Shutdown();
	static void PostSwap();
	static void ShowUI();

	static void QueueAllTests();
	static bool IsTestQueueEmpty();
	static void GetResultSummary(int *tested, int *success);

	static ImGuiTestEngine *GetEngine() { return engine; }

private:
	static ImGuiTestEngine *engine;
	static bool initialized;
};

#endif // ENABLE_IMGUI_TEST_ENGINE
