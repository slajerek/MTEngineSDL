#ifndef _CRenderShaderFlatColorD3D11_h_
#define _CRenderShaderFlatColorD3D11_h_

#include "CRenderShader.h"

#if defined(MT_RENDER_BACKEND_D3D11)

class CRenderBackendD3D11;

// Fills its quad with one constant colour, ignoring the texture entirely.
//
// The D3D11 twin of CRenderShaderFlatColorMetal and CRenderShaderFlatColorOpenGL4,
// and it exists for the twins' sake: the shader family needs its plumbing --
// compile, pixel-shader creation, ImGui draw-callback dispatch, live-context
// access -- proven independently of any real shader's maths, so that a failure
// in the video converter is a shader bug rather than an infrastructure bug.
//
// NOT OPTIONAL. MT_ShaderProbe maps a NULL CreateFlatColorShader() to
// SHADER_PROBE_UNSUPPORTED, and `render_backend_shader_probe` asserts
// SHADER_PROBE_READY on EVERY backend by design -- so a NULL here makes the
// whole Windows ImGui suite unreachable, not merely thinner. It has to run and
// assert on all three backends for a second reason too: imgui_test_engine has
// NO Skipped status, so a test that returns early on one backend is counted as
// PASSED there, which is a permanent false green in the headline number.
//
// A PIXEL SHADER ONLY. The draw it replaces is one of ImGui's own, so ImGui's
// vertex shader, input layout and b0 projection buffer stay bound; a custom
// vertex shader would oblige its own ID3D11InputLayout built against THAT
// bytecode, for no gain at all. See Shaders/FlatColor.hlsl.
class CRenderShaderFlatColorD3D11 : public CRenderShader
{
public:
	CRenderShaderFlatColorD3D11(CRenderBackendD3D11 *renderBackend, float r, float g, float b, float a);
	virtual ~CRenderShaderFlatColorD3D11();

	virtual void CompileShaders() override;
	virtual void UseShaderProgram() override;
	virtual void ResetState() override;

	// Called from inside the ImGui draw callback with the live device context,
	// already null-checked.
	void BindTo(void *deviceContext);

private:
	CRenderBackendD3D11 *renderBackend;
	void *pixelShaderPtr;    // ID3D11PixelShader *, retained
	void *constantBufferPtr; // ID3D11Buffer *,      retained
	float color[4];
	// Latch. Without it a failed creation is retried every frame, which turns
	// one diagnostic into a scrolling wall of them.
	bool compileFailed;
};

#endif   // MT_RENDER_BACKEND_D3D11
#endif
