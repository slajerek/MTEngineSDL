#ifndef _CRenderBackend_h_
#define _CRenderBackend_h_

#include "SYS_Defs.h"
#include <SDL3/SDL.h>
#include "imgui.h"
#include "EImageGpuFormat.h"
#include "ERenderTextureFormat.h"

class CSlrImage;
class CRenderTarget;
class CMaskedTileShader;
class CVideoYUVConverter;
class CRenderShader;
class CRenderShaderCustomFragment;
enum VID_DisplayColorGamut : int;

// What the on-screen surface is made of. Backend-neutral on purpose: putting an
// MTLPixelFormat in this header would break every plain-C++ translation unit
// that includes it, and there are many.
enum ERenderSurfaceFormat
{
	RENDER_SURFACE_RGBA8,      // the SDR default on every backend
	RENDER_SURFACE_RGBA16F,    // float -- the HDR surface (S-4 Task 11). NOT simply
	                           // "linear": extended sRGB on Metal, and on D3D11 it
	                           // holds sRGB-ENCODED values that the present-time
	                           // resolve pass turns into scRGB. Ask
	                           // GetSurfaceIsLinearColorSpace(), never the format.
};

// ---------------------------------------------------------------------------
// Scissor arithmetic, factored out so it can be TESTED
//
// A Metal render pass aborts the process if a scissor rect leaves the render
// target, and the abort lands inside vendored ImGui code with no numbers in it
// -- so the arithmetic that produces the rect lives here, where a test can feed
// it the exact conditions a live resize creates.
//
// Reproduces what ImGui_ImplMetal_RenderDrawData does, deliberately: same
// clip_off/clip_scale projection, same clamp to the frame buffer, same
// truncating NSUInteger conversion. If the two ever drift apart this stops
// predicting anything, which is why it says so here.
// ---------------------------------------------------------------------------

struct SVidScissor
{
	int x = 0, y = 0, w = 0, h = 0;
	bool skipped = false;      // ImGui would `continue` past this command
	bool degenerate = false;   // survives ImGui's guard but truncates to 0 wide/high
	bool exceeds = false;      // leaves the attachment -- the fatal one
};

// fbW/fbH are ImGui's framebuffer (DisplaySize * FramebufferScale);
// attW/attH are the ACTUAL render target. They are separate arguments because
// the whole bug class is them disagreeing.
SVidScissor VID_ScissorForClipRect(const ImVec4 &clipRect,
								   const ImVec2 &clipOff, const ImVec2 &clipScale,
								   int fbW, int fbH, int attW, int attH);

// Clamp ImGui's own framebuffer down to the attachment, so the backend's
// internal clamp -- computed from DisplaySize -- can never permit a rect the
// render target cannot hold.
void VID_ClampDrawDataToAttachment(ImDrawData *drawData, int attW, int attH);

// DEBUG diagnosis: walk every command and report any scissor that would be
// rejected or silently degenerate, WITH the numbers. Returns how many were
// found. Costs a pass over the draw data, so callers gate it.
int VID_ReportBadScissors(const ImDrawData *drawData, int attW, int attH,
						  const char *whereFrom);

// this is generic class to wrap ImGui's render backends

class CRenderBackend
{
public:
	CRenderBackend(const char *name);
	SDL_Window *mainWindow;
	
	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized);
	virtual void CreateRenderContext();
	virtual void InitRenderPipeline();
	virtual void CreateFontsTexture();
	virtual void NewFrame(ImVec4 clearColor);
	virtual void PresentFrameBuffer(ImVec4 clearColor);
	virtual void ApplyDisplayColorGamut(VID_DisplayColorGamut gamut);
	virtual void Shutdown();
	
	virtual void CreateTexture(CSlrImage *image);
	virtual void UpdateTextureLinearScaling(CSlrImage *image);
	virtual void ReBindTexture(CSlrImage *image);
	virtual void DeleteTexture(CSlrImage *image);

	// Compressed texture format this backend/device can upload, or
	// IMG_GPU_UNCOMPRESSED if none. Default: none (safe for any backend
	// that does not override this).
	virtual EImageGpuFormat GetPreferredCompressedFormat() { return IMG_GPU_UNCOMPRESSED; }

	// Which RESIDENT texture formats this backend can actually upload.
	//
	// A backend that returns false for RGBA16F is not broken -- callers fall
	// back to RGBA8, exactly as they do for a NULL shader factory. The default
	// answer is deliberately the conservative one, so a backend that has never
	// heard of float textures cannot accidentally claim to support them.
	virtual bool SupportsTextureFormat(ERenderTextureFormat fmt)
	{
		return fmt == RENDER_TEXTURE_RGBA8;
	}

	// ---------------------------------------------------------------------
	// Capability
	// ---------------------------------------------------------------------

	// Whether raw OpenGL calls made OUTSIDE this class are valid. Guards the
	// GL residue that outlives S-4, and any app code that still reaches for GL
	// directly -- under Metal such calls are silent no-ops that render nothing.
	virtual bool SupportsOpenGLShaders() { return false; }

	// The on-screen surface's pixel format. Every ON-SCREEN pipeline state must
	// be built against this rather than hardcoding one, because switching the
	// layer to float invalidates any pipeline baked at 8-bit. Offscreen targets
	// follow their own CRenderTarget, NOT this.
	virtual ERenderSurfaceFormat GetSurfaceFormat() { return RENDER_SURFACE_RGBA8; }

	// Granted HDR headroom on the display this window is on; 1.0 means "no extra
	// range". Backends that cannot do HDR return 1.0 forever, so callers need no
	// #ifdef. This is a POLL, never a cached property: macOS grants headroom
	// lazily (it reads 1.0 for the first seconds after the request and ramps)
	// and then keeps moving it with display brightness and ambient light.
	virtual float GetDisplayHdrHeadroom() { return 1.0f; }

	// Is the on-screen surface EXTENDED RANGE -- can it carry values above 1.0
	// at all? False on any SDR surface, and false on OpenGL, whose surface
	// clips there whatever the texture format can hold.
	//
	// Hoisted to the base class (S-5) because backend-agnostic APP code has to
	// ask: the decision to spend twice the memory on a float image is only
	// worth making when the surface can actually show the extra range.
	virtual bool GetSurfaceIsExtendedRange() { return false; }

	// Does the surface want LINEAR values, or encoded ones? Distinct from the
	// question above -- both extended sRGB and extended LINEAR sRGB are
	// extended-range, and they disagree only about the transfer curve. Getting
	// this one wrong is what washed out S-4's first HDR surface.
	virtual bool GetSurfaceIsLinearColorSpace() { return false; }

	// Describe the CONTENT now on screen to the compositor, as HDR10 mastering
	// metadata (CAEDRMetadata on macOS).
	//
	// maxComponent is the content's peak linear value with 1.0 = SDR reference
	// white; pass 1.0 (or less) for ordinary SDR content, which CLEARS the
	// metadata rather than leaving the previous image's peak standing. That
	// clearing matters: without it the first HDR photo of a session would go on
	// describing every SDR photo viewed after it.
	//
	// A no-op on any backend that has no such concept, so callers need no
	// #ifdef.
	// Describe the CONTENT now on screen to the compositor, where the surface
	// is one that HDR10 mastering metadata actually describes.
	//
	// MEASURED (2026-08-20): on an EXTENDED-RANGE surface this metadata is
	// actively harmful -- it makes above-white and SDR white render IDENTICALLY,
	// destroying the very distinction a float pipeline exists to produce, because
	// HDR10 describes PQ content and an extended-sRGB buffer is not PQ. The Metal
	// implementation therefore applies it only on a PQ layer and otherwise
	// guarantees the layer carries none. See CRenderBackendMetal.mm for the
	// experiment.
	//
	// maxComponent is the content's peak linear value with 1.0 = SDR reference
	// white; 1.0 or less means "ordinary SDR content", which CLEARS any metadata
	// rather than leaving the previous image's peak standing.
	virtual void SetSurfaceEdrMetadata(float maxComponent) { (void)maxComponent; }

	// Whether the surface currently carries EDR metadata. For tests.
	virtual bool GetSurfaceHasEdrMetadata() { return false; }

	// ---------------------------------------------------------------------
	// Test-support readback
	// ---------------------------------------------------------------------

	// Read back a rectangle of the current frame as tightly-packed RGBA8,
	// top-down, in FRAMEBUFFER pixels (the caller handles ImGui-space scaling).
	// Returns false when this backend cannot read back, so a test fails loudly
	// instead of comparing two empty buffers and passing.
	virtual bool ReadFramebufferPixels(int x, int y, int w, int h, unsigned int *outRGBA) { return false; }

	// Read an ARBITRARY texture back to RGBA8, top-down, for tests.
	//
	// Distinct from ReadFramebufferPixels(), which reads what was just
	// presented. This one takes a texture handle -- typically a CRenderTarget's
	// colour attachment -- because the video orientation guard has to inspect
	// the render-to-texture output that never reaches the screen. Its GL half
	// used to call glBindTexture/glGetTexImage inline in the test, which made
	// that assertion structurally impossible to run on any other backend.
	//
	// `outRGBA` must hold w*h unsigned ints. Returns false when the backend
	// cannot read the texture, which callers must report as a capability gap
	// rather than as a failed assertion.
	virtual bool ReadTexturePixels(void *texture, int w, int h, unsigned int *outRGBA) { return false; }

	// Read back a rectangle of the SWAPCHAIN / on-screen surface, as opposed to
	// whatever offscreen target the frame was composed into.
	//
	// EXISTS FOR EXACTLY ONE ASSERTION, and it is worth the API surface: on a
	// backend that composes into an offscreen buffer and RESOLVES it to the
	// swapchain (S-6's D3D11 path), ReadFramebufferPixels() reads the offscreen
	// side by design -- it is the only place pixels exist in headless -- so
	// nothing else can see what the resolve actually produced. Comparing the
	// two proves the resolve is the identity it claims to be on an SDR
	// swapchain; without it, a resolve that quietly decoded every pixel would
	// pass every test we have.
	//
	// False on any backend that has nothing to distinguish -- OpenGL and Metal
	// render straight to the surface, so the question does not arise there and
	// a caller must report "unavailable" rather than "failed".
	//
	// LIKE EVERY OTHER READBACK HERE, THIS IS A RENDER-THREAD CALL. D3D11's
	// immediate context is not thread-safe and imgui_test_engine runs TestFunc
	// on its own thread; drive it through MT_ShaderProbe's request/poll pair,
	// never inline in a test.
	virtual bool ReadSwapchainPixels(int x, int y, int w, int h, unsigned int *outRGBA) { return false; }

	// The same, for an RGBA16F texture, as FLOATS -- because the whole point of
	// such a texture is the values that do not fit in 8 bits, and reading one
	// back through the RGBA8 path above would clamp away exactly what a test
	// wants to assert.
	//
	// `outRGBA` must hold w*h*4 floats. Returns false when the backend cannot
	// read the texture, which callers must report as a capability gap rather
	// than as a failed assertion.
	virtual bool ReadTexturePixelsFloat(void *texture, int w, int h, float *outRGBA) { return false; }

	// ---------------------------------------------------------------------
	// Factories. All return NULL on a backend that cannot provide the thing;
	// every caller must tolerate that by drawing its plain/unshaded fallback
	// rather than dereferencing.
	// ---------------------------------------------------------------------

	virtual CRenderTarget *CreateRenderTarget() { return NULL; }
	virtual CMaskedTileShader *CreateMaskedTileShader(bool queued) { return NULL; }

	// A shader that fills its quad with a constant colour, ignoring the texture.
	//
	// Test/diagnostic only, and worth the API surface: it proves the shader
	// plumbing -- compile, program/pipeline creation, ImGui draw-callback
	// dispatch, live-encoder access -- independently of any real shader's maths,
	// so a failure in the masked-tile or CRT port is a shader bug rather than an
	// infrastructure bug. Present on EVERY backend deliberately, so the test
	// that exercises it asserts on each rather than being a Metal-only test that
	// imgui_test_engine would silently count as passed under OpenGL.
	//
	// Caller owns the returned shader and must call CompileShaders() on it.
	virtual CRenderShader *CreateFlatColorShader(float r, float g, float b, float a) { return NULL; }

	// A fragment shader whose SOURCE is supplied at runtime and can be replaced
	// while the app runs -- the seam an in-app shader editor draws through.
	//
	// Caller owns the result and must call SetFragmentSource() on it, ON THE
	// RENDER THREAD, before it draws anything.
	//
	// Present on EVERY backend deliberately, for the same reason
	// CreateFlatColorShader gives above: a facility that exists on one backend
	// and silently not another is a trap for whoever picks it up next.
	virtual CRenderShaderCustomFragment *CreateCustomFragmentShader(const char *name) { return NULL; }

	// --- per-DRAW texture filtering -------------------------------------
	//
	// OpenGL carries the min/mag filter as per-TEXTURE state -- see
	// UpdateTextureLinearScaling(), which calls glTexParameteri on the texture
	// itself -- so a blit needs to do nothing and every image samples the way it
	// asked to.
	//
	// Metal's sampler is a per-DRAW binding shared by everything in the render
	// pass, so an image that wants nearest magnification must say so around its
	// own draw or it silently gets whatever the last draw left bound (the linear
	// default). That is why c64d's memory map and bitmap fonts came out blurred
	// on Metal while looking correct on OpenGL: nothing was wrong with the
	// texture, the filter simply had nowhere to live.
	//
	// NeedsPerDrawSamplerSelection() is false by default, so backends whose
	// filter is per-texture pay nothing at all -- not even a callback that
	// splits the draw list.
	// True when THIS image cannot be drawn correctly with whatever sampler ImGui
	// leaves bound, so the blit layer must bracket its draw. False for the
	// common case, which then costs nothing at all -- not even a callback that
	// splits the draw list.
	virtual bool ImageNeedsSamplerOverride(CSlrImage *image) { return false; }

	// Queue a draw callback establishing the right filtering for `image`, and
	// (the second) restoring what ImGui expects afterwards. Called only around
	// images for which ImageNeedsSamplerOverride() returned true.
	virtual void QueueSamplerForImage(CSlrImage *image) {}
	virtual void QueueDefaultSampler() {}
	virtual CVideoYUVConverter *CreateVideoYUVConverter() { return NULL; }

	// ---------------------------------------------------------------------
	// Video plane textures. Handles use the same void* convention CSlrImage
	// uses -- never GLuint, which is 32-bit and would truncate an
	// id<MTLTexture>. `channels` is 1 or 2; `bytesPerChannel` is 1 for 8-bit
	// planes and 2 for the 10-bit ones (which carry 0..1023 in 16 bits).
	// ---------------------------------------------------------------------

	virtual void *CreatePlaneTexture(int width, int height, int channels, int bytesPerChannel) { return NULL; }
	virtual void UpdatePlaneTexture(void *tex, const void *data, int width, int height, int stride) {}
	virtual void DeletePlaneTexture(void *tex) {}

	// The CM-E display LUT is a 3D texture and the 2D plane methods cannot carry
	// it. Without these a Metal video path would either keep GL calls that
	// silently no-op -- leaving video UN-COLOUR-MANAGED on Metal only -- or drop
	// LUT support without saying so. `edge` is the lattice size; the format is
	// RGBA16 unorm on every backend.
	virtual void *CreateLutTexture3D(int edge) { return NULL; }
	virtual void UpdateLutTexture3D(void *tex, const void *data, int edge) {}
	virtual void DeleteLutTexture3D(void *tex) {}

	virtual ~CRenderBackend();
	
	const char *name;
};

#endif
