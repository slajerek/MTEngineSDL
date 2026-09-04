#ifndef _CRenderShaderCustomFragment_h_
#define _CRenderShaderCustomFragment_h_

#include <string>
#include <cstddef>

// ShaderToy's complete uniform set, plus two of our own. 240 bytes.
//
// THE OFFSETS WERE COMPILED, not reasoned about. Do not reorder to tidy this:
// every one is asserted below, and a disagreement between this and a shader
// preamble is a silent wrong-value bug that reads as a shader defect rather
// than a struct defect.
//
//   off  field                 GLSL                HLSL / MSL
//     0  resolution[3]         vec3                float3 / packed_float3
//    12  time                  float
//    16  timeDelta             float
//    20  frameRate             float
//    24  frame                 int
//    28  padding
//    32  mouse[4]              vec4
//    48  date[4]               vec4
//    64  channelTime[4]        float[4]            float4      <- NOT an array
//    80  channelResolution     vec3[4]             float3[4]   16-byte stride
//   144  sampleRate            float
//   148  padding
//   160  channelUvTransform[4][4] vec4[4]  ours, not ShaderToy's
//   224  channelWrap[4]        vec4        ours, not ShaderToy's
//
// THREE TRAPS LIVE IN THIS TABLE, all of them silent when got wrong.
//
// 1. THE PADDING AT 28 AND 148 IS LOAD-BEARING. HLSL and MSL align a 16-byte
//    vector to 16 bytes, so without it iMouse would sit at 24 in C++ and 32
//    in the shader; the shader would read neighbouring fields and merely look
//    wrong.
//
// 2. iChannelTime IS DECLARED float4 IN HLSL AND MSL, never an array. HLSL's
//    constant-buffer packing gives every array element its own 16-byte
//    boundary, so `float iChannelTime[4]` occupies 64 bytes there rather than
//    16 -- which would push iChannelResolution to 128 and iSampleRate to 192
//    against a 240-byte upload, on D3D11 only. Vector subscripting reads the
//    same. GLSL keeps the true array: it uploads field by field with
//    glUniform* and has no packing rule to violate.
//
// 3. MSL's PADDING MUST BE packed_float3, not float3. A plain MSL float3 is
//    16 bytes with 16-byte alignment, so the padding itself gets padded and
//    every field after it shifts. Measured: `xcrun metal` rejects the
//    240-byte assert with float3 and accepts it with packed_float3. The same
//    rule is why resolution is packed_float3 there.
//
// iChannelUvTransform and iChannelWrap are not ShaderToy's. They are consumed
// by mtChannelUV(), the function behind the texChannelN macros -- see the
// backends' preambles -- so that vflip and wrapping happen in the shader and
// CSlrImage's power-of-two padding is corrected. A pasted ShaderToy shader
// never mentions them: it says texChannel0(uv) and gets all three.
struct SShaderToyUniforms
{
	float resolution[3];
	float time;
	float timeDelta;
	float frameRate;
	int   frame;
	float _pad0;
	float mouse[4];
	float date[4];
	float channelTime[4];
	float channelResolution[4][4];
	float sampleRate;
	float _pad1[3];
	// PER CHANNEL: .xy is the uv scale (CSlrImage::defaultTexEndX/Y, the
	// fraction of the padded texture the image occupies), .z is vflip, .w is
	// unused. Not a scale alone any more, hence the name.
	//
	// VFLIP IS 1.0 BY DEFAULT, and it is not a workaround for how the engine
	// loads images -- CSlrImage stores them top-down, the same way ImGui wants
	// them, which is why a thumbnail drawn with ImGui::Image is upright.
	// ShaderToy's fragCoord has its origin at the BOTTOM LEFT, so a shader
	// computing `uv = fragCoord / iResolution.xy` hands v=0 to the BOTTOM of
	// the screen while v=0 is the TOP row of the texture. shadertoy.com has
	// exactly this problem and solves it exactly this way: a per-channel vflip
	// that defaults to on.
	float channelUvTransform[4][4];
	float channelWrap[4];
};

static_assert(sizeof(SShaderToyUniforms) == 240, "uniform block must be 240 bytes");
static_assert(offsetof(SShaderToyUniforms, time) == 12, "iTime at 12");
static_assert(offsetof(SShaderToyUniforms, mouse) == 32, "iMouse at 32");
static_assert(offsetof(SShaderToyUniforms, date) == 48, "iDate at 48");
static_assert(offsetof(SShaderToyUniforms, channelTime) == 64, "iChannelTime at 64");
static_assert(offsetof(SShaderToyUniforms, channelResolution) == 80, "iChannelResolution at 80");
static_assert(offsetof(SShaderToyUniforms, sampleRate) == 144, "iSampleRate at 144");
static_assert(offsetof(SShaderToyUniforms, channelUvTransform) == 160, "iChannelUvTransform at 160");
static_assert(offsetof(SShaderToyUniforms, channelWrap) == 224, "iChannelWrap at 224");

// Four input channels, as ShaderToy has. THEY BIND AT SLOTS 1..4, NOT 0..3:
// ImGui claims slot 0 for its own draw command -- PSSetShaderResources(0,1,..),
// setFragmentTexture atIndex:0, glBindTexture on the active unit -- and it
// does so AFTER the callback that installs this shader, so a channel at slot 0
// is overwritten before a pixel is drawn. Verified on all three backends, and
// measured on OpenGL with a probe texture (2026-09-05).
//
// The slot number is invisible to the shader source. The variable is still
// iChannel0; only its declaration says where it reads from. Nothing is parsed
// or rewritten, and a pasted texture(iChannel0, uv) compiles untouched.
static const int kShaderChannelCount = 4;

enum EShaderChannelFilter { SHADER_CHANNEL_NEAREST = 0, SHADER_CHANNEL_LINEAR = 1 };

// Wrap is applied by the texChannelN macro, in the shader, and the hardware
// sampler stays CLAMP on every backend. CSlrImage pads every texture to a
// power of two, so hardware REPEAT would tile the PADDING rather than the
// image -- which defeats the only reason to want repeat.
enum EShaderChannelWrap { SHADER_CHANNEL_CLAMP = 0, SHADER_CHANNEL_REPEAT = 1 };


// A fragment shader whose SOURCE is supplied at runtime and can be replaced
// while the app runs -- the seam behind an in-app shader editor.
//
// A PURE INTERFACE, DELIBERATELY NOT DERIVED FROM CRenderShader.
//
// Each backend's implementation derives from its own shader base
// (CRenderShaderOpenGL4, CRenderShaderMetal) AND from this. Those bases already
// derive from CRenderShader, which carries data -- isCompiled, screenWidth. If
// this interface derived from it too, an implementation would hold TWO
// CRenderShader subobjects and `shader->isCompiled` would not compile at all.
// Virtual inheritance would fix that by touching every shader subclass in the
// engine, which is a large change to make for one example. So: no base, and the
// three calls a host needs to draw are re-declared here as pure virtuals, which
// the most-derived class satisfies for both bases with a single override.
//
// D3D11 is the exception that costs nothing: its shaders derive from
// CRenderShader directly, with no backend base in between, so there is no
// diamond there to begin with.
class CRenderShaderCustomFragment
{
public:
	virtual ~CRenderShaderCustomFragment() {}

	// RENDER THREAD ONLY. Rebuilds from `mainImageSource` wrapped in this
	// backend's preamble and entry point.
	//
	// ON FAILURE THE PREVIOUS WORKING PROGRAM STAYS BOUND AND DRAWABLE: build
	// the new one first and swap only on success. A host's editor then keeps
	// showing the last shader that worked while the user fixes a typo, instead
	// of going black on every keystroke that does not parse.
	//
	// Returns false and fills the error log.
	virtual bool SetFragmentSource(const char *mainImageSource) = 0;

	// The driver's diagnostics, verbatim. RETURNED, NOT LOGGED, and that is not
	// a style preference: LOGError is a no-op under GLOBAL_DEBUG_OFF, which
	// platform/Linux/src.Linux/DBG_Log.h sets and expands to `do {} while (0)`.
	// An error panel fed by the log would be empty precisely on the platform
	// where a headless CI run is the only way anyone ever sees the failure.
	//
	// Never NULL; the empty string when the last build succeeded.
	virtual const char *GetCompileErrorLog() = 0;

	// How many lines this backend prepends to the user's source, INCLUDING any
	// version line the base class adds. A host subtracts it before showing a
	// compiler's line number, or it sends the user to a line that does not
	// exist in the editor.
	//
	// COUNTED FROM THE STRING, never hand-written -- a hand count goes stale
	// the first time someone adds a uniform.
	virtual int GetPreambleLineCount() = 0;

	virtual void SetUniforms(const SShaderToyUniforms &u) = 0;

	// The texture for one channel, as CSlrImage::TexturePtr() returns it, or
	// NULL for "this channel is empty" -- which every backend samples as zero.
	//
	// RENDER THREAD ONLY, like SetFragmentSource and SetUniforms. They are
	// pure stores: no GPU call, no sampler allocated, every GPU call happens
	// later in the draw. But the values they write are read by that draw with
	// no synchronisation, so calling them from elsewhere is a data race. A
	// host that needs to set a channel from another thread queues the change,
	// the way an editor queues a rebuild.
	//
	// Creating a sampler object inside these is separately forbidden: it would
	// put newSamplerStateWithDescriptor: or CreateSamplerState on whatever
	// thread the caller happened to be.
	virtual void SetChannelTexture(int channel, void *texture) = 0;
	virtual void SetChannelSampler(int channel, EShaderChannelFilter filter,
								   EShaderChannelWrap wrap) = 0;

	// The drawing contract, mirroring CRenderShader's, so a host can draw
	// through this pointer alone. See MT_ShaderProbe.cpp for the pattern:
	// UseShaderProgram() queues an ImGui draw callback, the geometry is an
	// ordinary ImGui draw, ResetState() gives ImGui its pipeline back.
	virtual void UseShaderProgram() = 0;
	virtual void ResetState() = 0;

	// False when nothing has compiled yet or every build so far has failed.
	// A host draws its own fallback rather than issuing draws that silently
	// render nothing.
	virtual bool IsUsable() = 0;
};

#endif
