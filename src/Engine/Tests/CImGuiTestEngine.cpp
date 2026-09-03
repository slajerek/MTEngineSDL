#if MT_ENABLE_IMGUI_TEST_ENGINE

#include "CImGuiTestEngine.h"
#include "CTestSuite.h"   // MT_TestResultsPath()
#include "imgui_te_engine.h"
#include "imgui_te_ui.h"
#include "imgui_te_coroutine.h"
#include "imgui.h"
#include "DBG_Log.h"
#include "SYS_CommandLine.h"  // sysCommandLineArguments
#include <cstring>
#include <cstdio>
#include <signal.h>
#include <vector>
#include "Core/Render/VID_Main.h"
#include "Core/Render/CRenderBackend.h"

// ---------------------------------------------------------------------------
// Screen capture backend for the OpenGL4 renderer.
//
// imgui_test_engine calls this from ImGuiTestEngine_PostSwap() (i.e. after the
// framebuffer swap) whenever a test invokes ctx->CaptureScreenshotWindow().
// It must write exactly w*h RGBA pixels (in ImGui coordinate space, top row
// first) into `pixels`. Because the window is created with
// SDL_WINDOW_HIGH_PIXEL_DENSITY, the GL framebuffer may be a multiple of the ImGui
// coordinate space, so we read the scaled region and downsample (nearest) into
// the caller's w*h buffer, flipping vertically (GL origin is bottom-left).
//
// Reads the default read buffer (GL_BACK). The capture engine holds the
// captured UI steady across several consecutive frames, so reading post-swap
// yields a valid, identical frame.
// ---------------------------------------------------------------------------
static bool MT_ScreenCaptureFunc(ImGuiID /*viewport_id*/, int x, int y, int w, int h,
                                 unsigned int *pixels, void * /*user_data*/)
{
	if (w <= 0 || h <= 0 || pixels == nullptr)
		return false;

	ImGuiIO &io = ImGui::GetIO();
	float scaleX = io.DisplayFramebufferScale.x > 0.f ? io.DisplayFramebufferScale.x : 1.f;
	float scaleY = io.DisplayFramebufferScale.y > 0.f ? io.DisplayFramebufferScale.y : 1.f;

	int fbW = (int)(w * scaleX); if (fbW < 1) fbW = 1;
	int fbH = (int)(h * scaleY); if (fbH < 1) fbH = 1;
	int fbX = (int)(x * scaleX);
	// TOP-DOWN. The bottom-left conversion GL needs is the GL backend's business
	// now, not the caller's -- otherwise every future backend inherits an
	// OpenGL coordinate convention it does not share.
	int fbY = (int)(y * scaleY); if (fbY < 0) fbY = 0;

	// THE READBACK IS THE BACKEND'S JOB, not glReadPixels'.
	//
	// This function used to call glPixelStorei/glReadPixels directly. Under
	// Metal those are no-ops against no context, so every capture returned an
	// untouched buffer -- and a test comparing two blank images PASSES, which is
	// worse than having no test. Backends that cannot read back return false and
	// the capture is reported as failed rather than silently empty.
	//
	// The backend also owns the row order: the GL implementation flips (its
	// framebuffer origin is bottom-left) and the Metal one must not (its textures
	// are already top-down). Both hand back top-down, tightly-packed RGBA8.
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == nullptr)
		return false;

	std::vector<unsigned int> buf((size_t)fbW * (size_t)fbH, 0u);
	if (!backend->ReadFramebufferPixels(fbX, fbY, fbW, fbH, buf.data()))
		return false;

	// Downsample into the caller's w*h buffer. No vertical flip here any more --
	// the backend already delivered top-down.
	for (int row = 0; row < h; ++row)
	{
		int srcRow = (int)(row * scaleY);
		if (srcRow < 0) srcRow = 0;
		if (srcRow >= fbH) srcRow = fbH - 1;
		for (int col = 0; col < w; ++col)
		{
			int srcCol = (int)(col * scaleX);
			if (srcCol >= fbW) srcCol = fbW - 1;
			pixels[(size_t)row * (size_t)w + col] =
				buf[(size_t)srcRow * (size_t)fbW + srcCol] | 0xFF000000u;
		}
	}
	return true;
}

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

	// Print each test's log to stdout on CLI test runs.
	//
	// ConfigLogToTTY defaults to false, which means a failing ImGui test
	// records WHY it failed into its own in-memory log and then throws it away:
	// the run reports "0/7 passed" and not one line saying what went wrong.
	// That is precisely why the game app's 0/7 sat unexplained across several
	// stages -- the information was never emitted, so nobody could act on it.
	//
	// Gated on the command line rather than always-on because an interactive
	// session that opens the Test Engine UI has its own log window and does not
	// want the terminal flooded. Checked here rather than via gHeadlessMode
	// because that flag is not set yet for every app at Init() time, and
	// the game app calls Init() unconditionally rather than only under
	// --run-tests as the photo app does.
	bool isCliTestRun = false;
	for (int i = 0; i < (int)sysCommandLineArguments.size(); i++)
	{
		const char *arg = sysCommandLineArguments[i];
		if (strcmp(arg, "--run-tests") == 0 || strcmp(arg, "--run-imgui-test") == 0)
		{
			isCliTestRun = true;
			break;
		}
	}
	te_io.ConfigLogToTTY = isCliTestRun;
	te_io.CoroutineFuncs = Coroutine_ImplStdThread_GetInterface();
	te_io.ScreenCaptureFunc = MT_ScreenCaptureFunc;

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

// Same format as CTestSuite::WriteResults -- `[name] STATUS: summary`, a `---`
// separator, then one `RESULT: p/t passed` line. Deliberately identical rather
// than merely similar: tests/run_test.sh and every CI job parse the RESULT line,
// and two spellings of "the results file" is one spelling too many.
void CImGuiTestEngine::WriteResults(const char *path)
{
	// Default (and every caller passes none): the one resolved path, so the
	// imgui suite and CTestSuite cannot disagree about where results go.
	if (path == NULL)
		path = MT_TestResultsPath();

	if (!engine)
	{
		LOGError("CImGuiTestEngine::WriteResults: no engine");
		return;
	}

	ImVector<ImGuiTest *> tests;
	ImGuiTestEngine_GetTestList(engine, &tests);

	FILE *f = fopen(path, "w");
	if (!f)
	{
		LOGError("CImGuiTestEngine::WriteResults: Failed to open %s", path);
		return;
	}

	int passed = 0, ran = 0, skipped = 0;
	for (ImGuiTest *t : tests)
	{
		const char *name = t->Name ? t->Name : "(unnamed)";
		const char *cat = t->Category ? t->Category : "";

		// Unknown means the test was never queued or never reached -- report it as
		// SKIP rather than PASS. A test that did not run is not a test that passed,
		// and defaulting the other way is how a suite goes quiet instead of red.
		switch (t->Output.Status)
		{
			case ImGuiTestStatus_Success:
				fprintf(f, "[%s] PASS: %s\n", name, cat);
				passed++; ran++;
				break;
			case ImGuiTestStatus_Error:
				fprintf(f, "[%s] FAIL: %s\n", name, cat);
				ran++;
				break;
			default:
				fprintf(f, "[%s] SKIP: %s (did not run)\n", name, cat);
				skipped++;
				break;
		}
	}

	fprintf(f, "---\n");
	if (skipped > 0)
		fprintf(f, "RESULT: %d/%d passed, %d SKIPPED (did not run)\n", passed, ran, skipped);
	else
		fprintf(f, "RESULT: %d/%d passed\n", passed, ran);
	fclose(f);

	LOGM("CImGuiTestEngine: Results written to %s", path);
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

#endif // MT_ENABLE_IMGUI_TEST_ENGINE
