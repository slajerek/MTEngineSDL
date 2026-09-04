#include "CRenderShaderCustomFragmentMetal.h"
#include "CRenderBackendMetal.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "imgui.h"

#import <Metal/Metal.h>

// The preamble -- everything the host's mainImage needs in scope, and nothing
// more. GetPreambleLineCount() COUNTS this string; do not state a number.
//
// packed_float3, not float3: see the header. The #defines make the body read
// like the GLSL even though the uniforms arrive as a parameter.
//
// TWO SPELLINGS DIFFER FROM THE GLSL AND ARE NOT COSMETIC:
//
//  * channelTime is a float4, not float[4]. An MSL array member of floats has
//    a 4-byte stride while HLSL pads each element to 16, and the C++ struct is
//    laid out for HLSL; float4 indexes the same as an array in both languages.
//
//  * channelResolution is float4[4] in the struct (the 16-byte stride the C++
//    uses) but iChannelResolution hands out float3s through mtChannelRes.
//    Without that, `float3 r = iChannelResolution[0];` -- which a pasted
//    ShaderToy shader may well write -- would be a type error here and compile
//    everywhere else.
static const char *kPreamble = R"MSL(#include <metal_stdlib>
using namespace metal;
struct MTVertexOut { float4 position [[position]]; float2 uv; float4 color; };
struct MTShaderToyUniforms {
	packed_float3 resolution; float time; float timeDelta; float frameRate; int frame; float _pad0;
	float4 mouse; float4 date; float4 channelTime; float4 channelResolution[4];
	float sampleRate; packed_float3 _pad1; float4 channelUvTransform[4]; float4 channelWrap;
};
static_assert(sizeof(MTShaderToyUniforms) == 240, "MSL uniforms must match SShaderToyUniforms");
struct MTChannelRes { float3 v[4]; float3 operator[](int i) const { return v[i]; } };
static inline MTChannelRes mtChannelRes(constant MTShaderToyUniforms &U)
{
	MTChannelRes r;
	for (int i = 0; i < 4; i++) r.v[i] = float3(U.channelResolution[i].xyz);
	return r;
}
#define iResolution float3(U.resolution)
#define iTime       U.time
#define iTimeDelta  U.timeDelta
#define iFrameRate  U.frameRate
#define iFrame      U.frame
#define iMouse      U.mouse
#define iDate       U.date
#define iChannelTime U.channelTime
#define iChannelResolution mtChannelRes(U)
#define iSampleRate U.sampleRate
static inline float2 mtChannelUV(int n, float2 uv, constant MTShaderToyUniforms &U)
{
	// VFLIP FIRST -- see the GLSL copy of this function for why it exists at
	// all: ShaderToy's fragCoord is bottom-left, the texture's v=0 is its top
	// row, and shadertoy.com solves it with the same per-channel default.
	if (U.channelUvTransform[n].z > 0.5) uv.y = 1.0 - uv.y;
	uv = (U.channelWrap[n] > 0.5) ? fract(uv) : clamp(uv, 0.0, 1.0);
	return uv * U.channelUvTransform[n].xy;
}
#define texChannel0(uv) iChannel0.sample(iChannel0Sampler, mtChannelUV(0, uv, U))
#define texChannel1(uv) iChannel1.sample(iChannel1Sampler, mtChannelUV(1, uv, U))
#define texChannel2(uv) iChannel2.sample(iChannel2Sampler, mtChannelUV(2, uv, U))
#define texChannel3(uv) iChannel3.sample(iChannel3Sampler, mtChannelUV(3, uv, U))
#define MT_CHANNEL_PARAMS texture2d<float> iChannel0, sampler iChannel0Sampler, \
						  texture2d<float> iChannel1, sampler iChannel1Sampler, \
						  texture2d<float> iChannel2, sampler iChannel2Sampler, \
						  texture2d<float> iChannel3, sampler iChannel3Sampler
#define MAIN_IMAGE void mainImage(thread float4 &fragColor, float2 fragCoord, \
								  constant MTShaderToyUniforms &U, MT_CHANNEL_PARAMS)
void mainImage(thread float4 &fragColor, float2 fragCoord, constant MTShaderToyUniforms &U,
			   MT_CHANNEL_PARAMS);
)MSL";

// The trailer: the fragment entry, then the vertex stage.
//
// AFTER the host's code, so nothing here shifts a reported line number. MSL is
// ONE library holding both entry points -- CompileShaders() looks up
// GetVertexFunctionName() and GetFragmentFunctionName() in the same library --
// so the vertex function has to be in this string. MTVertexIn, MTUniforms and
// mtVertexMain are copied verbatim from platform/MacOS/shaders/MTShaderToy.metal
// and MUST keep matching the ImDrawVert vertex descriptor CompileShaders()
// builds: the callback only swaps the pipeline, ImGui supplies the geometry.
static const char *kTrailer = R"MSL(
fragment float4 mtFragmentMain(MTVertexOut in [[stage_in]],
							   constant MTShaderToyUniforms &U [[buffer(0)]],
							   texture2d<float> iChannel0 [[texture(1)]],
							   sampler iChannel0Sampler [[sampler(1)]],
							   texture2d<float> iChannel1 [[texture(2)]],
							   sampler iChannel1Sampler [[sampler(2)]],
							   texture2d<float> iChannel2 [[texture(3)]],
							   sampler iChannel2Sampler [[sampler(3)]],
							   texture2d<float> iChannel3 [[texture(4)]],
							   sampler iChannel3Sampler [[sampler(4)]])
{
	// ShaderToy's origin is bottom-left and its coordinate is in pixels;
	// ImGui's UV is top-left and normalised.
	float2 fragCoord = float2(in.uv.x * U.resolution.x,
							  (1.0 - in.uv.y) * U.resolution.y);
	float4 outColor = float4(0.0);
	mainImage(outColor, fragCoord, U,
			  iChannel0, iChannel0Sampler, iChannel1, iChannel1Sampler,
			  iChannel2, iChannel2Sampler, iChannel3, iChannel3Sampler);
	// Modulate by the vertex colour, as MTShaderToy.metal does.
	return outColor * in.color;
}

struct MTVertexIn
{
	float2 position [[attribute(0)]];
	float2 uv       [[attribute(1)]];
	uchar4 color    [[attribute(2)]];
};

struct MTUniforms
{
	float4x4 projectionMatrix;
};

vertex MTVertexOut mtVertexMain(MTVertexIn in [[stage_in]],
								constant MTUniforms &uniforms [[buffer(1)]])
{
	MTVertexOut out;
	out.position = uniforms.projectionMatrix * float4(in.position, 0, 1);
	out.uv = in.uv;
	out.color = float4(in.color) / float4(255.0);
	return out;
}
)MSL";

CRenderShaderCustomFragmentMetal::CRenderShaderCustomFragmentMetal(
		CRenderBackendMetal *renderBackend, const char *name)
: CRenderShaderMetal(renderBackend, name)
{
}

CRenderShaderCustomFragmentMetal::~CRenderShaderCustomFragmentMetal()
{
	// ~CRenderShaderMetal releases libraryPtr and pipelinePtr, and tolerates
	// both being NULL -- the case the seam test exercises by creating one of
	// these and deleting it uncompiled. The channel objects are ours, and each
	// is guarded for the same reason.
	if (samplerNearestPtr != NULL)
	{
		id<MTLSamplerState> s = (__bridge_transfer id<MTLSamplerState>)samplerNearestPtr;
		s = nil;
		samplerNearestPtr = NULL;
	}
	if (samplerLinearPtr != NULL)
	{
		id<MTLSamplerState> s = (__bridge_transfer id<MTLSamplerState>)samplerLinearPtr;
		s = nil;
		samplerLinearPtr = NULL;
	}
	if (blackTexturePtr != NULL)
	{
		id<MTLTexture> t = (__bridge_transfer id<MTLTexture>)blackTexturePtr;
		t = nil;
		blackTexturePtr = NULL;
	}
}

const char *CRenderShaderCustomFragmentMetal::GetMetalShaderSource()
{
	return fullSource.c_str();
}

const void *CRenderShaderCustomFragmentMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = 0;
	return NULL;
}

int CRenderShaderCustomFragmentMetal::GetPreambleLineCount()
{
	int lines = 0;
	for (const char *p = kPreamble; *p != '\0'; p++)
	{
		if (*p == '\n')
			lines++;
	}
	// No version line here -- unlike the GL path, CRenderShaderMetal prepends
	// nothing of its own.
	return lines;
}

bool CRenderShaderCustomFragmentMetal::SetFragmentSource(const char *mainImageSource)
{
	if (mainImageSource == NULL)
		mainImageSource = "";

	fullSource  = kPreamble;
	fullSource += mainImageSource;
	fullSource += "\n";
	fullSource += kTrailer;

	// BUILD NEW, SWAP ON SUCCESS. Hold the working library and pipeline aside
	// so CompileShaders() builds fresh ones into the members; on failure they
	// go back and the host keeps drawing the last shader that worked.
	void *oldLibrary  = libraryPtr;   libraryPtr  = NULL;
	void *oldPipeline = pipelinePtr;  pipelinePtr = NULL;

	// BOTH LATCHES. compileFailed exists to stop a failed compile being retried
	// every frame; leaving it set would make this call do nothing at all.
	isCompiled = false;
	compileFailed = false;

	CompileShaders();

	if (!isCompiled)
	{
		// Release whatever the failed attempt did manage to create, then put
		// the last good pair back.
		if (libraryPtr != NULL)
		{
			id<MTLLibrary> partial = (__bridge_transfer id<MTLLibrary>)libraryPtr;
			partial = nil;
			libraryPtr = NULL;
		}
		libraryPtr  = oldLibrary;
		pipelinePtr = oldPipeline;
		isCompiled  = (oldPipeline != NULL);
		compileFailed = false;
		return false;
	}

	// __bridge_transfer, matching ~CRenderShaderMetal: these were retained with
	// __bridge_retained, and handing them back to ARC is how that is undone.
	if (oldLibrary != NULL)
	{
		id<MTLLibrary> library = (__bridge_transfer id<MTLLibrary>)oldLibrary;
		library = nil;
	}
	if (oldPipeline != NULL)
	{
		id<MTLRenderPipelineState> pipeline = (__bridge_transfer id<MTLRenderPipelineState>)oldPipeline;
		pipeline = nil;
	}
	return true;
}

void CRenderShaderCustomFragmentMetal::SetShaderVars(void *encoder)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)encoder;
	// atIndex:0 matches the entry point's [[buffer(0)]]. sizeof is 240, pinned
	// by the static_asserts in CRenderShaderCustomFragment.h and repeated as a
	// static_assert inside the MSL preamble, so the two cannot drift apart
	// without one of them refusing to build.
	[enc setFragmentBytes:&uniforms length:sizeof(SShaderToyUniforms) atIndex:0];

	// INDICES 1..4, never 0: imgui_impl_metal binds the draw command's own
	// texture at fragment index 0, after the callback that installed this
	// pipeline, so slot 0 is not ours to use.
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		void *tex = (channelTexture[i] != NULL) ? channelTexture[i] : BlackTexture();
		[enc setFragmentTexture:(__bridge id<MTLTexture>)tex atIndex:(1 + i)];
		[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)SamplerFor(channelFilter[i])
							 atIndex:(1 + i)];
	}
}

void CRenderShaderCustomFragmentMetal::ResetState()
{
	// UNBIND 1..4 BEFORE ImGui's state reset. ImDrawCallback_ResetRenderState
	// makes imgui_impl_metal re-establish its pipeline and its own index-0
	// texture; it knows nothing of the higher slots, so a texture left there
	// stays retained by the encoder until the frame ends. Nothing samples it
	// -- ImGui's fragment shader reads slot 0 only -- so this is hygiene
	// rather than a fix, which is why it is worth stating that it IS hygiene.
	ImDrawList *drawList = ImGui::GetWindowDrawList();
	drawList->AddCallback([](const ImDrawList *list, const ImDrawCmd *cmd)
	{
		CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
		if (backend == NULL)
			return;
		id<MTLRenderCommandEncoder> enc =
			(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
		if (enc == nil)
			return;
		for (int i = 0; i < kShaderChannelCount; i++)
		{
			[enc setFragmentTexture:nil atIndex:(1 + i)];
			[enc setFragmentSamplerState:nil atIndex:(1 + i)];
		}
	}, (void *)this);

	CRenderShaderMetal::ResetState();
}

void *CRenderShaderCustomFragmentMetal::SamplerFor(EShaderChannelFilter filter)
{
	void **slot = (filter == SHADER_CHANNEL_LINEAR) ? &samplerLinearPtr : &samplerNearestPtr;
	if (*slot != NULL)
		return *slot;

	id<MTLDevice> device = (__bridge id<MTLDevice>)renderBackend->GetMetalDevice();
	if (device == nil)
		return NULL;

	MTLSamplerDescriptor *descriptor = [[MTLSamplerDescriptor alloc] init];
	MTLSamplerMinMagFilter f = (filter == SHADER_CHANNEL_LINEAR) ? MTLSamplerMinMagFilterLinear
																 : MTLSamplerMinMagFilterNearest;
	descriptor.minFilter = f;
	descriptor.magFilter = f;
	// CLAMP, always -- MT_CHUV in the preamble does the wrapping, because
	// CSlrImage pads every texture to a power of two and hardware repeat would
	// tile the padding rather than the image.
	descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
	descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;

	id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:descriptor];
	*slot = (__bridge_retained void *)sampler;
	return *slot;
}

void *CRenderShaderCustomFragmentMetal::BlackTexture()
{
	if (blackTexturePtr != NULL)
		return blackTexturePtr;

	id<MTLDevice> device = (__bridge id<MTLDevice>)renderBackend->GetMetalDevice();
	if (device == nil)
		return NULL;

	MTLTextureDescriptor *descriptor =
		[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
														   width:1 height:1 mipmapped:NO];
	id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
	const unsigned char black[4] = { 0, 0, 0, 255 };
	[texture replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:black bytesPerRow:4];

	blackTexturePtr = (__bridge_retained void *)texture;
	return blackTexturePtr;
}

// --- channels -------------------------------------------------------------
//
// Pure stores, RENDER THREAD ONLY. SetShaderVars does the binding, inside the
// draw callback where an encoder exists.

void CRenderShaderCustomFragmentMetal::SetChannelTexture(int channel, void *texture)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelTexture[channel] = texture;
}

void CRenderShaderCustomFragmentMetal::SetChannelSampler(int channel, EShaderChannelFilter filter,
							  EShaderChannelWrap wrap)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelFilter[channel] = filter;
	channelWrapMode[channel] = wrap;
}
