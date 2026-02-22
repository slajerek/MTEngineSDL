#ifdef ENABLE_IMGUI_TEST_ENGINE

#include "CImGuiTestEngine.h"
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"
#include "imgui_te_coroutine.h"
#include "DBG_Log.h"

ImGuiTestEngine *CImGuiTestEngine::engine = nullptr;
bool CImGuiTestEngine::initialized = false;

void CImGuiTestEngine::Init()
{
	if (initialized)
		return;

	LOGM("CImGuiTestEngine::Init");

	engine = ImGuiTestEngine_CreateContext();
	ImGuiTestEngineIO &te_io = ImGuiTestEngine_GetIO(engine);
	te_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
	te_io.CoroutineFuncs = Coroutine_ImplStdThread_GetInterface();

	ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
	ImGuiTestEngine_InstallDefaultCrashHandler();

	initialized = true;
	LOGM("CImGuiTestEngine: initialized with %s coroutine backend", "std::thread");
}

void CImGuiTestEngine::Shutdown()
{
	if (!initialized)
		return;

	LOGM("CImGuiTestEngine::Shutdown");
	ImGuiTestEngine_Stop(engine);
	ImGuiTestEngine_DestroyContext(engine);
	engine = nullptr;
	initialized = false;
}

void CImGuiTestEngine::PostSwap()
{
	if (engine)
		ImGuiTestEngine_PostSwap(engine);
}

void CImGuiTestEngine::ShowUI()
{
	if (engine)
	{
		static bool show = true;
		ImGuiTestEngine_ShowTestEngineWindows(engine, &show);
	}
}

void CImGuiTestEngine::QueueAllTests()
{
	if (engine)
		ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests);
}

bool CImGuiTestEngine::IsTestQueueEmpty()
{
	return engine ? ImGuiTestEngine_IsTestQueueEmpty(engine) : true;
}

void CImGuiTestEngine::GetResultSummary(int *tested, int *success)
{
	if (engine)
	{
		ImGuiTestEngineResultSummary summary;
		ImGuiTestEngine_GetResultSummary(engine, &summary);
		*tested = summary.CountTested;
		*success = summary.CountSuccess;
	}
}

#endif // ENABLE_IMGUI_TEST_ENGINE
