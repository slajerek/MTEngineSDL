#ifndef _CRenderBackendMetal_h_
#define _CRenderBackendMetal_h_

#include "CRenderBackend.h"

class CRenderBackendMetal : public CRenderBackend
{
public:
	CRenderBackendMetal();

	// Can this machine actually create a Metal device?
	//
	// Added when Metal became the macOS DEFAULT rather than an opt-in. While it
	// was opt-in, "Metal always exists on a Mac" was a fair assumption and
	// CreateSDLWindow's SYS_FatalExit was an acceptable failure mode: a user who
	// asked for Metal and got a fatal exit had at least asked. As the default it
	// is not acceptable -- a Mac that cannot make a device (a remoted session, a
	// GPU in a bad state) would fail to boot the app at all, with editing
	// settings.hjson by hand as the only way out.
	//
	// Consulted from VID_IsRenderBackendAvailable(), which is the one query the
	// factory reads, so this is the single point where "fall back to OpenGL"
	// can happen -- the same shape as CRenderBackendD3D11::IsAvailable(), and
	// for the same reason. Probed once per process: it is reachable from
	// per-frame menu code.
	static bool IsAvailable();

	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized);

	SDL_Renderer* renderer;
	
	virtual void CreateRenderContext();
	virtual void InitRenderPipeline();
	virtual void CreateFontsTexture();
	virtual void NewFrame(ImVec4 clearColor);
	virtual void PresentFrameBuffer(ImVec4 clearColor);
	virtual void ApplyDisplayColorGamut(VID_DisplayColorGamut gamut) override;
	virtual void Shutdown();

	virtual void CreateTexture(CSlrImage *image);
	virtual void UpdateTextureLinearScaling(CSlrImage *image);
	virtual void ReBindTexture(CSlrImage *image);
	virtual void DeleteTexture(CSlrImage *image);

	virtual EImageGpuFormat GetPreferredCompressedFormat() override;

	// --- S-4 seams -------------------------------------------------------
	//
	// All the Metal objects live as file-statics in the .mm and the header
	// deliberately exposes no Metal TYPES (it is included from plain C++ all
	// over the engine), so these hand out opaque void* that the .mm bridges
	// back. Every one may be NULL -- before InitRenderPipeline(), or on a frame
	// NewFrame() aborted -- and every caller must handle that.
	void *GetCurrentRenderCommandEncoder();
	void *GetMetalDevice();
	void *GetMetalCommandQueue();

	// MTLPixelFormat of the drawable, as a raw integer (see the .mm).
	unsigned int GetColorPixelFormatRaw();

	// Read-only surface description, for tests and diagnostics. Deliberately
	// NOT an accessor for the CAMetalLayer itself: handing that out invites app
	// code to reconfigure the surface behind the backend's back, and a test does
	// not need a layer, it needs these two facts.
	bool GetSurfaceIsFloatFormat();
	virtual bool GetSurfaceIsExtendedRange() override;
	virtual bool GetSurfaceIsLinearColorSpace() override;

	virtual ERenderSurfaceFormat GetSurfaceFormat() override;
	virtual float GetDisplayHdrHeadroom() override;
	virtual bool ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA) override;
	virtual bool ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA) override;
	virtual bool ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA) override;
	virtual bool SupportsTextureFormat(ERenderTextureFormat fmt) override;
	virtual void SetSurfaceEdrMetadata(float maxComponent) override;
	virtual bool GetSurfaceHasEdrMetadata() override;

	virtual CRenderTarget *CreateRenderTarget() override;
	virtual CMaskedTileShader *CreateMaskedTileShader(bool queued) override;
	virtual CRenderShader *CreateFlatColorShader(float r, float g, float b, float a) override;
	virtual CRenderShaderCustomFragment *CreateCustomFragmentShader(const char *name) override;

	// ImGui's Metal default sampler is linear/linear/mip-linear, which is right
	// for everything except point magnification -- so only that needs a bracket.
	virtual bool ImageNeedsSamplerOverride(CSlrImage *image) override;
	virtual void QueueSamplerForImage(CSlrImage *image) override;
	virtual void QueueDefaultSampler() override;
	virtual CVideoYUVConverter *CreateVideoYUVConverter() override;

	virtual void *CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel) override;
	virtual void UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride) override;
	virtual void DeletePlaneTexture(void *tex) override;

	virtual void *CreateLutTexture3D(int edge) override;
	virtual void UpdateLutTexture3D(void *tex, const void *data, int edge) override;
	virtual void DeleteLutTexture3D(void *tex) override;

	virtual ~CRenderBackendMetal();
};

#endif
