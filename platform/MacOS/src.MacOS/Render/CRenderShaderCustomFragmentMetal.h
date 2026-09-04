#ifndef _CRenderShaderCustomFragmentMetal_h_
#define _CRenderShaderCustomFragmentMetal_h_

#include "CRenderShaderMetal.h"
#include "CRenderShaderCustomFragment.h"

// The Metal custom fragment shader: MSL supplied by the host at runtime.
//
// It takes newLibraryWithSource -- the path CRenderShaderMetal calls the
// development one and which nothing shipping had ever exercised. A probe run
// under --render-backend=metal confirmed it works before this was written; see
// the plan's Task 0.
//
// TWO ASYMMETRIES AGAINST THE GLSL AND HLSL SIDES, both forced by MSL:
//
//  1. THE UNIFORM STRUCT SAYS packed_float3. A plain MSL float3 is 16 bytes
//     with 16-byte alignment, so `float3 resolution; float time;` would put
//     time at offset 16 while SShaderToyUniforms puts it at 12 -- shifting
//     every field after it, silently. The engine's own MTShaderToy.metal
//     dodges the same trap by using float2 + float + float.
//
//  2. mainImage TAKES A THIRD PARAMETER. GLSL uniforms and HLSL cbuffer
//     members are both global scope; MSL constant-buffer arguments are
//     function parameters, and a program-scope `constant` needs a constant
//     initializer, so there is no way to make them global. The preamble
//     #defines iResolution and friends onto that parameter, so the BODY still
//     reads the same as the GLSL -- only the signature differs.
//
// The vertex stage goes in the TRAILER, after the host's code, not in the
// preamble: everything before the host's first line shifts its compiler line
// numbers, and GetPreambleLineCount() is what a host subtracts to undo that.
class CRenderShaderCustomFragmentMetal : public CRenderShaderMetal,
										 public CRenderShaderCustomFragment
{
public:
	CRenderShaderCustomFragmentMetal(CRenderBackendMetal *renderBackend, const char *name);
	virtual ~CRenderShaderCustomFragmentMetal();

	// --- CRenderShaderMetal ---------------------------------------------
	virtual const char *GetMetalShaderSource() override;
	// NULL, so LoadLibrary() takes the source branch. This shader has no
	// embedded .metallib and cannot have one: its source is typed at runtime.
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength) override;
	virtual void SetShaderVars(void *encoder) override;

	// --- CRenderShaderCustomFragment ------------------------------------
	virtual bool SetFragmentSource(const char *mainImageSource) override;
	virtual const char *GetCompileErrorLog() override { return lastCompileLog.c_str(); }
	virtual int GetPreambleLineCount() override;
	virtual void SetUniforms(const SShaderToyUniforms &u) override { uniforms = u; }
	virtual bool IsUsable() override { return isCompiled; }

	virtual void SetChannelTexture(int channel, void *texture) override;
	virtual void SetChannelSampler(int channel, EShaderChannelFilter filter,
								   EShaderChannelWrap wrap) override;


	virtual void UseShaderProgram() override { CRenderShaderMetal::UseShaderProgram(); }
	virtual void ResetState() override;

private:

	void *channelTexture[kShaderChannelCount] = {};
	EShaderChannelFilter channelFilter[kShaderChannelCount] = {};
	EShaderChannelWrap channelWrapMode[kShaderChannelCount] = {};
	std::string fullSource;
	SShaderToyUniforms uniforms = {};

	// TWO sampler states, not four. Wrap is done in the shader by MT_CHUV
	// (CSlrImage pads to a power of two, so hardware repeat would tile the
	// padding), which leaves the filter as the only axis that varies.
	void *samplerNearestPtr = NULL;   // id<MTLSamplerState>, retained
	void *samplerLinearPtr = NULL;    // id<MTLSamplerState>, retained
	// 1x1 opaque black, bound wherever the host left a channel empty. Metal's
	// validation layer objects to a shader sampling a texture slot that was
	// never set, and an unset channel should read black in any case.
	void *blackTexturePtr = NULL;     // id<MTLTexture>, retained

	void *SamplerFor(EShaderChannelFilter filter);
	void *BlackTexture();
};

#endif
