#ifndef _CRenderTarget_h_
#define _CRenderTarget_h_

#include "ERenderTextureFormat.h"

// Backend-neutral render-to-texture target.
//
// WHY BeginPass() RETURNS SOMETHING, when the OpenGL original did not:
// under GL, "bind a target" is a global state change and the drawing code needs
// nothing handed back -- which is exactly why CVideoYUVShader has always worked
// without ever being given a target object. Metal has no such global state: a
// draw MUST be encoded into a specific MTLRenderCommandEncoder created from
// this target's own MTLRenderPassDescriptor. An interface that only said "bind"
// would be unimplementable on Metal, so BeginPass() hands the encoder back and
// the GL implementation returns a non-NULL sentinel that its callers ignore.
//
// Texture handles are the `void *` convention CSlrImage already uses for its
// own textures (a GLuint cast through uintptr_t under GL, an id<MTLTexture>
// under Metal). Never GLuint: that is 32-bit and a Metal texture pointer is 64,
// so a GLuint boundary silently truncates half the pointer.
class CRenderTarget
{
public:
	virtual ~CRenderTarget() {}

	// (Re)create at width x height in `fmt`. A no-op when the size AND format
	// already match, so it is safe to call every frame. Returns false (and
	// logs) on failure.
	//
	// The format is DEFAULTED to RGBA8 so every pre-S-5 caller is untouched and
	// the SDR path cannot change by accident. RENDER_TEXTURE_RGBA16F exists for
	// HDR video playback, whose above-white values an 8-bit target simply
	// cannot hold (S-5 Phase 5).
	//
	// A caller that changes only the FORMAT must still get a new texture --
	// implementations must include the format in their "already matches" test,
	// not just the dimensions.
	virtual bool Create(int width, int height,
						ERenderTextureFormat fmt = RENDER_TEXTURE_RGBA8) = 0;

	// The format this target was actually created with. A consumer that encodes
	// a draw into this target needs it: on Metal a render pipeline whose colour
	// attachment format disagrees with the pass it is encoded into is a
	// validation failure at DRAW time, not at creation (see
	// CRenderShaderMetal.mm), so the YUV converter selects its pipeline from
	// this rather than assuming.
	virtual ERenderTextureFormat GetFormat() const = 0;

	// Open a render pass against this target and return the handle a draw needs
	// to issue commands into it, or NULL on failure. Callers treat NULL as
	// "cannot draw this frame" uniformly on every backend.
	virtual void *BeginPass() = 0;

	// Open a pass that first CLEARS to the given colour, which may lie outside
	// 0..1 on a float target. Same return contract as BeginPass().
	//
	// Separate from BeginPass() rather than a defaulted argument on it because
	// the two backends clear at different moments: Metal can only clear as part
	// of the render-pass descriptor, i.e. at BeginPass() time, while GL clears
	// with a command after binding. A "clear" callable mid-pass is therefore
	// not expressible on Metal at all, and this seam is honest about that by
	// folding the clear into opening the pass.
	//
	// Additive: BeginPass() keeps its exact existing behaviour on both
	// backends, so no existing caller changes. (Note those behaviours are not
	// identical to each other -- Metal's plain BeginPass clears to transparent
	// black and GL's does not. That predates this and is invisible to the video
	// path, whose quad covers the whole target opaquely; it is recorded here
	// rather than silently "fixed" as a side effect of an HDR change.)
	virtual void *BeginPassWithClear(float r, float g, float b, float a) = 0;

	virtual void EndPass() = 0;

	// The colour attachment. Hand straight to ImGui as an ImTextureID.
	virtual void *GetTexture() const = 0;

	virtual int GetWidth() const = 0;
	virtual int GetHeight() const = 0;
	virtual void Destroy() = 0;
};

#endif
