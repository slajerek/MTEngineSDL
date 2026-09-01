#ifndef _CRenderBackendD3D11_h_
#define _CRenderBackendD3D11_h_

#include "CRenderBackend.h"

#if defined(MT_RENDER_BACKEND_D3D11)

// ===========================================================================
// CRenderBackendD3D11 -- Direct3D 11 render backend (S-6)
// ===========================================================================
//
// NO D3D TYPES IN THIS HEADER, deliberately, and for the same reason
// CRenderBackendMetal.h names no Metal types: it is included from plain C++
// translation units all over the engine, and dragging <d3d11.h> (and therefore
// <windows.h>) into every one of them is a compile-time tax and a macro
// minefield -- `min`, `max`, `byte`, `near`, `far`. Everything Direct3D lives
// as file-statics in the .cpp; the seams below hand out opaque void* that the
// .cpp bridges back, exactly as the Metal backend does.
//
// WHAT THIS BACKEND DOES DIFFERENTLY FROM METAL, and why:
//
//   * IT PROBES BEFORE THE PROCESS COMMITS. Metal's CreateSDLWindow calls
//     SYS_FatalExit on failure and nothing tests MTLCreateSystemDefaultDevice()
//     first -- fine on a Mac, where Metal always exists. A Windows machine that
//     cannot create a D3D11 device is a real case (a bare VM, a remoted
//     session, a driver in a bad state), so IsAvailable() answers that question
//     without side effects and VID_IsRenderBackendAvailable() caches it. A
//     capability query that disagrees with what the factory does is worse than
//     no query at all.
//
//   * EVERYTHING RENDERS INTO AN OFFSCREEN RGBA16F TARGET, and one present-time
//     resolve pass converts it for the swapchain. macOS can simply declare its
//     layer kCGColorSpaceExtendedSRGB -- the truthful description of an
//     sRGB-ENCODED pipeline. DXGI has no such colour space: its float HDR path
//     is scRGB (DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) and the G10 means
//     LINEAR. So Windows cannot dodge the question, and the resolve is the
//     answer that leaves ImGui, every app's UI and every existing shader
//     untouched. See Shaders/Resolve.hlsl.
//
//   * `void *` TEXTURE HANDLES ARE ID3D11ShaderResourceView*, ALWAYS. That is
//     what ImGui's DX11 backend takes as an ImTextureID, so a CSlrImage's
//     handle can be drawn without translation, and the underlying
//     ID3D11Texture2D is always recoverable with GetResource(). Never an
//     ID3D11Texture2D*, never a raw index: one convention, and the SRV keeps
//     the resource alive on its own.
//
// SEE ALSO the method table in the .cpp, which states for EVERY virtual on
// CRenderBackend whether this backend implements it, inherits the default, or
// deliberately returns NULL -- and why.
// ===========================================================================

class CRenderBackendD3D11 : public CRenderBackend
{
public:
	CRenderBackendD3D11();
	virtual ~CRenderBackendD3D11();

	// Can this MACHINE create a D3D11 device at all? No window, no swapchain,
	// no side effects: it creates a null-HWND device, asks, and releases.
	//
	// Called by VID_IsRenderBackendAvailable(), which caches the answer for the
	// process -- so this must be safe to call before SDL_Init, before any
	// window exists, and from whichever thread asks first.
	static bool IsAvailable();

	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized) override;
	virtual void CreateRenderContext() override;
	virtual void InitRenderPipeline() override;
	virtual void CreateFontsTexture() override;
	virtual void NewFrame(ImVec4 clearColor) override;
	virtual void PresentFrameBuffer(ImVec4 clearColor) override;
	virtual void ApplyDisplayColorGamut(VID_DisplayColorGamut gamut) override;
	virtual void Shutdown() override;

	virtual void CreateTexture(CSlrImage *image) override;
	virtual void UpdateTextureLinearScaling(CSlrImage *image) override;
	virtual void ReBindTexture(CSlrImage *image) override;
	virtual void DeleteTexture(CSlrImage *image) override;

	// IMG_GPU_UNCOMPRESSED for S-6, and that is a scope decision, not a
	// capability one: every D3D11 device supports BC7, but reporting it obliges
	// CreateTexture() to take the block-padded, mip-chained upload arm that
	// this stage does not implement. Claiming a format we cannot upload would
	// fail at bind time with a plausible-looking garbage texture.
	virtual EImageGpuFormat GetPreferredCompressedFormat() override;
	virtual bool SupportsTextureFormat(ERenderTextureFormat fmt) override;

	// FALSE, and this is load-bearing for PROCESS SURVIVAL rather than for
	// correctness. CRenderBackendOpenGL4::GetRenderBackendOpenGL4() calls
	// SYS_FatalExit when the live backend is not named "OpenGL4", and c64d
	// reaches it from its CRT-monitor and ShaderToy factories -- the ONLY thing
	// stopping that fatal exit is that both call sites check this first.
	virtual bool SupportsOpenGLShaders() override { return false; }

	// --- surface description ---------------------------------------------
	//
	// READ Shaders/Resolve.hlsl AND THE .cpp NOTES BEFORE CHANGING ANY OF
	// THESE THREE. They are about DIFFERENT TARGETS and the whole stage turns
	// on not confusing them:
	//   GetSurfaceIsLinearColorSpace() describes the OFFSCREEN target -- what
	//     producers must WRITE -- and is FALSE on an SDR and an HDR swapchain
	//     alike, because the pipeline writes sRGB-encoded values and the
	//     resolve decodes them afterwards.
	//   GetSurfaceFormat() and GetSurfaceIsExtendedRange() describe the
	//     SWAPCHAIN and the display -- what survives to the screen.
	virtual ERenderSurfaceFormat GetSurfaceFormat() override;
	virtual bool GetSurfaceIsExtendedRange() override;
	virtual bool GetSurfaceIsLinearColorSpace() override;
	virtual float GetDisplayHdrHeadroom() override;
	virtual void SetSurfaceEdrMetadata(float maxComponent) override;
	virtual bool GetSurfaceHasEdrMetadata() override;

	// --- test-support readback -------------------------------------------
	virtual bool ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA) override;
	virtual bool ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA) override;
	virtual bool ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA) override;
	virtual bool ReadSwapchainPixels(int x, int y, int w, int h, unsigned int *outRGBA) override;

	// --- factories ---------------------------------------------------------
	virtual CRenderTarget *CreateRenderTarget() override;
	virtual CRenderShader *CreateFlatColorShader(float r, float g, float b, float a) override;
	virtual CVideoYUVConverter *CreateVideoYUVConverter() override;

	// CreateMaskedTileShader() is deliberately NOT overridden: it serves c64d
	// and the game app, which keep working on OpenGL, and every caller already
	// tolerates NULL by drawing its unshaded fallback. Porting more HLSL blind
	// is what would make this stage unfinishable.

	// --- per-draw samplers -------------------------------------------------
	//
	// D3D11's sampler is a per-DRAW binding like Metal's, not per-TEXTURE state
	// like GL's, so these three are MANDATORY here: without them c64d's memory
	// map and every bitmap font come out blurred, and the
	// render_backend_nearest_magnification test -- which asserts on EVERY
	// backend -- fails.
	virtual bool ImageNeedsSamplerOverride(CSlrImage *image) override;
	virtual void QueueSamplerForImage(CSlrImage *image) override;
	virtual void QueueDefaultSampler() override;

	// --- video plane / LUT textures ---------------------------------------
	virtual void *CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel) override;
	virtual void UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride) override;
	virtual void DeletePlaneTexture(void *tex) override;
	virtual void *CreateLutTexture3D(int edge) override;
	virtual void UpdateLutTexture3D(void *tex, const void *data, int edge) override;
	virtual void DeleteLutTexture3D(void *tex) override;

	// --- seams for the D3D shader/converter classes ------------------------
	//
	// Opaque, for the same reason the header names no D3D types. Every one may
	// be NULL before CreateRenderContext(), and every caller must handle that.
	// EXACTLY TWO, and both have real consumers: CRenderShaderFlatColorD3D11
	// and CVideoYUVShaderD3D11 need the device to create their objects and the
	// context is fetched per draw from ImGui's published render state. A
	// linear-sampler accessor and a current-target-format accessor were written
	// alongside these and removed before shipping -- nothing called either, and
	// a seam with no consumer is a promise the next person has to keep.
	void *GetD3DDevice();          // ID3D11Device *
	void *GetD3DDeviceContext();   // ID3D11DeviceContext *, the IMMEDIATE one
};

#endif   // MT_RENDER_BACKEND_D3D11
#endif
