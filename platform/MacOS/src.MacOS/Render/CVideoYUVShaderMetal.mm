#include "CVideoYUVShaderMetal.h"
#include "Core/Render/CRenderTarget.h"
#include "DBG_Log.h"
#include "CRenderBackendMetal.h"
#include "Core/Render/VID_Main.h"

#import <Metal/Metal.h>
#import <simd/simd.h>

// ---------------------------------------------------------------------------
// MSL port of CVideoYUVShader's GLSL. Every constant below is copied from that
// file rather than re-derived -- a divergence here is a colour shift that only
// a side-by-side comparison would catch.
// ---------------------------------------------------------------------------
static const char *kMetalShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct VSOut {
    float4 position [[position]];
    float2 uv;
};

struct Uniforms {
    float4 transform;   // x, y, w, h in NDC
    int    rotation;    // 0/90/180/270, applied as a UV transform
    int    mode;        // EYUVShaderMode ordinal
    int    colorSpace;  // normalized VPX_CS_*: 1=601, 2=709, 5=2020ncl
    int    fullRange;
    int    hasAlpha;
    int    useLut;
    float  alpha;
    float  lutScale;    // (N-1)/N
    float  lutOffset;   // 1/(2N)
    // --- S-5 Phase 5: HDR transfer -------------------------------------
    int    colorTrc;    // 16 = PQ, 18 = HLG, anything else = SDR (skip it all)
    int    floatTarget; // 1: keep above-white. 0: tone-map into 0..1
    int    surfaceLinear;
    int    surfaceP3;
    float  toneMapHeadroom;
};

// ---------------------------------------------------------------------------
// HDR transfer maths -- GENERATED FROM CVideoTransferFunctions.h.
//
// Every constant below is copied from that header and MUST be updated with it.
// This is not a suggestion: the ImGui test `hdr_shader_agrees_with_transfer_header` runs a ramp of code values
// through THIS compiled shader on the GPU and compares against that header
// evaluated on the CPU, so a drift fails there with a named value
// rather than as an unexplained tolerance miss in a whole-frame comparison.
//
// A shader cannot include a C++ header (this source is a string handed to the
// Metal compiler), which is exactly why the agreement test exists.
// ---------------------------------------------------------------------------

// PQ (SMPTE ST 2084) EOTF: encoded 0..1 -> absolute luminance, 1.0 == 10000 nits.
static inline float PqEotf(float e) {
    const float m1 = 0.1593017578125, m2 = 78.84375;
    const float c1 = 0.8359375, c2 = 18.8515625, c3 = 18.6875;
    float p = pow(max(e, 0.0), 1.0 / m2);
    float num = p - c1;
    float den = c2 - c3 * p;
    if (num <= 0.0 || den <= 0.0) return 0.0;
    return pow(num / den, 1.0 / m1);
}

// HLG (ARIB STD-B67) inverse OETF: encoded 0..1 -> SCENE linear.
static inline float HlgInverseOetf(float e) {
    const float a = 0.17883277, b = 0.28466892, c = 0.55991073;
    e = max(e, 0.0);
    return (e <= 0.5) ? (e * e) / 3.0 : (exp((e - c) / a) + b) / 12.0;
}

// The IEC sRGB curve, sign-symmetric and CONTINUED past 1.0 rather than
// clamped -- that continuation is how kCGColorSpaceExtendedSRGB defines a
// value of 2.0 as "twice reference white". Mirrors SrgbExtendedEncode in
// MT_SrgbCurve.h (it lived in CImageData.h until S-6 A1 split the curve into
// a leaf header).
static inline float SrgbExtendedEncode(float v) {
    float a = abs(v);
    float e = (a <= 0.0031308) ? (a * 12.92) : (1.055 * pow(a, 1.0 / 2.4) - 0.055);
    return (v < 0.0) ? -e : e;
}

// Extended Reinhard, normalised so `headroom` maps exactly to 1.0. At
// headroom 1.0 it is EXACTLY the identity on 0..1, which is the property the
// SDR regression rests on. Same curve as CImageData::ConvertRGBA16FToRGBA8,
// which is what the POSTER goes through when the resident format is 8-bit.
static inline float ToneMapReinhard(float v, float headroom) {
    float h = max(headroom, 1.0);
    v = max(v, 0.0);
    float t = v * (1.0 + v / (h * h)) / (1.0 + v);
    return min(t, 1.0);
}

// Fullscreen quad, matching the GL path's two triangles in NDC [0,1] UV space.
constant float2 kQuadPos[4]  = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };

vertex VSOut yuvVertex(uint vid [[vertex_id]],
                       constant Uniforms &u [[buffer(0)]])
{
    float2 aPos = kQuadPos[vid];
    VSOut out;
    float2 pos = aPos * u.transform.zw + u.transform.xy;
    out.position = float4(pos, 0.0, 1.0);

    // V IS FLIPPED RELATIVE TO POSITION, exactly as the GL path does it.
    //
    // The GL quad carries TWO attributes and they are not the same: aPos runs
    // (0,0)..(1,1) while aTexCoord is (aPos.x, 1 - aPos.y) -- see
    // CVideoYUVShader::CreateQuadVAO, "texcoord Y flipped for video
    // orientation". Porting `uv = aPos` dropped that flip and every video
    // played upside-down on Metal.
    //
    // ONE flip, not two. GL additionally inverts the quad's Y through its
    // transform (-1, 1, 2, -2) because a GL FBO is bottom-up while every
    // CSlrImage consumer draws texel row 0 at the top; a Metal texture is
    // already top-down, so this path keeps the plain (-1, -1, 2, 2) transform
    // and gets the same result. Copying BOTH of GL's inversions would have
    // cancelled out and left the video upside-down again.
    //
    // Rotation is then derived from the flipped uv, matching GL, which applies
    // it to aTexCoord: rot90 is a COUNTER-clockwise turn of the coded pixels,
    // so u_src = 1 - v_out, v_src = u_out (and symmetrically for 180/270).
    float2 uv = float2(aPos.x, 1.0 - aPos.y);
    if (u.rotation == 90)       out.uv = float2(1.0 - uv.y, uv.x);
    else if (u.rotation == 180) out.uv = float2(1.0 - uv.x, 1.0 - uv.y);
    else if (u.rotation == 270) out.uv = float2(uv.y, 1.0 - uv.x);
    else                        out.uv = uv;
    return out;
}

fragment float4 yuvFragment(VSOut in [[stage_in]],
                            constant Uniforms &u        [[buffer(0)]],
                            texture2d<float> texY       [[texture(0)]],
                            texture2d<float> texU       [[texture(1)]],
                            texture2d<float> texV       [[texture(2)]],
                            texture2d<float> texA       [[texture(3)]],
                            texture3d<float> texLut     [[texture(4)]],
                            sampler samp                [[sampler(0)]])
{
    float y, uu, vv;

    if (u.mode == 2) {
        // YUV420P10: R16 planes holding the raw 10-bit code word (0..1023)
        // LSB-aligned. Sampling normalizes by /65535, so scale back by
        // 65535/1023 to recover the 10-bit-normalized [0,1] sample.
        const float k10 = 65535.0 / 1023.0;
        y  = texY.sample(samp, in.uv).r * k10;
        uu = texU.sample(samp, in.uv).r * k10;
        vv = texV.sample(samp, in.uv).r * k10;
    } else if (u.mode == 1) {
        // NV12: texU is the interleaved U,V plane (RG8).
        y = texY.sample(samp, in.uv).r;
        float2 uv2 = texU.sample(samp, in.uv).rg;
        uu = uv2.r;
        vv = uv2.g;
    } else {
        y  = texY.sample(samp, in.uv).r;
        uu = texU.sample(samp, in.uv).r;
        vv = texV.sample(samp, in.uv).r;
    }

    float yNorm, uNorm, vNorm;
    if (u.fullRange != 0) {
        yNorm = y;
        uNorm = uu - 0.5;
        vNorm = vv - 0.5;
    } else {
        yNorm = (y  - 16.0/255.0)  * (255.0/219.0);
        uNorm = (uu - 128.0/255.0) * (255.0/224.0);
        vNorm = (vv - 128.0/255.0) * (255.0/224.0);
    }

    float3 rgb;
    if (u.colorSpace == 5) {
        // BT.2020 non-constant luminance
        rgb.r = yNorm + 1.4746 * vNorm;
        rgb.g = yNorm - 0.16455 * uNorm - 0.57135 * vNorm;
        rgb.b = yNorm + 1.8814 * uNorm;
    } else if (u.colorSpace == 2) {
        // BT.709
        rgb.r = yNorm + 1.5748 * vNorm;
        rgb.g = yNorm - 0.1873 * uNorm - 0.4681 * vNorm;
        rgb.b = yNorm + 1.8556 * uNorm;
    } else {
        // BT.601
        rgb.r = yNorm + 1.402 * vNorm;
        rgb.g = yNorm - 0.344136 * uNorm - 0.714136 * vNorm;
        rgb.b = yNorm + 1.772 * uNorm;
    }
    // --- S-5 Phase 5: the HDR transfer ---------------------------------
    //
    // For an SDR clip this whole block is skipped and the original
    // clamp-then-LUT runs exactly as before -- byte-identical, which is what
    // keeps every existing clip unchanged.
    if (u.colorTrc == 16 || u.colorTrc == 18) {
        // 1. EOTF -> LINEAR, 1.0 == SDR reference white (203 nit, BT.2408).
        float3 lin;
        if (u.colorTrc == 16) {
            const float kPqScale = 10000.0 / 203.0;
            lin = float3(PqEotf(rgb.r), PqEotf(rgb.g), PqEotf(rgb.b)) * kPqScale;
        } else {
            const float kHlgScale = 1000.0 / 203.0;
            lin = float3(HlgInverseOetf(rgb.r), HlgInverseOetf(rgb.g), HlgInverseOetf(rgb.b));
            // HLG's display OOTF, driven by BT.2020 luma, applied only once all
            // three channels exist. PQ has no OOTF, which is why one transfer
            // can be right while the other is wrong.
            float ys = 0.2627 * lin.r + 0.6780 * lin.g + 0.0593 * lin.b;
            float gain = (ys > 0.0) ? pow(ys, 0.2) : 0.0;
            lin = lin * gain * kHlgScale;
        }

        // 2. BT.2020 -> sRGB primaries, linear, D65 (pure primaries change).
        //    Rows sum to 1.0 so neutral stays neutral.
        float3 srgbLin;
        srgbLin.r =  1.6605 * lin.r - 0.5876 * lin.g - 0.0728 * lin.b;
        srgbLin.g = -0.1246 * lin.r + 1.1329 * lin.g - 0.0083 * lin.b;
        srgbLin.b = -0.0182 * lin.r - 0.1006 * lin.g + 1.1187 * lin.b;

        // 3. PRIMARIES, for BOTH arms. The surface may be Display P3 rather
        //    than sRGB, and the POSTER applies this stage in both arms too --
        //    PC_EncodeForSurface runs its primaries step whatever the resident
        //    format turns out to be, and CSlrImage's 8-bit conversion then
        //    undoes only the TRANSFER (SrgbExtendedDecode), never the
        //    primaries. So a shader that applied P3 only on the float arm
        //    would put playback in sRGB primaries while the poster sat in P3,
        //    on the gate-closed path -- the phase's own divergence, back
        //    again, in the one arm Task 6 exists to keep aligned.
        //
        //    Order matters and matches the poster: primaries FIRST, then the
        //    tone-map, because the poster tone-maps values that already went
        //    through PC_EncodeForSurface.
        //
        //    FULL PRECISION, copied from PC_kLinearSrgbToLinearP3
        //    (DevelopMath.cpp:286). Rows sum to 1.0, so neutral stays neutral.
        if (u.surfaceP3 != 0) {
            float3 p3;
            p3.r = 0.8224621 * srgbLin.r + 0.1775380 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.g = 0.0331941 * srgbLin.r + 0.9668058 * srgbLin.g + 0.0000000 * srgbLin.b;
            p3.b = 0.0170827 * srgbLin.r + 0.0723974 * srgbLin.g + 0.9105199 * srgbLin.b;
            srgbLin = p3;
        }

        if (u.floatTarget != 0) {
            // 4a. GATE OPEN. Finish in the surface's own space, keeping values
            //     above 1.0. This mirrors PC_EncodeForSurface, which is what
            //     the POSTER goes through -- matching it IS the phase's
            //     acceptance criterion.
            if (u.surfaceLinear != 0) {
                rgb = srgbLin;              // extended LINEAR surface: as-is
            } else {
                rgb = float3(SrgbExtendedEncode(srgbLin.r),
                             SrgbExtendedEncode(srgbLin.g),
                             SrgbExtendedEncode(srgbLin.b));
            }
        } else {
            // 4b. GATE CLOSED -- and this is NOT a consolation prize. It is
            //     the arm every user without headroom sees, which is most
            //     users most of the time, and today it is the broken one.
            //     Tone-map in LINEAR, then encode to plain sRGB.
            rgb = float3(SrgbExtendedEncode(ToneMapReinhard(srgbLin.r, u.toneMapHeadroom)),
                         SrgbExtendedEncode(ToneMapReinhard(srgbLin.g, u.toneMapHeadroom)),
                         SrgbExtendedEncode(ToneMapReinhard(srgbLin.b, u.toneMapHeadroom)));
        }
    } else {
        // THE SDR PATH, UNCHANGED. The clamp stays here and only here --
        // removing it wholesale would change every existing clip, and it is
        // the above-white killer only on the HDR branch, which no longer runs
        // through it.
        rgb = clamp(rgb, 0.0, 1.0);
    }

    // CM-E display transform, over ENCODED R'G'B'. No LUT bound must be
    // bit-identical to the pre-CM-E path, which is why useLut gates it rather
    // than binding an identity lattice.
    //
    // For HDR clips useLut is ALREADY 0 and stays that way: CM-E's
    // InstallColorLutOnPlayer installs no LUT when trc is 16/18, because an
    // SDR source->display transform over PQ/HLG values would be confidently
    // wrong. The poster lane skips its equivalent for the same reason. Left
    // closed here deliberately; re-opening it for the tone-mapped arm is a
    // follow-up that must move BOTH lanes together or they disagree again.
    if (u.useLut != 0) {
        rgb = texLut.sample(samp, rgb * u.lutScale + u.lutOffset).rgb;
    }

    float a = u.alpha;
    if (u.hasAlpha != 0) {
        a *= texA.sample(samp, in.uv).r;
    }
    return float4(rgb, a);
}
)MSL";

namespace
{
	struct MetalYUVUniforms
	{
		simd::float4 transform;
		int   rotation;
		int   mode;
		int   colorSpace;
		int   fullRange;
		int   hasAlpha;
		int   useLut;
		float alpha;
		float lutScale;
		float lutOffset;
		// --- S-5 Phase 5: HDR transfer. MUST match the MSL Uniforms struct
		// field for field and in ORDER -- this is a raw byte copy into the
		// shader's buffer, so a mismatch here silently misreads every field
		// after it rather than failing to compile.
		int   colorTrc;
		int   floatTarget;
		int   surfaceLinear;
		int   surfaceP3;
		float toneMapHeadroom;
		float _pad[3];         // keep the struct 16-byte friendly
	};

	// Fills the HDR half from the caller's description. One place, so the two
	// draw paths cannot disagree -- and Render() passes a DEFAULT-constructed
	// SVideoHdrOutput, which is how the "no leaked PQ EOTF into another app's
	// cutscene" guarantee is actually implemented rather than merely intended.
	static void ApplyHdrUniforms(MetalYUVUniforms &u, const SVideoHdrOutput &hdr)
	{
		u.colorTrc        = hdr.IsHdr() ? hdr.colorTrc : 2;
		u.floatTarget     = hdr.floatTarget ? 1 : 0;
		u.surfaceLinear   = hdr.surfaceIsLinear ? 1 : 0;
		u.surfaceP3       = hdr.surfaceIsP3 ? 1 : 0;
		u.toneMapHeadroom = (hdr.toneMapHeadroom >= 1.0f) ? hdr.toneMapHeadroom : 1.0f;
	}
}

CVideoYUVShaderMetal::CVideoYUVShaderMetal(void *device, void *commandQueue)
: devicePtr(NULL), queuePtr(NULL), libraryPtr(NULL), pipelinePtr(NULL),
  pipelineFloatPtr(NULL), pipelineSurfacePtr(NULL), surfacePipelineFormat(0), samplerPtr(NULL),
  dummyTexPtr(NULL), dummyLut3DPtr(NULL), compiled(false), compileFailed(false)
{
	devicePtr = (void *)CFBridgingRetain((__bridge id)device);
	queuePtr  = (void *)CFBridgingRetain((__bridge id)commandQueue);
}

CVideoYUVShaderMetal::~CVideoYUVShaderMetal()
{
	if (libraryPtr)          CFRelease(libraryPtr);
	if (pipelineFloatPtr)    CFRelease(pipelineFloatPtr);
	if (pipelineSurfacePtr)  CFRelease(pipelineSurfacePtr);
	if (pipelinePtr)   CFRelease(pipelinePtr);
	if (samplerPtr)    CFRelease(samplerPtr);
	if (dummyTexPtr)   CFRelease(dummyTexPtr);
	if (dummyLut3DPtr) CFRelease(dummyLut3DPtr);
	if (queuePtr)      CFRelease(queuePtr);
	if (devicePtr)     CFRelease(devicePtr);
}

// ---------------------------------------------------------------------------
// PipelineForPixelFormat -- one pipeline per colour-attachment format
// ---------------------------------------------------------------------------
//
// A MTLRenderPipelineState bakes in its colour-attachment format, and Metal
// validates it against the render pass at DRAW time rather than at creation --
// so a mismatch is a runtime abort in a place that looks nothing like the
// cause. CRenderShaderMetal.mm carries the same warning for the same reason.
//
// This converter now draws into THREE different attachment formats:
//   - RGBA8Unorm       the offscreen target for SDR clips (and HDR ones with
//                      the session gate closed)
//   - RGBA16Float      the offscreen target for HDR clips with the gate open
//   - the SURFACE's    the direct-to-screen Render() path, which encodes into
//     format           the FRAME's encoder -- BGRA8Unorm normally, RGBA16Float
//                      whenever the HDR surface is active
//
// That third case was ALREADY latently wrong before Phase 5: Render() set the
// hardcoded RGBA8 pipeline whatever the surface was, so it would have failed
// validation on an HDR surface. It is fixed here rather than left, because the
// same cache answers it.
//
// Three formats, three cached slots, no dictionary: the set is closed and
// known, and a std::map of Objective-C handles in a hot draw path buys nothing.
void *CVideoYUVShaderMetal::PipelineForPixelFormat(unsigned long mtlPixelFormat)
{
	const MTLPixelFormat fmt = (MTLPixelFormat)mtlPixelFormat;

	if (fmt == MTLPixelFormatRGBA8Unorm && pipelinePtr != NULL)
		return pipelinePtr;
	if (fmt == MTLPixelFormatRGBA16Float && pipelineFloatPtr != NULL)
		return pipelineFloatPtr;
	if (pipelineSurfacePtr != NULL && surfacePipelineFormat == mtlPixelFormat)
		return pipelineSurfacePtr;

	id<MTLDevice> device = (__bridge id<MTLDevice>)devicePtr;
	id<MTLLibrary> library = (__bridge id<MTLLibrary>)libraryPtr;
	if (device == nil || library == nil)
		return NULL;

	NSError *error = nil;
	MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
	desc.vertexFunction   = [library newFunctionWithName:@"yuvVertex"];
	desc.fragmentFunction = [library newFunctionWithName:@"yuvFragment"];
	desc.colorAttachments[0].pixelFormat = fmt;
	desc.colorAttachments[0].blendingEnabled = YES;
	desc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
	desc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
	desc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
	desc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

	id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:desc error:&error];
	if (pso == nil)
	{
		LOGError("CVideoYUVShaderMetal: pipeline creation failed for pixelFormat %lu: %s",
				 mtlPixelFormat,
				 error ? [[error localizedDescription] UTF8String] : "(no error object)");
		return NULL;
	}

	void *retained = (void *)CFBridgingRetain(pso);
	if (fmt == MTLPixelFormatRGBA8Unorm)
	{
		pipelinePtr = retained;
	}
	else if (fmt == MTLPixelFormatRGBA16Float)
	{
		pipelineFloatPtr = retained;
	}
	else
	{
		// The surface slot holds one format at a time. A surface format change
		// is a rare event (HDR toggling, a monitor move), so releasing and
		// rebuilding beats carrying an unbounded cache.
		if (pipelineSurfacePtr != NULL)
			CFRelease(pipelineSurfacePtr);
		pipelineSurfacePtr = retained;
		surfacePipelineFormat = mtlPixelFormat;
	}
	return retained;
}

bool CVideoYUVShaderMetal::Compile()
{
	// LATCHED. Without this a failed compile would be retried on every frame the
	// converter is used, leaking a library and a pipeline each time -- the same
	// trap CRenderShaderOpenGL4::UseShaderProgram() has with isCompiled.
	if (compiled)      return true;
	if (compileFailed) return false;

	@autoreleasepool
	{
		id<MTLDevice> device = (__bridge id<MTLDevice>)devicePtr;
		if (device == nil) { compileFailed = true; return false; }

		NSError *error = nil;
		id<MTLLibrary> library =
			[device newLibraryWithSource:[NSString stringWithUTF8String:kMetalShaderSource]
								 options:nil
								   error:&error];
		if (library == nil)
		{
			// Print the compiler diagnostic. A Metal shader that fails to compile
			// otherwise renders nothing with no explanation at all.
			LOGError("CVideoYUVShaderMetal: MSL compile failed: %s",
					 error ? [[error localizedDescription] UTF8String] : "(no error object)");
			compileFailed = true;
			return false;
		}

		// KEEP THE LIBRARY. A pipeline bakes in its colour-attachment format
		// and Metal validates that against the PASS at draw time, so one
		// pipeline cannot serve an RGBA8 target, an RGBA16Float target and the
		// surface. Holding the compiled library lets the others be built on
		// demand without recompiling the MSL (S-5 Phase 5).
		libraryPtr = (void *)CFBridgingRetain(library);

		// The RGBA8 one eagerly: it is what every SDR clip uses, and building
		// it here keeps Compile()'s "did the shader work at all" contract.
		pipelinePtr = PipelineForPixelFormat((unsigned long)MTLPixelFormatRGBA8Unorm);
		if (pipelinePtr == NULL)
		{
			compileFailed = true;
			return false;
		}

		MTLSamplerDescriptor *sd = [[MTLSamplerDescriptor alloc] init];
		sd.minFilter = MTLSamplerMinMagFilterLinear;
		sd.magFilter = MTLSamplerMinMagFilterLinear;
		sd.sAddressMode = MTLSamplerAddressModeClampToEdge;
		sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
		sd.rAddressMode = MTLSamplerAddressModeClampToEdge;
		samplerPtr = (void *)CFBridgingRetain([device newSamplerStateWithDescriptor:sd]);

		// Metal validates that every texture slot the shader declares is bound,
		// even on branches that never execute -- unlike GL, where an unbound
		// sampler is merely undefined. These 1x1 stand-ins keep the unused slots
		// legal without branching the shader into per-mode variants.
		MTLTextureDescriptor *dd = [MTLTextureDescriptor
			texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm width:1 height:1 mipmapped:NO];
		dd.usage = MTLTextureUsageShaderRead;
		dd.storageMode = MTLStorageModeShared;
		id<MTLTexture> dummy = [device newTextureWithDescriptor:dd];
		uint8_t zero = 0;
		[dummy replaceRegion:MTLRegionMake2D(0,0,1,1) mipmapLevel:0 withBytes:&zero bytesPerRow:1];
		dummyTexPtr = (void *)CFBridgingRetain(dummy);

		MTLTextureDescriptor *d3 = [[MTLTextureDescriptor alloc] init];
		d3.textureType = MTLTextureType3D;
		d3.pixelFormat = MTLPixelFormatRGBA16Unorm;
		d3.width = d3.height = d3.depth = 1;
		d3.usage = MTLTextureUsageShaderRead;
		d3.storageMode = MTLStorageModeShared;
		id<MTLTexture> dummy3 = [device newTextureWithDescriptor:d3];
		uint16_t zeros[4] = {0,0,0,0};
		[dummy3 replaceRegion:MTLRegionMake3D(0,0,0,1,1,1) mipmapLevel:0 slice:0
					withBytes:zeros bytesPerRow:8 bytesPerImage:8];
		dummyLut3DPtr = (void *)CFBridgingRetain(dummy3);

		compiled = true;
		return true;
	}
}

void CVideoYUVShaderMetal::RenderToTarget(EYUVShaderMode mode,
										  void *texY, void *texUVorU, void *texV,
										  bool hasAlpha, void *texA,
										  int colorSpace, bool fullRange,
										  int rotationDegrees,
										  void *lutTexture, int lutEdge,
										  CRenderTarget &target,
										  const SVideoHdrOutput &hdr)
{
	if (!Compile())
		return;

	@autoreleasepool
	{
		// THE PIPELINE MUST MATCH THE PASS. Pick it from the target's own
		// format -- an RGBA16Float target encoded with the RGBA8 pipeline is a
		// draw-time validation failure, not a wrong colour (S-5 Phase 5).
		void *pso = PipelineForPixelFormat(
			(target.GetFormat() == RENDER_TEXTURE_RGBA16F)
				? (unsigned long)MTLPixelFormatRGBA16Float
				: (unsigned long)MTLPixelFormatRGBA8Unorm);
		if (pso == NULL)
			return;

		id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)target.BeginPass();
		if (enc == nil)
			return;

		id<MTLTexture> dummy  = (__bridge id<MTLTexture>)dummyTexPtr;
		id<MTLTexture> dummy3 = (__bridge id<MTLTexture>)dummyLut3DPtr;

		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pso];
		[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)samplerPtr atIndex:0];
		[enc setFragmentTexture:(texY      ? (__bridge id<MTLTexture>)texY      : dummy) atIndex:0];
		[enc setFragmentTexture:(texUVorU  ? (__bridge id<MTLTexture>)texUVorU  : dummy) atIndex:1];
		[enc setFragmentTexture:(texV      ? (__bridge id<MTLTexture>)texV      : dummy) atIndex:2];
		[enc setFragmentTexture:(texA      ? (__bridge id<MTLTexture>)texA      : dummy) atIndex:3];
		[enc setFragmentTexture:(lutTexture? (__bridge id<MTLTexture>)lutTexture: dummy3) atIndex:4];

		MetalYUVUniforms u = {};
		// Full-target quad in NDC: the offscreen target is exactly the display
		// size, so the transform maps the [0,1] quad onto [-1,1].
		u.transform = simd_make_float4(-1.0f, -1.0f, 2.0f, 2.0f);
		u.rotation   = rotationDegrees;
		u.mode       = (int)mode;
		u.colorSpace = colorSpace;
		u.fullRange  = fullRange ? 1 : 0;
		u.hasAlpha   = hasAlpha ? 1 : 0;
		u.useLut     = (lutTexture != NULL && lutEdge >= 2) ? 1 : 0;
		u.alpha      = 1.0f;
		if (u.useLut)
		{
			// Half-texel correction so the lattice endpoints land on texel
			// centres -- identical to the GL path.
			u.lutScale  = (float)(lutEdge - 1) / (float)lutEdge;
			u.lutOffset = 1.0f / (2.0f * (float)lutEdge);
		}
		ApplyHdrUniforms(u, hdr);

		[enc setVertexBytes:&u length:sizeof(u) atIndex:0];
		[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];

		target.EndPass();
	}
}

void CVideoYUVShaderMetal::Render(void *texY, void *texU, void *texV, void *texA,
								  bool hasAlpha, float alpha,
								  int colorSpace, bool fullRange,
								  float x, float y, float w, float h,
								  float screenW, float screenH,
								  const SVideoHdrOutput &hdr)
{
	// Direct-to-screen. This entry point has a live caller (CGuiViewVideoPlayer
	// draws through it from an ImGui draw callback), so it is ported rather than
	// dropped -- an earlier draft of the S-4 plan twice recorded it as dead,
	// from a grep that missed the call through a callback struct.
	//
	// It draws into the FRAME's encoder, which the caller must already have open;
	// unlike RenderToTarget there is no pass of our own to begin.
	if (!Compile())
		return;
	if (screenW <= 0.0f || screenH <= 0.0f)
		return;

	CRenderBackendMetal *backend = (CRenderBackendMetal *)VID_GetRenderBackend();
	if (backend == NULL)
		return;
	// May legitimately be nil: NewFrame() aborts the frame on a 0x0 window or a
	// nil drawable, and this can be reached from a draw callback on such a frame.
	id<MTLRenderCommandEncoder> enc =
		(__bridge id<MTLRenderCommandEncoder>)backend->GetCurrentRenderCommandEncoder();
	if (enc == nil)
		return;

	@autoreleasepool
	{
		id<MTLTexture> dummy  = (__bridge id<MTLTexture>)dummyTexPtr;
		id<MTLTexture> dummy3 = (__bridge id<MTLTexture>)dummyLut3DPtr;

		// This draws into the FRAME's encoder, so the pass is the SURFACE --
		// BGRA8Unorm normally, RGBA16Float whenever the HDR surface is active.
		// Using the RGBA8 pipeline here was already latently wrong before
		// Phase 5; it would have failed validation on an HDR surface.
		void *pso = PipelineForPixelFormat((unsigned long)backend->GetColorPixelFormatRaw());
		if (pso == NULL)
			return;

		[enc setRenderPipelineState:(__bridge id<MTLRenderPipelineState>)pso];
		[enc setFragmentSamplerState:(__bridge id<MTLSamplerState>)samplerPtr atIndex:0];
		[enc setFragmentTexture:(texY ? (__bridge id<MTLTexture>)texY : dummy) atIndex:0];
		[enc setFragmentTexture:(texU ? (__bridge id<MTLTexture>)texU : dummy) atIndex:1];
		[enc setFragmentTexture:(texV ? (__bridge id<MTLTexture>)texV : dummy) atIndex:2];
		[enc setFragmentTexture:(texA ? (__bridge id<MTLTexture>)texA : dummy) atIndex:3];
		[enc setFragmentTexture:dummy3 atIndex:4];

		MetalYUVUniforms u = {};
		// Pixel rect -> NDC, matching the GL path's uTransform convention.
		float ndcX = (x / screenW) * 2.0f - 1.0f;
		float ndcY = 1.0f - ((y + h) / screenH) * 2.0f;
		float ndcW = (w / screenW) * 2.0f;
		float ndcH = (h / screenH) * 2.0f;
		u.transform  = simd_make_float4(ndcX, ndcY, ndcW, ndcH);
		u.rotation   = 0;                       // Render() has always been 0
		u.mode       = (int)EYUVShaderMode::YUV420_3Plane;
		u.colorSpace = colorSpace;
		u.fullRange  = fullRange ? 1 : 0;
		u.hasAlpha   = hasAlpha ? 1 : 0;
		u.useLut     = 0;
		u.alpha      = alpha;
		// EVERY HDR uniform reset, from the caller's value (default-constructed
		// unless a caller deliberately opts in). GL uniforms are program state
		// that survives between draws and this entry point is shared with other
		// applications -- the game app draws its cutscenes through it -- so a PQ
		// EOTF left set by the last RenderToTarget() would apply itself to
		// somebody else's SDR video. Metal re-uploads the whole struct each
		// draw so it cannot leak the same way, but the two backends must agree
		// on behaviour or the divergence is the bug.
		ApplyHdrUniforms(u, hdr);

		[enc setVertexBytes:&u length:sizeof(u) atIndex:0];
		[enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
		[enc drawPrimitives:MTLPrimitiveTypeTriangleStrip vertexStart:0 vertexCount:4];
	}
}
