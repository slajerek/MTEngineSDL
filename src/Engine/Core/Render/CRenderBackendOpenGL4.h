#ifndef _CRenderBackendOpenGL4_h_
#define _CRenderBackendOpenGL4_h_

#include "CRenderBackend.h"

class CRenderBackendOpenGL4 : public CRenderBackend
{
public:
	CRenderBackendOpenGL4();

	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized);

	SDL_GLContext glContext;
	virtual void CreateRenderContext();
	virtual void InitRenderPipeline();
	virtual void CreateFontsTexture();
	virtual void NewFrame(ImVec4 clearColor);
	virtual void PresentFrameBuffer(ImVec4 clearColor);
	virtual void Shutdown();

	virtual void CreateTexture(CSlrImage *image);
	virtual void UpdateTextureLinearScaling(CSlrImage *image);
	virtual void ReBindTexture(CSlrImage *image);
	virtual void DeleteTexture(CSlrImage *image);

	virtual EImageGpuFormat GetPreferredCompressedFormat() override;

	// This IS the OpenGL backend, so raw GL outside this class is valid here.
	virtual bool SupportsOpenGLShaders() override { return true; }

	// GL_RGBA16F + GL_HALF_FLOAT is core since GL 3.0, and this is a 4.1 core
	// context, so the upload genuinely works. Whether it is WORTH doing is a
	// different question and not this method's: the OpenGL surface clips above
	// 1.0, so the app's session gate refuses float on this backend and the
	// memory is never spent. Reporting the honest capability keeps the two
	// questions separate.
	virtual bool SupportsTextureFormat(ERenderTextureFormat fmt) override
	{
		return fmt == RENDER_TEXTURE_RGBA8 || fmt == RENDER_TEXTURE_RGBA16F;
	}

	virtual bool ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA) override;

	virtual CMaskedTileShader *CreateMaskedTileShader(bool queued) override;
	virtual CRenderShader *CreateFlatColorShader(float r, float g, float b, float a) override;
	virtual CRenderShaderCustomFragment *CreateCustomFragmentShader(const char *name) override;

	virtual bool ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA) override;

	// The float sibling. S-5 Phase 5: without this, every float-target
	// assertion in the HDR video tests is unrunnable on OpenGL -- which is
	// the DEFAULT backend on all three platforms -- and the established
	// caller shape degrades an unavailable readback to a LogWarning, i.e. a
	// PASS. A test that cannot run must fail, not quietly succeed.
	virtual bool ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA) override;
	virtual bool ImageNeedsSamplerOverride(CSlrImage *image) override;
	virtual void QueueSamplerForImage(CSlrImage *image) override;
	virtual void QueueDefaultSampler() override;
	virtual CRenderTarget *CreateRenderTarget() override;
	virtual CVideoYUVConverter *CreateVideoYUVConverter() override;

	virtual void *CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel) override;
	virtual void UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride) override;
	virtual void DeletePlaneTexture(void *tex) override;

	virtual void *CreateLutTexture3D(int edge) override;
	virtual void UpdateLutTexture3D(void *tex, const void *data, int edge) override;
	virtual void DeleteLutTexture3D(void *tex) override;

	void SetupGlSlVersion();
	const char *glslVersionString;
	const char *GetGlSlVersion();

	// Cached result of GL extension scan; -1 means not yet probed.
	int cachedCompressedFormat;
	
	virtual ~CRenderBackendOpenGL4();
	
	static CRenderBackendOpenGL4 *GetRenderBackendOpenGL4();
	static bool CheckOpenGLError();
};

#define ASSERT_OPENGL()  { if (CRenderBackendOpenGL4::CheckOpenGLError() == true) { SYS_FatalExit("OpenGL error"); } }
//#define ASSERT_OPENGL()  { CRenderBackendOpenGL4::CheckOpenGLError(); }

#endif

