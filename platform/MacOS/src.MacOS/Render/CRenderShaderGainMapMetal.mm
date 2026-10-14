#include "Generated/MTGainMapMetallib.h"
#include "CRenderShaderGainMapMetal.h"
#include "CRenderBackendMetal.h"
#include "Core/Render/CRenderTarget.h"
#include "Core/Render/MT_GainMapUniforms.h"
#include "Core/Render/VID_GainMapDraw.h"
#include "CSlrImage.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "imgui.h"

#import <Metal/Metal.h>
#include <cstring>

CRenderShaderGainMapMetal::CRenderShaderGainMapMetal(CRenderBackendMetal *renderBackend)
: CRenderShaderMetal(renderBackend, "GainMap")
{
	this->surfacePipelineFormat = 0;
	this->failedPipelineFormat = 0;
	this->testPipelinePtr = NULL;
	this->plainPipelinePtr = NULL;
	// Synchronous, so the factory's caller reads IsUsable() as the final answer
	// (VID_GainMapShaderUsable latches a failure; there is no queued variant).
	CompileShaders();
}

CRenderShaderGainMapMetal::~CRenderShaderGainMapMetal()
{
	if (testPipelinePtr != NULL)
	{
		id<MTLRenderPipelineState> p = (__bridge_transfer id<MTLRenderPipelineState>)testPipelinePtr;
		p = nil;
		testPipelinePtr = NULL;
	}
	if (plainPipelinePtr != NULL)
	{
		id<MTLRenderPipelineState> p = (__bridge_transfer id<MTLRenderPipelineState>)plainPipelinePtr;
		p = nil;
		plainPipelinePtr = NULL;
	}
	// pipelinePtr and libraryPtr: ~CRenderShaderMetal.
}

const char *CRenderShaderGainMapMetal::GetMetalShaderSource()
{
	return kMTGainMapMetalSource;
}

const void *CRenderShaderGainMapMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = kMTGainMapMetallibLength;
	return kMTGainMapMetallibData;
}

void *CRenderShaderGainMapMetal::BuildPipeline(unsigned int pixelFormat, bool blending, const char *fragment)
{
	if (fragment == NULL)
		fragment = GetFragmentFunctionName();
	@autoreleasepool
	{
		id<MTLLibrary> library = (__bridge id<MTLLibrary>)libraryPtr;
		id<MTLDevice> device = (__bridge id<MTLDevice>)renderBackend->GetMetalDevice();
		if (library == nil || device == nil)
			return NULL;

		id<MTLFunction> vertexFunction = [library newFunctionWithName:
										  [NSString stringWithUTF8String:GetVertexFunctionName()]];
		id<MTLFunction> fragmentFunction = [library newFunctionWithName:
											[NSString stringWithUTF8String:fragment]];
		if (vertexFunction == nil || fragmentFunction == nil)
		{
			LOGError("CRenderShaderGainMapMetal: library has no '%s'/'%s'",
					 GetVertexFunctionName(), fragment);
			return NULL;
		}

		// ImGui's own ImDrawVert stream (imgui_impl_metal.mm): the production draw's
		// geometry is ImGui's, the callback only swaps the pipeline. The test draw
		// feeds the same layout.
		MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
		vd.attributes[0].offset = offsetof(ImDrawVert, pos);
		vd.attributes[0].format = MTLVertexFormatFloat2;
		vd.attributes[0].bufferIndex = 0;
		vd.attributes[1].offset = offsetof(ImDrawVert, uv);
		vd.attributes[1].format = MTLVertexFormatFloat2;
		vd.attributes[1].bufferIndex = 0;
		vd.attributes[2].offset = offsetof(ImDrawVert, col);
		vd.attributes[2].format = MTLVertexFormatUChar4;
		vd.attributes[2].bufferIndex = 0;
		vd.layouts[0].stepRate = 1;
		vd.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
		vd.layouts[0].stride = sizeof(ImDrawVert);

		MTLRenderPipelineDescriptor *d = [[MTLRenderPipelineDescriptor alloc] init];
		d.vertexFunction = vertexFunction;
		d.fragmentFunction = fragmentFunction;
		d.vertexDescriptor = vd;
		d.colorAttachments[0].pixelFormat = (MTLPixelFormat)pixelFormat;
		d.colorAttachments[0].blendingEnabled = blending ? YES : NO;
		if (blending)
		{
			// ImGui's own blend state (imgui_impl_metal.mm).
			d.colorAttachments[0].rgbBlendOperation = MTLBlendOperationAdd;
			d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
			d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
			d.colorAttachments[0].alphaBlendOperation = MTLBlendOperationAdd;
			d.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
			d.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
		}

		NSError *error = nil;
		id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:d error:&error];
		if (pipeline == nil)
		{
			LOGError("CRenderShaderGainMapMetal: pipeline creation failed (format %u): %s", pixelFormat,
					 error ? [[error localizedDescription] UTF8String] : "(no error object)");
			return NULL;
		}
		return (__bridge_retained void *)pipeline;
	}
}

void CRenderShaderGainMapMetal::CompileShaders()
{
	if (isCompiled || compileFailed)
		return;
	lastCompileLog.clear();

	if (!LoadLibrary())
	{
		compileFailed = true;
		return;
	}
	const unsigned int fmt = renderBackend->GetColorPixelFormatRaw();
	pipelinePtr = BuildPipeline(fmt, true);
	testPipelinePtr = BuildPipeline((unsigned int)MTLPixelFormatRGBA16Float, false);
	plainPipelinePtr = BuildPipeline((unsigned int)MTLPixelFormatRGBA16Float, false, "mtFragmentPlain");
	if (pipelinePtr == NULL || testPipelinePtr == NULL || plainPipelinePtr == NULL)
	{
		compileFailed = true;
		return;
	}
	surfacePipelineFormat = fmt;
	isCompiled = true;
	LOGD("CRenderShaderGainMapMetal: compiled (surface format %u)", fmt);
}

void *CRenderShaderGainMapMetal::SurfacePipeline()
{
	const unsigned int fmt = renderBackend->GetColorPixelFormatRaw();
	if (fmt == surfacePipelineFormat && pipelinePtr != NULL)
		return pipelinePtr;
	// LATCHED per format: a pipeline that failed to build for this format is not
	// rebuilt synchronously on every draw of every frame (review round 2) -- only a
	// format change (SDR <-> HDR) retries. Every draw meanwhile is suppressed.
	if (fmt == failedPipelineFormat)
		return NULL;

	// The surface switched SDR <-> HDR, or the shader was compiled before the first
	// frame: rebuild before use.
	if (pipelinePtr != NULL)
	{
		id<MTLRenderPipelineState> old = (__bridge_transfer id<MTLRenderPipelineState>)pipelinePtr;
		old = nil;
		pipelinePtr = NULL;
	}
	pipelinePtr = BuildPipeline(fmt, true);
	surfacePipelineFormat = fmt;
	if (pipelinePtr == NULL)
	{
		failedPipelineFormat = fmt;
		LOGError("CRenderShaderGainMapMetal: the surface pipeline for format %u failed to build -- gain-mapped "
				 "draws are suppressed until the surface format changes", fmt);
	}
	else
	{
		failedPipelineFormat = 0;
		LOGD("CRenderShaderGainMapMetal: surface pipeline rebuilt for format %u", fmt);
	}
	return pipelinePtr;
}

namespace
{
	// The callback's user data. ImGui COPIES it into the draw list
	// (AddCallback(cb, &e, sizeof e)), so every draw carries its own map and
	// uniforms and nothing is read from the shader object but its pipeline.
	struct SGainMapDrawEntry
	{
		CRenderShaderGainMapMetal *self;
		void *map;
		SMTGainMapUniforms u;
	};
}

bool CRenderShaderGainMapMetal::AddDrawCallback(ImDrawList *dl, void *mapTexture, const SMTGainMapUniforms &u)
{
	if (dl == NULL || !isCompiled)
		return false;
	// The pipeline for the surface AS IT IS NOW, built (or rebuilt after an
	// SDR <-> HDR switch) at RECORD time, on the same thread that renders: a
	// failure here can still suppress the image's draw, which a failure inside the
	// render-time callback no longer can (Decision R-4).
	if (SurfacePipeline() == NULL)
		return false;
	SGainMapDrawEntry e;
	e.self = this;
	e.map = mapTexture;
	e.u = u;
	dl->AddCallback(&CRenderShaderGainMapMetal::DrawCallback, &e, sizeof(e));
	return true;
}

// Runs at RENDER time, long after the blit that queued it (CRenderShaderMetal's
// UseShaderProgram lambda, the same rules): fetch the encoder freshly and tolerate
// nil -- NewFrame() aborts the frame when there is no drawable and the draw lists
// are still walked.
void CRenderShaderGainMapMetal::DrawCallback(const ImDrawList *dl, const ImDrawCmd *cmd)
{
	(void)dl;
	if (cmd == NULL || cmd->UserCallbackData == NULL || cmd->UserCallbackDataSize != (int)sizeof(SGainMapDrawEntry))
		return;
	// Copied out: ImGui's callback storage is a byte buffer with no alignment promise.
	SGainMapDrawEntry e;
	memcpy(&e, cmd->UserCallbackData, sizeof(e));

	CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
	if (backend == NULL)
		return;
	id<MTLRenderCommandEncoder> encoder =
		(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
	if (encoder == nil)
		return;
	id<MTLRenderPipelineState> pipeline = (__bridge id<MTLRenderPipelineState>)e.self->SurfacePipeline();
	if (pipeline == nil)
	{
		// Only reachable when the surface format changed between recording the draw
		// (where AddDrawCallback checked the pipeline) and rendering it, AND the
		// rebuild failed: this one frame shows the raw base. Logged once, loudly --
		// the next frame's record-time check suppresses the draw.
		static bool logged = false;
		if (!logged)
		{
			logged = true;
			LOGError("CRenderShaderGainMapMetal: no pipeline at render time (format changed mid-frame and the "
					 "rebuild failed) -- one frame drew the raw base; later draws are suppressed");
		}
		return;
	}

	[encoder setRenderPipelineState:pipeline];
	[encoder setFragmentBytes:&e.u length:sizeof(SMTGainMapUniforms) atIndex:0];
	[encoder setFragmentTexture:(__bridge id<MTLTexture>)e.map atIndex:1];
}

bool CRenderShaderGainMapMetal::DrawQuadToTarget(CRenderTarget *target, void *pipeline, void *sampler, void *tex0,
                                                 void *tex1, const SMTGainMapUniforms *u, const float rect[4],
                                                 const float uv[4], const float rgba[4])
{
	if (target == NULL || pipeline == NULL || sampler == NULL || tex0 == NULL)
		return false;
	const float W = (float)target->GetWidth(), H = (float)target->GetHeight();
	if (W <= 0.f || H <= 0.f)
		return false;

	// Two triangles in the target's pixel space; the vertex colour in ImGui's
	// packing (R in the low byte).
	auto toByte = [](float v) { v = v < 0.f ? 0.f : (v > 1.f ? 1.f : v); return (unsigned int)(v * 255.f + 0.5f); };
	const ImU32 col = toByte(rgba[0]) | (toByte(rgba[1]) << 8) | (toByte(rgba[2]) << 16) | (toByte(rgba[3]) << 24);
	const float x0 = rect[0], y0 = rect[1], x1 = rect[2], y1 = rect[3];
	const float u0 = uv[0], v0 = uv[1], u1 = uv[2], v1 = uv[3];
	ImDrawVert quad[6];
	auto V = [&](int i, float x, float y, float tu, float tv) {
		quad[i].pos = ImVec2(x, y); quad[i].uv = ImVec2(tu, tv); quad[i].col = col;
	};
	V(0, x0, y0, u0, v0); V(1, x1, y0, u1, v0); V(2, x1, y1, u1, v1);
	V(3, x0, y0, u0, v0); V(4, x1, y1, u1, v1); V(5, x0, y1, u0, v1);

	// The same orthographic projection ImGui builds (imgui_impl_metal.mm), for
	// L = 0, R = W, T = 0, B = H.
	const float L = 0.f, R = W, T = 0.f, B = H, N = 0.f, F = 1.f;
	const float ortho[4][4] =
	{
		{ 2.0f / (R - L),     0.0f,               0.0f,          0.0f },
		{ 0.0f,               2.0f / (T - B),     0.0f,          0.0f },
		{ 0.0f,               0.0f,               1 / (F - N),   0.0f },
		{ (R + L) / (L - R),  (T + B) / (B - T),  N / (F - N),   1.0f },
	};

	@autoreleasepool
	{
		id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)target->BeginPassWithClear(0, 0, 0, 0);
		if (enc == nil)
			return false;
		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pipeline];
		[enc setVertexBytes:quad length:sizeof(quad) atIndex:0];
		[enc setVertexBytes:ortho length:sizeof(ortho) atIndex:1];
		[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)sampler atIndex:0];
		[enc setFragmentTexture:(__bridge id<MTLTexture>)tex0 atIndex:0];
		if (tex1 != NULL)
			[enc setFragmentTexture:(__bridge id<MTLTexture>)tex1 atIndex:1];
		if (u != NULL)
			[enc setFragmentBytes:u length:sizeof(SMTGainMapUniforms) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
		// EndPass commits on the backend's queue. Anything committed after it on the
		// same queue -- the frame that draws the composite, a readback
		// (ReadTexturePixelsFloat waits) -- sees this pass done.
		target->EndPass();
	}
	return true;
}

bool CRenderShaderGainMapMetal::RenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4])
{
	if (!isCompiled || base == NULL || target == NULL || base->gainMap == NULL)
		return false;
	CSlrImage *map = base->gainMap;
	if (!base->isBound || !map->isBound || base->TexturePtr() == NULL || map->TexturePtr() == NULL)
		return false;
	if (target->GetFormat() != RENDER_TEXTURE_RGBA16F)
		return false;

	SMTGainMapUniforms u;
	MT_GainMapUniformsFrom(base->gainMapApply, base, map, &u);
	// The sampler production binds for this image (ScopedImageSampler ->
	// QueueSamplerForImage): magnify-nearest for a cache image, linear otherwise.
	void *sampler = renderBackend->GetImageSamplerState(base->linearScaling);
	const float rect[4] = { 0.f, 0.f, (float)target->GetWidth(), (float)target->GetHeight() };
	const float uv[4] = { base->defaultTexStartX, base->defaultTexStartY, base->defaultTexEndX, base->defaultTexEndY };
	return DrawQuadToTarget(target, testPipelinePtr, sampler, base->TexturePtr(), map->TexturePtr(), &u, rect, uv, rgba);
}

bool CRenderShaderGainMapMetal::RenderComposite(CSlrImage *base, CRenderTarget *target)
{
	if (!isCompiled || base == NULL || target == NULL || base->gainMap == NULL)
		return false;
	CSlrImage *map = base->gainMap;
	if (!base->isBound || !map->isBound || base->TexturePtr() == NULL || map->TexturePtr() == NULL)
		return false;

	// The base TEXTURE's own size: the composite keeps its texel layout, so the
	// caller draws it with the base's UVs (a power-of-two-padded base included).
	id<MTLTexture> bt = (__bridge id<MTLTexture>)base->TexturePtr();
	const int TW = (int)bt.width, TH = (int)bt.height;
	if (TW <= 0 || TH <= 0 || !target->Create(TW, TH, RENDER_TEXTURE_RGBA16F))
		return false;

	SMTGainMapUniforms u;
	MT_GainMapUniformsFrom(base->gainMapApply, base, map, &u);
	u.flags[3] = 2.f;   // the composite: one apply per base texel, never the filtered branch
	void *sampler = renderBackend->GetImageSamplerState(base->linearScaling);   // bound, unused by that branch
	// The image rect in target pixels: exactly one fragment per base texel.
	const float rect[4] = { base->defaultTexStartX * TW, base->defaultTexStartY * TH,
	                        base->defaultTexEndX * TW,   base->defaultTexEndY * TH };
	const float uv[4] = { base->defaultTexStartX, base->defaultTexStartY, base->defaultTexEndX, base->defaultTexEndY };
	const float white[4] = { 1.f, 1.f, 1.f, 1.f };
	return DrawQuadToTarget(target, testPipelinePtr, sampler, base->TexturePtr(), map->TexturePtr(), &u, rect, uv,
	                        white);
}

bool CRenderShaderGainMapMetal::DrawTextureToTargetForTest(void *texture, const float uvRect[4], bool linearScaling,
                                                           CRenderTarget *target, const float rgba[4])
{
	if (!isCompiled || texture == NULL || target == NULL || target->GetFormat() != RENDER_TEXTURE_RGBA16F)
		return false;
	void *sampler = renderBackend->GetImageSamplerState(linearScaling);
	const float rect[4] = { 0.f, 0.f, (float)target->GetWidth(), (float)target->GetHeight() };
	return DrawQuadToTarget(target, plainPipelinePtr, sampler, texture, NULL, NULL, rect, uvRect, rgba);
}
