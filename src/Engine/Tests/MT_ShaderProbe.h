#ifndef _MT_ShaderProbe_h_
#define _MT_ShaderProbe_h_

class CSlrImage;

// A tiny ImGui window that draws one rectangle through a backend-created
// shader, so a pixel-capture test can prove the shader plumbing end to end:
// compile -> program/pipeline -> ImGui draw callback -> live encoder -> pixels.
//
// It lives in the ENGINE rather than in any one app because all three hosts need
// the same proof and none of them has a natural place to draw a bare shaded
// quad. It draws nothing at all unless a test opens it.
//
// EVERYTHING THAT TOUCHES THE GPU HAPPENS ON THE RENDER THREAD. Open/Close only
// record a request; the shader is created, compiled and destroyed inside
// MT_ShaderProbeRender(). imgui_test_engine runs TestFunc on its own thread, and
// the OpenGL context is bound to the render thread -- calling glCreateShader
// from the test thread does not fail, it crashes. Metal happens to tolerate it,
// which makes this exactly the kind of bug that passes on one backend.

enum EShaderProbeState
{
	SHADER_PROBE_CLOSED = 0,
	SHADER_PROBE_PENDING,          // requested; the render thread has not built it yet
	SHADER_PROBE_READY,            // compiled and drawing
	SHADER_PROBE_UNSUPPORTED,      // the active backend offers no such shader
	SHADER_PROBE_COMPILE_FAILED,   // the backend made one and it would not build
};

// Draw the probe if it is open, and service any pending open/close request.
// Call once per frame from the render path.
void MT_ShaderProbeRender();

// Request a flat-colour probe of the given colour, or close it. Both return
// immediately; poll MT_ShaderProbeGetState() (i.e. Yield until it leaves
// SHADER_PROBE_PENDING) for the outcome.
void MT_ShaderProbeOpenFlatColor(float r, float g, float b, float a);
void MT_ShaderProbeClose();

EShaderProbeState MT_ShaderProbeGetState();

// True once the probe is drawing with a compiled shader. False is ambiguous on
// its own -- use MT_ShaderProbeGetState() to tell "not built yet" from "this
// backend cannot" from "it failed to compile", which fail for entirely
// different reasons.
bool MT_ShaderProbeIsShaderUsable();

// Open the probe with a magnified 2x2 checker IMAGE instead of a shaded quad,
// with the given texture filtering. Used to prove point (nearest) magnification
// actually reaches the GPU: a 2x2 image blown up to fill the window contains
// EXACTLY four colours under point filtering and a smooth ramp of dozens under
// linear, so the two are trivially separable from a capture and the assertion
// cannot pass for the wrong reason.
//
// Returns false only if the request could not be recorded at all; poll
// MT_ShaderProbeGetState() as with the shader probes.
void MT_ShaderProbeOpenScaledImage(bool linearScaling);

// Open the probe with a 2x2 FP16 image whose pixels are ABOVE WHITE, so a
// read-back can prove the value survived the round trip to the GPU instead of
// being clamped on the way in.
//
// Separate from the scaled-image probe rather than a flag on it: that one
// guards magnification filtering, and a texture-format regression reporting
// itself as a filtering failure would send the next reader to the wrong place.
void MT_ShaderProbeOpenFloatImage();

// The probe's CSlrImage, for a test that needs to read its texture back.
// NULL until the probe reaches SHADER_PROBE_READY.
CSlrImage *MT_ShaderProbeGetImage();

// --- Float RENDER TARGET probe (S-5 Phase 5 Task 3) -------------------------
//
// Everything above proves float TEXTURES work. This proves rendering INTO
// float works, which is a different capability and the one HDR video playback
// actually needs: both CRenderTarget implementations used to hardcode 8 bits,
// so the YUV converter had nowhere to put an above-white value.
//
// It runs ENTIRELY ON THE RENDER THREAD for the reason this file's header
// comment gives: imgui_test_engine runs TestFunc on its own thread and the GL
// context is bound to the render thread, so creating an FBO from a TestFunc
// does not fail, it CRASHES. (Confirmed the hard way while writing this: the
// first version of the test did the work inline and took the whole ImGui suite
// down with it. Metal tolerated the same code, which is precisely the kind of
// bug that passes on one backend.)
//
// Request it, poll MT_ShaderProbeGetFloatTargetState() until it leaves
// SHADER_PROBE_PENDING, then read the results.
void MT_ShaderProbeRunFloatTargetCheck();

EShaderProbeState MT_ShaderProbeGetFloatTargetState();

// Results, valid once the state is SHADER_PROBE_READY. Each is the value read
// back from the target after clearing it to a known colour.
struct SFloatTargetProbeResult
{
	bool  createdRgba8 = false;      // an RGBA8 target reports RGBA8
	bool  switchedToFloat = false;   // same size, new format -> actually re-created
	bool  readBackOk = false;        // ReadTexturePixelsFloat succeeded
	float minR = 0.0f, minG = 0.0f, maxB = 0.0f;   // across ALL texels
	int   rgba8ClampedR = -1;        // the 8-bit re-clear's red channel, 0..255
};
const SFloatTargetProbeResult &MT_ShaderProbeGetFloatTargetResult();

// Read the FLOAT IMAGE probe's texture back, on the RENDER THREAD.
//
// Same reason as everything else here: ReadTexturePixelsFloat is a GL call
// under OpenGL, and a GL call from a TestFunc crashes. This was invisible
// until S-5 Phase 5 gave OpenGL a real ReadTexturePixelsFloat -- before that
// the base class returned false without touching GL, so the caller's
// "unavailable, log a warning" branch fired and the test passed having proved
// nothing. Implementing the read-back turned that silent no-op into a crash,
// which is how the masking came to light.
//
// Request after the float-image probe reports READY, then poll
// MT_ShaderProbeGetFloatImageReadbackState().
void MT_ShaderProbeRunFloatImageReadback();
EShaderProbeState MT_ShaderProbeGetFloatImageReadbackState();

struct SFloatImageReadbackResult
{
	bool  ok = false;
	float texel[4][4] = {};     // 2x2 image, RGBA per texel
};
const SFloatImageReadbackResult &MT_ShaderProbeGetFloatImageReadbackResult();

// --- SWAPCHAIN vs OFFSCREEN readback (S-6 Task A5) -------------------------
//
// Proves the present-time RESOLVE PASS is the identity it claims to be.
//
// A backend that composes into an offscreen buffer and resolves it to the
// swapchain has TWO sets of pixels, and every existing readback here sees only
// the first: ReadFramebufferPixels() reads the offscreen side by design,
// because in headless that is the only place pixels exist at all. So a resolve
// that quietly decoded every pixel -- displaying mid-grey 0.5 as 0.21 across
// the whole UI, which is the exact defect this stage's plan spent a review
// round removing -- would pass every test in this file.
//
// This reads BOTH for the same frame and hands back the largest per-channel
// difference. On an SDR swapchain that must be at most 1 LSB: not zero, because
// the GPU's float->UNORM8 rounding on the resolve and the CPU's half->byte
// rounding in ReadFramebufferPixels differ on .5 boundaries -- but 1 is nowhere
// near the ~73 a decode would produce.
//
// UNSUPPORTED on any backend that renders straight to the surface (OpenGL,
// Metal), where the question does not arise. That is a capability answer, not
// a failure, and a caller must report it as such.
//
// RENDER THREAD, like everything else here: D3D11's immediate context is not
// thread-safe and TestFunc runs on its own thread.
void MT_ShaderProbeRunSwapchainReadback();
EShaderProbeState MT_ShaderProbeGetSwapchainReadbackState();

// Service the swapchain readback. **CALL THIS AFTER THE FRAME HAS BEEN
// COMPOSITED AND RESOLVED** -- from the end of PresentFrameBuffer(), NOT from
// MT_ShaderProbeRender().
//
// This has its own entry point for a reason that is easy to get wrong and
// impossible to see: MT_ShaderProbeRender() runs BEFORE ImGui::Render() and
// long before the resolve, so at that instant the offscreen holds only
// NewFrame()'s clear and the back buffer holds the PREVIOUS frame's composited
// result. Comparing those two either fails on a correct implementation (if the
// sampled rect contains any UI) or -- worse -- PASSES on a broken one, because
// a cleared-to-black offscreen resolves to black whether the resolve decodes or
// not. That is exactly the false green this whole API exists to prevent.
void MT_ShaderProbeServiceSwapchainReadback();

struct SSwapchainReadbackResult
{
	bool ok = false;              // both readbacks succeeded
	bool supported = false;       // this backend distinguishes the two at all
	int  maxChannelDelta = -1;    // worst |offscreen - swapchain| over the rect
	int  sampledPixels = 0;
};
const SSwapchainReadbackResult &MT_ShaderProbeGetSwapchainReadbackResult();

// --- HDR transfer agreement probe (S-5 Phase 5 Task 7) ---------------------
//
// THE POINT OF THE WHOLE PHASE, made numerical.
//
// Runs a ramp of known code values through the REAL, COMPILED YUV shader into
// a float render target and reads it back, so the thing under test is the
// shader's own compiled constants -- not a C++ re-transcription of them, which
// would be a third copy testing itself in a circle.
//
// A shader cannot include CVideoTransferFunctions.h (its source is a string
// handed to the driver), so the two copies can only be kept honest by
// evaluating both and comparing. That is what this exists for.
//
// The ramp is written into the Y plane as a flat grey (U = V = neutral), so
// each step is an achromatic code value whose expected output the CPU can
// compute exactly.
// `surfaceP3` drives the linear-sRGB -> linear-Display-P3 primaries stage.
// It is a parameter rather than a hardcoded false because those 24 digits
// are duplicated into two shaders and would otherwise be the one piece of
// this phase's maths that nothing pins.
void MT_ShaderProbeRunHdrTransferCheck(int colorTrc, bool floatTarget, bool surfaceP3 = false);
EShaderProbeState MT_ShaderProbeGetHdrTransferState();

struct SHdrTransferProbeResult
{
	static const int kSteps = 16;
	bool  ok = false;
	float codeValue[kSteps] = {};   // the Y' code word fed in, 0..1
	float outR[kSteps] = {};        // what the shader produced
	float outG[kSteps] = {};
	float outB[kSteps] = {};
};
const SHdrTransferProbeResult &MT_ShaderProbeGetHdrTransferResult();

// The window's ImGui name, for CaptureAddWindow().
const char *MT_ShaderProbeWindowName();

#endif
