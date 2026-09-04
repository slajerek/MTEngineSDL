#ifndef _CRenderShaderCustomFragmentOpenGL4_h_
#define _CRenderShaderCustomFragmentOpenGL4_h_

#include "CRenderShaderOpenGL4.h"
#include "CRenderShaderCustomFragment.h"

// The OpenGL4 custom fragment shader: GLSL supplied by the host at runtime.
//
// The host writes ONLY mainImage(), exactly as shadertoy.com does. This class
// prepends a preamble declaring the uniforms and appends a main() that turns
// ImGui's top-left UV into ShaderToy's bottom-left pixel coordinate.
//
// TWO BASES, and see CRenderShaderCustomFragment.h for why the interface has no
// base of its own: CRenderShaderOpenGL4 brings the GL plumbing (compile, the
// ImGui draw callback, uniform lookup) and CRenderShaderCustomFragment is the
// backend-neutral contract a host holds. A single override of UseShaderProgram()
// and ResetState() satisfies both.
class CRenderShaderCustomFragmentOpenGL4 : public CRenderShaderOpenGL4,
										   public CRenderShaderCustomFragment
{
public:
	CRenderShaderCustomFragmentOpenGL4(CRenderBackendOpenGL4 *renderBackend, const char *name);
	virtual ~CRenderShaderCustomFragmentOpenGL4();

	// --- CRenderShaderOpenGL4 -------------------------------------------
	virtual const char *GetFragmentShaderSource() override;
	virtual void GetUniformsLocations() override;
	virtual void SetShaderVars() override;

	// --- CRenderShaderCustomFragment ------------------------------------
	virtual bool SetFragmentSource(const char *mainImageSource) override;
	virtual const char *GetCompileErrorLog() override { return lastCompileLog.c_str(); }
	virtual int GetPreambleLineCount() override;
	virtual void SetUniforms(const SShaderToyUniforms &u) override { uniforms = u; }
	virtual bool IsUsable() override { return isCompiled; }

	// Task 1 stubs: stores only, binding arrives with the per-backend work.
	virtual void SetChannelTexture(int channel, void *texture) override;
	virtual void SetChannelSampler(int channel, EShaderChannelFilter filter,
								   EShaderChannelWrap wrap) override;


	// ONE override serves both bases -- the interface re-declares these so a
	// host can draw through the interface pointer alone.
	virtual void UseShaderProgram() override { CRenderShaderOpenGL4::UseShaderProgram(); }
	virtual void ResetState() override;

private:

	void *channelTexture[kShaderChannelCount] = {};
	EShaderChannelFilter channelFilter[kShaderChannelCount] = {};
	EShaderChannelWrap channelWrapMode[kShaderChannelCount] = {};
	// preamble + the host's mainImage + entry point. Held because the base
	// asks for the source by pointer during CompileShaders().
	std::string fullSource;

	SShaderToyUniforms uniforms = {};

	GLint locResolution = -1;
	GLint locTime = -1;
	GLint locTimeDelta = -1;
	GLint locFrameRate = -1;
	GLint locFrame = -1;
	GLint locMouse = -1;
	GLint locDate = -1;
	GLint locChannelTime = -1;
	GLint locSampleRate = -1;
	GLint locChannelUvTransform = -1;
	GLint locChannelWrap = -1;
	// iChannelResolution is uploaded ELEMENT BY ELEMENT -- see SetShaderVars.
	GLint locChannelResolution[kShaderChannelCount] = { -1, -1, -1, -1 };
	GLint locChannelSampler[kShaderChannelCount] = { -1, -1, -1, -1 };

	// Two sampler objects, nearest and linear, both CLAMP. Created once and
	// reused. Wrap is done in the shader, so no sampler ever repeats.
	GLuint samplerNearest = 0;
	GLuint samplerLinear = 0;
	GLuint SamplerFor(EShaderChannelFilter filter);
};

#endif
