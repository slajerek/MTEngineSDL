#include "CGLRenderTarget.h"
#include "VID_ImageBinding.h"
#include "DBG_Log.h"

CGLRenderTarget::CGLRenderTarget()
{
}

CGLRenderTarget::~CGLRenderTarget()
{
	Destroy();
}

bool CGLRenderTarget::Create(int w, int h, ERenderTextureFormat fmt)
{
	if (w <= 0 || h <= 0)
	{
		LOGError("CGLRenderTarget::Create: invalid size %dx%d", w, h);
		return false;
	}

	// The FORMAT is part of the "already correct" test, not just the size. A
	// caller switching an existing target from RGBA8 to RGBA16F at the same
	// dimensions must get a new texture -- otherwise it would keep writing
	// above-white values into an 8-bit attachment that silently clamps them,
	// which is the exact failure this format parameter exists to remove.
	if (fbo != 0 && width == w && height == h && format == fmt)
		return true; // already the right size AND format -- no-op

	Destroy();

	// Save/restore the caller's FBO binding around creation -- ImGui (or
	// whatever else is mid-frame) may have its own FBO bound.
	GLint prevFBO = 0;
	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFBO);

	glGenTextures(1, &colorTexture);
	glBindTexture(GL_TEXTURE_2D, colorTexture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	// GL_RGBA16F + GL_HALF_FLOAT is colour-renderable in GL 3.0+ core, so this
	// is legal in the 4.1 core context the engine creates. The completeness
	// check below is what actually proves the driver accepted it.
	if (fmt == RENDER_TEXTURE_RGBA16F)
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, w, h, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
	else
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
	glBindTexture(GL_TEXTURE_2D, 0);

	glGenFramebuffers(1, &fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTexture, 0);

	GLenum drawBuffer = GL_COLOR_ATTACHMENT0;
	glDrawBuffers(1, &drawBuffer);

	GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	bool ok = (status == GL_FRAMEBUFFER_COMPLETE);
	if (!ok)
	{
		// NAME THE FORMAT. An incomplete FBO here surfaces downstream as
		// "video went black" rather than "this driver will not render to
		// float", and those send a reader to completely different places.
		LOGError("CGLRenderTarget::Create: FBO incomplete (status=0x%x) at %dx%d, format=%s",
				 (unsigned int)status, w, h,
				 (fmt == RENDER_TEXTURE_RGBA16F) ? "RGBA16F" : "RGBA8");
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFBO);

	if (!ok)
	{
		Destroy();
		return false;
	}

	width = w;
	height = h;
	format = fmt;
	return true;
}

void CGLRenderTarget::BindAsTarget()
{
	// Defensive: callers are expected to check Create()'s return value before
	// binding, but guard against fbo==0 anyway -- without this, a caller that
	// skipped the check would silently rebind the DEFAULT framebuffer (id 0)
	// with a bogus 0x0 viewport instead of getting an obvious no-op.
	if (fbo == 0)
	{
		LOGError("CGLRenderTarget::BindAsTarget: called with no FBO created (Create() never succeeded) -- ignoring");
		return;
	}

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &savedFBO);
	glGetIntegerv(GL_VIEWPORT, savedViewport);

	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	glViewport(0, 0, width, height);
}

void CGLRenderTarget::Unbind()
{
	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)savedFBO);
	glViewport(savedViewport[0], savedViewport[1], savedViewport[2], savedViewport[3]);
}

void CGLRenderTarget::Destroy()
{
	if (fbo != 0)
	{
		glDeleteFramebuffers(1, &fbo);
		fbo = 0;
	}
	if (colorTexture != 0)
	{
		// DO NOT glDeleteTextures() here. The color texture is the one GL
		// object of this target that escapes into ImGui draw lists (wrapped by
		// CSlrImageExternalTexture and recorded via Blit()/AddImage() during
		// GUI recording). Destroy() is typically reached from MT_Render()
		// (e.g. a video player tear-down on navigation), which the frame loop
		// runs AFTER this frame's draw lists were recorded but BEFORE they are
		// executed -- an immediate delete would make the backend bind a
		// deleted texture name at present time, raising GL_INVALID_OPERATION
		// in a core profile context (fatal at the next ASSERT_OPENGL).
		// Defer to the next frame's VID_BindImages() instead, which runs
		// before any recording. (The FBO above is never referenced by draw
		// lists, so its immediate deletion stays safe.)
		VID_PostDeleteGLTexture(colorTexture);
		colorTexture = 0;
	}
	width = 0;
	height = 0;
	format = RENDER_TEXTURE_RGBA8;
}

// --- CRenderTarget pass interface ------------------------------------------
//
// A fixed non-NULL sentinel, because GL has nothing to hand back: "binding" a
// target is a global state change, which is exactly why the video shader has
// always worked without ever being given a target object. Metal returns a real
// MTLRenderCommandEncoder here, so callers can treat NULL as failure on both.
static int kGLRenderTargetPassSentinel = 1;

void *CGLRenderTarget::BeginPass()
{
	if (fbo == 0)
		return NULL;
	BindAsTarget();
	return &kGLRenderTargetPassSentinel;
}

void *CGLRenderTarget::BeginPassWithClear(float r, float g, float b, float a)
{
	if (fbo == 0)
		return NULL;
	BindAsTarget();
	// glClearColor takes floats and an RGBA16F attachment stores them
	// unclamped, so an above-white or negative clear survives here -- which is
	// the whole point on a float target. On an RGBA8 attachment the same call
	// clamps, exactly as it always has.
	//
	// SAVED AND RESTORED, because glClearColor is GLOBAL state and this class
	// otherwise leaves no footprint outside its own FBO binding (which
	// BindAsTarget/Unbind already save and restore, for the same reason).
	// Leaking it happens to be harmless today -- CRenderBackendOpenGL4's frame
	// loop sets the colour immediately before its own glClear every frame, and
	// so does imgui_impl_opengl3 -- but "harmless because of what every current
	// caller happens to do" is not an invariant, and this is a public seam.
	GLfloat savedClearColor[4] = { 0, 0, 0, 0 };
	glGetFloatv(GL_COLOR_CLEAR_VALUE, savedClearColor);
	glClearColor(r, g, b, a);
	glClear(GL_COLOR_BUFFER_BIT);
	glClearColor(savedClearColor[0], savedClearColor[1],
				 savedClearColor[2], savedClearColor[3]);
	return &kGLRenderTargetPassSentinel;
}

void CGLRenderTarget::EndPass()
{
	Unbind();
}
