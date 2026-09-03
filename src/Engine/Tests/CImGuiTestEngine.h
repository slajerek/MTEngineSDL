#pragma once

#if MT_ENABLE_IMGUI_TEST_ENGINE

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

	// Write the run's results in EXACTLY CTestSuite's format, so one parser reads
	// both suites. Until this existed the imgui path emitted a single LOGM line and
	// no file, so a runner or a CI job had to grep log text for a number -- which is
	// how a suite stops running and nobody notices.
	// `path` is relative to the working directory, like CTestSuite's.
	// NULL means MT_TestResultsPath(): --results-file / MT_TEST_RESULTS /
	// the historical relative default. Callers pass nothing, so both test
	// paths resolve identically without an app-side change.
	static void WriteResults(const char *path = NULL);

	static ImGuiTestEngine *GetEngine() { return engine; }
	static bool showUI;
	static void (*onVisibilityChanged)(bool visible);

private:
	static ImGuiTestEngine *engine;
	static bool initialized;
};

#endif // MT_ENABLE_IMGUI_TEST_ENGINE
