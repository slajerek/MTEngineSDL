#include "CTestDisplayProfile.h"

#include "VID_Main.h"
#include "CIccProfileCodec.h"
#include "ICC_SRGBProfile.h"
#include "DBG_Log.h"

#include <cstdio>
#include <cstring>
#include <vector>

using namespace std;

#define DP_ASSERT(cond, msg) \
	do { \
		bool dpOk = (cond); \
		if (!dpOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestDisplayProfile: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

CTestDisplayProfile::CTestDisplayProfile() {}
CTestDisplayProfile::~CTestDisplayProfile() {}

void CTestDisplayProfile::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// ------------------------------------------------- 1. always a valid profile
	u8 *bytes = NULL; u32 size = 0;
	DP_ASSERT(VID_GetMainDisplayICCProfile(&bytes, &size), "main display profile query succeeds");
	DP_ASSERT(bytes != NULL && size > 0, "main display profile is non-empty");
	DP_ASSERT(CIccProfileCodec::ValidateHeader(bytes, size),
	          "main display profile is a structurally valid ICC profile");

	u8 *bytes2 = NULL; u32 size2 = 0;
	DP_ASSERT(VID_GetMainDisplayICCProfile(&bytes2, &size2), "second query succeeds");
	DP_ASSERT(size == size2 && memcmp(bytes, bytes2, size) == 0,
	          "two consecutive queries return identical bytes");

	// Which path ran is informational -- log it so the real-app check on a
	// P3 display can tell the live path from the fallback.
	{
		const vector<uint8_t> srgb = ICC_BuildSRGBProfileV2();
		bool isFallback = (size == srgb.size() && memcmp(bytes, &srgb[0], size) == 0);
		LOGM("CTestDisplayProfile: %s path (%d bytes)",
		     isFallback ? "built-in sRGB FALLBACK" : "live display profile", (int)size);
	}
	delete[] bytes; delete[] bytes2;

	// ---------------------------------------------- 1b. the fallback branch
	{
		u8 *fb = NULL; u32 fbSize = 0;
		DP_ASSERT(VID_GetDisplayICCProfileForWindow(NULL, &fb, &fbSize),
		          "NULL window still yields a profile");
		DP_ASSERT(fb != NULL && CIccProfileCodec::ValidateHeader(fb, fbSize),
		          "the fallback profile is structurally valid");
		const vector<uint8_t> srgb = ICC_BuildSRGBProfileV2();
		DP_ASSERT(fbSize == srgb.size() && memcmp(fb, &srgb[0], fbSize) == 0,
		          "the fallback is exactly the built-in sRGB profile");
		delete[] fb;
	}

	// ------------------------------------------------------ 2. serial bumps
	SDL_Window *mainWindow = VID_GetMainSDLWindow();
	DP_ASSERT(mainWindow != NULL, "a main SDL window exists (headless creates one, unshown)");
	const Uint32 mainId = SDL_GetWindowID(mainWindow);

	u64 s0 = VID_GetMainDisplayProfileSerial();

	// SDL3 flattened window events: no SDL_WINDOWEVENT wrapper with a sub-type
	// in ev.window.event -- each sub-event is now its own ev.type. The
	// SDL_VERSION_ATLEAST(2,0,18) guard around ICCPROF_CHANGED goes with it;
	// SDL3 has always had that event.
	SDL_Event ev;
	memset(&ev, 0, sizeof(ev));
	ev.window.windowID = mainId;

	ev.type = SDL_EVENT_WINDOW_ICCPROF_CHANGED;
	VID_HandleDisplayProfileEvent(&ev);
	DP_ASSERT(VID_GetMainDisplayProfileSerial() == s0 + 1,
	          "ICCPROF_CHANGED on the main window bumps the serial");

	u64 s1 = VID_GetMainDisplayProfileSerial();
	ev.type = SDL_EVENT_WINDOW_DISPLAY_CHANGED;
	VID_HandleDisplayProfileEvent(&ev);
	DP_ASSERT(VID_GetMainDisplayProfileSerial() == s1 + 1,
	          "DISPLAY_CHANGED on the main window bumps the serial");

	// ------------------------------------------------------- 3. filtering
	u64 s2 = VID_GetMainDisplayProfileSerial();

	ev.window.windowID = mainId + 1;              // some other window
	ev.type = SDL_EVENT_WINDOW_DISPLAY_CHANGED;
	VID_HandleDisplayProfileEvent(&ev);
	DP_ASSERT(VID_GetMainDisplayProfileSerial() == s2,
	          "a display change on another window does NOT bump the serial");

	ev.window.windowID = mainId;
	ev.type = SDL_EVENT_WINDOW_MOVED;              // unrelated event
	VID_HandleDisplayProfileEvent(&ev);
	DP_ASSERT(VID_GetMainDisplayProfileSerial() == s2,
	          "an unrelated window event does NOT bump the serial");

	TestCompleted(true, "Display profile discovery, sRGB fallback and serial-bump filtering verified");
}

void CTestDisplayProfile::Cancel() { isRunning = false; }
