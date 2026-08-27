#include "CRenderBackendD3D11.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "SYS_Main.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "CSlrImage.h"
// CImageData.h -- for HalfToFloat() and getResultDataForUpload(), both of which
// this backend genuinely needs. IT ALSO DRAGS png.h AND A GLOBAL
// `using namespace std;` INTO A TU THAT INCLUDES <windows.h>, which is the very
// hazard Task A1 split MT_SrgbCurve.h out to avoid. What actually holds the
// std::byte / rpcndr.h `byte` ambiguity off here is `_HAS_STD_BYTE=0`, set in
// ALL FOUR engine configurations and all four app configurations -- a
// pre-existing repo-wide setting, not A1's split, which covers only
// MT_SurfaceEncoding.h. If you are reading this because the first Windows
// compile of this file died in png.h or on `byte`, that is the thread to pull:
// check _HAS_STD_BYTE survived, then narrow this include.
#include "CImageData.h"
// NOTE: MT_SurfaceEncoding.h is deliberately NOT included. Its arithmetic is
// transcribed into Shaders/Resolve.hlsl and executed on the GPU; nothing on the
// C++ side of this backend evaluates it. Including it "for the reference" would
// be an include nobody can justify from the code.
#include "Core/Render/CRenderTarget.h"
#include "Video/CVideoYUVConverter.h"
#include "CRenderShaderFlatColorD3D11.h"
#include "CVideoYUVShaderD3D11.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_sdl3.h"

#include "Tests/MT_ShaderProbe.h"

#include <d3d11.h>
#include <dxgi1_6.h>
#include <cstring>

#include "Generated/ResolveVSBytecode.h"
#include "Generated/ResolvePSBytecode.h"

// LINK INPUTS LIVE IN THE SOURCE, NOT IN THE PROJECT, and that is deliberate.
// MTEngineSDL is a StaticLibrary, so its own <Link><AdditionalDependencies> is
// inert -- every app re-lists the engine's dependencies itself. The factory arm
// in VID_Main.cpp drags this object into PhotoCruise, LightHeroes AND c64d, all
// three of which would otherwise fail to link with no D3D code of their own.
// A #pragma here is honoured by MSVC and by the ClangCL toolset the engine
// actually builds with, and it cannot be forgotten in a fourth app later.
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// ===========================================================================
// EVERY VIRTUAL ON CRenderBackend, AND WHAT THIS BACKEND DOES WITH IT
// ===========================================================================
//
// The interface has 38 virtuals and NONE of them is pure -- every one has a
// default body -- so "not implemented" is a silent, working state and the only
// way to know what a backend actually does is to write it down. Round 2 of the
// S-6 plan review added this table for exactly that reason.
//
//   IMPLEMENTED
//     CreateSDLWindow, CreateRenderContext, InitRenderPipeline,
//     CreateFontsTexture (empty: ImGui 1.93 owns font textures),
//     NewFrame, PresentFrameBuffer, ApplyDisplayColorGamut, Shutdown,
//     CreateTexture, UpdateTextureLinearScaling, ReBindTexture, DeleteTexture,
//     GetPreferredCompressedFormat, SupportsTextureFormat,
//     SupportsOpenGLShaders (false -- see the header, it is load-bearing),
//     GetSurfaceFormat, GetSurfaceIsExtendedRange, GetSurfaceIsLinearColorSpace,
//     GetDisplayHdrHeadroom, SetSurfaceEdrMetadata (logged no-op),
//     GetSurfaceHasEdrMetadata (false),
//     ReadFramebufferPixels, ReadTexturePixels, ReadTexturePixelsFloat,
//     ReadSwapchainPixels (D3D-only: proves the resolve is an identity),
//     CreateRenderTarget, CreateFlatColorShader, CreateVideoYUVConverter,
//     ImageNeedsSamplerOverride, QueueSamplerForImage, QueueDefaultSampler,
//     CreatePlaneTexture/Update/Delete, CreateLutTexture3D/Update/Delete.
//
//   DELIBERATELY LEFT ON THE DEFAULT
//     CreateMaskedTileShader -> NULL. It serves c64d and LightHeroes, which
//       keep working on OpenGL, and every caller already draws its unshaded
//       fallback rather than dereferencing. Porting more HLSL blind is what
//       would make this stage unfinishable.
//
// THE FOUR THAT ARE MANDATORY AND LOOK OPTIONAL, because without them nothing
// the suites test can run: the three plane-texture methods (every video plane
// AND the shader probe's test textures, so hdr_shader_agrees_with_transfer_header
// cannot run without them), the three LUT methods (CM-E), ReadFramebufferPixels
// (what MT_CaptureWindowRGBA and therefore EVERY capture test reads), and the
// three sampler methods (render_backend_nearest_magnification asserts on every
// backend).
//
// ===========================================================================
// HANDLE CONVENTION
// ===========================================================================
//
// Every `void *` texture handle that crosses this interface is an
// ID3D11ShaderResourceView*. That is what imgui_impl_dx11 takes as an
// ImTextureID, so a CSlrImage's handle draws without translation; the
// underlying ID3D11Texture2D is always recoverable with GetResource(); and the
// SRV holds a reference, so the resource cannot die while a handle to it is
// alive. One convention, no exceptions -- never an ID3D11Texture2D*, never an
// index. (Metal's equivalent rule is "never GLuint": a 32-bit handle type
// silently truncates half a pointer.)
// ===========================================================================

// --- the device and the frame ---------------------------------------------

static ID3D11Device        *gDevice        = NULL;
static ID3D11DeviceContext *gContext       = NULL;   // the IMMEDIATE context
static IDXGISwapChain1     *gSwapChain     = NULL;
static IDXGISwapChain3     *gSwapChain3    = NULL;   // for SetColorSpace1, may be NULL
static ID3D11RenderTargetView *gBackBufferRTV = NULL;
static HWND                 gHwnd          = NULL;
static DXGI_FORMAT          gSwapChainFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
static int                  gSwapChainW = 0, gSwapChainH = 0;

// THE OFFSCREEN TARGET IS ALWAYS RGBA16F, on an SDR swapchain as well as an HDR
// one, and the extra bandwidth is bought deliberately.
//
// One: an 8-bit offscreen would CLAMP the above-white values HDR video and
// float photos produce, before the resolve ever saw them -- destroying the
// range at the first step of the pipeline that exists to carry it.
// Two: the same pass then runs in both modes, so there is no HDR-only code path
// to rot unnoticed while everyone develops on SDR. That is worth more than the
// memory: this programme has already shipped two defects that only appeared on
// the arm nobody exercised.
static ID3D11Texture2D          *gOffscreenTex = NULL;
static ID3D11RenderTargetView   *gOffscreenRTV = NULL;
static ID3D11ShaderResourceView *gOffscreenSRV = NULL;
static int gOffscreenW = 0, gOffscreenH = 0;

static bool gFrameValid = false;      // NewFrame got a usable surface
static bool gFrameIsHeadless = false;

// Whether ImGui_ImplDX11_Init() actually ran. ImGui_ImplDX11_Shutdown()
// dereferences its backend data behind an IM_ASSERT that COMPILES OUT IN
// RELEASE, and InitRenderPipeline() returns early when the device could not be
// created -- exactly the path CreateRenderContext() promises the app will
// "run without rendering rather than crashing". Without this it runs, and then
// crashes at exit inside ImGui, with a stack that names ImGui rather than this
// backend. Also makes a second Shutdown() a no-op.
static bool gImGuiDX11Initialised = false;
// SEPARATE FROM THE ABOVE, and it has to be. gImGuiDX11Initialised means "the
// RENDERER backend was initialised and must be shut down"; this means "the
// PLATFORM backend was initialised". They diverge on exactly one path -- no
// device -- where the platform half runs and the renderer half does not.
// Guarding re-entry on the renderer flag would have left that path able to call
// ImGui_ImplSDL3_InitForOther twice, which trips
// IM_ASSERT(io.BackendPlatformUserData == nullptr) in Debug and leaks the
// backend data in Release: the very failure the guard exists to prevent.
static bool gImGuiPlatformInitialised = false;

// --- the resolve pass ------------------------------------------------------

static ID3D11VertexShader *gResolveVS = NULL;
static ID3D11PixelShader  *gResolvePS = NULL;
static ID3D11Buffer       *gResolveCB = NULL;
static ID3D11SamplerState *gPointSampler = NULL;
static ID3D11RasterizerState *gResolveRasterizer = NULL;
static ID3D11BlendState      *gResolveBlend = NULL;
static ID3D11DepthStencilState *gResolveDepth = NULL;

// MUST MATCH Shaders/Resolve.hlsl's cbuffer FIELD FOR FIELD AND IN ORDER: this
// is a raw byte copy into the constant buffer, so a mismatch silently misreads
// every field after it rather than failing to compile. 16 bytes, which is also
// D3D11's minimum constant-buffer granularity.
struct SResolveConstants
{
	float sdrWhiteScRgb;     // offset  0
	int   swapchainIsLinear; // offset  4
	float pad[2];            // offset  8..15
};
static_assert(sizeof(SResolveConstants) == 16, "SResolveConstants must match Resolve.hlsl's cbuffer exactly");

// --- per-draw samplers -----------------------------------------------------

static ID3D11SamplerState *gSamplerLinear     = NULL;
static ID3D11SamplerState *gSamplerMagNearest = NULL;

// --- live SDL HDR properties, POLLED, never latched ------------------------
//
// SDL updates these dynamically and raises SDL_EVENT_WINDOW_HDR_STATE_CHANGED
// when they move -- the user drags the window to another monitor, or moves the
// Windows SDR-brightness slider. A value read once at startup is wrong from the
// first such event onwards, and reads as "this display has no HDR". Refreshed
// at the top of every NewFrame().
static float gSdrWhiteLevel = 1.0f;
static float gHdrHeadroom   = 1.0f;
static bool  gHdrEnabled    = false;


static void D3D11ReleaseFrameTargets();
static bool D3D11EnsureOffscreen(int w, int h);
static bool D3D11ShadersArePlaceholders();

CRenderBackendD3D11::CRenderBackendD3D11()
: CRenderBackend("D3D11")
{
}

CRenderBackendD3D11::~CRenderBackendD3D11()
{
}

// ---------------------------------------------------------------------------
// IsAvailable -- can this MACHINE do D3D11 at all?
// ---------------------------------------------------------------------------
//
// No window, no swapchain, no side effects: a null-HWND device is created,
// asked to exist, and released. This is the deliberate difference from the
// Metal precedent, whose CreateSDLWindow calls SYS_FatalExit on failure because
// on a Mac Metal always exists. A Windows box that cannot make a D3D11 device
// is a real case, and taking the process down for it would be a support
// incident rather than a fallback.
//
// Called at most ONCE per process: VID_IsRenderBackendAvailable() caches it,
// because it is asked from per-frame UI and creating a device to answer a menu
// is not acceptable at that rate.
bool CRenderBackendD3D11::IsAvailable()
{
	// A PLACEHOLDER RESOLVE SHADER MEANS THIS BACKEND CANNOT DRAW, and the
	// answer belongs HERE rather than in InitRenderPipeline().
	//
	// This is the only query the factory consults, and it is consulted BEFORE
	// the backend is constructed -- so it is the one place a "fall back to
	// OpenGL" can actually happen. InitRenderPipeline only logs; by the time it
	// runs the process has already committed, and the result would be a running
	// app with a black window and a log line nobody reads. Three comments used
	// to promise a fallback that was not implemented anywhere.
	if (D3D11ShadersArePlaceholders())
	{
		LOGError("CRenderBackendD3D11::IsAvailable: the embedded Resolve bytecode is a PLACEHOLDER "
				 "(length 0). Run tools/embed-hlsl-shaders.ps1 with fxc.exe on PATH and commit the "
				 "generated headers. Falling back to OpenGL.");
		return false;
	}

	static const D3D_FEATURE_LEVEL kLevels[] = { D3D_FEATURE_LEVEL_11_0 };

	ID3D11Device *device = NULL;
	ID3D11DeviceContext *context = NULL;
	D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;

	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
								   kLevels, (UINT)(sizeof(kLevels) / sizeof(kLevels[0])),
								   D3D11_SDK_VERSION, &device, &got, &context);
	if (FAILED(hr))
	{
		// WARP is a software rasteriser and would "work" -- but it would be
		// slower than OpenGL for a compositor like this one, so a machine that
		// needs it is a machine that should stay on OpenGL. Reported, not used.
		LOGError("CRenderBackendD3D11::IsAvailable: no hardware D3D11 device (hr=0x%08x); D3D11 is unavailable here", (unsigned)hr);
		return false;
	}

	if (context != NULL) context->Release();
	if (device  != NULL) device->Release();
	LOGM("CRenderBackendD3D11::IsAvailable: hardware D3D11 device available (feature level 0x%04x)", (unsigned)got);
	return true;
}

// ---------------------------------------------------------------------------
// Window
// ---------------------------------------------------------------------------

SDL_Window *CRenderBackendD3D11::CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized)
{
	// THE SAME FLAGS AS THE OTHER TWO BACKENDS, minus SDL_WINDOW_OPENGL, and
	// with NO SDL_CreateRenderer: Metal needs one only to obtain its
	// CAMetalLayer, and D3D11 takes the HWND directly. Creating an SDL renderer
	// we never draw through would put a second, competing presentation path on
	// the same window.
	//
	// SDL_WINDOW_HIGH_PIXEL_DENSITY is OPT-IN: dropping it ships a non-Retina
	// backbuffer, and the symptom looks like a UI-scale bug rather than a
	// missing flag.
	SDL_WindowFlags windowFlags = (SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);

	mainWindow = SDL_CreateWindow(title, w, h, windowFlags);
	if (mainWindow == NULL)
	{
		LOGError("CRenderBackendD3D11::CreateSDLWindow: SDL_CreateWindow failed: %s", SDL_GetError());
		return NULL;
	}
	// SDL3 dropped x/y from SDL_CreateWindow; the window is created HIDDEN, so
	// positioning it now is exactly equivalent and there is no visible jump.
	SDL_SetWindowPosition(mainWindow, x, y);

	gHwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(mainWindow),
										 SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
	if (gHwnd == NULL)
		LOGError("CRenderBackendD3D11::CreateSDLWindow: SDL gave no HWND -- the swapchain cannot be created");

	return mainWindow;
}

// ---------------------------------------------------------------------------
// Device and swapchain
// ---------------------------------------------------------------------------

void CRenderBackendD3D11::CreateRenderContext()
{
	if (gHwnd == NULL)
	{
		LOGError("CRenderBackendD3D11::CreateRenderContext: no HWND");
		return;
	}

	UINT flags = 0;
#if defined(DEBUG) || defined(_DEBUG)
	// The debug layer turns silent misuse into a message. It requires the
	// Graphics Tools optional feature, which is NOT installed by default, so
	// this falls back rather than failing the launch of a Debug build on a
	// machine that lacks it.
	flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

	static const D3D_FEATURE_LEVEL kLevels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
	D3D_FEATURE_LEVEL got = (D3D_FEATURE_LEVEL)0;

	HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, flags,
								   kLevels, (UINT)(sizeof(kLevels) / sizeof(kLevels[0])),
								   D3D11_SDK_VERSION, &gDevice, &got, &gContext);
#if defined(DEBUG) || defined(_DEBUG)
	if (FAILED(hr))
	{
		LOGError("CRenderBackendD3D11: device creation with the debug layer failed (hr=0x%08x); "
				 "retrying without it -- install the Graphics Tools optional feature to get D3D debug output",
				 (unsigned)hr);
		hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
							   kLevels, (UINT)(sizeof(kLevels) / sizeof(kLevels[0])),
							   D3D11_SDK_VERSION, &gDevice, &got, &gContext);
	}
#endif
	if (FAILED(hr))
	{
		// IsAvailable() said yes moments ago, so reaching here means something
		// changed underneath us. Nothing to fall back to at this point -- the
		// factory already committed -- so say so loudly and leave gDevice NULL;
		// every entry point below guards on it and the app runs without
		// rendering rather than crashing.
		LOGError("CRenderBackendD3D11::CreateRenderContext: D3D11CreateDevice failed (hr=0x%08x)", (unsigned)hr);
		return;
	}

	// --- the swapchain ----------------------------------------------------
	//
	// THE FORMAT IS DECIDED BY VID_IsHdrRequested() ALONE. Not by
	// CheckColorSpaceSupport, not by whether SetColorSpace1 succeeded, not by
	// whether a display has headroom.
	//
	// Two reasons, and the second is the one that bites. First, that is the
	// gate Metal applies (a bare `if (VID_IsHdrRequested())`), and
	// render_backend_hdr_surface asserts GetSurfaceFormat() against exactly
	// that on every backend -- keying it on anything else fails the test by
	// construction. Second, DXGI treats every R16G16B16A16_FLOAT back buffer as
	// scRGB (G10) whether or not SetColorSpace1 was ever called, so a flag
	// keyed on that call's HRESULT would, on any failure, leave an FP16
	// swapchain being fed sRGB-ENCODED values with the resolve set to identity:
	// S-4's washed-out bug, on the very path this stage exists for.
	//
	// FP16 flip-model swapchains are supported on all D3D11 hardware and DWM
	// composites scRGB on SDR outputs too, so there is nothing to veto.
	const bool wantHdr = VID_IsHdrRequested();
	gSwapChainFormat = wantHdr ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;

	IDXGIDevice *dxgiDevice = NULL;
	IDXGIAdapter *adapter = NULL;
	IDXGIFactory2 *factory = NULL;
	if (SUCCEEDED(gDevice->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgiDevice)) &&
		SUCCEEDED(dxgiDevice->GetAdapter(&adapter)) &&
		SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory)))
	{
		int pxW = 0, pxH = 0;
		SDL_GetWindowSizeInPixels(mainWindow, &pxW, &pxH);
		if (pxW <= 0) pxW = 1;
		if (pxH <= 0) pxH = 1;

		DXGI_SWAP_CHAIN_DESC1 sd = {};
		sd.Width  = (UINT)pxW;
		sd.Height = (UINT)pxH;
		sd.Format = gSwapChainFormat;
		sd.SampleDesc.Count = 1;
		sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		sd.BufferCount = 2;
		// FLIP MODEL IS REQUIRED, not preferred: the HDR colour-space calls have
		// no effect at all on a bitblt swapchain, so a DISCARD/SEQUENTIAL
		// swapchain would make every SetColorSpace1 succeed and change nothing.
		sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
		sd.Scaling = DXGI_SCALING_NONE;

		hr = factory->CreateSwapChainForHwnd(gDevice, gHwnd, &sd, NULL, NULL, &gSwapChain);
		if (FAILED(hr))
		{
			LOGError("CRenderBackendD3D11: CreateSwapChainForHwnd failed (hr=0x%08x, format=%d)",
					 (unsigned)hr, (int)gSwapChainFormat);
			// AND FORGET THE FORMAT WE ASKED FOR. It was assigned before the
			// attempt, and GetSurfaceFormat() reads it -- so leaving it would
			// have render_backend_hdr_surface pass, reporting an HDR surface,
			// on a backend that has no surface at all.
			gSwapChainFormat = DXGI_FORMAT_UNKNOWN;
		}
		else
		{
			gSwapChainW = pxW;
			gSwapChainH = pxH;

			// SDL OWNS FULLSCREEN FOR THIS WINDOW, and DXGI does not know that.
			// Left alone, DXGI installs its own Alt+Enter handler and drives a
			// fullscreen transition SDL never learns about -- while this app is
			// fullscreen-FIRST and drives it through SDL_SetWindowFullscreen.
			// DXGI_MWA_NO_WINDOW_CHANGES additionally stops it reacting to
			// window messages behind SDL's back. Classic first-week D3D11 bug.
			IDXGIFactory1 *parent = NULL;
			if (SUCCEEDED(gSwapChain->GetParent(__uuidof(IDXGIFactory1), (void **)&parent)) && parent != NULL)
			{
				parent->MakeWindowAssociation(gHwnd, DXGI_MWA_NO_ALT_ENTER | DXGI_MWA_NO_WINDOW_CHANGES);
				parent->Release();
			}

			// IDXGISwapChain3 for SetColorSpace1. Optional: absent on very old
			// Windows 10 builds, where HDR is not available anyway.
			if (FAILED(gSwapChain->QueryInterface(__uuidof(IDXGISwapChain3), (void **)&gSwapChain3)))
				gSwapChain3 = NULL;
		}
	}
	else
	{
		LOGError("CRenderBackendD3D11: could not reach IDXGIFactory2 from the device");
	}

	if (factory)    factory->Release();
	if (adapter)    adapter->Release();
	if (dxgiDevice) dxgiDevice->Release();

	LOGM("CRenderBackendD3D11: device created, swapchain %dx%d format=%d (hdrRequested=%s)",
		 gSwapChainW, gSwapChainH, (int)gSwapChainFormat, wantHdr ? "yes" : "no");
}

// ---------------------------------------------------------------------------
// Frame targets: the back-buffer view and the offscreen RGBA16F target
// ---------------------------------------------------------------------------

static void D3D11ReleaseFrameTargets()
{
	// EVERY back-buffer reference must be gone before ResizeBuffers, or it
	// fails with DXGI_ERROR_INVALID_CALL -- including the one the immediate
	// context is still holding as a bound render target, which is why the
	// caller also has to OMSetRenderTargets(0, ...) and Flush(). This is the
	// classic first D3D11 crash; it is written before the first window drag
	// rather than after.
	if (gBackBufferRTV) { gBackBufferRTV->Release(); gBackBufferRTV = NULL; }
}

static bool D3D11CreateBackBufferRTV()
{
	if (gSwapChain == NULL || gDevice == NULL)
		return false;
	ID3D11Texture2D *backBuffer = NULL;
	HRESULT hr = gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer);
	if (FAILED(hr) || backBuffer == NULL)
	{
		LOGError("CRenderBackendD3D11: swapchain GetBuffer failed (hr=0x%08x)", (unsigned)hr);
		return false;
	}
	hr = gDevice->CreateRenderTargetView(backBuffer, NULL, &gBackBufferRTV);
	backBuffer->Release();
	if (FAILED(hr))
	{
		LOGError("CRenderBackendD3D11: CreateRenderTargetView(backbuffer) failed (hr=0x%08x)", (unsigned)hr);
		return false;
	}
	return true;
}

static bool D3D11EnsureOffscreen(int w, int h)
{
	if (gDevice == NULL || w <= 0 || h <= 0)
		return false;
	if (gOffscreenTex != NULL && gOffscreenW == w && gOffscreenH == h)
		return true;

	if (gOffscreenSRV) { gOffscreenSRV->Release(); gOffscreenSRV = NULL; }
	if (gOffscreenRTV) { gOffscreenRTV->Release(); gOffscreenRTV = NULL; }
	if (gOffscreenTex) { gOffscreenTex->Release(); gOffscreenTex = NULL; }
	gOffscreenW = gOffscreenH = 0;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	// ALWAYS FP16 -- see the note on gOffscreenTex. An 8-bit offscreen would
	// clamp above-white before the resolve ever saw it.
	td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
	td.SampleDesc.Count = 1;
	td.Usage = D3D11_USAGE_DEFAULT;
	td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

	HRESULT hr = gDevice->CreateTexture2D(&td, NULL, &gOffscreenTex);
	if (FAILED(hr))
	{
		LOGError("CRenderBackendD3D11: offscreen CreateTexture2D %dx%d failed (hr=0x%08x)", w, h, (unsigned)hr);
		return false;
	}
	if (FAILED(gDevice->CreateRenderTargetView(gOffscreenTex, NULL, &gOffscreenRTV)) ||
		FAILED(gDevice->CreateShaderResourceView(gOffscreenTex, NULL, &gOffscreenSRV)))
	{
		LOGError("CRenderBackendD3D11: offscreen view creation failed");
		if (gOffscreenSRV) { gOffscreenSRV->Release(); gOffscreenSRV = NULL; }
		if (gOffscreenRTV) { gOffscreenRTV->Release(); gOffscreenRTV = NULL; }
		gOffscreenTex->Release(); gOffscreenTex = NULL;
		return false;
	}
	gOffscreenW = w;
	gOffscreenH = h;
	return true;
}

// RESIZE. CRenderBackend has no resize hook, so this runs at the top of
// NewFrame() -- the same place the Metal backend updates its drawableSize, and
// for the same reason: nothing is bound yet, and the logic lives in one place.
//
// WHERE THAT ACTUALLY RUNS is worth knowing: the engine's event filter renders
// a frame INLINE on SDL_EVENT_WINDOW_RESIZED, so during a live drag this
// executes inside SDL_PushEvent on the main thread's modal size loop. D3D11
// tolerates that (the pthread_main_np guard beside it is macOS-only). The point
// of doing it in NewFrame() is not to escape the filter.
static bool D3D11ResizeIfNeeded(int w, int h)
{
	if (gSwapChain == NULL || w <= 0 || h <= 0)
		return false;
	if (w == gSwapChainW && h == gSwapChainH && gBackBufferRTV != NULL)
		return true;

	// Order matters and every step is required: drop our RTV, unbind whatever
	// the context still holds, flush so the driver lets go, THEN resize.
	D3D11ReleaseFrameTargets();
	if (gContext != NULL)
	{
		ID3D11RenderTargetView *none[1] = { NULL };
		gContext->OMSetRenderTargets(1, none, NULL);
		gContext->Flush();
	}

	HRESULT hr = gSwapChain->ResizeBuffers(0, (UINT)w, (UINT)h, DXGI_FORMAT_UNKNOWN, 0);
	if (FAILED(hr))
	{
		LOGError("CRenderBackendD3D11: ResizeBuffers %dx%d failed (hr=0x%08x)", w, h, (unsigned)hr);
		// Try to put the old views back so the next frame is not blank.
		D3D11CreateBackBufferRTV();
		return false;
	}
	gSwapChainW = w;
	gSwapChainH = h;
	if (!D3D11CreateBackBufferRTV())
		return false;
	return D3D11EnsureOffscreen(w, h);
}

// ---------------------------------------------------------------------------
// Shaders, samplers and fixed state
// ---------------------------------------------------------------------------

// A placeholder bytecode header carries length 0. THE BACKEND REFUSES TO RUN
// on one rather than rendering nothing: the shaders were authored on a machine
// with no HLSL compiler, and until a Windows build regenerates them there is
// no resolve pass, which would mean a black window with a green log.
static bool D3D11ShadersArePlaceholders()
{
	return kResolveVSBytecodeLength == 0 || kResolvePSBytecodeLength == 0;
}

static bool D3D11CreateResolvePipeline()
{
	if (gDevice == NULL)
		return false;

	if (D3D11ShadersArePlaceholders())
	{
		LOGError("CRenderBackendD3D11: the embedded shader bytecode is a PLACEHOLDER (length 0). "
				 "Run tools/embed-hlsl-shaders.ps1 with fxc.exe on PATH and commit the generated "
				 "headers. The D3D11 backend cannot start.");
		return false;
	}

	HRESULT hr = gDevice->CreateVertexShader(kResolveVSBytecodeData, (SIZE_T)kResolveVSBytecodeLength, NULL, &gResolveVS);
	if (FAILED(hr)) { LOGError("CRenderBackendD3D11: CreateVertexShader(Resolve) failed (hr=0x%08x)", (unsigned)hr); return false; }
	hr = gDevice->CreatePixelShader(kResolvePSBytecodeData, (SIZE_T)kResolvePSBytecodeLength, NULL, &gResolvePS);
	if (FAILED(hr)) { LOGError("CRenderBackendD3D11: CreatePixelShader(Resolve) failed (hr=0x%08x)", (unsigned)hr); return false; }

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(SResolveConstants);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	hr = gDevice->CreateBuffer(&bd, NULL, &gResolveCB);
	if (FAILED(hr)) { LOGError("CRenderBackendD3D11: resolve constant buffer failed (hr=0x%08x)", (unsigned)hr); return false; }

	// POINT sampling: the offscreen target is created at exactly the
	// swapchain's size, so this is a 1:1 blit and any filtering would forfeit
	// the bit-exactness the SDR arm promises.
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	if (FAILED(gDevice->CreateSamplerState(&sd, &gPointSampler)))
		return false;

	// NO CULLING and NO DEPTH: the resolve draws one full-screen triangle whose
	// winding nobody should have to reason about, and ImGui leaves whatever
	// state it likes bound. Setting all three explicitly means the resolve does
	// not depend on what the last draw happened to leave behind.
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	rd.ScissorEnable = FALSE;
	if (FAILED(gDevice->CreateRasterizerState(&rd, &gResolveRasterizer)))
		return false;

	// BLENDING OFF. The resolve REPLACES the back buffer; blending it would mix
	// in whatever FLIP_DISCARD left there, which is undefined by definition.
	//
	// EVERY FIELD MUST HOLD A VALID ENUMERANT EVEN WHERE THE Enable IS FALSE.
	// D3D11 range-checks the WHOLE descriptor at Create time and zero is out of
	// range for D3D11_BLEND (1..19), D3D11_BLEND_OP (1..5),
	// D3D11_COMPARISON_FUNC (1..8) and D3D11_STENCIL_OP (1..8) alike -- so a
	// `= {}` descriptor with only the Enable set is REJECTED with E_INVALIDARG.
	// The proof is in this tree: imgui_impl_dx11.cpp ZeroMemory's its own
	// depth-stencil desc and then writes six fields it will never use, with
	// DepthEnable and StencilEnable both false. Nobody writes six redundant
	// lines for fun.
	//
	// The cost of getting it wrong is not an error anyone sees: the resolve
	// pipeline is simply never created, InitRenderPipeline logs and carries on,
	// the offscreen fills correctly, every readback test passes -- and the
	// window is black.
	D3D11_BLEND_DESC bl = {};
	bl.AlphaToCoverageEnable = FALSE;
	bl.RenderTarget[0].BlendEnable = FALSE;
	bl.RenderTarget[0].SrcBlend       = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlend      = D3D11_BLEND_ZERO;
	bl.RenderTarget[0].BlendOp        = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
	bl.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	if (FAILED(gDevice->CreateBlendState(&bl, &gResolveBlend)))
		return false;

	D3D11_DEPTH_STENCIL_DESC ds = {};
	ds.DepthEnable = FALSE;
	ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	ds.DepthFunc = D3D11_COMPARISON_ALWAYS;      // 0 is not a valid COMPARISON_FUNC
	ds.StencilEnable = FALSE;
	ds.StencilReadMask  = D3D11_DEFAULT_STENCIL_READ_MASK;
	ds.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
	ds.FrontFace.StencilFailOp = ds.FrontFace.StencilDepthFailOp =
		ds.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	ds.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	ds.BackFace = ds.FrontFace;
	if (FAILED(gDevice->CreateDepthStencilState(&ds, &gResolveDepth)))
		return false;

	return true;
}

// OUR OWN SAMPLERS, NOT imgui_impl_dx11's, and there are three reasons -- the
// third only visible by reading the vendored file.
//
//  1. GL PARITY. UpdateTextureLinearScaling() sets MIN=LINEAR, MAG=NEAREST for
//     a non-linear image: nearest MAGNIFICATION only. ImGui's "nearest" sampler
//     is D3D11_FILTER_MIN_MAG_MIP_POINT -- point on minification too -- so a
//     downscaled bitmap font would alias here and not on OpenGL.
//  2. MIPS. The compressed-atlas path (KTX2/UASTC -> BC7) depends on
//     mip-linear, which ImGui's point sampler does not have.
//  3. BOTH of ImGui's samplers set MinLOD = MaxLOD = 0.0f from one shared
//     descriptor -- so its LINEAR one never samples a mip either. Reusing it as
//     "the safe default" would silently pin every mipped texture to level 0.
//     Recorded in MTENGINE_PATCHES.md as an upgrade hazard.
static bool D3D11CreateSamplers()
{
	if (gDevice == NULL)
		return false;

	D3D11_SAMPLER_DESC sd = {};
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = D3D11_FLOAT32_MAX;   // NOT 0: see reason 3 above

	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	if (FAILED(gDevice->CreateSamplerState(&sd, &gSamplerLinear)))
		return false;

	// The one that matters: point magnification, LINEAR minification, and mips
	// -- exactly what glTexParameteri(MIN=LINEAR, MAG=NEAREST) does.
	sd.Filter = D3D11_FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR;
	if (FAILED(gDevice->CreateSamplerState(&sd, &gSamplerMagNearest)))
		return false;

	return true;
}

void CRenderBackendD3D11::InitRenderPipeline()
{
	// RE-ENTRY. There is exactly ONE caller today (VID_Main::VID_Init) and no
	// re-init path exists, so this guard is inert -- it is here because the
	// ImGui halves below are not idempotent, and a future resize- or
	// device-lost path that calls this twice would trip
	// IM_ASSERT("Already initialized a platform backend!") rather than do
	// anything useful. Keyed on the PLATFORM flag, which is the only one set on
	// every path through this function.
	if (gImGuiPlatformInitialised)
		return;

	if (gDevice == NULL || gContext == NULL)
	{
		// THE PLATFORM HALF STILL RUNS. This is the "no device" path, and its
		// documented promise is that the app runs without rendering rather than
		// crashing -- but VID_Main calls ImGui_ImplSDL3_NewFrame() every frame
		// and ImGui_ImplSDL3_Shutdown() on exit UNCONDITIONALLY, and both
		// dereference their backend data immediately after an IM_ASSERT that
		// compiles OUT in Release. Returning here without initialising the SDL3
		// platform backend therefore turns "no device" into a null dereference
		// on frame 1, in the only configuration we ship.
		//
		// InitForOther, not InitForD3D: the renderer half genuinely is absent,
		// and InitForOther is what SDL3's backend offers for exactly this
		// case -- input, windowing and timing, no renderer coupling.
		// CRenderBackendOpenGL4 has always initialised its platform half
		// unconditionally; this makes D3D11 agree with it.
		LOGError("CRenderBackendD3D11::InitRenderPipeline: no device -- running with no rendering; the platform backend still initialises");
		ImGui_ImplSDL3_InitForOther(mainWindow);
		gImGuiPlatformInitialised = true;
		// gImGuiDX11Initialised stays FALSE on purpose: there is no DX11
		// backend to shut down, and Shutdown() keys the DX11 teardown off it.
		return;
	}

	if (!D3D11CreateBackBufferRTV())
		LOGError("CRenderBackendD3D11::InitRenderPipeline: no back-buffer view");
	if (!D3D11EnsureOffscreen(gSwapChainW, gSwapChainH))
		LOGError("CRenderBackendD3D11::InitRenderPipeline: no offscreen target");
	if (!D3D11CreateResolvePipeline())
		LOGError("CRenderBackendD3D11::InitRenderPipeline: the resolve pass is unavailable -- nothing will reach the screen");
	if (!D3D11CreateSamplers())
		LOGError("CRenderBackendD3D11::InitRenderPipeline: sampler creation failed");

	// THE COLOUR-SPACE CALLS ARE EVIDENCE, NOT A GATE. They are made and their
	// results logged for Task B4's numbers, and NOTHING reads them back:
	// uSwapchainIsLinear, GetSurfaceFormat() and the resolve mode are all
	// functions of the swapchain FORMAT and of nothing else. DXGI already
	// treats an FP16 back buffer as scRGB whether or not SetColorSpace1 was
	// called, so keying anything on this HRESULT would mean that on failure we
	// wrote sRGB-encoded values into a linear buffer.
	if (gSwapChain3 != NULL && gSwapChainFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
	{
		UINT support = 0;
		HRESULT hrCheck = gSwapChain3->CheckColorSpaceSupport(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709, &support);
		HRESULT hrSet = gSwapChain3->SetColorSpace1(DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
		LOGM("CRenderBackendD3D11: scRGB colour space -- CheckColorSpaceSupport hr=0x%08x support=0x%x, SetColorSpace1 hr=0x%08x",
			 (unsigned)hrCheck, (unsigned)support, (unsigned)hrSet);
	}

	ImGui_ImplDX11_Init(gDevice, gContext);
	ImGui_ImplSDL3_InitForD3D(mainWindow);
	gImGuiDX11Initialised = true;
	gImGuiPlatformInitialised = true;
}

void CRenderBackendD3D11::CreateFontsTexture()
{
	// Nothing to do: ImGui 1.93 (IMGUI_HAS_TEXTURES) owns font-atlas textures
	// and drives ImGui_ImplDX11_UpdateTexture itself. The Metal backend's twin
	// is an empty stub saying the same thing.
}

// ---------------------------------------------------------------------------
// The frame
// ---------------------------------------------------------------------------

// Bind the frame's offscreen target and remember it, so a CRenderTarget that
// opens a pass mid-frame can put it back. One place, so the two callers cannot
// disagree about what "the frame's target" is.
static void D3D11BindFrameTarget()
{
	if (gContext == NULL || gOffscreenRTV == NULL)
		return;
	gContext->OMSetRenderTargets(1, &gOffscreenRTV, NULL);
	D3D11_VIEWPORT vp = {};
	vp.TopLeftX = 0.0f;
	vp.TopLeftY = 0.0f;
	vp.Width  = (float)gOffscreenW;
	vp.Height = (float)gOffscreenH;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	gContext->RSSetViewports(1, &vp);
}

void CRenderBackendD3D11::NewFrame(ImVec4 clearColor)
{
	gFrameValid = false;
	if (gDevice == NULL || gContext == NULL)
		return;

	int w = 0, h = 0;
	SDL_GetWindowSizeInPixels(mainWindow, &w, &h);
	if (w <= 0 || h <= 0)
		return;   // hidden window, early frame, or mid-resize: nothing useful to draw

	if (!D3D11ResizeIfNeeded(w, h))
		return;
	if (!D3D11EnsureOffscreen(w, h))
		return;

	// POLL, NEVER LATCH. SDL updates these when the window moves to another
	// monitor or the user drags the Windows SDR-brightness slider, and raises
	// SDL_EVENT_WINDOW_HDR_STATE_CHANGED as it does. Reading them here rather
	// than reacting to that event means one code path and no chance of missing
	// an event during a frame we skipped -- the same "poll, never latch" rule
	// S-3 recorded for macOS EDR headroom.
	{
		SDL_PropertiesID props = SDL_GetWindowProperties(mainWindow);
		gHdrEnabled    = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_HDR_ENABLED_BOOLEAN, false);
		gSdrWhiteLevel = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_SDR_WHITE_LEVEL_FLOAT, 1.0f);
		gHdrHeadroom   = SDL_GetFloatProperty(props, SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, 1.0f);
		// A zero or negative white level would black the screen out through the
		// resolve's multiply. SDL should never report one; the guard costs
		// nothing and the failure it prevents is total.
		if (!(gSdrWhiteLevel > 0.0f))
			gSdrWhiteLevel = 1.0f;
		if (!(gHdrHeadroom >= 1.0f))
			gHdrHeadroom = 1.0f;

		// LOG ON CHANGE ONLY, not every frame -- this poll runs every NewFrame().
		// S-6 Task B4/B5 ask for these numbers on record (SDL white-level float,
		// SDL headroom float): this is where the SDR-brightness slider test's
		// evidence comes from -- move the slider with the app visible and the
		// two lines below should print a new gSdrWhiteLevel while the app's own
		// UI brightness moves with it.
		static float sLoggedWhiteLevel = -1.0f;
		static float sLoggedHeadroom = -1.0f;
		static bool  sLoggedHdrEnabled = false;
		static bool  sEverLogged = false;
		if (!sEverLogged || gSdrWhiteLevel != sLoggedWhiteLevel ||
			gHdrHeadroom != sLoggedHeadroom || gHdrEnabled != sLoggedHdrEnabled)
		{
			LOGM("CRenderBackendD3D11: SDL window HDR properties -- hdrEnabled=%s "
				 "sdrWhiteScRgb=%.4f headroom=%.4f",
				 gHdrEnabled ? "true" : "false", gSdrWhiteLevel, gHdrHeadroom);
			sLoggedWhiteLevel = gSdrWhiteLevel;
			sLoggedHeadroom = gHdrHeadroom;
			sLoggedHdrEnabled = gHdrEnabled;
			sEverLogged = true;
		}
	}

	gFrameIsHeadless = gHeadlessMode;

	D3D11BindFrameTarget();

	// PREMULTIPLIED, matching what the Metal backend passes to its clear colour
	// -- the engine's ImVec4 clear colour carries alpha and both backends must
	// clear to the same thing or a capture comparison across backends drifts.
	const float clear[4] = { clearColor.x * clearColor.w,
							 clearColor.y * clearColor.w,
							 clearColor.z * clearColor.w,
							 clearColor.w };
	gContext->ClearRenderTargetView(gOffscreenRTV, clear);

	ImGui_ImplDX11_NewFrame();
	gFrameValid = true;
}

// The resolve. One full-screen triangle from the offscreen target into the back
// buffer, running in BOTH modes -- see Shaders/Resolve.hlsl for why the SDR arm
// is an exact identity rather than "the same maths at scale 1.0".
static void D3D11RunResolvePass()
{
	if (gContext == NULL || gBackBufferRTV == NULL || gOffscreenSRV == NULL ||
		gResolveVS == NULL || gResolvePS == NULL || gResolveCB == NULL)
		return;

	SResolveConstants c;
	c.sdrWhiteScRgb = gSdrWhiteLevel;
	// A FUNCTION OF THE SWAPCHAIN FORMAT AND NOTHING ELSE. Not of
	// SetColorSpace1's HRESULT, not of whether the display has headroom: DXGI
	// treats every FP16 back buffer as scRGB whether or not that call was made,
	// so keying this on an API return code would write ENCODED values into a
	// LINEAR buffer whenever the call failed.
	c.swapchainIsLinear = (gSwapChainFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 1 : 0;
	c.pad[0] = c.pad[1] = 0.0f;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(gContext->Map(gResolveCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, &c, sizeof(c));
		gContext->Unmap(gResolveCB, 0);
	}

	// EVERY PIECE OF STATE SET EXPLICITLY. ImGui_ImplDX11_RenderDrawData has
	// just run and restored what IT saved, which is not the same as leaving the
	// pipeline in a state this pass can assume -- and the resolve's promise of
	// a bit-exact identity on SDR depends on the blend state being REPLACE and
	// the depth test being off. Both vertex shaders write z = 0 and D3D11's
	// default depth state is DepthEnable = TRUE with COMPARISON_LESS.
	gContext->OMSetRenderTargets(1, &gBackBufferRTV, NULL);

	D3D11_VIEWPORT vp = {};
	vp.Width  = (float)gSwapChainW;
	vp.Height = (float)gSwapChainH;
	vp.MinDepth = 0.0f;
	vp.MaxDepth = 1.0f;
	gContext->RSSetViewports(1, &vp);

	gContext->IASetInputLayout(NULL);        // positions come from SV_VertexID
	// A REAL one-element array rather than IASetVertexBuffers(0, 0, NULL, NULL,
	// NULL): NumBuffers = 0 with three NULL arrays should be a no-op, but the
	// documented-safe idiom is this one and the debug layer is known to
	// complain about NULL pStrides/pOffsets. In code nobody can run, prefer the
	// form that cannot be wrong.
	{
		ID3D11Buffer *noVB[1] = { NULL };
		UINT zero[1] = { 0 };
		gContext->IASetVertexBuffers(0, 1, noVB, zero, zero);
	}
	gContext->IASetIndexBuffer(NULL, DXGI_FORMAT_UNKNOWN, 0);
	gContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
	gContext->VSSetShader(gResolveVS, NULL, 0);
	gContext->PSSetShader(gResolvePS, NULL, 0);
	gContext->GSSetShader(NULL, NULL, 0);
	gContext->HSSetShader(NULL, NULL, 0);
	gContext->DSSetShader(NULL, NULL, 0);
	gContext->CSSetShader(NULL, NULL, 0);
	gContext->PSSetConstantBuffers(0, 1, &gResolveCB);
	gContext->PSSetShaderResources(0, 1, &gOffscreenSRV);
	gContext->PSSetSamplers(0, 1, &gPointSampler);
	gContext->RSSetState(gResolveRasterizer);
	const float blendFactor[4] = { 0, 0, 0, 0 };
	gContext->OMSetBlendState(gResolveBlend, blendFactor, 0xFFFFFFFF);
	gContext->OMSetDepthStencilState(gResolveDepth, 0);

	gContext->Draw(3, 0);

	// UNBIND THE OFFSCREEN SRV. It is a render target again next frame, and
	// D3D11 refuses to bind a resource as both input and output -- it would
	// silently drop the RTV binding and log a debug-layer warning, so the next
	// frame would draw nowhere.
	ID3D11ShaderResourceView *none[1] = { NULL };
	gContext->PSSetShaderResources(0, 1, none);
}

void CRenderBackendD3D11::PresentFrameBuffer(ImVec4 clearColor)
{
	if (!gFrameValid || gContext == NULL)
		return;

	// THE SCISSOR MUST FIT THE ATTACHMENT, and only we can guarantee it.
	// ImGui clamps every clip rect to DisplaySize * FramebufferScale -- ImGui's
	// OWN framebuffer, in points -- while the attachment is sized in PIXELS
	// from SDL. Two sources, two moments, and they diverge during a LIVE RESIZE
	// because the engine's event filter re-enters rendering from inside the
	// resize. D3D11 does not abort the way Metal does, but an out-of-range
	// scissor still drops draws, so the same clamp runs here.
	{
		ImDrawData *dd = ImGui::GetDrawData();
		if (dd != NULL)
		{
#if defined(DEBUG) || defined(_DEBUG)
			VID_ReportBadScissors(dd, gOffscreenW, gOffscreenH, "pre-clamp");
#endif
			VID_ClampDrawDataToAttachment(dd, gOffscreenW, gOffscreenH);
#if defined(DEBUG) || defined(_DEBUG)
			const int stillBad = VID_ReportBadScissors(dd, gOffscreenW, gOffscreenH, "POST-clamp");
			if (stillBad > 0)
				LOGError("VID scissor: %d rect(s) still bad AFTER clamping to the attachment", stillBad);
#endif
		}
	}

	ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

	ImGuiIO &io = ImGui::GetIO();
	if (io.ConfigFlags & ImGuiConfigFlags_ViewportsEnable)
	{
		// SECONDARY VIEWPORTS ARE SDR, BY DESIGN. imgui_impl_dx11 gives each
		// platform window its own R8G8B8A8 swapchain and draws straight into it
		// -- no offscreen target, no resolve, no HDR. They hold tool windows,
		// not the image, and the alternative is a resolve pass per floating
		// panel. If that ever changes, Renderer_CreateWindow /
		// Renderer_RenderWindow are where it would go.
		ImGui::UpdatePlatformWindows();
		ImGui::RenderPlatformWindowsDefault();
	}

	D3D11RunResolvePass();

	// THE SWAPCHAIN-VS-OFFSCREEN READBACK IS SERVICED HERE, and nowhere else.
	//
	// AFTER THE RESOLVE AND BEFORE PRESENT, and both halves of that are load
	// bearing.
	//
	// After the resolve, because this is the only moment in the frame at which
	// the offscreen target and the back buffer hold the SAME picture.
	// MT_ShaderProbeRender() runs before ImGui::Render() and before the
	// resolve, where the offscreen still holds only NewFrame()'s clear and the
	// back buffer holds the PREVIOUS frame -- a comparison made there would
	// fail on correct code, or, if the sampled rect happened to be flat black,
	// PASS on a resolve that decoded every pixel. That is exactly the false
	// green the probe exists to prevent.
	//
	// BEFORE Present, because DXGI_SWAP_EFFECT_FLIP_DISCARD means what it says:
	// after Present the back buffer's contents are DISCARDED and reading them
	// is undefined. Headless skips Present so it would happen to work there --
	// which is worse, not better, because the suites run headless and the
	// windowed case would rot unnoticed.
	MT_ShaderProbeServiceSwapchainReadback();

	// HEADLESS SKIPS **PRESENT ALONE**, not the resolve.
	//
	// The window is created SDL_WINDOW_HIDDEN and VID_PostInit only shows it
	// when not headless, so Present would return DXGI_STATUS_OCCLUDED and
	// FLIP_DISCARD would leave the back buffer undefined afterwards. But the
	// resolve still RUNS, into that back buffer, because that is exactly what
	// the swapchain-vs-offscreen readback test reads -- and reading it before
	// any Present is what makes the contents defined.
	if (!gFrameIsHeadless && gSwapChain != NULL)
	{
		HRESULT hr = gSwapChain->Present(1, 0);
		if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
		{
			// A driver update, a GPU hang, or a laptop switching GPUs. Nothing
			// this stage can recover from -- device-lost recovery means
			// rebuilding every texture in the app -- so say so precisely rather
			// than dying somewhere further along with no explanation.
			LOGError("CRenderBackendD3D11: DEVICE REMOVED/RESET on Present (hr=0x%08x, reason=0x%08x). "
					 "Rendering is over for this process; restart the application.",
					 (unsigned)hr, (unsigned)(gDevice ? gDevice->GetDeviceRemovedReason() : 0));
			gFrameValid = false;
		}
	}

}

void CRenderBackendD3D11::ApplyDisplayColorGamut(VID_DisplayColorGamut gamut)
{
	// scRGB's PRIMARIES ARE FIXED AT BT.709. There is no extended-Display-P3
	// analogue to switch to the way the Metal backend does, so anything other
	// than sRGB is refused rather than silently ignored.
	//
	// Nothing in the four repos currently writes a non-sRGB render gamut on
	// Windows -- VID_GetMainWindowRenderColorGamut() returns a static that only
	// macOS code sets -- so this is a guard against the day one appears, not a
	// fix for a live bug.
	if (gamut != VID_DISPLAY_COLOR_GAMUT_SRGB && gamut != VID_DISPLAY_COLOR_GAMUT_UNKNOWN)
	{
		LOGError("CRenderBackendD3D11::ApplyDisplayColorGamut: only sRGB is available on this backend "
				 "(scRGB's primaries are fixed at BT.709); ignoring request for gamut %d", (int)gamut);
	}
}

// ---------------------------------------------------------------------------
// Shutdown -- ORDER MATTERS, and Metal is NOT the precedent
// ---------------------------------------------------------------------------
//
// VID_Shutdown() calls this before ImGui_ImplSDL3_Shutdown() and
// ImGui::DestroyContext(). Metal's implementation is a single
// ImGui_ImplMetal_Shutdown() because ARC owns everything else; D3D11 owns
// nothing for you, and a leaked device can fault or hang at exit -- noisily,
// in an app that has _exit()-style teardown paths where it would be hard to
// attribute.
void CRenderBackendD3D11::Shutdown()
{
	// 1. ImGui first: it holds device objects of its own (shaders, buffers,
	//    the font texture) and must release them while the device is alive.
	//    GUARDED -- see gImGuiDX11Initialised.
	if (gImGuiDX11Initialised)
	{
		ImGui_ImplDX11_Shutdown();
		gImGuiDX11Initialised = false;
	}
	// The PLATFORM half is torn down by VID_Main (ImGui_ImplSDL3_Shutdown,
	// unconditional), not here -- this only forgets that it happened, so a
	// re-created backend initialises cleanly.
	gImGuiPlatformInitialised = false;

	// 2. Everything we made.
	if (gResolveDepth)      { gResolveDepth->Release();      gResolveDepth = NULL; }
	if (gResolveBlend)      { gResolveBlend->Release();      gResolveBlend = NULL; }
	if (gResolveRasterizer) { gResolveRasterizer->Release(); gResolveRasterizer = NULL; }
	if (gPointSampler)      { gPointSampler->Release();      gPointSampler = NULL; }
	if (gResolveCB)         { gResolveCB->Release();         gResolveCB = NULL; }
	if (gResolvePS)         { gResolvePS->Release();         gResolvePS = NULL; }
	if (gResolveVS)         { gResolveVS->Release();         gResolveVS = NULL; }
	if (gSamplerMagNearest) { gSamplerMagNearest->Release(); gSamplerMagNearest = NULL; }
	if (gSamplerLinear)     { gSamplerLinear->Release();     gSamplerLinear = NULL; }
	if (gOffscreenSRV)      { gOffscreenSRV->Release();      gOffscreenSRV = NULL; }
	if (gOffscreenRTV)      { gOffscreenRTV->Release();      gOffscreenRTV = NULL; }
	if (gOffscreenTex)      { gOffscreenTex->Release();      gOffscreenTex = NULL; }
	D3D11ReleaseFrameTargets();

	// 3. Let go of everything the context still references, and make sure the
	//    driver has actually seen it, before the swapchain goes.
	if (gContext != NULL)
	{
		gContext->OMSetRenderTargets(0, NULL, NULL);
		gContext->ClearState();
		gContext->Flush();
	}

	// 4. The swapchain -- and NEVER while it is in fullscreen: releasing a
	//    fullscreen swapchain is undefined. We never call SetFullscreenState
	//    (SDL owns fullscreen, and MakeWindowAssociation stops DXGI doing it
	//    behind our back), so this is belt and braces for a future that changes
	//    that.
	if (gSwapChain3 != NULL) { gSwapChain3->Release(); gSwapChain3 = NULL; }
	if (gSwapChain != NULL)
	{
		BOOL fullscreen = FALSE;
		if (SUCCEEDED(gSwapChain->GetFullscreenState(&fullscreen, NULL)) && fullscreen)
			gSwapChain->SetFullscreenState(FALSE, NULL);
		gSwapChain->Release();
		gSwapChain = NULL;
	}

	// 5. Context, then device. In that order.
	if (gContext != NULL) { gContext->Release(); gContext = NULL; }
	if (gDevice  != NULL) { gDevice->Release();  gDevice  = NULL; }

	// 6. And forget the surface description, so nothing answers a query about a
	//    surface that no longer exists.
	// UNKNOWN, matching the creation-failure path 680 lines above. Resetting to
	// a POSITIVE format would have Shutdown() and "we never got a surface"
	// answer differently about the same condition.
	gSwapChainFormat = DXGI_FORMAT_UNKNOWN;
	gSwapChainW = gSwapChainH = 0;
	gOffscreenW = gOffscreenH = 0;
	gHwnd = NULL;
	gFrameValid = false;
	gHdrEnabled = false;
	gSdrWhiteLevel = 1.0f;
	gHdrHeadroom = 1.0f;
}

// ---------------------------------------------------------------------------
// Surface description
// ---------------------------------------------------------------------------

ERenderSurfaceFormat CRenderBackendD3D11::GetSurfaceFormat()
{
	return (gSwapChainFormat == DXGI_FORMAT_R16G16B16A16_FLOAT)
		? RENDER_SURFACE_RGBA16F : RENDER_SURFACE_RGBA8;
}

bool CRenderBackendD3D11::GetSurfaceIsExtendedRange()
{
	// The swapchain can CARRY above-white values AND the display can show them.
	// Both halves are needed: CheckColorSpaceSupport says yes for FP16/G10 on
	// SDR displays too, so an FP16 swapchain alone proves nothing about whether
	// the extra range reaches anybody's eyes -- and this query is what the app
	// uses to decide whether to spend twice the memory on a float image.
	return (gSwapChainFormat == DXGI_FORMAT_R16G16B16A16_FLOAT) && gHdrEnabled;
}

bool CRenderBackendD3D11::GetSurfaceIsLinearColorSpace()
{
	// FALSE. ON AN SDR AND ON AN scRGB SWAPCHAIN ALIKE. Read this before
	// "fixing" it -- the swapchain really is linear when HDR is on, and this
	// still answers false.
	//
	// The query does not mean "is the swapchain linear". It means "what
	// encoding must the values we WRITE be in", and its consumers act on that
	// reading: CVideoPlayer copies it into SVideoHdrOutput.surfaceIsLinear, on
	// which both YUV shaders SKIP the surface encode and write linear light;
	// PC_ResidentFormat copies it into PCSurfaceEncoding.isLinear, on which
	// PhotoCruise's photo transform becomes the identity.
	//
	// Every one of those producers draws into the OFFSCREEN target, which is
	// extended-sRGB-ENCODED exactly like the macOS layer, and Resolve.hlsl
	// decodes it afterwards. Answer true and HDR video and float photos enter
	// linear, get decoded a SECOND time -- 0.5 becomes 0.21 -- and come out
	// crushed, on precisely the content this stage exists for, while the UI
	// looks fine. The swapchain's linearity is the resolve pass's private
	// knowledge and must not leak through here.
	return false;
}

float CRenderBackendD3D11::GetDisplayHdrHeadroom()
{
	// SDL_PROP_WINDOW_HDR_HEADROOM_FLOAT, refreshed every NewFrame. SDL
	// guarantees 1.0 whenever HDR is off, which is exactly the value the
	// tone-map wants, so there is no special case here.
	return gHdrHeadroom;
}

void CRenderBackendD3D11::SetSurfaceEdrMetadata(float maxComponent)
{
	// A LOGGED NO-OP IN S-6, and deliberately so.
	//
	// IDXGISwapChain4::SetHDRMetaData describes HDR10 MASTERING metadata, which
	// is about PQ-encoded BT.2020 content. This swapchain is scRGB. S-5
	// MEASURED what happens when that metadata is applied to a non-PQ surface
	// on macOS: above-white and SDR white render IDENTICALLY -- it destroys the
	// exact distinction a float pipeline exists to produce. The existing test
	// asserts !GetSurfaceHasEdrMetadata() after SetSurfaceEdrMetadata(4.0) on
	// EVERY backend, so a real call here would fail it.
	//
	// The plumbing is kept rather than deleted because a PQ 10-bit swapchain is
	// a plausible later stage, and that is where SetHDRMetaData belongs.
	(void)maxComponent;
}

bool CRenderBackendD3D11::GetSurfaceHasEdrMetadata()
{
	return false;   // never set -- see SetSurfaceEdrMetadata
}

EImageGpuFormat CRenderBackendD3D11::GetPreferredCompressedFormat()
{
	// A SCOPE DECISION, NOT A CAPABILITY ONE. Every D3D11 device supports BC7,
	// but reporting it obliges CreateTexture() to take the block-padded,
	// mip-chained upload arm that S-6 does not implement -- and a format we
	// claim but cannot upload produces a plausible-looking garbage texture
	// rather than an error.
	return IMG_GPU_UNCOMPRESSED;
}

bool CRenderBackendD3D11::SupportsTextureFormat(ERenderTextureFormat fmt)
{
	// Both, honestly. R16G16B16A16_FLOAT is a first-class texture format on
	// every D3D11 device. Following OpenGL's precedent of reporting the real
	// capability even when the app's own gate refuses to use it.
	return fmt == RENDER_TEXTURE_RGBA8 || fmt == RENDER_TEXTURE_RGBA16F;
}

void *CRenderBackendD3D11::GetD3DDevice()        { return gDevice; }
void *CRenderBackendD3D11::GetD3DDeviceContext() { return gContext; }

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

// Create a 2D texture and its SRV in one step, and hand back ONLY the SRV --
// the handle convention. The texture reference is dropped immediately: the SRV
// holds one of its own, so the resource lives exactly as long as the handle.
static ID3D11ShaderResourceView *D3D11MakeTexture(DXGI_FORMAT fmt, int w, int h,
												  const void *pixels, int bytesPerPixel)
{
	if (gDevice == NULL || w <= 0 || h <= 0)
		return NULL;

	D3D11_TEXTURE2D_DESC td = {};
	td.Width = (UINT)w;
	td.Height = (UINT)h;
	td.MipLevels = 1;
	td.ArraySize = 1;
	td.Format = fmt;
	td.SampleDesc.Count = 1;
	// DYNAMIC + CPU_ACCESS_WRITE, not DEFAULT + UpdateSubresource: video planes
	// are rewritten every frame and the map/discard path is what that is for.
	// Still textures pay one extra copy at creation, which is nothing.
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = pixels;
	init.SysMemPitch = (UINT)(w * bytesPerPixel);

	ID3D11Texture2D *tex = NULL;
	HRESULT hr = gDevice->CreateTexture2D(&td, pixels ? &init : NULL, &tex);
	if (FAILED(hr) || tex == NULL)
	{
		LOGError("CRenderBackendD3D11: CreateTexture2D %dx%d fmt=%d failed (hr=0x%08x)", w, h, (int)fmt, (unsigned)hr);
		return NULL;
	}
	ID3D11ShaderResourceView *srv = NULL;
	hr = gDevice->CreateShaderResourceView(tex, NULL, &srv);
	tex->Release();
	if (FAILED(hr))
	{
		LOGError("CRenderBackendD3D11: CreateShaderResourceView failed (hr=0x%08x)", (unsigned)hr);
		return NULL;
	}
	return srv;
}

// Upload into a DYNAMIC texture, row by row. THE ROW STRIDE IS NOT THE ROW
// WIDTH: D3D11 chooses its own pitch and Map reports it, so copying
// w*bytesPerPixel*h bytes in one memcpy writes rows into the middle of other
// rows on any texture whose width is not the driver's preferred multiple. That
// produces plausible-looking diagonal garbage rather than an error -- the same
// shape of bug the Metal backend documents for bytesPerRow.
static void D3D11UploadRows(ID3D11ShaderResourceView *srv, const void *data,
							int w, int h, int srcPitch, int bytesPerPixel)
{
	if (gContext == NULL || srv == NULL || data == NULL)
		return;
	ID3D11Resource *res = NULL;
	srv->GetResource(&res);
	if (res == NULL)
		return;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(gContext->Map(res, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		const int rowBytes = w * bytesPerPixel;
		const int src = (srcPitch > 0) ? srcPitch : rowBytes;
		const unsigned char *s = (const unsigned char *)data;
		unsigned char *d = (unsigned char *)mapped.pData;
		for (int y = 0; y < h; y++)
			memcpy(d + (size_t)y * mapped.RowPitch, s + (size_t)y * src, (size_t)rowBytes);
		gContext->Unmap(res, 0);
	}
	res->Release();
}

void CRenderBackendD3D11::CreateTexture(CSlrImage *image)
{
	if (image == NULL || image->loadImageData == NULL)
	{
		LOGError("CRenderBackendD3D11::CreateTexture: image is NULL");
		return;
	}

	// SOURCE DIMENSIONS COME FROM loadImageData, NOT rasterWidth/rasterHeight.
	//
	// rasterWidth/Height is the POT-PADDED texture size, while the buffer
	// getResultDataForUpload() returns is only padded on some paths --
	// LoadImageForRebinding hands over the RAW decoded buffer at its original
	// size. Uploading rasterWidth*rasterHeight from that reads off the end.
	// Metal's driver kills the process for it; D3D11's Map/memcpy would happily
	// read past the allocation, which is worse.
	const int srcW = image->loadImageData->width;
	const int srcH = image->loadImageData->height;

	// NO COMPRESSED-TEXTURE BRANCH, unlike the Metal backend, and it is a
	// consequence of GetPreferredCompressedFormat() returning
	// IMG_GPU_UNCOMPRESSED: nothing asks CImageData for a BC7/ASTC mip chain,
	// so nothing arrives here with one. Should a KTX2 image reach this by some
	// other route, getResultDataForUpload() returns NULL and the branch below
	// logs "not uploadable" -- a wrong-looking image with an explanation, which
	// is the right failure for a format this stage deliberately does not claim.
	const bool isFloatTex = (image->residentFormat == RENDER_TEXTURE_RGBA16F);
	const int bpp = isFloatTex ? 8 : 4;

	unsigned char *pixels = image->loadImageData->getResultDataForUpload();
	if (pixels == NULL)
	{
		LOGError("CRenderBackendD3D11::CreateTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		image->boundFormat = RENDER_TEXTURE_RGBA8;   // nothing bound, so no bound format
		return;
	}

	ID3D11ShaderResourceView *srv =
		D3D11MakeTexture(isFloatTex ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM,
						 srcW, srcH, pixels, bpp);
	if (srv == NULL)
		return;

	image->boundFormat = image->residentFormat;
	image->texturePtr.store((void *)srv, std::memory_order_release);
}

void CRenderBackendD3D11::UpdateTextureLinearScaling(CSlrImage *image)
{
	// NOTHING TO DO, and that is the whole reason ImageNeedsSamplerOverride
	// exists. D3D11's filter is part of the SAMPLER, which is a per-draw
	// binding, not per-texture state the way glTexParameteri is. The filtering
	// decision is made at blit time by QueueSamplerForImage().
	(void)image;
}

void CRenderBackendD3D11::ReBindTexture(CSlrImage *image)
{
	if (image == NULL)
	{
		LOGError("CRenderBackendD3D11::ReBindTexture: image is NULL");
		return;
	}
	if (!image->isBound)
	{
		LOGError("CRenderBackendD3D11::ReBindTexture: image is not bound, creating");
		CreateTexture(image);
		return;
	}
	// A RESIDENT FORMAT CHANGE NEEDS A NEW TEXTURE, not an upload into the old
	// one: RGBA8 <-> RGBA16F changes the bytes per pixel, and writing 8-byte
	// pixels into a 4-byte allocation is an overrun.
	if (image->residentFormat != image->boundFormat)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

	ID3D11ShaderResourceView *srv =
		(ID3D11ShaderResourceView *)image->texturePtr.load(std::memory_order_acquire);
	if (srv == NULL || image->loadImageData == NULL)
		return;

	// Does the decoded buffer still FIT? A resize between bind and rebind
	// otherwise overruns. Same buffer-vs-raster distinction as CreateTexture,
	// and this is the path LoadImageForRebinding actually takes.
	ID3D11Resource *res = NULL;
	srv->GetResource(&res);
	ID3D11Texture2D *tex = NULL;
	if (res != NULL)
		res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
	if (res != NULL) res->Release();
	if (tex == NULL)
		return;
	D3D11_TEXTURE2D_DESC td = {};
	tex->GetDesc(&td);
	tex->Release();

	// EXACT DIMENSIONS, not "fits". D3D11_MAP_WRITE_DISCARD discards the WHOLE
	// subresource and hands back a fresh, UNINITIALISED allocation -- so any
	// row or column the upload does not write comes back as whatever the
	// driver's allocator had lying there. Metal's replaceRegion and GL's
	// glTexSubImage2D both PRESERVE untouched texels, so this is a D3D-only
	// divergence: a smaller rebind that "fits" would leave real garbage in the
	// margins, sampled by anything whose UVs outrun the new dimensions, and it
	// would look like a decoder bug.
	const int srcW = image->loadImageData->width;
	const int srcH = image->loadImageData->height;
	if ((UINT)srcW != td.Width || (UINT)srcH != td.Height)
	{
		DeleteTexture(image);
		CreateTexture(image);
		return;
	}

	unsigned char *pixels = image->loadImageData->getResultDataForUpload();
	if (pixels == NULL)
	{
		LOGError("CRenderBackendD3D11::ReBindTexture: image type %2.2x is not uploadable",
				 image->loadImageData->getImageType());
		return;
	}
	const int bpp = (image->boundFormat == RENDER_TEXTURE_RGBA16F) ? 8 : 4;
	D3D11UploadRows(srv, pixels, srcW, srcH, srcW * bpp, bpp);
}

void CRenderBackendD3D11::DeleteTexture(CSlrImage *image)
{
	if (image == NULL)
	{
		LOGError("CRenderBackendD3D11::DeleteTexture: image is NULL");
		return;
	}
	image->boundFormat = RENDER_TEXTURE_RGBA8;   // nothing bound, so no bound format
	if (!image->isBound)
	{
		LOGError("CRenderBackendD3D11::DeleteTexture: image is not bound");
		return;
	}

	// NO DEFERRED RELEASE QUEUE, unlike the Metal backend -- but NOT because
	// reference counting makes one unnecessary. Be precise about this, because
	// the shorter version of the argument licenses a real use-after-free.
	//
	// Refcounting DOES cover the GPU: the immediate context AddRefs any
	// resource it has bound and a recorded command holds its own reference, so
	// releasing our handle while the GPU still reads is defined. That is the
	// genuine difference from Metal, where binding a RELEASED id<MTLTexture>
	// crashes inside AGX.
	//
	// It does NOT cover the case Metal's queue was actually written for: an
	// ImDrawCmd stores this SRV as a RAW ImTextureID that D3D11 knows nothing
	// about, so a delete between the blit that recorded the id and
	// PresentFrameBuffer's RenderDrawData would be a straight use-after-free.
	// What makes that safe is the ENGINE's discipline, not this backend's:
	// every Deallocate() runs from VID_BindImages() at the TOP of the frame,
	// before any blit has recorded a texture id. A future mid-frame delete
	// needs the parked-release queue after all.
	ID3D11ShaderResourceView *srv =
		(ID3D11ShaderResourceView *)image->texturePtr.load(std::memory_order_acquire);
	if (srv != NULL)
		srv->Release();
	image->texturePtr.store(NULL, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Video plane and LUT textures
// ---------------------------------------------------------------------------

void *CRenderBackendD3D11::CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel)
{
	// THE FORMATS ARE NOT FREE CHOICES -- VideoYUV.hlsl's arithmetic depends on
	// them. UNORM, never UINT: Sample() is illegal on a UINT SRV, and the
	// 10-bit path's 65535/1023 rescale assumes sampling normalised by 65535.
	DXGI_FORMAT fmt;
	if (channels == 2)
		fmt = (bytesPerChannel == 2) ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
	else
		fmt = (bytesPerChannel == 2) ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
	return D3D11MakeTexture(fmt, width, height, NULL, channels * bytesPerChannel);
}

void CRenderBackendD3D11::UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride)
{
	if (tex == NULL || data == NULL)
		return;
	ID3D11ShaderResourceView *srv = (ID3D11ShaderResourceView *)tex;
	ID3D11Resource *res = NULL;
	srv->GetResource(&res);
	if (res == NULL)
		return;
	ID3D11Texture2D *t = NULL;
	res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&t);
	res->Release();
	if (t == NULL)
		return;
	D3D11_TEXTURE2D_DESC td = {};
	t->GetDesc(&td);
	t->Release();

	// Same WRITE_DISCARD rule as ReBindTexture: a partial update is not a
	// partial update, it is a whole new uninitialised surface with some of it
	// written. Refuse rather than produce garbage.
	if ((UINT)width != td.Width || (UINT)height != td.Height)
	{
		LOGError("CRenderBackendD3D11::UpdatePlaneTexture: %dx%d into a %ux%u plane -- "
				 "WRITE_DISCARD cannot do a partial update; recreate the plane instead",
				 width, height, (unsigned)td.Width, (unsigned)td.Height);
		return;
	}

	int bytesPerTexel = 1;
	switch (td.Format)
	{
		case DXGI_FORMAT_R8_UNORM:     bytesPerTexel = 1; break;
		case DXGI_FORMAT_R8G8_UNORM:   bytesPerTexel = 2; break;
		case DXGI_FORMAT_R16_UNORM:    bytesPerTexel = 2; break;
		case DXGI_FORMAT_R16G16_UNORM: bytesPerTexel = 4; break;
		default:                       bytesPerTexel = 1; break;
	}
	D3D11UploadRows(srv, data, width, height, stride, bytesPerTexel);
}

void CRenderBackendD3D11::DeletePlaneTexture(void *tex)
{
	if (tex != NULL)
		((ID3D11ShaderResourceView *)tex)->Release();
}

void *CRenderBackendD3D11::CreateLutTexture3D(int edge)
{
	if (gDevice == NULL || edge < 2)
		return NULL;

	D3D11_TEXTURE3D_DESC td = {};
	td.Width = td.Height = td.Depth = (UINT)edge;
	td.MipLevels = 1;
	// RGBA16 UNORM on both other backends; the CM-E lattice is 16 bits per
	// channel and anything narrower would band the display transform.
	td.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
	td.Usage = D3D11_USAGE_DYNAMIC;
	td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
	td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	ID3D11Texture3D *tex = NULL;
	if (FAILED(gDevice->CreateTexture3D(&td, NULL, &tex)) || tex == NULL)
	{
		LOGError("CRenderBackendD3D11::CreateLutTexture3D: CreateTexture3D(%d) failed", edge);
		return NULL;
	}
	ID3D11ShaderResourceView *srv = NULL;
	HRESULT hr = gDevice->CreateShaderResourceView(tex, NULL, &srv);
	tex->Release();
	if (FAILED(hr))
		return NULL;
	return srv;
}

void CRenderBackendD3D11::UpdateLutTexture3D(void *tex, const void *data, int edge)
{
	if (gContext == NULL || tex == NULL || data == NULL || edge < 2)
		return;
	ID3D11Resource *res = NULL;
	((ID3D11ShaderResourceView *)tex)->GetResource(&res);
	if (res == NULL)
		return;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(gContext->Map(res, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		// A 3D texture has BOTH a row pitch and a DEPTH pitch, and the driver
		// picks both. Walking the source with edge*8 while the destination uses
		// mapped.RowPitch/DepthPitch is the 3D form of the stride trap.
		const int rowBytes = edge * 8;   // RGBA16 = 8 bytes/texel
		const unsigned char *s = (const unsigned char *)data;
		unsigned char *d = (unsigned char *)mapped.pData;
		for (int z = 0; z < edge; z++)
		{
			for (int y = 0; y < edge; y++)
			{
				memcpy(d + (size_t)z * mapped.DepthPitch + (size_t)y * mapped.RowPitch,
					   s + ((size_t)z * edge + y) * rowBytes,
					   (size_t)rowBytes);
			}
		}
		gContext->Unmap(res, 0);
	}
	res->Release();
}

void CRenderBackendD3D11::DeleteLutTexture3D(void *tex)
{
	if (tex != NULL)
		((ID3D11ShaderResourceView *)tex)->Release();
}

// ---------------------------------------------------------------------------
// Per-draw samplers
// ---------------------------------------------------------------------------

bool CRenderBackendD3D11::ImageNeedsSamplerOverride(CSlrImage *image)
{
	// Only a NON-linear image needs a bracket: ImGui's default is linear, which
	// is right for everything else. Same predicate as the Metal backend.
	return image != NULL && !image->linearScaling;
}

// The callbacks run at RENDER time, long after the blit that queued them, so
// they fetch the live context from ImGui rather than capturing anything.
// ImGui_ImplDX11_RenderDrawData publishes it in
// GetPlatformIO().Renderer_RenderState for exactly the duration of the walk,
// and it is NULL outside that -- a callback that ran with no frame open must do
// nothing rather than dereference.
static void D3D11SetSampler(ID3D11SamplerState *sampler)
{
	ImGui_ImplDX11_RenderState *rs =
		(ImGui_ImplDX11_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
	if (rs == NULL || rs->DeviceContext == NULL || sampler == NULL)
		return;
	rs->DeviceContext->PSSetSamplers(0, 1, &sampler);
}

static void D3D11DrawCallbackSamplerMagNearest(const ImDrawList *, const ImDrawCmd *)
{
	D3D11SetSampler(gSamplerMagNearest);
}

static void D3D11DrawCallbackSamplerLinear(const ImDrawList *, const ImDrawCmd *)
{
	D3D11SetSampler(gSamplerLinear);
}

void CRenderBackendD3D11::QueueSamplerForImage(CSlrImage *image)
{
	if (image == NULL)
		return;
	ImGui::GetWindowDrawList()->AddCallback(
		image->linearScaling ? D3D11DrawCallbackSamplerLinear : D3D11DrawCallbackSamplerMagNearest,
		NULL);
}

void CRenderBackendD3D11::QueueDefaultSampler()
{
	ImGui::GetWindowDrawList()->AddCallback(D3D11DrawCallbackSamplerLinear, NULL);
}

// ---------------------------------------------------------------------------
// Readback
// ---------------------------------------------------------------------------
//
// ALL OF THESE ARE RENDER-THREAD CALLS. D3D11's immediate context is not
// thread-safe and imgui_test_engine runs TestFunc on its own thread; the
// request/poll pairs in MT_ShaderProbe exist so a test never touches this
// directly. That rule is not theoretical here -- it has taken this suite down
// once already on the OpenGL backend, where a GL call from a TestFunc crashes
// while Metal happened to tolerate the same code.

// Copy an arbitrary texture into a STAGING copy the CPU can map. One helper,
// because every readback below needs the same four steps and getting the
// staging desc wrong (BindFlags != 0, or the wrong Usage) fails with an HRESULT
// nobody can attribute.
static ID3D11Texture2D *D3D11StageTexture(ID3D11Texture2D *src, int w, int h, DXGI_FORMAT *outFmt)
{
	if (gDevice == NULL || gContext == NULL || src == NULL)
		return NULL;

	D3D11_TEXTURE2D_DESC sd = {};
	src->GetDesc(&sd);
	if (outFmt != NULL)
		*outFmt = sd.Format;
	if ((UINT)w > sd.Width || (UINT)h > sd.Height || w <= 0 || h <= 0)
		return NULL;

	// CopyResource requires IDENTICAL descriptions, which is why the desc is
	// copied from the source rather than built. Every texture this backend
	// makes is 1 mip / 1 slice, so refuse anything else LOUDLY rather than
	// silently copying nothing -- CopyResource returns void and would give no
	// hint at all.
	if (sd.MipLevels != 1 || sd.ArraySize != 1)
	{
		LOGError("CRenderBackendD3D11: cannot stage a texture with %u mips / %u slices",
				 (unsigned)sd.MipLevels, (unsigned)sd.ArraySize);
		return NULL;
	}

	D3D11_TEXTURE2D_DESC td = sd;
	td.Usage = D3D11_USAGE_STAGING;
	td.BindFlags = 0;                                  // a staging texture binds to nothing
	td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
	td.MiscFlags = 0;

	ID3D11Texture2D *staging = NULL;
	if (FAILED(gDevice->CreateTexture2D(&td, NULL, &staging)))
	{
		LOGError("CRenderBackendD3D11: staging texture creation failed (fmt=%d)", (int)sd.Format);
		return NULL;
	}
	// CopyResource, not CopySubresourceRegion: the descs match exactly, and the
	// whole-resource form is the one the driver fast-paths.
	gContext->CopyResource(staging, src);
	return staging;
}

// Convert one mapped staging row-set into the RGBA8 contract every caller of
// ReadFramebufferPixels/ReadTexturePixels expects, whatever the source format.
//
// THE CONTRACT IS RGBA8 REGARDLESS OF SURFACE FORMAT, exactly as on Metal, so
// the float case is converted HERE rather than pushed onto every caller. Values
// above 1.0 are the extra headroom HDR exists to carry, so this is lossy ON
// PURPOSE: the capture path's job is "did the right thing get drawn", and it
// must answer the same way on every backend and in both surface formats. A test
// that needs true HDR values uses the float readback instead.
static void D3D11ConvertToRgba8(const D3D11_MAPPED_SUBRESOURCE &m, DXGI_FORMAT fmt,
								int x, int y, int w, int h, unsigned int *out)
{
	for (int row = 0; row < h; row++)
	{
		const unsigned char *src = (const unsigned char *)m.pData + (size_t)(y + row) * m.RowPitch;
		unsigned int *dst = out + (size_t)row * w;
		if (fmt == DXGI_FORMAT_R16G16B16A16_FLOAT)
		{
			const u16 *halves = (const u16 *)src + (size_t)x * 4;
			for (int col = 0; col < w; col++)
			{
				unsigned int rgba = 0;
				for (int c = 0; c < 4; c++)
				{
					float v = HalfToFloat(halves[col * 4 + c]);
					if (!(v > 0.0f)) v = 0.0f;    // also folds NaN to 0
					if (v > 1.0f)    v = 1.0f;
					rgba |= ((unsigned int)(v * 255.0f + 0.5f)) << (c * 8);
				}
				dst[col] = rgba;
			}
		}
		else if (fmt == DXGI_FORMAT_B8G8R8A8_UNORM)
		{
			// Swizzle to the RGBA contract so no caller has to know which
			// format produced the buffer. Invisible to a "not all zero" check,
			// and it would make every cross-backend colour comparison fail in a
			// way that looks like a shader bug.
			const unsigned int *p = (const unsigned int *)src + x;
			for (int col = 0; col < w; col++)
			{
				const unsigned int v = p[col];
				dst[col] = (v & 0xFF00FF00u) | ((v & 0x00FF0000u) >> 16) | ((v & 0x000000FFu) << 16);
			}
		}
		else if (fmt == DXGI_FORMAT_R8G8B8A8_UNORM || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
		{
			memcpy(dst, (const unsigned int *)src + x, (size_t)w * sizeof(unsigned int));
		}
		else
		{
			// NOT A 32-BIT FORMAT, and reading it as one over-reads the staging
			// allocation by up to 4x -- this backend hands out R8_UNORM,
			// R8G8_UNORM, R16_UNORM and R16G16_UNORM plane SRVs, and
			// ReadTexturePixels takes an arbitrary handle. Zero rather than
			// read past the map; the caller sees a blank buffer instead of a
			// crash inside Map's allocation, and ReadTexturePixels refuses such
			// a format up front anyway.
			memset(dst, 0, (size_t)w * sizeof(unsigned int));
		}
	}
}

// READS THE OFFSCREEN TARGET, not the swapchain, and that is by design.
//
// It is the only place pixels exist at all in headless -- Present is skipped
// there and FLIP_DISCARD leaves the back buffer undefined afterwards -- and it
// is what MT_CaptureWindowRGBA, and therefore every capture-based test, reads.
// ReadSwapchainPixels() below is the other side of the coin, and comparing the
// two is what proves the resolve pass is the identity it claims to be.
bool CRenderBackendD3D11::ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA)
{
	if (w <= 0 || h <= 0 || outRGBA == NULL || gOffscreenTex == NULL || gContext == NULL)
		return false;
	if (x < 0 || y < 0 || x + w > gOffscreenW || y + h > gOffscreenH)
		return false;

	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *staging = D3D11StageTexture(gOffscreenTex, gOffscreenW, gOffscreenH, &fmt);
	if (staging == NULL)
		return false;

	// NO VERTICAL FLIP. D3D textures are top-down, which is this function's
	// contract on every backend. The GL path flips because ITS framebuffer
	// origin is bottom-left; doing it on both sides yields a mirrored capture
	// that a "not all zero" assertion happily accepts.
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	bool ok = false;
	if (SUCCEEDED(gContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		D3D11ConvertToRgba8(mapped, fmt, x, y, w, h, outRGBA);
		gContext->Unmap(staging, 0);
		ok = true;
	}
	staging->Release();
	return ok;
}

bool CRenderBackendD3D11::ReadSwapchainPixels(int x, int y, int w, int h, unsigned int *outRGBA)
{
	if (w <= 0 || h <= 0 || outRGBA == NULL || gSwapChain == NULL || gContext == NULL)
		return false;
	if (x < 0 || y < 0 || x + w > gSwapChainW || y + h > gSwapChainH)
		return false;

	ID3D11Texture2D *backBuffer = NULL;
	if (FAILED(gSwapChain->GetBuffer(0, __uuidof(ID3D11Texture2D), (void **)&backBuffer)) || backBuffer == NULL)
		return false;

	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *staging = D3D11StageTexture(backBuffer, gSwapChainW, gSwapChainH, &fmt);
	backBuffer->Release();
	if (staging == NULL)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	bool ok = false;
	if (SUCCEEDED(gContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		D3D11ConvertToRgba8(mapped, fmt, x, y, w, h, outRGBA);
		gContext->Unmap(staging, 0);
		ok = true;
	}
	staging->Release();
	return ok;
}

bool CRenderBackendD3D11::ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA)
{
	if (texture == NULL || outRGBA == NULL || w <= 0 || h <= 0 || gContext == NULL)
		return false;

	ID3D11Resource *res = NULL;
	((ID3D11ShaderResourceView *)texture)->GetResource(&res);
	if (res == NULL)
		return false;
	ID3D11Texture2D *tex = NULL;
	res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
	res->Release();
	if (tex == NULL)
		return false;

	D3D11_TEXTURE2D_DESC td = {};
	tex->GetDesc(&td);
	// BOUND CHECK FIRST. We stage the WHOLE texture and then convert a w*h
	// window out of the map, so a caller asking for more than the texture holds
	// walks off the end of the mapped allocation -- 16 KB out of a 1 KB map for
	// the 64x64-from-4x4 case. ReadTexturePixelsFloat has had this check all
	// along; the two siblings disagreed, and the callers in
	// CTestVideoRenderSmoke pass dimensions from CLIP METADATA rather than from
	// the texture, which is exactly where a rotated or display-vs-coded-size
	// mismatch comes from.
	if ((UINT)w > td.Width || (UINT)h > td.Height)
	{
		LOGError("CRenderBackendD3D11::ReadTexturePixels: %dx%d requested from a %ux%u texture",
				 w, h, (unsigned)td.Width, (unsigned)td.Height);
		tex->Release();
		return false;
	}
	// A CAPABILITY ANSWER, not a failure: this function's contract is RGBA8 out,
	// and a single- or two-channel plane texture is outside it. Returning false
	// makes the caller report a capability gap, which is what CRenderBackend.h
	// asks for -- reading one anyway would over-read the staging allocation.
	if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT &&
		td.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
		td.Format != DXGI_FORMAT_R8G8B8A8_UNORM &&
		td.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB)
	{
		LOGError("CRenderBackendD3D11::ReadTexturePixels: format %d is not readable as RGBA8", (int)td.Format);
		tex->Release();
		return false;
	}
	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *staging = D3D11StageTexture(tex, (int)td.Width, (int)td.Height, &fmt);
	tex->Release();
	if (staging == NULL)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	bool ok = false;
	if (SUCCEEDED(gContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		D3D11ConvertToRgba8(mapped, fmt, 0, 0, w, h, outRGBA);
		gContext->Unmap(staging, 0);
		ok = true;
	}
	staging->Release();
	return ok;
}

// THE FLOAT SIBLING, AND IT IS NOT OPTIONAL.
//
// OpenGL shipped without one for a whole stage. The single existing caller
// degraded a `false` return into a passing LogWarning, so every float assertion
// in the HDR tests silently proved NOTHING on the default backend -- and
// implementing it later turned that silent no-op into a crash, which is how the
// masking came to light. Writing both from the start is the lesson.
bool CRenderBackendD3D11::ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA)
{
	if (texture == NULL || outRGBA == NULL || w <= 0 || h <= 0 || gContext == NULL)
		return false;

	ID3D11Resource *res = NULL;
	((ID3D11ShaderResourceView *)texture)->GetResource(&res);
	if (res == NULL)
		return false;
	ID3D11Texture2D *tex = NULL;
	res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&tex);
	res->Release();
	if (tex == NULL)
		return false;

	D3D11_TEXTURE2D_DESC td = {};
	tex->GetDesc(&td);
	if (td.Format != DXGI_FORMAT_R16G16B16A16_FLOAT || (UINT)w > td.Width || (UINT)h > td.Height)
	{
		// A capability answer, not a failure: the caller must report "this
		// texture is not float" rather than "the read failed".
		tex->Release();
		return false;
	}

	DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
	ID3D11Texture2D *staging = D3D11StageTexture(tex, (int)td.Width, (int)td.Height, &fmt);
	tex->Release();
	if (staging == NULL)
		return false;

	D3D11_MAPPED_SUBRESOURCE mapped = {};
	bool ok = false;
	if (SUCCEEDED(gContext->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)))
	{
		for (int row = 0; row < h; row++)
		{
			// 8 BYTES PER PIXEL, and the driver's own RowPitch. Reading
			// RGBA16Float at w*4 does not fail -- it returns half a row per row
			// and looks like plausible garbage.
			const u16 *src = (const u16 *)((const unsigned char *)mapped.pData + (size_t)row * mapped.RowPitch);
			float *dst = outRGBA + (size_t)row * w * 4;
			for (int i = 0; i < w * 4; i++)
				dst[i] = HalfToFloat(src[i]);
		}
		gContext->Unmap(staging, 0);
		ok = true;
	}
	staging->Release();
	return ok;
}

// ---------------------------------------------------------------------------
// Render target
// ---------------------------------------------------------------------------

class CD3D11RenderTarget : public CRenderTarget
{
public:
	CD3D11RenderTarget() {}
	virtual ~CD3D11RenderTarget() { Destroy(); }

	virtual bool Create(int w, int h, ERenderTextureFormat fmt = RENDER_TEXTURE_RGBA8) override
	{
		// THE FORMAT JOINS THE "ALREADY CORRECT" TEST. A caller switching an
		// existing target from RGBA8 to RGBA16F at the same dimensions must get
		// a NEW texture -- otherwise it keeps writing above-white values into
		// an 8-bit attachment that clamps them away, which is exactly what the
		// format parameter exists to stop. That omission was a latent bug on
		// both other backends until S-5 Phase 5.
		if (w == width && h == height && fmt == format && texture != NULL)
			return true;
		Destroy();
		if (gDevice == NULL || w <= 0 || h <= 0)
			return false;

		D3D11_TEXTURE2D_DESC td = {};
		td.Width = (UINT)w;
		td.Height = (UINT)h;
		td.MipLevels = 1;
		td.ArraySize = 1;
		td.Format = (fmt == RENDER_TEXTURE_RGBA16F)
			? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_DEFAULT;
		td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

		if (FAILED(gDevice->CreateTexture2D(&td, NULL, &texture)) || texture == NULL)
		{
			LOGError("CD3D11RenderTarget::Create: CreateTexture2D %dx%d fmt=%d failed", w, h, (int)fmt);
			texture = NULL;
			return false;
		}
		if (FAILED(gDevice->CreateRenderTargetView(texture, NULL, &rtv)) ||
			FAILED(gDevice->CreateShaderResourceView(texture, NULL, &srv)))
		{
			LOGError("CD3D11RenderTarget::Create: view creation failed");
			Destroy();
			return false;
		}
		width = w;
		height = h;
		format = fmt;
		return true;
	}

	virtual ERenderTextureFormat GetFormat() const override { return format; }

	virtual void *BeginPass() override
	{
		// Transparent black, which is what this path has always cleared to on
		// Metal. (GL's plain BeginPass does NOT clear -- a documented,
		// pre-existing asymmetry, invisible to the video path whose quad covers
		// the whole target opaquely. Following Metal keeps the two backends
		// this stage compares alike.)
		return BeginPassInternal(0.0f, 0.0f, 0.0f, 0.0f, true);
	}

	virtual void *BeginPassWithClear(float r, float g, float b, float a) override
	{
		return BeginPassInternal(r, g, b, a, true);
	}

	virtual void EndPass() override
	{
		// PUT THE FRAME'S TARGET BACK. This is not tidiness -- it is the
		// difference between a working frame and a blank one.
		//
		// D3D11 has ONE immediate context, and CVideoPlayer calls
		// RenderToTarget() MID-FRAME on the render thread, between NewFrame()
		// and PresentFrameBuffer(). An EndPass() that left the video target
		// bound would send every subsequent ImGui draw of that frame nowhere.
		// The GL twin does the same thing for the same reason
		// (CGLRenderTarget::Unbind restores the saved FBO and viewport); Metal
		// does not need to, because its pass owns its own encoder.
		D3D11BindFrameTarget();
	}

	virtual void *GetTexture() const override { return srv; }
	virtual int GetWidth() const override { return width; }
	virtual int GetHeight() const override { return height; }

	virtual void Destroy() override
	{
		if (srv)     { srv->Release();     srv = NULL; }
		if (rtv)     { rtv->Release();     rtv = NULL; }
		if (texture) { texture->Release(); texture = NULL; }
		width = height = 0;
	}

private:
	void *BeginPassInternal(float r, float g, float b, float a, bool clear)
	{
		if (gContext == NULL || rtv == NULL)
			return NULL;
		gContext->OMSetRenderTargets(1, &rtv, NULL);
		D3D11_VIEWPORT vp = {};
		vp.Width  = (float)width;
		vp.Height = (float)height;
		vp.MinDepth = 0.0f;
		vp.MaxDepth = 1.0f;
		gContext->RSSetViewports(1, &vp);
		if (clear)
		{
			// An above-white clear SURVIVES on a float target:
			// ClearRenderTargetView takes floats and an RGBA16F attachment
			// stores them unclamped.
			const float c[4] = { r, g, b, a };
			gContext->ClearRenderTargetView(rtv, c);
		}
		// A NON-NULL SENTINEL, not an encoder. Metal must hand back a real
		// MTLRenderCommandEncoder because a draw can only be encoded into one;
		// D3D11's "bind a target" is global context state, exactly like GL's,
		// so callers treat this the way they treat GL's sentinel -- non-NULL
		// means "you may draw", and they never dereference it.
		return (void *)this;
	}

	ID3D11Texture2D          *texture = NULL;
	ID3D11RenderTargetView   *rtv = NULL;
	ID3D11ShaderResourceView *srv = NULL;
	int width = 0, height = 0;
	ERenderTextureFormat format = RENDER_TEXTURE_RGBA8;
};

CRenderTarget *CRenderBackendD3D11::CreateRenderTarget()
{
	if (gDevice == NULL)
		return NULL;
	return new CD3D11RenderTarget();
}

CRenderShader *CRenderBackendD3D11::CreateFlatColorShader(float r, float g, float b, float a)
{
	if (gDevice == NULL)
		return NULL;
	return new CRenderShaderFlatColorD3D11(this, r, g, b, a);
}

CVideoYUVConverter *CRenderBackendD3D11::CreateVideoYUVConverter()
{
	if (gDevice == NULL)
		return NULL;
	return new CVideoYUVShaderD3D11(this);
}

#endif   // MT_RENDER_BACKEND_D3D11
