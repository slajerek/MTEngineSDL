#include "CRenderShaderCustomFragmentOpenGL4.h"
#include "DBG_Log.h"

// The preamble. iChannel0 is declared because ShaderToy has one and a pasted
// snippet may name it. Note that declaring it does NOT silence the base's
// "uniform 'iChannel0' not found" line: the driver optimises out any uniform
// the source does not READ, so it is missing from the linked program either
// way. That line is the engine's existing behaviour for every shader that does
// not sample a texture.
//
// GetPreambleLineCount() COUNTS this string rather than stating a number, and
// adds one for the #version line CompileShaders() puts in front of everything.
static const char *kPreamble = R"GLSL(in vec2 Frag_UV;
in vec4 Frag_Color;
uniform sampler2D iChannel0;
uniform sampler2D iChannel1;
uniform sampler2D iChannel2;
uniform sampler2D iChannel3;
uniform vec3 iResolution;
uniform float iTime;
uniform float iTimeDelta;
uniform float iFrameRate;
uniform int iFrame;
uniform vec4 iMouse;
uniform vec4 iDate;
uniform float iChannelTime[4];
uniform vec3 iChannelResolution[4];
uniform float iSampleRate;
uniform vec4 iChannelUvTransform[4];
uniform vec4 iChannelWrap;
layout (location = 0) out vec4 Out_Color;
#define MAIN_IMAGE void mainImage(out vec4 fragColor, in vec2 fragCoord)
vec2 mtChannelUV(int n, vec2 uv)
{
	// VFLIP FIRST. ShaderToy's fragCoord starts at the BOTTOM left, so a
	// shader's `uv = fragCoord / iResolution.xy` puts v=0 at the bottom of the
	// screen -- while v=0 is the TOP row of the texture, because CSlrImage
	// stores images top-down the way ImGui wants them. Without this the
	// picture is upside down, which is the same reason shadertoy.com has a
	// per-channel vflip and defaults it to on.
	if (iChannelUvTransform[n].z > 0.5) uv.y = 1.0 - uv.y;
	uv = (iChannelWrap[n] > 0.5) ? fract(uv) : clamp(uv, 0.0, 1.0);
	// Scale LAST: the wrap has to happen in the image's own 0..1 space, not in
	// the padded texture's.
	return uv * iChannelUvTransform[n].xy;
}
#define texChannel0(uv) texture(iChannel0, mtChannelUV(0, uv))
#define texChannel1(uv) texture(iChannel1, mtChannelUV(1, uv))
#define texChannel2(uv) texture(iChannel2, mtChannelUV(2, uv))
#define texChannel3(uv) texture(iChannel3, mtChannelUV(3, uv))
void mainImage(out vec4 fragColor, in vec2 fragCoord);
)GLSL";

static const char *kEntry = R"GLSL(
void main()
{
	// ShaderToy's origin is bottom-left and its coordinate is in pixels;
	// ImGui's UV is top-left and normalised.
	vec2 fragCoord = vec2(Frag_UV.s * iResolution.x,
						  (1.0 - Frag_UV.t) * iResolution.y);
	mainImage(Out_Color, fragCoord);
	// Modulate by the vertex colour, as CRenderShaderOpenGL4ShaderToy does. The
	// quad is drawn white so this changes nothing visually -- but leaving
	// Frag_Color unread makes the driver warn on every compile, and a warning
	// on a correct shader is exactly what teaches a user to ignore the log.
	Out_Color *= Frag_Color;
}
)GLSL";

CRenderShaderCustomFragmentOpenGL4::CRenderShaderCustomFragmentOpenGL4(
		CRenderBackendOpenGL4 *renderBackend, const char *name)
: CRenderShaderOpenGL4(renderBackend, name)
{
}

CRenderShaderCustomFragmentOpenGL4::~CRenderShaderCustomFragmentOpenGL4()
{
	// GUARDED, and the guard is the point: the seam test creates one of these
	// on the test engine's thread and deletes it without ever compiling, so
	// this destructor must not issue a GL call in that case. glDeleteProgram(0)
	// is a documented no-op, but being explicit says why the check is here.
	if (shaderHandle != 0)
	{
		glDeleteProgram(shaderHandle);
		shaderHandle = 0;
	}
	// Guarded like the program: the seam test builds one of these on the test
	// engine's thread and deletes it without ever compiling, so no GL call may
	// be unconditional here.
	if (samplerNearest != 0) { glDeleteSamplers(1, &samplerNearest); samplerNearest = 0; }
	if (samplerLinear != 0)  { glDeleteSamplers(1, &samplerLinear);  samplerLinear = 0; }
}

const char *CRenderShaderCustomFragmentOpenGL4::GetFragmentShaderSource()
{
	return fullSource.c_str();
}

int CRenderShaderCustomFragmentOpenGL4::GetPreambleLineCount()
{
	int lines = 0;
	for (const char *p = kPreamble; *p != '\0'; p++)
	{
		if (*p == '\n')
			lines++;
	}

	// Plus the "#version NNN\n" that CRenderShaderOpenGL4::CompileShaders()
	// prepends as line 1 of every source it builds. Forgetting it puts every
	// reported line number one out.
	return lines + 1;
}

bool CRenderShaderCustomFragmentOpenGL4::SetFragmentSource(const char *mainImageSource)
{
	if (mainImageSource == NULL)
		mainImageSource = "";

	fullSource  = kPreamble;
	fullSource += mainImageSource;
	fullSource += "\n";
	fullSource += kEntry;

	// BUILD NEW, SWAP ON SUCCESS. Step the working program out of the base's
	// way so CompileShaders() builds a fresh one and, if it fails, deletes only
	// its own partial objects -- it zeroes shaderHandle on failure, which would
	// otherwise throw away the shader the host is still drawing with.
	GLuint oldHandle = shaderHandle;
	shaderHandle = 0;

	// BOTH LATCHES. CompileShaders() returns immediately on either; they exist
	// to stop a failed compile being retried every frame, so a rebuild that
	// leaves them set does nothing at all and reports the previous verdict.
	isCompiled = false;
	compileAttemptedAndFailed = false;

	CompileShaders();

	if (!isCompiled)
	{
		// Put the last good program back. lastCompileLog now holds the driver's
		// words, which GetCompileErrorLog() hands to the host.
		shaderHandle = oldHandle;
		isCompiled = (oldHandle != 0);
		compileAttemptedAndFailed = false;
		return false;
	}

	if (oldHandle != 0)
		glDeleteProgram(oldHandle);

	// No GetUniformsLocations() here: CompileShaders() already called it on the
	// new program before setting isCompiled. Calling it again would re-run every
	// lookup, and re-log the base's "uniform 'iChannel0' not found" line, for a
	// result that is already stored.
	return true;
}

void CRenderShaderCustomFragmentOpenGL4::GetUniformsLocations()
{
	CRenderShaderOpenGL4::GetUniformsLocations();

	char name[32];
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		snprintf(name, sizeof(name), "iChannel%d", i);
		locChannelSampler[i] = glGetUniformLocation(shaderHandle, name);
		// ELEMENT BY ELEMENT. glUniform3fv wants tightly packed vec3s at a
		// 12-byte stride; the C++ array has a 16-byte stride because that is
		// what HLSL and MSL need. One call with a count of 4 would feed the
		// wrong floats into elements 1..3.
		snprintf(name, sizeof(name), "iChannelResolution[%d]", i);
		locChannelResolution[i] = glGetUniformLocation(shaderHandle, name);
	}
	locFrameRate      = glGetUniformLocation(shaderHandle, "iFrameRate");
	locDate           = glGetUniformLocation(shaderHandle, "iDate");
	locChannelTime    = glGetUniformLocation(shaderHandle, "iChannelTime");
	locSampleRate     = glGetUniformLocation(shaderHandle, "iSampleRate");
	locChannelUvTransform = glGetUniformLocation(shaderHandle, "iChannelUvTransform");
	locChannelWrap    = glGetUniformLocation(shaderHandle, "iChannelWrap");

	// glGetUniformLocation DIRECTLY, not the base's GetUniformLocation helper,
	// which LOGErrors when a uniform is missing.
	//
	// For a shader the HOST wrote that is the normal case, not a fault: the
	// driver optimises out every uniform the source does not read, so the
	// stock Tunnel preset -- which uses iResolution and iTime and nothing else
	// -- would print three ERROR lines on every single compile. An editor that
	// screams on a correct shader teaches its user to ignore the log.
	locResolution = glGetUniformLocation(shaderHandle, "iResolution");
	locTime       = glGetUniformLocation(shaderHandle, "iTime");
	locTimeDelta  = glGetUniformLocation(shaderHandle, "iTimeDelta");
	locFrame      = glGetUniformLocation(shaderHandle, "iFrame");
	locMouse      = glGetUniformLocation(shaderHandle, "iMouse");
}

void CRenderShaderCustomFragmentOpenGL4::SetShaderVars()
{
	CRenderShaderOpenGL4::SetShaderVars();

	// NO -1 GUARDS. A uniform the host's shader never reads is optimised out by
	// the driver and its location comes back -1, and glUniform* on -1 is a
	// documented no-op. Guarding would only hide which uniforms a shader
	// actually uses.
	glUniform3f(locResolution, uniforms.resolution[0], uniforms.resolution[1], uniforms.resolution[2]);
	glUniform1f(locTime, uniforms.time);
	glUniform1f(locTimeDelta, uniforms.timeDelta);
	glUniform1i(locFrame, uniforms.frame);
	glUniform4f(locMouse, uniforms.mouse[0], uniforms.mouse[1], uniforms.mouse[2], uniforms.mouse[3]);

	glUniform1f(locFrameRate, uniforms.frameRate);
	glUniform1f(locSampleRate, uniforms.sampleRate);
	glUniform4fv(locDate, 1, uniforms.date);
	glUniform1fv(locChannelTime, kShaderChannelCount, uniforms.channelTime);
	glUniform4fv(locChannelUvTransform, kShaderChannelCount, &uniforms.channelUvTransform[0][0]);
	glUniform4fv(locChannelWrap, 1, uniforms.channelWrap);
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		glUniform3f(locChannelResolution[i], uniforms.channelResolution[i][0],
					uniforms.channelResolution[i][1], uniforms.channelResolution[i][2]);
	}

	// THE SAMPLER UNITS ARE ASSIGNED HERE, NOT AFTER LINK, and that is not a
	// stylistic choice. CRenderShaderOpenGL4 looks iChannel0 up as ITS OWN
	// texture uniform (:137) and resets it to unit 0 inside UseShaderProgram
	// (:236), on every frame, immediately before calling this function. An
	// assignment made once after glLinkProgram is overwritten on the first
	// frame and every frame after, and channel 0 silently samples ImGui's
	// draw texture instead of its own.
	//
	// Units 1..4, never 0: ImGui claims slot 0 for its own draw command,
	// after the callback that installs this shader.
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		glUniform1i(locChannelSampler[i], i + 1);

		glActiveTexture(GL_TEXTURE1 + i);
		glBindTexture(GL_TEXTURE_2D, (GLuint)(intptr_t)channelTexture[i]);
		// A SAMPLER OBJECT, never glTexParameteri: filter state set that way
		// lives on the TEXTURE, so writing it per frame would permanently
		// change how that image is filtered in every other draw in the
		// application. Free to do here -- imgui_impl_opengl3 binds a sampler
		// only ever at unit 0.
		glBindSampler(i + 1, SamplerFor(channelFilter[i]));
	}

	// RESTORE THE ACTIVE UNIT, and the reason is not the obvious one.
	// MEASURED 2026-09-05: removing this breaks nothing visible, which is what
	// makes it dangerous. imgui_impl_opengl3 sets glActiveTexture(GL_TEXTURE0)
	// ONCE PER FRAME in RenderDrawData's state backup (:482), not per draw
	// command, and its own shader always reads unit 0 (:391). So leaving unit
	// 4 active does not lose the font: later bindings land on unit 4 while
	// ImGui keeps reading a stale-but-correct atlas on unit 0. What breaks is
	// the first later draw needing a DIFFERENT texture -- an ImGui::Image, the
	// Image Loader example -- which silently shows the atlas instead. A
	// conditional wrong-texture failure that depends on window draw order is
	// worse than a loud one.
	glActiveTexture(GL_TEXTURE0);
}

GLuint CRenderShaderCustomFragmentOpenGL4::SamplerFor(EShaderChannelFilter filter)
{
	GLuint *slot = (filter == SHADER_CHANNEL_LINEAR) ? &samplerLinear : &samplerNearest;
	if (*slot == 0)
	{
		GLint f = (filter == SHADER_CHANNEL_LINEAR) ? GL_LINEAR : GL_NEAREST;
		glGenSamplers(1, slot);
		glSamplerParameteri(*slot, GL_TEXTURE_MIN_FILTER, f);
		glSamplerParameteri(*slot, GL_TEXTURE_MAG_FILTER, f);
		// CLAMP, always. Wrapping is done by the MT_CHUV macro in the shader,
		// because CSlrImage pads every texture to a power of two and hardware
		// REPEAT would tile the PADDING rather than the image.
		glSamplerParameteri(*slot, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glSamplerParameteri(*slot, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	}
	return *slot;
}

void CRenderShaderCustomFragmentOpenGL4::ResetState()
{
	// UNBIND UNITS 1..4 BEFORE handing ImGui its pipeline back. Nothing else
	// restores them: imgui_impl_opengl3 saves and restores only slot 0's
	// texture and sampler (:625), and other engine shaders use units 1..4 --
	// CRenderShaderMaskedTile is the precedent -- so anything left bound here
	// leaks into whatever draws next.
	for (int i = 0; i < kShaderChannelCount; i++)
	{
		glActiveTexture(GL_TEXTURE1 + i);
		glBindTexture(GL_TEXTURE_2D, 0);
		glBindSampler(i + 1, 0);
	}
	glActiveTexture(GL_TEXTURE0);

	CRenderShaderOpenGL4::ResetState();
}

// --- channels -------------------------------------------------------------
//
// Stores only, for now. Slots 1..4 get bound in the task that gives this
// backend its channel implementation; until then a host can configure
// channels and nothing samples them.

void CRenderShaderCustomFragmentOpenGL4::SetChannelTexture(int channel, void *texture)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelTexture[channel] = texture;
}

void CRenderShaderCustomFragmentOpenGL4::SetChannelSampler(int channel, EShaderChannelFilter filter,
							  EShaderChannelWrap wrap)
{
	if (channel < 0 || channel >= kShaderChannelCount)
		return;
	channelFilter[channel] = filter;
	channelWrapMode[channel] = wrap;
}
