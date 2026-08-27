#include "MT_ShaderProbe.h"
#include "CRenderShader.h"
#include "VID_Main.h"
#include "DBG_Log.h"
#include "imgui.h"
#include "CSlrImage.h"
#include "CImageData.h"
#include "VID_Blits.h"
#include "Core/Render/CRenderTarget.h"
#include "Core/Render/CRenderBackend.h"
#include "CVideoYUVConverter.h"

#include <atomic>
#include <vector>

static CRenderShader *gProbeShader = NULL;          // render thread only
static std::atomic<int> gProbeState(SHADER_PROBE_CLOSED);

// The pending request. Written by the requesting thread, read by the render
// thread; gRequestPending is the release/acquire handshake that publishes the
// colour with it.
static float gRequestColor[4] = { 0, 0, 0, 0 };

// Image-probe mode: draw a magnified 2x2 image rather than a shaded quad.
static CSlrImage *gProbeImage = NULL;          // render thread only
static bool gRequestImageMode = false;
static bool gRequestImageLinear = false;
static bool gRequestFloatImage = false;
static std::atomic<bool> gRequestOpen(false);
static std::atomic<bool> gRequestPending(false);

// Fixed size in ImGui units. Small enough that the capture is cheap, large
// enough that a one-pixel border cannot move the measured fraction much.
static const float kProbeWidth = 200.0f;
static const float kProbeHeight = 120.0f;

const char *MT_ShaderProbeWindowName()
{
	return "ShaderProbe";
}

void MT_ShaderProbeOpenFlatColor(float r, float g, float b, float a)
{
	gRequestColor[0] = r;
	gRequestColor[1] = g;
	gRequestColor[2] = b;
	gRequestColor[3] = a;
	gRequestOpen.store(true, std::memory_order_relaxed);
	gProbeState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gRequestPending.store(true, std::memory_order_release);
}

CSlrImage *MT_ShaderProbeGetImage()
{
	return gProbeImage;
}

void MT_ShaderProbeOpenFloatImage()
{
	gRequestFloatImage = true;
	gRequestImageMode = false;
	gRequestOpen.store(true, std::memory_order_relaxed);
	gProbeState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gRequestPending.store(true, std::memory_order_release);
}

void MT_ShaderProbeOpenScaledImage(bool linearScaling)
{
	gRequestImageMode = true;
	gRequestImageLinear = linearScaling;
	gRequestOpen.store(true, std::memory_order_relaxed);
	gProbeState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gRequestPending.store(true, std::memory_order_release);
}

void MT_ShaderProbeClose()
{
	gRequestImageMode = false;
	gRequestFloatImage = false;
	gRequestOpen.store(false, std::memory_order_relaxed);
	gProbeState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gRequestPending.store(true, std::memory_order_release);
}

EShaderProbeState MT_ShaderProbeGetState()
{
	return (EShaderProbeState)gProbeState.load(std::memory_order_acquire);
}

bool MT_ShaderProbeIsShaderUsable()
{
	return MT_ShaderProbeGetState() == SHADER_PROBE_READY;
}

// Render thread. Destroys the old shader and, if one was asked for, builds the
// new one. Both halves must be here: deleting a GL program is as much a
// context-bound call as creating one.
static void ServicePendingRequest()
{
	if (!gRequestPending.exchange(false, std::memory_order_acq_rel))
		return;

	if (gProbeShader != NULL)
	{
		delete gProbeShader;
		gProbeShader = NULL;
	}
	if (gProbeImage != NULL)
	{
		delete gProbeImage;
		gProbeImage = NULL;
	}

	if (!gRequestOpen.load(std::memory_order_relaxed))
	{
		gProbeState.store(SHADER_PROBE_CLOSED, std::memory_order_release);
		return;
	}

	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL)
	{
		gProbeState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	if (gRequestFloatImage)
	{
		// A 2x2 FP16 image whose pixels sit ABOVE WHITE. 2.0 is chosen because
		// it is exactly representable in half, so a read-back that returns
		// anything else failed for a reason worth reporting rather than a
		// rounding one.
		CImageData *imageData = new CImageData(2, 2, IMG_TYPE_RGBA_16F);
		imageData->AllocImage(false, true);
		for (int y = 0; y < 2; y++)
			for (int x = 0; x < 2; x++)
				imageData->SetPixelResultFloat(x, y, 2.0f, 2.0f, 2.0f, 1.0f);

		gProbeImage = new CSlrImage(imageData, false, false);
		gProbeImage->loadImageData = imageData;
		// SET BY HAND on purpose: the resident funnel (S-5 Task 3) is what
		// makes this automatic, and it does not exist yet at this task's
		// commit. Once it does, this line becomes redundant rather than wrong.
		gProbeImage->residentFormat =
			(VID_GetRenderBackend() != NULL &&
			 VID_GetRenderBackend()->SupportsTextureFormat(RENDER_TEXTURE_RGBA16F))
				? RENDER_TEXTURE_RGBA16F : RENDER_TEXTURE_RGBA8;
		gProbeImage->BindImage();
		gProbeState.store(SHADER_PROBE_READY, std::memory_order_release);
		return;
	}

	if (gRequestImageMode)
	{
		// A 2x2 checker of four MAXIMALLY separated colours. Point magnification
		// reproduces exactly these four; linear interpolation produces a smooth
		// ramp between them across the whole window, so a distinct-colour count
		// separates the two with an enormous margin rather than a threshold that
		// has to be tuned.
		CImageData *imageData = new CImageData(2, 2, IMG_TYPE_RGBA);
		imageData->AllocImage(false, true);
		imageData->SetPixelResultRGBA(0, 0, 255,   0,   0, 255);
		imageData->SetPixelResultRGBA(1, 0,   0, 255,   0, 255);
		imageData->SetPixelResultRGBA(0, 1,   0,   0, 255, 255);
		imageData->SetPixelResultRGBA(1, 1, 255, 255,   0, 255);

		// bindNow = false: binding is the render thread's job and this already
		// runs there, but the engine's own binding queue is what CSlrImage
		// expects to go through.
		gProbeImage = new CSlrImage(imageData, gRequestImageLinear, false);
		gProbeImage->loadImageData = imageData;
		gProbeImage->BindImage();
		gProbeState.store(SHADER_PROBE_READY, std::memory_order_release);
		return;
	}

	gProbeShader = backend->CreateFlatColorShader(gRequestColor[0], gRequestColor[1],
												 gRequestColor[2], gRequestColor[3]);
	if (gProbeShader == NULL)
	{
		LOGError("MT_ShaderProbe: the active render backend provides no flat-colour shader");
		gProbeState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	gProbeShader->CompileShaders();
	if (!gProbeShader->isCompiled)
	{
		LOGError("MT_ShaderProbe: the flat-colour shader failed to compile");
		gProbeState.store(SHADER_PROBE_COMPILE_FAILED, std::memory_order_release);
		return;
	}

	gProbeState.store(SHADER_PROBE_READY, std::memory_order_release);
}


// --- Float RENDER TARGET probe (S-5 Phase 5 Task 3) -------------------------

static std::atomic<int>  gFloatTargetState(SHADER_PROBE_CLOSED);
static std::atomic<bool> gFloatTargetRequested(false);
static SFloatTargetProbeResult gFloatTargetResult;   // render thread writes, test reads after READY

void MT_ShaderProbeRunFloatTargetCheck()
{
	gFloatTargetResult = SFloatTargetProbeResult();
	gFloatTargetState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gFloatTargetRequested.store(true, std::memory_order_release);
}

EShaderProbeState MT_ShaderProbeGetFloatTargetState()
{
	return (EShaderProbeState)gFloatTargetState.load(std::memory_order_acquire);
}

const SFloatTargetProbeResult &MT_ShaderProbeGetFloatTargetResult()
{
	return gFloatTargetResult;
}

// RENDER THREAD ONLY.
static void ServiceFloatTargetRequest()
{
	if (!gFloatTargetRequested.exchange(false, std::memory_order_acquire))
		return;

	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL || !backend->SupportsTextureFormat(RENDER_TEXTURE_RGBA16F))
	{
		gFloatTargetState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	CRenderTarget *rt = backend->CreateRenderTarget();
	if (rt == NULL)
	{
		gFloatTargetState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	SFloatTargetProbeResult r;

	// 1. The default is still RGBA8 -- a defaulted parameter must not have
	//    moved the SDR path.
	if (rt->Create(4, 4))
		r.createdRgba8 = (rt->GetFormat() == RENDER_TEXTURE_RGBA8);

	// 2. SAME SIZE, different format. This is the case a dimension-only
	//    "already correct" test silently gets wrong: the target keeps its
	//    8-bit texture and goes on clamping the values the switch was made for.
	if (rt->Create(4, 4, RENDER_TEXTURE_RGBA16F))
		r.switchedToFloat = (rt->GetFormat() == RENDER_TEXTURE_RGBA16F);

	// 3. THE POINT: clear to an above-white (and partly negative) colour and
	//    read it back. An 8-bit attachment cannot represent 2.5 at all.
	if (r.switchedToFloat)
	{
		if (rt->BeginPassWithClear(2.5f, 1.75f, -0.25f, 1.0f) != NULL)
			rt->EndPass();

		float px[4 * 4 * 4] = { 0 };
		r.readBackOk = backend->ReadTexturePixelsFloat(rt->GetTexture(), 4, 4, px);
		if (r.readBackOk)
		{
			// Reduced across ALL 16 texels, not just the first: a wrong
			// bytesPerRow leaves texel 0 perfectly correct and the rest
			// garbage, which is the shape S-4 hit twice.
			r.minR = px[0]; r.minG = px[1]; r.maxB = px[2];
			for (int i = 1; i < 16; i++)
			{
				if (px[i * 4 + 0] < r.minR) r.minR = px[i * 4 + 0];
				if (px[i * 4 + 1] < r.minG) r.minG = px[i * 4 + 1];
				if (px[i * 4 + 2] > r.maxB) r.maxB = px[i * 4 + 2];
			}
		}
	}

	// 4. Back to RGBA8, which must CLAMP -- proving the FORMAT carried the
	//    range rather than something incidental about the read-back path.
	if (rt->Create(4, 4, RENDER_TEXTURE_RGBA8) && rt->GetFormat() == RENDER_TEXTURE_RGBA8)
	{
		if (rt->BeginPassWithClear(2.5f, 1.75f, -0.25f, 1.0f) != NULL)
			rt->EndPass();
		unsigned int px8[4 * 4] = { 0 };
		if (backend->ReadTexturePixels(rt->GetTexture(), 4, 4, px8))
			r.rgba8ClampedR = (int)(px8[0] & 0xFF);
	}

	rt->Destroy();
	delete rt;

	gFloatTargetResult = r;
	gFloatTargetState.store(SHADER_PROBE_READY, std::memory_order_release);
}


static std::atomic<int>  gFloatImageReadState(SHADER_PROBE_CLOSED);
static std::atomic<bool> gFloatImageReadRequested(false);
static SFloatImageReadbackResult gFloatImageReadResult;

void MT_ShaderProbeRunFloatImageReadback()
{
	gFloatImageReadResult = SFloatImageReadbackResult();
	gFloatImageReadState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gFloatImageReadRequested.store(true, std::memory_order_release);
}

EShaderProbeState MT_ShaderProbeGetFloatImageReadbackState()
{
	return (EShaderProbeState)gFloatImageReadState.load(std::memory_order_acquire);
}

const SFloatImageReadbackResult &MT_ShaderProbeGetFloatImageReadbackResult()
{
	return gFloatImageReadResult;
}

// --- swapchain vs offscreen readback (S-6 A5) ------------------------------

static std::atomic<int>  gSwapReadState(SHADER_PROBE_CLOSED);
static std::atomic<bool> gSwapReadRequested(false);
static SSwapchainReadbackResult gSwapReadResult;

void MT_ShaderProbeRunSwapchainReadback()
{
	gSwapReadResult = SSwapchainReadbackResult();
	gSwapReadState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gSwapReadRequested.store(true, std::memory_order_release);
}

EShaderProbeState MT_ShaderProbeGetSwapchainReadbackState()
{
	return (EShaderProbeState)gSwapReadState.load(std::memory_order_acquire);
}

const SSwapchainReadbackResult &MT_ShaderProbeGetSwapchainReadbackResult()
{
	return gSwapReadResult;
}

static void ServiceSwapchainReadbackRequest();

void MT_ShaderProbeServiceSwapchainReadback()
{
	ServiceSwapchainReadbackRequest();
}

// RENDER THREAD ONLY, and AFTER THE RESOLVE -- reached through
// MT_ShaderProbeServiceSwapchainReadback(), never from MT_ShaderProbeRender().
static void ServiceSwapchainReadbackRequest()
{
	if (!gSwapReadRequested.exchange(false, std::memory_order_acquire))
		return;

	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL)
	{
		gSwapReadState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	// A SMALL RECT, not the whole surface. The point is a per-channel
	// comparison, and 64x64 from the top-left corner is enough of the UI to
	// contain chrome, text antialiasing and background -- i.e. mid-tones, which
	// is exactly where a wrongly-applied transfer curve is largest. A
	// full-surface readback would allocate megabytes on both sides for no more
	// evidence.
	const int kW = 64, kH = 64;
	SSwapchainReadbackResult r;

	std::vector<unsigned int> offscreen((size_t)kW * kH, 0u);
	std::vector<unsigned int> swapchain((size_t)kW * kH, 0u);

	// ASK THE SWAPCHAIN FIRST. On a backend that renders straight to the
	// surface this returns false immediately and nothing else has to run --
	// and, more importantly, "unsupported" is then reported without a
	// successful offscreen read making it look like a real comparison that
	// happened to fail.
	if (!backend->ReadSwapchainPixels(0, 0, kW, kH, swapchain.data()))
	{
		gSwapReadResult = r;   // supported=false, ok=false
		gSwapReadState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}
	r.supported = true;

	if (!backend->ReadFramebufferPixels(0, 0, kW, kH, offscreen.data()))
	{
		// READY with ok = false, NOT COMPILE_FAILED. Nothing was compiled here,
		// and that enumerant means "the backend made a shader and it would not
		// build" -- borrowing it would send whoever reads the log looking at
		// shaders. The result struct already models this: `supported` says the
		// backend distinguishes the two buffers, `ok` says the comparison
		// actually happened.
		gSwapReadResult = r;
		gSwapReadState.store(SHADER_PROBE_READY, std::memory_order_release);
		return;
	}

	int worst = 0;
	for (size_t i = 0; i < offscreen.size(); i++)
	{
		for (int c = 0; c < 3; c++)   // RGB only: alpha is not resolved
		{
			const int a = (int)((offscreen[i] >> (c * 8)) & 0xFF);
			const int b = (int)((swapchain[i] >> (c * 8)) & 0xFF);
			const int d = (a > b) ? (a - b) : (b - a);
			if (d > worst) worst = d;
		}
	}
	r.ok = true;
	r.maxChannelDelta = worst;
	r.sampledPixels = kW * kH;
	gSwapReadResult = r;
	gSwapReadState.store(SHADER_PROBE_READY, std::memory_order_release);
}

// RENDER THREAD ONLY.
static void ServiceFloatImageReadbackRequest()
{
	if (!gFloatImageReadRequested.exchange(false, std::memory_order_acquire))
		return;

	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL || gProbeImage == NULL)
	{
		gFloatImageReadState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	SFloatImageReadbackResult r;
	float px[2 * 2 * 4] = { 0 };
	r.ok = backend->ReadTexturePixelsFloat(gProbeImage->TexturePtr(), 2, 2, px);
	if (r.ok)
	{
		for (int i = 0; i < 4; i++)
			for (int c = 0; c < 4; c++)
				r.texel[i][c] = px[i * 4 + c];
	}
	gFloatImageReadResult = r;
	gFloatImageReadState.store(SHADER_PROBE_READY, std::memory_order_release);
}


// --- HDR transfer agreement probe (S-5 Phase 5 Task 7) ---------------------

static std::atomic<int>  gHdrTransferState(SHADER_PROBE_CLOSED);
static std::atomic<bool> gHdrTransferRequested(false);
static int  gHdrTransferTrc = 16;
static bool gHdrTransferFloat = true;
static bool gHdrTransferP3 = false;
static SHdrTransferProbeResult gHdrTransferResult;

void MT_ShaderProbeRunHdrTransferCheck(int colorTrc, bool floatTarget, bool surfaceP3)
{
	gHdrTransferResult = SHdrTransferProbeResult();
	gHdrTransferTrc = colorTrc;
	gHdrTransferFloat = floatTarget;
	gHdrTransferP3 = surfaceP3;
	gHdrTransferState.store(SHADER_PROBE_PENDING, std::memory_order_relaxed);
	gHdrTransferRequested.store(true, std::memory_order_release);
}

EShaderProbeState MT_ShaderProbeGetHdrTransferState()
{
	return (EShaderProbeState)gHdrTransferState.load(std::memory_order_acquire);
}

const SHdrTransferProbeResult &MT_ShaderProbeGetHdrTransferResult()
{
	return gHdrTransferResult;
}

// RENDER THREAD ONLY.
static void ServiceHdrTransferRequest()
{
	if (!gHdrTransferRequested.exchange(false, std::memory_order_acquire))
		return;

	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL)
	{
		gHdrTransferState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	CVideoYUVConverter *conv = backend->CreateVideoYUVConverter();
	CRenderTarget *rt = backend->CreateRenderTarget();
	if (conv == NULL || rt == NULL || !conv->Compile())
	{
		delete conv;
		if (rt) { rt->Destroy(); delete rt; }
		gHdrTransferState.store(SHADER_PROBE_UNSUPPORTED, std::memory_order_release);
		return;
	}

	const int N = SHdrTransferProbeResult::kSteps;
	SHdrTransferProbeResult r;

	// One texel per step, in a 1-pixel-tall image. The Y plane carries the
	// ramp; U and V are neutral (128), so every step is ACHROMATIC and its
	// expected output is computable in closed form on the CPU.
	//
	// FULL RANGE is used deliberately: it makes the code word land in the
	// shader's maths unscaled, so a disagreement is attributable to the
	// TRANSFER rather than to limited-range expansion.
	u8 yPlane[N], uPlane[N], vPlane[N];
	for (int i = 0; i < N; i++)
	{
		// Spread across the useful part of the curve. Step 0 is black and the
		// last step is the code word for peak, so both ends are covered.
		const int code = (i * 255) / (N - 1);
		yPlane[i] = (u8)code;
		uPlane[i] = 128;
		vPlane[i] = 128;
		r.codeValue[i] = (float)code / 255.0f;
	}

	void *texY = backend->CreatePlaneTexture(N, 1, 1, 1);
	void *texU = backend->CreatePlaneTexture(N, 1, 1, 1);
	void *texV = backend->CreatePlaneTexture(N, 1, 1, 1);
	if (texY && texU && texV)
	{
		backend->UpdatePlaneTexture(texY, yPlane, N, 1, N);
		backend->UpdatePlaneTexture(texU, uPlane, N, 1, N);
		backend->UpdatePlaneTexture(texV, vPlane, N, 1, N);

		const ERenderTextureFormat fmt = gHdrTransferFloat
			? RENDER_TEXTURE_RGBA16F : RENDER_TEXTURE_RGBA8;
		if (rt->Create(N, 1, fmt))
		{
			SVideoHdrOutput hdr;
			hdr.colorTrc = gHdrTransferTrc;
			hdr.floatTarget = gHdrTransferFloat;
			hdr.surfaceIsLinear = false;   // plain extended sRGB, the shipped case
			hdr.surfaceIsP3 = gHdrTransferP3;
			hdr.toneMapHeadroom = 1.0f;    // identity Reinhard: the 8-bit arm's
										   // curve is then exactly SrgbExtendedEncode

			// colorSpace 5 = BT.2020 ncl, fullRange = true (see above).
			conv->RenderToTarget(EYUVShaderMode::YUV420_3Plane,
								 texY, texU, texV, false, NULL,
								 /*colorSpace=*/5, /*fullRange=*/true,
								 /*rotationDegrees=*/0, NULL, 0, *rt, hdr);

			if (gHdrTransferFloat)
			{
				float px[SHdrTransferProbeResult::kSteps * 4] = { 0 };
				r.ok = backend->ReadTexturePixelsFloat(rt->GetTexture(), N, 1, px);
				if (r.ok)
					for (int i = 0; i < N; i++)
					{
						r.outR[i] = px[i * 4 + 0];
						r.outG[i] = px[i * 4 + 1];
						r.outB[i] = px[i * 4 + 2];
					}
			}
			else
			{
				unsigned int px8[SHdrTransferProbeResult::kSteps] = { 0 };
				r.ok = backend->ReadTexturePixels(rt->GetTexture(), N, 1, px8);
				if (r.ok)
					for (int i = 0; i < N; i++)
					{
						r.outR[i] = (float)((px8[i] >>  0) & 0xFF) / 255.0f;
						r.outG[i] = (float)((px8[i] >>  8) & 0xFF) / 255.0f;
						r.outB[i] = (float)((px8[i] >> 16) & 0xFF) / 255.0f;
					}
			}
		}
	}

	backend->DeletePlaneTexture(texY);
	backend->DeletePlaneTexture(texU);
	backend->DeletePlaneTexture(texV);
	rt->Destroy();
	delete rt;
	delete conv;

	gHdrTransferResult = r;
	gHdrTransferState.store(SHADER_PROBE_READY, std::memory_order_release);
}

void MT_ShaderProbeRender()
{
	ServiceFloatTargetRequest();
	ServiceFloatImageReadbackRequest();
	// NOT ServiceSwapchainReadbackRequest() -- see MT_ShaderProbeServiceSwapchainReadback()
	// in the header. This function runs before ImGui::Render() and before the
	// resolve, which is the wrong half of the frame for that comparison.
	ServiceHdrTransferRequest();
	ServicePendingRequest();

	if (gProbeImage != NULL)
	{
		ImGui::SetNextWindowSize(ImVec2(kProbeWidth, kProbeHeight), ImGuiCond_Always);
		ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
							   | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
							   | ImGuiWindowFlags_NoTitleBar;
		if (ImGui::Begin(MT_ShaderProbeWindowName(), NULL, flags))
		{
			// Through Blit(), NOT a raw AddImage: the filter selection lives in
			// the blit layer, so calling AddImage directly would test nothing.
			ImVec2 p0 = ImGui::GetWindowPos();
			Blit(gProbeImage, p0.x, p0.y, 0.0f,
				 ImGui::GetWindowWidth(), ImGui::GetWindowHeight());
		}
		ImGui::End();
		return;
	}

	if (gProbeShader == NULL || !gProbeShader->isCompiled)
		return;

	// No title bar, no resize, no scrollbars: the capture must be the shaded
	// rectangle, not chrome that shifts the measured fractions between runs.
	ImGui::SetNextWindowSize(ImVec2(kProbeWidth, kProbeHeight), ImGuiCond_Always);
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize
						   | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings
						   | ImGuiWindowFlags_NoTitleBar;
	if (ImGui::Begin(MT_ShaderProbeWindowName(), NULL, flags))
	{
		ImDrawList *drawList = ImGui::GetWindowDrawList();
		ImVec2 p0 = ImGui::GetWindowPos();
		ImVec2 p1 = ImVec2(p0.x + ImGui::GetWindowWidth(), p0.y + ImGui::GetWindowHeight());

		// AddRectFilled draws through the font atlas' white pixel, i.e. the
		// ordinary ImGui vertex path -- which is exactly the path the shader
		// callback has to intercept. A real image here would additionally test
		// image binding and blur what a failure means.
		gProbeShader->UseShaderProgram();
		drawList->AddRectFilled(p0, p1, IM_COL32(255, 255, 255, 255));
		gProbeShader->ResetState();
	}
	ImGui::End();
}
