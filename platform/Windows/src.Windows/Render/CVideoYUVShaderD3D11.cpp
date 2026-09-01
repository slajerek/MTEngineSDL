#include "CVideoYUVShaderD3D11.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "CRenderBackendD3D11.h"
#include "Core/Render/CRenderTarget.h"
#include "DBG_Log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <cstring>

#include "Generated/VideoYuvVSBytecode.h"
#include "Generated/VideoYuvPSBytecode.h"

// MUST MIRROR Shaders/VideoYUV.hlsl's cbuffer FIELD FOR FIELD AND IN ORDER.
//
// This is a raw byte copy into the constant buffer, so a mismatch does NOT fail
// to compile -- it silently misreads every field after the first one that moved,
// and the symptom is a colour or a geometry that is wrong in a way nobody can
// attribute. The MSL twin carries the identical warning about its own struct.
//
// 80 bytes. HLSL packs into 16-byte registers and forbids a vector straddling
// one: float4 at 0, then FOURTEEN scalars (16..71), then a float2 at 72 which
// sits wholly inside register 4. A constant buffer's ByteWidth must be a
// multiple of 16, and 80 is.
struct SYuvConstants
{
	float transform[4];      //  0..15
	int   rotation;          // 16
	int   mode;              // 20
	int   colorSpace;        // 24
	int   fullRange;         // 28
	int   hasAlpha;          // 32
	int   useLut;            // 36
	float alpha;             // 40
	float lutScale;          // 44
	float lutOffset;         // 48
	int   colorTrc;          // 52
	int   floatTarget;       // 56
	int   surfaceLinear;     // 60
	int   surfaceP3;         // 64
	float toneMapHeadroom;   // 68
	float pad[2];            // 72..79
};
static_assert(sizeof(SYuvConstants) == 80, "SYuvConstants must match VideoYUV.hlsl's cbuffer exactly");

CVideoYUVShaderD3D11::CVideoYUVShaderD3D11(CRenderBackendD3D11 *backend)
: backend(backend), vertexShaderPtr(NULL), pixelShaderPtr(NULL), constantBufferPtr(NULL),
  samplerPtr(NULL), rasterizerPtr(NULL), rasterizerScissorPtr(NULL), blendPtr(NULL), depthPtr(NULL),
  dummy2DPtr(NULL), dummy3DPtr(NULL), compiled(false), compileFailed(false)
{
}

CVideoYUVShaderD3D11::~CVideoYUVShaderD3D11()
{
	if (dummy3DPtr)        ((ID3D11ShaderResourceView *)dummy3DPtr)->Release();
	if (dummy2DPtr)        ((ID3D11ShaderResourceView *)dummy2DPtr)->Release();
	if (depthPtr)          ((ID3D11DepthStencilState *)depthPtr)->Release();
	if (blendPtr)          ((ID3D11BlendState *)blendPtr)->Release();
	if (rasterizerScissorPtr) ((ID3D11RasterizerState *)rasterizerScissorPtr)->Release();
	if (rasterizerPtr)     ((ID3D11RasterizerState *)rasterizerPtr)->Release();
	if (samplerPtr)        ((ID3D11SamplerState *)samplerPtr)->Release();
	if (constantBufferPtr) ((ID3D11Buffer *)constantBufferPtr)->Release();
	if (pixelShaderPtr)    ((ID3D11PixelShader *)pixelShaderPtr)->Release();
	if (vertexShaderPtr)   ((ID3D11VertexShader *)vertexShaderPtr)->Release();
}

bool CVideoYUVShaderD3D11::Compile()
{
	// LATCHED, both ways. Without it a failed creation is retried on every
	// frame the converter is used, leaking a shader and a buffer each time --
	// the same trap CRenderShaderOpenGL4::UseShaderProgram has with isCompiled.
	if (compiled)      return true;
	if (compileFailed) return false;

	ID3D11Device *device = (ID3D11Device *)(backend ? backend->GetD3DDevice() : NULL);
	if (device == NULL) { compileFailed = true; return false; }

	if (kVideoYuvVSBytecodeLength == 0 || kVideoYuvPSBytecodeLength == 0)
	{
		LOGError("CVideoYUVShaderD3D11: the embedded VideoYUV bytecode is a PLACEHOLDER (length 0). "
				 "Run tools/embed-hlsl-shaders.ps1 with fxc.exe on PATH and commit the generated headers.");
		compileFailed = true;
		return false;
	}

	ID3D11VertexShader *vs = NULL;
	ID3D11PixelShader  *ps = NULL;
	if (FAILED(device->CreateVertexShader(kVideoYuvVSBytecodeData, (SIZE_T)kVideoYuvVSBytecodeLength, NULL, &vs)) ||
		FAILED(device->CreatePixelShader (kVideoYuvPSBytecodeData, (SIZE_T)kVideoYuvPSBytecodeLength, NULL, &ps)))
	{
		LOGError("CVideoYUVShaderD3D11: shader creation failed");
		if (vs) vs->Release();
		if (ps) ps->Release();
		compileFailed = true;
		return false;
	}
	vertexShaderPtr = vs;
	pixelShaderPtr = ps;

	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(SYuvConstants);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
	ID3D11Buffer *cb = NULL;
	if (FAILED(device->CreateBuffer(&bd, NULL, &cb))) { compileFailed = true; return false; }
	constantBufferPtr = cb;

	// LINEAR with CLAMP addressing, and MaxLOD unclamped. The chroma planes are
	// half resolution and MUST be interpolated, and the CM-E 3D LUT is unusable
	// with point filtering. Explicitly NOT imgui_impl_dx11's samplers: BOTH of
	// those set MinLOD = MaxLOD = 0.
	D3D11_SAMPLER_DESC sd = {};
	sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
	sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
	sd.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
	sd.MinLOD = 0.0f;
	sd.MaxLOD = D3D11_FLOAT32_MAX;
	ID3D11SamplerState *samp = NULL;
	if (FAILED(device->CreateSamplerState(&sd, &samp))) { compileFailed = true; return false; }
	samplerPtr = samp;

	// CULL_NONE, EXPLICITLY, AND THIS IS THE D3D-ONLY HAZARD.
	//
	// D3D11's default rasterizer state culls BACK faces
	// (CullMode = D3D11_CULL_BACK, FrontCounterClockwise = FALSE). Metal's
	// default is MTLCullModeNone and GL's GL_CULL_FACE is off, so NEITHER
	// sibling ever had to think about winding -- and this quad's facing depends
	// on the SIGN of the transform's height, which the two entry points set
	// OPPOSITELY: Render() passes a pixel rect (h > 0) and RenderToTarget()
	// passes (-1, 1, 2, -2). One of the two would therefore be culled away
	// under the default state, with no error and no debug-layer message, on one
	// path only. Binding CULL_NONE removes the question entirely.
	// AND TWO OF THEM, because the two entry points want OPPOSITE answers about
	// SCISSORING -- one state cannot serve both.
	//
	// RenderToTarget() draws a full-target quad into its own pass and must
	// ignore whatever scissor rect was last set. Render() draws INSIDE ImGui's
	// draw-list walk and must clip.
	//
	// BE PRECISE ABOUT WHY, because the obvious explanation is wrong:
	// imgui_impl_dx11 does NOT apply a USER-CALLBACK command's ClipRect. Its
	// walk calls RSSetScissorRects only on the else-branch, the one that issues
	// DrawIndexed -- so the rect in force inside our callback is whatever the
	// last DRAWN command left, which is usually the host window's own
	// background quad and occasionally nothing at all. GL inherits exactly the
	// same stale rect (its ImGui backend has the identical structure and leaves
	// GL_SCISSOR_TEST enabled), and Metal inherits the encoder's. So D3D11
	// matching them means scissoring ON with an inherited rect -- and
	// Render() sets the rect ITSELF below rather than trusting the inheritance,
	// which is the only version of this that survives a video drawn into a
	// window with no preceding drawn command.
	D3D11_RASTERIZER_DESC rd = {};
	rd.FillMode = D3D11_FILL_SOLID;
	rd.CullMode = D3D11_CULL_NONE;
	rd.DepthClipEnable = TRUE;
	rd.ScissorEnable = FALSE;
	ID3D11RasterizerState *rs = NULL;
	if (FAILED(device->CreateRasterizerState(&rd, &rs))) { compileFailed = true; return false; }
	rasterizerPtr = rs;

	rd.ScissorEnable = TRUE;
	ID3D11RasterizerState *rsScissor = NULL;
	if (FAILED(device->CreateRasterizerState(&rd, &rsScissor))) { compileFailed = true; return false; }
	rasterizerScissorPtr = rsScissor;

	// Straight alpha blending, matching the Metal pipeline's descriptor.
	D3D11_BLEND_DESC bl = {};
	bl.RenderTarget[0].BlendEnable = TRUE;
	bl.RenderTarget[0].SrcBlend  = D3D11_BLEND_SRC_ALPHA;
	bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
	bl.RenderTarget[0].BlendOp   = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].SrcBlendAlpha  = D3D11_BLEND_ONE;
	bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
	bl.RenderTarget[0].BlendOpAlpha   = D3D11_BLEND_OP_ADD;
	bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
	ID3D11BlendState *blend = NULL;
	if (FAILED(device->CreateBlendState(&bl, &blend))) { compileFailed = true; return false; }
	blendPtr = blend;

	// DEPTH OFF, EXPLICITLY. The vertex shader writes z = 0 and D3D11's default
	// depth-stencil state is DepthEnable = TRUE with COMPARISON_LESS. Harmless
	// while no DSV is bound -- but imgui_impl_dx11 creates its own
	// DepthEnable = FALSE state and RESTORES the previous one, so "nothing is
	// bound" is a property of the current backend, not a guarantee.
	//
	// AND EVERY FIELD HOLDS A VALID ENUMERANT even though both Enables are
	// FALSE: D3D11 range-checks the whole descriptor and zero is out of range
	// for D3D11_COMPARISON_FUNC and D3D11_STENCIL_OP, so a `= {}` desc is
	// REJECTED with E_INVALIDARG -- which here would latch compileFailed and
	// make video silently never draw. imgui_impl_dx11.cpp populates the same
	// six fields for the same reason.
	D3D11_DEPTH_STENCIL_DESC dd = {};
	dd.DepthEnable = FALSE;
	dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
	dd.DepthFunc = D3D11_COMPARISON_ALWAYS;
	dd.StencilEnable = FALSE;
	dd.StencilReadMask  = D3D11_DEFAULT_STENCIL_READ_MASK;
	dd.StencilWriteMask = D3D11_DEFAULT_STENCIL_WRITE_MASK;
	dd.FrontFace.StencilFailOp = dd.FrontFace.StencilDepthFailOp =
		dd.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
	dd.FrontFace.StencilFunc = D3D11_COMPARISON_ALWAYS;
	dd.BackFace = dd.FrontFace;
	ID3D11DepthStencilState *depth = NULL;
	if (FAILED(device->CreateDepthStencilState(&dd, &depth))) { compileFailed = true; return false; }
	depthPtr = depth;

	// The 1x1 stand-ins. See the header for why they exist on a backend that
	// does not require them.
	{
		const unsigned char zero = 0;
		D3D11_TEXTURE2D_DESC td = {};
		td.Width = td.Height = 1;
		td.MipLevels = td.ArraySize = 1;
		td.Format = DXGI_FORMAT_R8_UNORM;
		td.SampleDesc.Count = 1;
		td.Usage = D3D11_USAGE_IMMUTABLE;
		td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA init = {};
		init.pSysMem = &zero;
		init.SysMemPitch = 1;
		ID3D11Texture2D *t = NULL;
		if (SUCCEEDED(device->CreateTexture2D(&td, &init, &t)) && t != NULL)
		{
			ID3D11ShaderResourceView *srv = NULL;
			if (SUCCEEDED(device->CreateShaderResourceView(t, NULL, &srv)))
				dummy2DPtr = srv;
			t->Release();
		}

		const unsigned short zeros[4] = { 0, 0, 0, 0 };
		D3D11_TEXTURE3D_DESC t3 = {};
		t3.Width = t3.Height = t3.Depth = 1;
		t3.MipLevels = 1;
		t3.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
		t3.Usage = D3D11_USAGE_IMMUTABLE;
		t3.BindFlags = D3D11_BIND_SHADER_RESOURCE;
		D3D11_SUBRESOURCE_DATA i3 = {};
		i3.pSysMem = zeros;
		i3.SysMemPitch = 8;
		i3.SysMemSlicePitch = 8;
		ID3D11Texture3D *t3d = NULL;
		if (SUCCEEDED(device->CreateTexture3D(&t3, &i3, &t3d)) && t3d != NULL)
		{
			ID3D11ShaderResourceView *srv = NULL;
			if (SUCCEEDED(device->CreateShaderResourceView(t3d, NULL, &srv)))
				dummy3DPtr = srv;
			t3d->Release();
		}
	}

	compiled = true;
	return true;
}

// ONE DRAW PATH FOR BOTH ENTRY POINTS. The only thing that differs between
// Render() and RenderToTarget() is which target is bound and what the transform
// is -- so everything else lives here, and the two cannot drift the way the
// Metal backend's two nearly-identical bodies could.
void CVideoYUVShaderD3D11::DrawQuad(EYUVShaderMode mode,
									void *texY, void *texUVorU, void *texV, void *texA,
									bool hasAlpha, float alpha,
									int colorSpace, bool fullRange, int rotationDegrees,
									void *lutTexture, int lutEdge,
									float ndcX, float ndcY, float ndcW, float ndcH,
									const SVideoHdrOutput &hdr,
									bool honourScissor,
									void *deviceContext)
{
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)deviceContext;
	if (ctx == NULL)
		return;

	SYuvConstants u = {};
	u.transform[0] = ndcX;
	u.transform[1] = ndcY;
	u.transform[2] = ndcW;
	u.transform[3] = ndcH;
	u.rotation   = rotationDegrees;
	u.mode       = (int)mode;
	u.colorSpace = colorSpace;
	u.fullRange  = fullRange ? 1 : 0;
	u.hasAlpha   = hasAlpha ? 1 : 0;
	u.useLut     = (lutTexture != NULL && lutEdge >= 2) ? 1 : 0;
	u.alpha      = alpha;
	if (u.useLut)
	{
		// Half-texel correction so the lattice endpoints land on texel centres
		// -- identical to the GL and Metal paths.
		u.lutScale  = (float)(lutEdge - 1) / (float)lutEdge;
		u.lutOffset = 1.0f / (2.0f * (float)lutEdge);
	}
	// EVERY HDR UNIFORM SET FROM THE CALLER'S VALUE, which is a
	// default-constructed SVideoHdrOutput unless a caller deliberately opts in.
	// That is how the "no leaked PQ EOTF into another app's cutscene" guarantee
	// is implemented rather than merely intended: GL uniforms are program state
	// that survives between draws, and this entry point is shared with
	// the game app's cutscenes. A D3D constant buffer is rewritten whole each
	// draw so it cannot leak the same way -- but the two backends must agree on
	// BEHAVIOUR, or the divergence is the bug.
	u.colorTrc        = hdr.IsHdr() ? hdr.colorTrc : 2;
	u.floatTarget     = hdr.floatTarget ? 1 : 0;
	u.surfaceLinear   = hdr.surfaceIsLinear ? 1 : 0;
	u.surfaceP3       = hdr.surfaceIsP3 ? 1 : 0;
	u.toneMapHeadroom = (hdr.toneMapHeadroom >= 1.0f) ? hdr.toneMapHeadroom : 1.0f;

	ID3D11Buffer *cb = (ID3D11Buffer *)constantBufferPtr;
	D3D11_MAPPED_SUBRESOURCE mapped = {};
	if (SUCCEEDED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
	{
		memcpy(mapped.pData, &u, sizeof(u));
		ctx->Unmap(cb, 0);
	}

	ID3D11ShaderResourceView *dummy2D = (ID3D11ShaderResourceView *)dummy2DPtr;
	ID3D11ShaderResourceView *dummy3D = (ID3D11ShaderResourceView *)dummy3DPtr;
	ID3D11ShaderResourceView *srvs[5] = {
		texY       ? (ID3D11ShaderResourceView *)texY       : dummy2D,
		texUVorU   ? (ID3D11ShaderResourceView *)texUVorU   : dummy2D,
		texV       ? (ID3D11ShaderResourceView *)texV       : dummy2D,
		texA       ? (ID3D11ShaderResourceView *)texA       : dummy2D,
		lutTexture ? (ID3D11ShaderResourceView *)lutTexture : dummy3D,
	};

	ID3D11SamplerState *samp = (ID3D11SamplerState *)samplerPtr;
	ctx->IASetInputLayout(NULL);      // positions come from SV_VertexID
	ctx->IASetVertexBuffers(0, 0, NULL, NULL, NULL);
	ctx->IASetIndexBuffer(NULL, DXGI_FORMAT_UNKNOWN, 0);
	ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
	ctx->VSSetShader((ID3D11VertexShader *)vertexShaderPtr, NULL, 0);
	ctx->PSSetShader((ID3D11PixelShader *)pixelShaderPtr, NULL, 0);
	ctx->GSSetShader(NULL, NULL, 0);
	// b0 ON BOTH STAGES: the vertex shader reads uTransform and uRotation from
	// the same cbuffer the pixel shader reads everything else from, exactly as
	// the MSL twin binds one uniform struct to both.
	ctx->VSSetConstantBuffers(0, 1, &cb);
	ctx->PSSetConstantBuffers(0, 1, &cb);
	ctx->PSSetShaderResources(0, 5, srvs);
	ctx->PSSetSamplers(0, 1, &samp);
	ctx->RSSetState((ID3D11RasterizerState *)(honourScissor ? rasterizerScissorPtr : rasterizerPtr));
	const float blendFactor[4] = { 0, 0, 0, 0 };
	ctx->OMSetBlendState((ID3D11BlendState *)blendPtr, blendFactor, 0xFFFFFFFF);
	ctx->OMSetDepthStencilState((ID3D11DepthStencilState *)depthPtr, 0);

	ctx->Draw(4, 0);

	// UNBIND THE FIVE SRVs. Every one of them is about to be written again --
	// a plane texture is Map/DISCARDed next frame, and a CRenderTarget's
	// texture becomes a render target -- and D3D11 refuses to bind a resource
	// as input and output at once, silently dropping the RTV binding.
	ID3D11ShaderResourceView *none[5] = { NULL, NULL, NULL, NULL, NULL };
	ctx->PSSetShaderResources(0, 5, none);
}

void CVideoYUVShaderD3D11::RenderToTarget(EYUVShaderMode mode,
										  void *texY, void *texUVorU, void *texV,
										  bool hasAlpha, void *texA,
										  int colorSpace, bool fullRange,
										  int rotationDegrees,
										  void *lutTexture, int lutEdge,
										  CRenderTarget &target,
										  const SVideoHdrOutput &hdr)
{
	if (!Compile())
		return;
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)(backend ? backend->GetD3DDeviceContext() : NULL);
	if (ctx == NULL)
		return;

	// BeginPass() binds the target and its viewport; EndPass() puts the FRAME's
	// target back, which matters because this runs MID-FRAME on the render
	// thread and D3D11 has one immediate context.
	if (target.BeginPass() == NULL)
		return;

	// Full-target quad in NDC: the offscreen target is exactly the display
	// size, so the transform maps the [0,1] quad onto [-1,1].
	//
	// METAL'S TRANSFORM, (-1, -1, 2, 2), NOT GL'S (-1, 1, 2, -2). This is the
	// same trap VideoYUV.hlsl's header warns about one level up, and the first
	// draft of this file fell straight into it. GL's NEGATIVE height exists
	// ONLY because a GL FBO is bottom-up; a D3D11 render target is top-down
	// like a Metal texture, and the vertex shader here is a byte-for-byte
	// transcription of Metal's. Taking GL's height as well as GL's uv flip is
	// the SECOND inversion, and the result is not a culled quad -- it is every
	// offscreen product (video posters, the t=0 still, any offscreen composite)
	// rendered VERTICALLY MIRRORED, while live playback through Render() stays
	// correct. So it reads as a poster bug rather than a shader bug.
	DrawQuad(mode, texY, texUVorU, texV, texA, hasAlpha, 1.0f,
			 colorSpace, fullRange, rotationDegrees, lutTexture, lutEdge,
			 -1.0f, -1.0f, 2.0f, 2.0f, hdr,
			 false,        // our own full-target pass: ignore any stale scissor
			 ctx);

	target.EndPass();
}

void CVideoYUVShaderD3D11::Render(void *texY, void *texU, void *texV, void *texA,
								  bool hasAlpha, float alpha,
								  int colorSpace, bool fullRange,
								  float x, float y, float w, float h,
								  float screenW, float screenH,
								  const SVideoHdrOutput &hdr)
{
	// DIRECT-TO-SCREEN, and this entry point HAS a live caller:
	// CGuiViewVideoPlayer draws through it from an ImGui draw callback, despite
	// an early draft of the S-4 plan twice recording it as dead -- the claim
	// came from grepping for `yuvShader->Render(`, while the real call goes
	// through a callback-data struct. A pattern-matched grep is not a proof of
	// deadness.
	//
	// It draws into whatever target the FRAME has bound; unlike RenderToTarget
	// there is no pass of our own to begin.
	if (!Compile())
		return;
	if (screenW <= 0.0f || screenH <= 0.0f)
		return;

	// THE LIVE CONTEXT COMES FROM ImGui, not from the backend, because this
	// runs INSIDE ImGui_ImplDX11_RenderDrawData's walk of the draw lists.
	// Renderer_RenderState is published for exactly that duration and is NULL
	// outside it -- a callback reached on a frame NewFrame() aborted must do
	// nothing rather than draw into a stale target.
	ImGui_ImplDX11_RenderState *rs =
		(ImGui_ImplDX11_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
	if (rs == NULL || rs->DeviceContext == NULL)
		return;

	// Pixel rect -> NDC, matching the GL path's uTransform convention.
	const float ndcX = (x / screenW) * 2.0f - 1.0f;
	const float ndcY = 1.0f - ((y + h) / screenH) * 2.0f;
	const float ndcW = (w / screenW) * 2.0f;
	const float ndcH = (h / screenH) * 2.0f;

	// SET OUR OWN SCISSOR RECT. See the note in Compile(): a user-callback
	// command's ClipRect is never applied by imgui_impl_dx11, so inheriting is
	// inheriting whatever the last DRAWN command happened to leave.
	{
		const D3D11_RECT clip = { (LONG)x, (LONG)y, (LONG)(x + w), (LONG)(y + h) };
		rs->DeviceContext->RSSetScissorRects(1, &clip);
	}

	DrawQuad(EYUVShaderMode::YUV420_3Plane, texY, texU, texV, texA,
			 hasAlpha, alpha, colorSpace, fullRange,
			 0,            // Render() has always been rotation 0
			 NULL, 0,      // and has never used the LUT
			 ndcX, ndcY, ndcW, ndcH, hdr,
			 true,         // inside ImGui's walk: HONOUR this command's clip rect
			 rs->DeviceContext);

	// THE CALLER OWNS THE RESET, and it must, because this function CANNOT do
	// it itself.
	//
	// We changed the input layout, both shaders, the primitive topology, the
	// vertex buffers, both constant buffers, the shader resources, the sampler
	// and all three fixed-function states -- and imgui_impl_dx11 restores NONE
	// of that around a user callback. Its BACKUP_DX11_STATE is taken once
	// before the whole draw walk and put back once after it; inside the walk a
	// user callback gets no save and no restore, only a SetupRenderState call
	// when the callback IS the DrawCallback_ResetRenderState identifier. So
	// everything in that list survives until something puts ImGui's pipeline
	// back before the next command. (This comment used to say "only SOME",
	// which understated the hazard.)
	//
	// It cannot be done from here. ImGui_ImplDX11_DrawCallback_ResetRenderState
	// has an INTENTIONALLY EMPTY BODY -- it is an IDENTIFIER that
	// ImGui_ImplDX11_RenderDrawData compares each command's UserCallback
	// against, and only then calls its own SetupRenderState. Calling it
	// directly does nothing at all, and we are inside that walk so we cannot
	// append a command to it either.
	//
	// The live caller already does the right thing: the game app's
	// CViewCutscene queues ImDrawCallback_ResetRenderState immediately after
	// the callback that reaches us. That obsolete sentinel still works --
	// ImDrawList::AddCallback translates it to
	// PlatformIO.DrawCallback_ResetRenderState -- so no app change is needed
	// for D3D11. A NEW caller that forgets it will corrupt every ImGui draw
	// after the video, which is why this is written down here rather than
	// assumed.
}

#endif   // MT_RENDER_BACKEND_D3D11
