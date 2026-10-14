#ifndef _CRenderShaderGainMapMetal_h_
#define _CRenderShaderGainMapMetal_h_

#include "CRenderShaderMetal.h"
#include "Core/Render/CGainMapShader.h"

struct ImDrawCmd;

// Metal implementation of the option C gain-map shader (MTGainMap.metal).
//
// Derives from CRenderShaderMetal for its LIBRARY loading only (embedded
// .metallib, source as the development path). The pipelines are its own, because
// they are not CRenderShaderMetal's one fixed pipeline:
//
//   * the SURFACE pipeline, built for whatever colour format the surface has. The
//     surface switches SDR <-> HDR at runtime (BGRA8 <-> RGBA16Float) and a
//     pipeline whose attachment format disagrees with the pass is a draw-time
//     validation failure, so the format it was built for is stored and the draw
//     callback rebuilds it when the surface's differs;
//   * the OFFSCREEN pipeline, RGBA16Float with blending off: the one-time
//     composite (RenderComposite) and RenderToTargetForTest;
//   * the PLAIN pipeline, RGBA16Float with blending off, ImGui's own fragment
//     function: DrawTextureToTargetForTest (a composite through the float arm's path).
//
// And it never reads shader-object state at render time: each draw's map and
// uniforms ride in the ImGui callback's own copied user data.
class CRenderShaderGainMapMetal : public CRenderShaderMetal, public CGainMapShader
{
public:
	CRenderShaderGainMapMetal(CRenderBackendMetal *renderBackend);   // compiles synchronously
	virtual ~CRenderShaderGainMapMetal();

	virtual const char *GetMetalShaderSource() override;
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength) override;

	// Both bases declare these; the forwarding names which one is meant.
	virtual void CompileShaders() override;
	virtual bool IsUsable() const override { return isCompiled; }

	virtual bool AddDrawCallback(ImDrawList *dl, void *mapTexture, const SMTGainMapUniforms &u) override;
	virtual bool RenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4]) override;
	virtual bool RenderComposite(CSlrImage *base, CRenderTarget *target) override;
	virtual bool DrawTextureToTargetForTest(void *texture, const float uvRect[4], bool linearScaling,
	                                        CRenderTarget *target, const float rgba[4]) override;

private:
	// Builds a pipeline from the loaded library for `pixelFormat` (raw MTLPixelFormat)
	// with fragment function `fragment` (NULL = mtFragmentMain). Returns a retained
	// id<MTLRenderPipelineState> as void*, or NULL (logged).
	void *BuildPipeline(unsigned int pixelFormat, bool blending, const char *fragment = NULL);
	// One textured quad into `target`: pixel rect x0 y0 x1 y1 of a W x H target, uv
	// rect `uv`, vertex colour `rgba`, through `pipeline` with texture 0 = `tex0`
	// (sampler `sampler`), texture 1 = `tex1` and fragment bytes `u` (NULL = none);
	// cleared to 0 first. The pass is committed on the backend's queue.
	bool DrawQuadToTarget(CRenderTarget *target, void *pipeline, void *sampler, void *tex0, void *tex1,
	                      const SMTGainMapUniforms *u, const float rect[4], const float uv[4],
	                      const float rgba[4]);
	// The surface pipeline for the backend's CURRENT colour format, rebuilt when it
	// changed. NULL when it cannot be built.
	void *SurfacePipeline();

	static void DrawCallback(const ImDrawList *dl, const ImDrawCmd *cmd);

	unsigned int surfacePipelineFormat;   // the raw MTLPixelFormat pipelinePtr was built for
	unsigned int failedPipelineFormat;    // a format whose pipeline failed to build (0 = none): not retried until the format changes
	void *testPipelinePtr;                // id<MTLRenderPipelineState>, RGBA16Float, blending off (composite + test)
	void *plainPipelinePtr;               // id<MTLRenderPipelineState>, RGBA16Float, blending off, mtFragmentPlain
};

#endif
