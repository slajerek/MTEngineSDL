#pragma once

// Forward-declared rather than included: CRenderTarget.h lives under
// Core/Render and this header is pulled in by app code whose include paths do
// not necessarily carry that directory. It is only used as a reference here, so
// a forward declaration keeps this seam genuinely self-contained -- which is
// the whole point of it.
class CRenderTarget;

// Backend-neutral YUV->RGB video conversion.
//
// EYUVShaderMode LIVES HERE, not in CVideoYUVShader.h, deliberately. That header
// includes CGLRenderTarget.h and <OpenGL/gl3.h>, so leaving the enum there would
// drag the OpenGL headers into CVideoYUVShaderMetal.mm and into every consumer
// of this supposedly backend-neutral seam. It is a plain enum with no graphics
// dependency of its own, so it belongs on the neutral side of the boundary.
enum class EYUVShaderMode
{
	YUV420_3Plane, // 3 separate single-channel 8-bit planes
	NV12,          // Y as one 8-bit plane, interleaved U,V as a single 2-channel plane
	YUV420P10,     // Y/U/V as 16-bit planes holding the raw 10-bit (0..1023) sample
};

// How an HDR clip's colour is finished off, once the shader has applied the
// EOTF and the BT.2020 -> sRGB primaries change (S-5 Phase 5).
//
// This mirrors the photo app's PC_EncodeForSurface, which is what the POSTER
// lane goes through -- and matching it is not a nicety, it IS the acceptance
// criterion for the phase: a clip must look the same playing as it does as a
// poster. The poster's own float product is LINEAR (CVideoFrameExtractor sets
// floatIsSurfaceEncoded = false) and the app encodes it afterwards; the shader
// has to land in the same place by itself, because there is no CPU pass after
// it.
struct SVideoHdrOutput
{
	// The clip's AVCOL_TRC. 16 = PQ, 18 = HLG; anything else means "not HDR"
	// and the whole chain below is skipped, leaving the SDR path byte-identical
	// to what it was before this existed.
	int colorTrc = 2;

	// True when the destination is RGBA16F and above-white values must be kept.
	// False means the 8-bit arm: tone-map into 0..1 instead. BOTH are required
	// -- most users most of the time have no headroom, and a PQ clip must look
	// CORRECT for them, not washed out.
	bool floatTarget = false;

	// The surface's own encoding, so the float arm can finish in the space the
	// compositor expects.
	//   surfaceIsLinear : write linear light (no transfer applied)
	//   surfaceIsP3     : apply linear sRGB -> linear Display P3 primaries
	// Both false is plain extended sRGB, which is what the engine ships today.
	// Omitting the P3 arm would make playback a different GAMUT from the poster
	// on exactly the displays most likely to be used.
	bool surfaceIsLinear = false;
	bool surfaceIsP3 = false;

	// The tone-map headroom for the 8-bit arm. MUST come from the same live
	// source the poster uses (CRenderBackend::GetDisplayHdrHeadroom, sampled at
	// the moment of the map -- macOS grants it lazily and then keeps moving it
	// with display brightness). A fixed value here would disagree with the
	// poster by construction. 1.0 makes the curve exactly the identity.
	float toneMapHeadroom = 1.0f;

	bool IsHdr() const { return colorTrc == 16 || colorTrc == 18; }
};

// Texture handles are the `void *` convention CSlrImage already uses (a GLuint
// cast through uintptr_t under GL, an id<MTLTexture> under Metal). Never GLuint:
// it is 32-bit and would silently truncate half of a Metal texture pointer.
class CVideoYUVConverter
{
public:
	virtual ~CVideoYUVConverter() {}

	virtual bool Compile() = 0;

	// Draws the converted frame straight to the current target at a pixel rect.
	//
	// This entry point HAS a live caller -- CGuiViewVideoPlayer draws through it
	// from an ImGui draw callback -- despite an early draft of the S-4 plan
	// twice recording it as dead. The claim came from grepping for
	// `yuvShader->Render(`; the real call is `d->shader->Render(...)` through a
	// callback-data struct. A pattern-matched grep is not a proof of deadness.
	//
	// `hdr` is defaulted, and Render() DELIBERATELY resets every HDR uniform to
	// this default rather than leaving whatever RenderToTarget() last set. GL
	// uniforms are program state that survives between draws -- the existing
	// `useLut = 0` line here exists for precisely that reason -- and this entry
	// point is shared with other applications (the game app's cutscenes), so a
	// leaked PQ EOTF would apply itself to somebody else's SDR video.
	virtual void Render(void *texY, void *texU, void *texV, void *texA,
						bool hasAlpha, float alpha,
						int colorSpace, bool fullRange,
						float x, float y, float w, float h,
						float screenW, float screenH,
						const SVideoHdrOutput &hdr = SVideoHdrOutput()) = 0;

	// Converts one YUV(A) frame to RGBA into `target`, display-oriented
	// (rotationDegrees applied as a UV transform in the vertex stage, not a CPU
	// pixel remap). `target` must already be sized to the DISPLAY (post-rotation)
	// dimensions.
	//
	// texUVorU depends on `mode`:
	//  - YUV420_3Plane / YUV420P10: the U plane (texV is the V plane)
	//  - NV12: the single interleaved U,V plane; texV is unused (may be NULL)
	//
	// lutTexture/lutEdge (CM-E): an optional 3D display colour LUT (edge^3
	// RGBA16 lattice) sampled after the YUV->RGB matrix on encoded R'G'B'.
	// Pass NULL/0 for no LUT -- that path must stay bit-identical to the
	// pre-CM-E behaviour on both backends.
	//
	// `hdr` describes the HDR transfer to apply, if any. Defaulted, so an SDR
	// clip's draw is unchanged in every particular.
	virtual void RenderToTarget(EYUVShaderMode mode,
								void *texY, void *texUVorU, void *texV,
								bool hasAlpha, void *texA,
								int colorSpace, bool fullRange,
								int rotationDegrees,
								void *lutTexture, int lutEdge,
								CRenderTarget &target,
								const SVideoHdrOutput &hdr = SVideoHdrOutput()) = 0;
};
