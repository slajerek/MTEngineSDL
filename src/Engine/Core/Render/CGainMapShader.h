#ifndef _CGainMapShader_h_
#define _CGainMapShader_h_

// Backend-neutral interface of the option C gain-map shader: an RGBA8 base drawn
// with its RGBA8 companion gain map, the map applied PER PIXEL at draw time
// (MT_UltraHdr.h, SMTGainMapApply -- the CPU twin MT_GainMapEvaluate is its
// oracle). Made by CRenderBackend::CreateGainMapShader(); NULL on a backend that
// has none (OpenGL; D3D11 until its port), and the caller then takes the float arm.
//
// UNLIKE CRenderShader the uniforms travel WITH EACH DRAW: ImGui copies the
// callback's user data into the draw list, so two gain-mapped images in one frame
// each carry their own map and constants -- no shader-object state is read at
// render time. Texture handles are the engine's `void *` convention (never GLuint).

struct ImDrawList;
class CSlrImage;
class CRenderTarget;
struct SMTGainMapUniforms;

class CGainMapShader
{
public:
	virtual ~CGainMapShader() {}

	virtual void CompileShaders() = 0;
	virtual bool IsUsable() const = 0;

	// The per-draw callback (ImGui copies `u`): switch to this pipeline, bind the
	// map at texture slot 1 and `u` at constant slot 0. Slot 0's texture and
	// sampler are the draw's own. Returns FALSE, queueing nothing, when the
	// shader cannot draw on the surface as it is now (its pipeline for the
	// current colour format could not be built): the caller must then SUPPRESS
	// the image draw (Decision R-4), exactly as for an unusable shader.
	virtual bool AddDrawCallback(ImDrawList *dl, void *mapTexture, const SMTGainMapUniforms &u) = 0;

	// Test only: draw `base` with its map over the whole of `target` through the
	// SAME fragment function, blending off, vertex colour `rgba`, through the sampler
	// production binds for `base` (magnify-nearest / minify-linear for a cache image).
	// A target smaller than the base is a MINIFIED draw (the filtered branch).
	// Render thread only. The pass is committed on the backend's command queue, so
	// a readback committed after it (ReadTexturePixelsFloat waits) sees it done.
	virtual bool RenderToTargetForTest(CSlrImage *base, CRenderTarget *target, const float rgba[4]) = 0;

	// THE ONE-TIME COMPOSITE (Task 3c): (re)creates `target` as RGBA16F at the base
	// TEXTURE's own size and renders base + map into it at 1:1 -- one apply per base
	// texel at its centre (uniform flags.w = 2), blending off, white -- so the target
	// holds what a float-arm texture of the same photo holds (RGBA16F, the apply's
	// encodeSrgb and matrix, base alpha) in the SAME texel layout as the base's
	// texture: a caller draws it with the base's own UVs. Texels outside the base's
	// image rect (power-of-two padding) are cleared to 0. Render thread only; the
	// pass is committed on the backend's queue BEFORE the frame that draws it is.
	// False (nothing usable in `target`) when the base or map is unbound or the
	// target cannot be created or opened.
	virtual bool RenderComposite(CSlrImage *base, CRenderTarget *target) = 0;

	// Test only: draws `texture` (rect u0 v0 u1 v1 of it) over the whole of
	// `target` (RGBA16F) through ImGui's OWN fragment function (vertex colour x the
	// texture as sampled) with the sampler production binds for an image of
	// `linearScaling`, blending off -- the float arm's draw path, to read back
	// what a composite puts on screen as floats. Render thread only.
	virtual bool DrawTextureToTargetForTest(void *texture, const float uvRect[4], bool linearScaling,
	                                        CRenderTarget *target, const float rgba[4]) = 0;
};

#endif
