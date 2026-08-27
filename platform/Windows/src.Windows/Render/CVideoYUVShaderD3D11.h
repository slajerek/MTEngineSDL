#ifndef _CVideoYUVShaderD3D11_h_
#define _CVideoYUVShaderD3D11_h_

#include "Video/CVideoYUVConverter.h"

#if defined(MT_RENDER_BACKEND_D3D11)

class CRenderBackendD3D11;

// YUV -> RGB video conversion on Direct3D 11 (S-6 Task A6).
//
// The shader is Shaders/VideoYUV.hlsl, the third transcription of the transfer
// maths after the MSL and the GLSL. READ THAT FILE'S HEADER before changing
// anything here: it records which pipeline state each draw path must bind, and
// two of those are D3D-only hazards the other backends never had to think
// about (the default rasterizer culls back faces; the default depth state
// tests).
//
// NO PIPELINE-STATE OBJECT, AND THAT IS THE BIG SIMPLIFICATION OVER METAL.
// A MTLRenderPipelineState bakes in its colour-attachment format and Metal
// validates it against the pass at DRAW time, so CVideoYUVShaderMetal keeps
// three cached pipelines -- RGBA8, RGBA16F, and the surface's. D3D11 has no
// such object: shaders, blend, rasterizer and depth state are all
// format-agnostic and only the render target itself differs. So there is one
// vertex shader, one pixel shader, one set of states, and nothing to select.
// What DOES carry over is the lesson that motivated the cache: write the
// RGBA16F path and the RGBA8 path through the SAME code and test both.
class CVideoYUVShaderD3D11 : public CVideoYUVConverter
{
public:
	CVideoYUVShaderD3D11(CRenderBackendD3D11 *backend);
	virtual ~CVideoYUVShaderD3D11();

	virtual bool Compile() override;

	virtual void Render(void *texY, void *texU, void *texV, void *texA,
						bool hasAlpha, float alpha,
						int colorSpace, bool fullRange,
						float x, float y, float w, float h,
						float screenW, float screenH,
						const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

	virtual void RenderToTarget(EYUVShaderMode mode,
								void *texY, void *texUVorU, void *texV,
								bool hasAlpha, void *texA,
								int colorSpace, bool fullRange,
								int rotationDegrees,
								void *lutTexture, int lutEdge,
								CRenderTarget &target,
								const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

private:
	void DrawQuad(EYUVShaderMode mode,
				  void *texY, void *texUVorU, void *texV, void *texA,
				  bool hasAlpha, float alpha,
				  int colorSpace, bool fullRange, int rotationDegrees,
				  void *lutTexture, int lutEdge,
				  float ndcX, float ndcY, float ndcW, float ndcH,
				  const SVideoHdrOutput &hdr,
				  bool honourScissor,
				  void *deviceContext);

	CRenderBackendD3D11 *backend;

	void *vertexShaderPtr;    // ID3D11VertexShader *
	void *pixelShaderPtr;     // ID3D11PixelShader *
	void *constantBufferPtr;  // ID3D11Buffer *
	void *samplerPtr;         // ID3D11SamplerState *
	// TWO RASTERIZER STATES, because the two entry points want opposite answers
	// about SCISSORING. Both are CULL_NONE -- see the .cpp for why that is not
	// optional on D3D11.
	void *rasterizerPtr;        // ScissorEnable = FALSE: RenderToTarget
	void *rasterizerScissorPtr; // ScissorEnable = TRUE:  Render, inside ImGui
	void *blendPtr;           // ID3D11BlendState *
	void *depthPtr;           // ID3D11DepthStencilState *, DepthEnable = FALSE

	// 1x1 stand-ins for the texture slots a given mode does not use. D3D11 does
	// not REQUIRE every declared slot to be bound the way Metal does -- an
	// unbound SRV reads as zero -- but binding them keeps the two backends
	// behaving identically rather than merely both working, and it stops the
	// debug layer flooding with DEVICE_DRAW_SHADERRESOURCEVIEW_NOT_SET on every
	// video frame. It also matters for correctness in one case: an unbound texA
	// with hasAlpha set would give alpha 0, i.e. invisible video.
	void *dummy2DPtr;         // ID3D11ShaderResourceView *
	void *dummy3DPtr;         // ID3D11ShaderResourceView *

	bool compiled;
	bool compileFailed;
};

#endif   // MT_RENDER_BACKEND_D3D11
#endif
