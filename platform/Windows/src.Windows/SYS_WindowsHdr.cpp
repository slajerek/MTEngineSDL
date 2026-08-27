#include "SYS_Defs.h"
#include "DBG_Log.h"

#include <SDL3/SDL.h>

// dxgi1_6.h ONLY -- no d3d11.h, deliberately. This file is OUTSIDE
// MT_RENDER_BACKEND_D3D11 and must link in an OpenGL-only Windows build, so
// it must not depend on anything the D3D11 backend owns. DXGI adapter/output
// enumeration needs no D3D device at all: CreateDXGIFactory1 alone is
// sufficient to walk outputs and read their descriptors.
#include <dxgi1_6.h>

// LINK INPUT LIVES IN THE SOURCE, same reasoning as CRenderBackendD3D11.cpp:
// the engine is a StaticLibrary, so its own <Link> section is inert and every
// app re-lists dependencies itself. UNCONDITIONAL here (not inside
// MT_RENDER_BACKEND_D3D11): this file compiles in every Windows build,
// OpenGL-only included, and dxgi.lib is a lightweight, always-present system
// import library -- CreateDXGIFactory1/adapter/output enumeration needs no
// GPU driver beyond what DXGI itself provides, so linking it unconditionally
// costs nothing and does not imply a D3D device was ever created.
#pragma comment(lib, "dxgi.lib")

// ---------------------------------------------------------------------------
// WINDOWS_GetMaxPotentialHdrHeadroom -- the Windows half of the HDR gate
// ---------------------------------------------------------------------------
//
// The once-per-session "can ANY attached display show above-white" question.
// VID_IsHdrRequested() resolves `hdrMode=auto` with it (VID_Main.cpp), and the
// answer decides whether the photo cache spends twice the memory on float
// images -- so it is a capability question, asked at init, not the live
// per-window headroom (that is CRenderBackend::GetDisplayHdrHeadroom(), a poll).
//
// The macOS analogue is MACOS_GetMaxPotentialHdrHeadroom() in
// SYS_MacOSWrapper.mm, which scans every NSScreen's
// maximumPotentialExtendedDynamicRangeColorComponentValue.
//
// S-6 TASK B4: NO DXGI IN THE GATE ITSELF. SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN
// (per DISPLAY, unlike the per-window properties the backend uses) already
// answers exactly the question this function asks, and the caller
// (VID_IsHdrRequested()) only tests `> 1.0f` -- so a plain SDL walk is both
// sufficient and the simplest thing that cannot get the DIRECTION of any
// ratio wrong. DXGI enumeration is used ONLY below, to LOG MaxLuminance for
// the record (Task B4/B5 evidence); it never feeds the returned value.
static float WindowsProbeSdlHdrHeadroom()
{
	int numDisplays = 0;
	SDL_DisplayID *displays = SDL_GetDisplays(&numDisplays);
	if (displays == NULL)
	{
		LOGError("WINDOWS_GetMaxPotentialHdrHeadroom: SDL_GetDisplays failed: %s", SDL_GetError());
		return 1.0f;
	}

	bool anyHdr = false;
	for (int i = 0; i < numDisplays; i++)
	{
		SDL_PropertiesID props = SDL_GetDisplayProperties(displays[i]);
		const bool hdrEnabled = (props != 0) &&
			SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
		LOGM("WINDOWS_GetMaxPotentialHdrHeadroom: display %d hdrEnabled=%s",
			 (int)displays[i], hdrEnabled ? "true" : "false");
		if (hdrEnabled)
			anyHdr = true;
	}
	SDL_free(displays);

	// The gate only asks `> 1.0f` (the `hdrMode=auto` arm of
	// VID_IsHdrRequested(); cited by SYMBOL because that line has already
	// moved once), so a nominal 2.0 is
	// sufficient and honest: it is not claimed as a measured ratio, only as
	// "yes, something here can show above-white". The live, per-window
	// headroom that DOES vary (and that tone-mapping actually uses) is
	// CRenderBackend::GetDisplayHdrHeadroom() -- SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT,
	// polled every frame, never latched here.
	// Say so in the LOG too, not only in this comment. VID_IsHdrRequested()
	// prints the value it gets as "max potential headroom 2.000", which reads
	// like a measurement to anyone reading a log months from now; the one
	// place that knows it is nominal is here.
	LOGM("WindowsProbeSdlHdrHeadroom: returning %s -- NOMINAL, not a measured "
		 "ratio (the gate only asks > 1.0). For a real ratio see the "
		 "MaxLuminance lines WindowsLogDxgiOutputs() logged just above, where any "
		 "were available.",
		 anyHdr ? "2.0" : "1.0");
	return anyHdr ? 2.0f : 1.0f;
}

// ---------------------------------------------------------------------------
// WINDOWS_IsAnyDisplayHdrCapable -- the LIVE half, for the Settings UI
// ---------------------------------------------------------------------------
//
// Deliberately a SEPARATE, uncached function rather than
// "WindowsProbeSdlHdrHeadroom() > 1.0f": that one is latched once per
// process (see WINDOWS_GetMaxPotentialHdrHeadroom() below) precisely because
// its caller, VID_IsHdrRequested(), must not disagree with itself about the
// swapchain format mid-session. This one exists for the opposite reason --
// a user can unplug the display they launched with and plug in an
// HDR-capable one, and the Settings pane's "On" radio must notice on its
// very next redraw. No logging here: unlike the once-per-session probe,
// this can run every frame the Settings pane is visible, and logging it
// that often would spam. Same walk, same SDL properties, just not cached
// and not logged.
bool WINDOWS_IsAnyDisplayHdrCapable()
{
	int numDisplays = 0;
	SDL_DisplayID *displays = SDL_GetDisplays(&numDisplays);
	if (displays == NULL)
		return false;

	bool anyHdr = false;
	for (int i = 0; i < numDisplays; i++)
	{
		SDL_PropertiesID props = SDL_GetDisplayProperties(displays[i]);
		if (props != 0 && SDL_GetBooleanProperty(props, SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false))
		{
			anyHdr = true;
			break;
		}
	}
	SDL_free(displays);
	return anyHdr;
}

// LOGGED EVIDENCE ONLY -- see the function comment above. Walks every DXGI
// output on every adapter and logs its colour space and MaxLuminance, which
// is the number Task B4/B5 asks to have on record (and, per
// MT_SurfaceEncoding.h's ScRgbSdrWhiteFromNits, is the FORWARD direction any
// future real-ratio computation must use: MaxLuminance / (80 * sdrWhiteScRgb),
// never its inverse -- getting that backwards is a 6400x error in the
// headroom, which then feeds tone-mapping).
static void WindowsLogDxgiOutputs()
{
	IDXGIFactory1 *factory = NULL;
	if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&factory)) || factory == NULL)
	{
		LOGM("WINDOWS_GetMaxPotentialHdrHeadroom: CreateDXGIFactory1 failed -- no DXGI output evidence available");
		return;
	}

	// SUCCEEDED(), not just "!= DXGI_ERROR_NOT_FOUND": that comparison is the
	// terminating condition for a WELL-BEHAVED driver, but any OTHER failure
	// HRESULT (undocumented for a conformant driver, not contractually
	// impossible on the odd VM/remote-session/old-driver setup this probe
	// exists to survive) leaves the out-pointer NULL per the COM contract --
	// dereferencing it unconditionally would crash every Windows launch of
	// all three apps, since this file compiles and runs unconditionally.
	// Found by S-6 B3/B4 milestone review round 2.
	UINT adapterIndex = 0;
	IDXGIAdapter1 *adapter = NULL;
	while (SUCCEEDED(factory->EnumAdapters1(adapterIndex, &adapter)) && adapter != NULL)
	{
		DXGI_ADAPTER_DESC1 adapterDesc = {};
		adapter->GetDesc1(&adapterDesc);
		char adapterName[128] = {0};
		WideCharToMultiByte(CP_UTF8, 0, adapterDesc.Description, -1, adapterName, sizeof(adapterName), NULL, NULL);

		UINT outputIndex = 0;
		IDXGIOutput *output = NULL;
		while (SUCCEEDED(adapter->EnumOutputs(outputIndex, &output)) && output != NULL)
		{
			IDXGIOutput6 *output6 = NULL;
			if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void **)&output6)) && output6 != NULL)
			{
				DXGI_OUTPUT_DESC1 desc1 = {};
				if (SUCCEEDED(output6->GetDesc1(&desc1)))
				{
					char outputName[64] = {0};
					WideCharToMultiByte(CP_UTF8, 0, desc1.DeviceName, -1, outputName, sizeof(outputName), NULL, NULL);
					LOGM("WINDOWS_GetMaxPotentialHdrHeadroom: adapter='%s' output='%s' colorSpace=%d "
						 "MaxLuminance=%.1f nits MinLuminance=%.4f nits MaxFullFrameLuminance=%.1f nits "
						 "bitsPerColor=%u",
						 adapterName, outputName, (int)desc1.ColorSpace, desc1.MaxLuminance,
						 desc1.MinLuminance, desc1.MaxFullFrameLuminance, (unsigned)desc1.BitsPerColor);
				}
				output6->Release();
			}
			else
			{
				LOGM("WINDOWS_GetMaxPotentialHdrHeadroom: adapter='%s' output %u has no IDXGIOutput6 "
					 "(older Windows 10) -- no MaxLuminance available for it", adapterName, outputIndex);
			}
			output->Release();
			outputIndex++;
		}
		adapter->Release();
		adapterIndex++;
	}
	factory->Release();
}

float WINDOWS_GetMaxPotentialHdrHeadroom()
{
	// LATCHED AFTER THE FIRST CALL, and that is this function's OWN
	// responsibility, not its caller's.
	//
	// The function comment above has always called this "the once-per-
	// session" question, and VID_IsHdrRequested() (VID_Main.cpp) does cache
	// its own answer -- but it is not the only caller. S-6 whole-subphase
	// review found PC_ResidentFormat.cpp's PC_PublishSurfaceSnapshot() calls
	// VID_GetMaxPotentialHdrHeadroom() DIRECTLY, uncached, from MT_Render()
	// every 30 frames, forever, on every Windows launch regardless of
	// backend or the --hdr flag. Before this file had a real body that cost
	// nothing (a 1-line stub); after it, that call site turned into a
	// CreateDXGIFactory1 + full adapter/output enumeration running on the
	// RENDER THREAD roughly twice a second for the life of the process --
	// confirmed in this VM's own logs at up to 258 adapter/output log lines
	// in 14 seconds. Caching HERE fixes it for every current and future
	// caller, not just the one found this pass, and matches what this
	// function already claimed to be doing.
	static bool sResolved = false;
	static float sResult = 1.0f;
	if (!sResolved)
	{
		WindowsLogDxgiOutputs();
		sResult = WindowsProbeSdlHdrHeadroom();
		sResolved = true;
	}
	return sResult;
}
