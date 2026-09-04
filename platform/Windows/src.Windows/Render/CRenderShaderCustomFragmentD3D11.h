#ifndef _CRenderShaderCustomFragmentD3D11_h_
#define _CRenderShaderCustomFragmentD3D11_h_

#include "CRenderShader.h"
#include "CRenderShaderCustomFragment.h"

#if defined(MT_RENDER_BACKEND_D3D11)

class CRenderBackendD3D11;

// The D3D11 custom fragment shader: HLSL supplied by the host at runtime.
//
// RUNTIME COMPILATION HERE DOES NOT CONTRADICT THE BYTECODE POLICY.
// tools/embed-hlsl-shaders.ps1 says the engine's shaders are bytecode only, and
// its stated reason is that "a second way to produce a shader is a second thing
// that can disagree with the first" -- a rule about a shader that ALSO has a
// committed .h of bytecode. Text a user types at runtime has none, so there is
// nothing for it to disagree with. The compiler is not a new dependency either:
// imgui_impl_dx11.cpp carries #pragma comment(lib, "d3dcompiler") and calls
// D3DCompile for its own two shaders on every launch. That script's header
// records this exception.
//
// NO DIAMOND HERE, unlike the GL and Metal implementations: the D3D11 shaders
// derive from CRenderShader directly, with no backend base in between, so
// deriving from it and from the seam interface gives only one CRenderShader.
//
// A PIXEL SHADER ONLY, and that is a decision rather than an omission. The draw
// this replaces is one of ImGui's own, so ImGui's vertex shader, input layout
// and b0 vertex constant buffer stay bound; a custom vertex shader would oblige
// its own ID3D11InputLayout built against THAT bytecode, for no gain at all --
// the geometry is ImGui's ImDrawVert stream either way. Shaders/FlatColor.hlsl
// argues the same point at length.
class CRenderShaderCustomFragmentD3D11 : public CRenderShader,
										 public CRenderShaderCustomFragment
{
public:
	CRenderShaderCustomFragmentD3D11(CRenderBackendD3D11 *renderBackend, const char *name);
	virtual ~CRenderShaderCustomFragmentD3D11();

	// --- CRenderShaderCustomFragment ------------------------------------
	virtual bool SetFragmentSource(const char *mainImageSource) override;
	virtual const char *GetCompileErrorLog() override { return lastCompileLog.c_str(); }
	virtual int GetPreambleLineCount() override;
	virtual void SetUniforms(const SShaderToyUniforms &u) override { uniforms = u; }
	virtual bool IsUsable() override { return isCompiled; }

	virtual void SetChannelTexture(int channel, void *texture) override;
	virtual void SetChannelSampler(int channel, EShaderChannelFilter filter,
								   EShaderChannelWrap wrap) override;


	virtual void UseShaderProgram() override;
	virtual void ResetState() override;

	// Called from inside the ImGui draw callback with the live device context,
	// already null-checked. Uploads the uniforms and binds the pixel shader.
	void BindTo(void *deviceContext);

private:

	void *channelTexture[kShaderChannelCount] = {};
	EShaderChannelFilter channelFilter[kShaderChannelCount] = {};
	EShaderChannelWrap channelWrapMode[kShaderChannelCount] = {};
	CRenderBackendD3D11 *renderBackend;
	std::string name;
	std::string fullSource;
	std::string lastCompileLog;

	void *pixelShaderPtr;     // ID3D11PixelShader *, retained
	void *constantBufferPtr;  // ID3D11Buffer *,      retained

	// TWO sampler states, not four. Wrap is done in the shader by MT_CHUV
	// (CSlrImage pads to a power of two, so hardware WRAP would tile the
	// padding), which leaves the filter as the only axis that varies.
	void *samplerNearestPtr = NULL;    // ID3D11SamplerState *,       retained
	void *samplerLinearPtr = NULL;     // ID3D11SamplerState *,       retained
	// 1x1 opaque black, bound wherever the host left a channel empty, so the
	// debug layer never sees a shader sample an unbound SRV.
	void *blackTextureViewPtr = NULL;  // ID3D11ShaderResourceView *, retained

	void *SamplerFor(EShaderChannelFilter filter);
	void *BlackTextureView();

	SShaderToyUniforms uniforms;
};

#endif   // MT_RENDER_BACKEND_D3D11
#endif
