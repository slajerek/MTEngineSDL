#pragma once

#include "SYS_Defs.h"
#include "Core/Render/CRenderTarget.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl3w.h>
#endif

// Minimal render-to-texture helper: one colour attachment + FBO.
//
// The attachment is RGBA8 by default and RGBA16F on request (S-5 Phase 5):
// HDR video playback produces above-white values that an 8-bit target cannot
// hold at all. GL_RGBA16F is colour-renderable in GL 3.0+ core, so the 4.1
// core context this engine uses supports it -- though in PRODUCTION the app's
// HDR session gate refuses float on OpenGL anyway (the GL surface clips above
// 1.0), so the float target is exercised mainly by tests here.
//
// This is the engine's first FBO code (Task 10). It exists so
// CVideoPlayer's RGBA output mode can convert decoded YUV planes to a
// display-oriented RGBA texture via CVideoYUVShader::RenderToTarget(),
// entirely off the visible framebuffer -- the result (GetTexture()) is a
// plain GL_TEXTURE_2D any later consumer (Plan 2's viewer) can sample
// like any other texture, with no knowledge of the source's rotation or
// YUV layout.
//
// Not thread-safe and not reentrant: Create()/BindAsTarget()/Unbind()/
// Destroy() must all be called from the GL thread with this target's
// owner's GL context current.
class CGLRenderTarget : public CRenderTarget
{
public:
	CGLRenderTarget();
	~CGLRenderTarget();

	// (Re)creates the FBO + colour texture at width x height in `fmt`. Safe to
	// call repeatedly -- a no-op if the size AND FORMAT already match,
	// otherwise destroys any existing target first. Returns false (and logs)
	// if the FBO fails the GL_FRAMEBUFFER_COMPLETE check -- which must be
	// re-checked for the float attachment, since an incomplete FBO presents
	// downstream as "video went black", not as "float unsupported". Requires
	// a current GL context.
	virtual bool Create(int width, int height,
						ERenderTextureFormat fmt = RENDER_TEXTURE_RGBA8) override;

	virtual ERenderTextureFormat GetFormat() const override { return format; }

	// Binds this target's FBO for drawing and sets the viewport to
	// (0, 0, width, height). Saves the FBO binding + viewport that were
	// active beforehand so Unbind() can restore them -- ImGui rendering may
	// be live around the caller (this can run mid-frame).
	// CRenderTarget's pass interface. BeginPass() returns a non-NULL SENTINEL
	// rather than a real handle: under GL, binding is a global state change and
	// the drawing code needs nothing back. Metal has no such state and returns a
	// real encoder, so callers treat NULL uniformly as "cannot draw".
	virtual void *BeginPass() override;
	virtual void *BeginPassWithClear(float r, float g, float b, float a) override;
	virtual void EndPass() override;

	void BindAsTarget();

	// Restores the FBO binding + viewport saved by BindAsTarget().
	void Unbind();

	// void*, matching the CSlrImage handle convention across the backend seam.
	// GetTextureGL() stays for GL-internal use, where a GLuint is what is wanted.
	virtual void *GetTexture() const override { return (void *)(uintptr_t)colorTexture; }
	GLuint GetTextureGL() const { return colorTexture; }
	virtual int GetWidth() const override { return width; }
	virtual int GetHeight() const override { return height; }

	// Releases the FBO + color texture. Safe to call when already empty.
	virtual void Destroy() override;

private:
	GLuint fbo = 0;
	GLuint colorTexture = 0;
	int width = 0;
	int height = 0;
	ERenderTextureFormat format = RENDER_TEXTURE_RGBA8;

	// Save/restore state for BindAsTarget()/Unbind() -- GLint, not GLuint,
	// to match the types glGetIntegerv() writes.
	GLint savedFBO = 0;
	GLint savedViewport[4] = {0, 0, 0, 0};
};
