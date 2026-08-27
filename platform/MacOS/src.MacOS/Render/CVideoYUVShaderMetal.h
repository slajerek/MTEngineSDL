#pragma once

#include "Video/CVideoYUVConverter.h"

// Metal implementation of the YUV->RGB video converter.
//
// A faithful port of CVideoYUVShader's GLSL, not a reimplementation: the same
// three sampling modes, the same limited/full range maths, the same three
// colourspace matrices, the same rotation-as-UV-transform, and the same
// optional CM-E display LUT sampled after the matrix on encoded R'G'B'. Any
// divergence shows up as a colour shift only a side-by-side comparison catches,
// so the constants are copied rather than re-derived.
class CVideoYUVShaderMetal : public CVideoYUVConverter
{
public:
	CVideoYUVShaderMetal(void *device, void *commandQueue);
	virtual ~CVideoYUVShaderMetal();

	virtual bool Compile() override;

	virtual void Render(void *texY, void *texU, void *texV, void *texA,
						bool hasAlpha, float alpha,
						int colorSpace, bool fullRange,
						float x, float y, float w, float h,
						float screenW, float screenH,
						const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

	virtual void RenderToTarget(EYUVShaderMode mode,
								void *texY, void *texUVorU, void *texV,
								bool hasAlpha, void *texA,
								int colorSpace, bool fullRange,
								int rotationDegrees,
								void *lutTexture, int lutEdge,
								CRenderTarget &target,
								const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

	bool IsCompiled() const { return compiled; }

private:
	// A render pipeline bakes in its colour-attachment format, and Metal
	// validates that against the PASS at DRAW time -- not at creation. So one
	// pipeline cannot serve both an RGBA8 offscreen target and an RGBA16Float
	// one, and the direct-to-screen path needs one matching the surface, which
	// is RGBA16Float whenever HDR is on. Returns an id<MTLRenderPipelineState>
	// as void*, or NULL if creation failed. See CRenderShaderMetal.mm, which
	// documents this same hazard.
	void *PipelineForPixelFormat(unsigned long mtlPixelFormat);

private:
	void *devicePtr;         // id<MTLDevice>,        retained
	void *queuePtr;          // id<MTLCommandQueue>,  retained
	void *libraryPtr;        // id<MTLLibrary>,       retained -- kept so a
	                         // pipeline for a NEW attachment format can be
	                         // built after Compile() without recompiling MSL
	void *pipelinePtr;       // id<MTLRenderPipelineState> for RGBA8Unorm, retained
	void *pipelineFloatPtr;  // ...for RGBA16Float, retained, built on demand
	void *pipelineSurfacePtr;// ...for the SURFACE's format (Render()), retained
	unsigned long surfacePipelineFormat; // MTLPixelFormat the above was built for
	void *samplerPtr;        // id<MTLSamplerState>,  retained
	void *dummyTexPtr;       // 1x1 R8, stands in for unbound plane slots
	void *dummyLut3DPtr;     // 1x1x1 RGBA16, stands in for "no LUT"
	bool compiled;
	bool compileFailed;      // latch: never retry a failed compile every frame
};
