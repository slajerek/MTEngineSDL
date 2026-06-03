#ifdef ENABLE_IMGUI_TEST_ENGINE

#include "CImGuiTestEngine.h"
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"
#include "imgui_te_coroutine.h"
#include "DBG_Log.h"
#include <signal.h>

ImGuiTestEngine *CImGuiTestEngine::engine = nullptr;
bool CImGuiTestEngine::initialized = false;
bool CImGuiTestEngine::showUI = true;
void (*CImGuiTestEngine::onVisibilityChanged)(bool visible) = nullptr;

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

	// Restore default signal handlers before destroying the engine context.
	// ImGuiTestEngine_InstallDefaultCrashHandler() hooks SIGABRT etc., and the
	// Unix handler calls abort() which re-raises SIGABRT, causing infinite spam
	// if any signal fires after the engine context is gone.
#if !defined(_WIN32)
	signal(SIGILL, SIG_DFL);
	signal(SIGABRT, SIG_DFL);
	signal(SIGFPE, SIG_DFL);
	signal(SIGSEGV, SIG_DFL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGBUS, SIG_DFL);
#endif

	ImGuiTestEngine_Stop(engine);
	// MT_Shutdown() is called before VID_Shutdown(), so the ImGui context is still alive here.
	// ConfigSavedSettings=false skips the assert that requires ImGui::DestroyContext() first,
	// allowing DestroyContext to unbind the context itself.
	ImGuiTestEngine_GetIO(engine).ConfigSavedSettings = false;
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
	if (engine && showUI)
	{
		bool wasTrueBeforeRender = true;
		ImGuiTestEngine_ShowTestEngineWindows(engine, &showUI);
		if (wasTrueBeforeRender && !showUI && onVisibilityChanged)
		{
			onVisibilityChanged(false);
		}
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
