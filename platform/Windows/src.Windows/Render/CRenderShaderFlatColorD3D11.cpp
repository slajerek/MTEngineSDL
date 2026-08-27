#include "CRenderShaderFlatColorD3D11.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "CRenderBackendD3D11.h"
#include "DBG_Log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <cstring>

#include "Generated/FlatColorPSBytecode.h"

CRenderShaderFlatColorD3D11::CRenderShaderFlatColorD3D11(CRenderBackendD3D11 *renderBackend,
														 float r, float g, float b, float a)
: renderBackend(renderBackend), pixelShaderPtr(NULL), constantBufferPtr(NULL), compileFailed(false)
{
	color[0] = r;
	color[1] = g;
	color[2] = b;
	color[3] = a;
	isCompiled = false;
}

CRenderShaderFlatColorD3D11::~CRenderShaderFlatColorD3D11()
{
	if (constantBufferPtr != NULL) { ((ID3D11Buffer *)constantBufferPtr)->Release(); constantBufferPtr = NULL; }
	if (pixelShaderPtr != NULL)    { ((ID3D11PixelShader *)pixelShaderPtr)->Release(); pixelShaderPtr = NULL; }
}

void CRenderShaderFlatColorD3D11::CompileShaders()
{
	if (isCompiled || compileFailed)
		return;

	ID3D11Device *device = (ID3D11Device *)(renderBackend ? renderBackend->GetD3DDevice() : NULL);
	if (device == NULL)
	{
		LOGError("CRenderShaderFlatColorD3D11: no D3D device yet");
		compileFailed = true;
		return;
	}

	// A PLACEHOLDER BYTECODE HEADER CARRIES LENGTH 0. Refuse loudly rather than
	// creating a shader from nothing: the .hlsl was authored on a machine with
	// no HLSL compiler, and until a Windows build regenerates the headers there
	// is nothing to create. Reported as a compile failure so
	// MT_ShaderProbe surfaces SHADER_PROBE_COMPILE_FAILED, which is
	// distinguishable from "this backend has no such shader".
	if (kFlatColorPSBytecodeLength == 0)
	{
		LOGError("CRenderShaderFlatColorD3D11: the embedded FlatColor bytecode is a PLACEHOLDER "
				 "(length 0). Run tools/embed-hlsl-shaders.ps1 with fxc.exe on PATH and commit "
				 "the generated headers.");
		compileFailed = true;
		return;
	}

	ID3D11PixelShader *ps = NULL;
	HRESULT hr = device->CreatePixelShader(kFlatColorPSBytecodeData,
										   (SIZE_T)kFlatColorPSBytecodeLength, NULL, &ps);
	if (FAILED(hr))
	{
		LOGError("CRenderShaderFlatColorD3D11: CreatePixelShader failed (hr=0x%08x)", (unsigned)hr);
		compileFailed = true;
		return;
	}

	// The colour is IMMUTABLE for the life of the shader -- the factory takes
	// it by value and nothing changes it -- so the buffer is created
	// IMMUTABLE too, with its contents supplied up front. Nothing to map, and
	// nothing that can be left stale.
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(color);        // 16 bytes, the constant-buffer granularity
	bd.Usage = D3D11_USAGE_IMMUTABLE;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	D3D11_SUBRESOURCE_DATA init = {};
	init.pSysMem = color;

	ID3D11Buffer *cb = NULL;
	hr = device->CreateBuffer(&bd, &init, &cb);
	if (FAILED(hr))
	{
		LOGError("CRenderShaderFlatColorD3D11: constant buffer creation failed (hr=0x%08x)", (unsigned)hr);
		ps->Release();
		compileFailed = true;
		return;
	}

	pixelShaderPtr = ps;
	constantBufferPtr = cb;
	isCompiled = true;
}

void CRenderShaderFlatColorD3D11::BindTo(void *deviceContext)
{
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)deviceContext;
	if (ctx == NULL || pixelShaderPtr == NULL || constantBufferPtr == NULL)
		return;
	ID3D11Buffer *cb = (ID3D11Buffer *)constantBufferPtr;
	ctx->PSSetShader((ID3D11PixelShader *)pixelShaderPtr, NULL, 0);
	// b0 OF THE PIXEL STAGE. ImGui's b0 is on the VERTEX stage (its projection
	// matrix) and its own pixel shader reads no constant buffer at all, so the
	// two do not collide -- PSSetConstantBuffers and VSSetConstantBuffers are
	// separate binding tables.
	ctx->PSSetConstantBuffers(0, 1, &cb);
}

void CRenderShaderFlatColorD3D11::UseShaderProgram()
{
	if (!isCompiled)
		return;

	ImGui::GetWindowDrawList()->AddCallback([](const ImDrawList *, const ImDrawCmd *cmd)
	{
		CRenderShaderFlatColorD3D11 *shader = (CRenderShaderFlatColorD3D11 *)cmd->UserCallbackData;
		if (shader == NULL)
			return;

		// FETCH THE CONTEXT FRESHLY AND TOLERATE NULL. The callback runs at
		// RENDER time, long after the blit that queued it, and
		// Renderer_RenderState is published only for the duration of
		// ImGui_ImplDX11_RenderDrawData -- so a callback reached outside that
		// walk (a frame NewFrame() aborted, whose draw lists are still walked)
		// must do nothing rather than dereference.
		ImGui_ImplDX11_RenderState *rs =
			(ImGui_ImplDX11_RenderState *)ImGui::GetPlatformIO().Renderer_RenderState;
		if (rs == NULL)
			return;
		shader->BindTo(rs->DeviceContext);
	}, (void *)this);
}

void CRenderShaderFlatColorD3D11::ResetState()
{
	// GIVE ImGui ITS PIXEL SHADER BACK. Be precise about what imgui_impl_dx11
	// restores around a user callback, because the answer is NOTHING. Its
	// BACKUP_DX11_STATE is captured ONCE before the whole draw walk and
	// restored ONCE after it (imgui_impl_dx11.cpp, ImGui_ImplDX11_RenderDrawData);
	// inside the walk, a user callback gets no save and no restore at all --
	// the only thing the walk does specially is call SetupRenderState when the
	// callback IS the DrawCallback_ResetRenderState identifier. So every piece
	// of state we touch here survives until this reset runs or the walk ends.
	// This reset is not belt-and-braces over a partial restore; it is the ONLY
	// thing putting ImGui's pipeline back. Do not delete it.
	//
	// And the PIXEL constant buffer is worse than the rest: BindTo() binds one
	// with PSSetConstantBuffers, and BACKUP_DX11_STATE has no PSConstantBuffer
	// field at all (it backs up the VERTEX one only), nor does
	// ImGui_ImplDX11_SetupRenderState ever set a pixel cbuffer. So ours
	// outlives even the end of the draw walk. Harmless only because ImGui's
	// own pixel shader reads no cbuffer -- which is luck, not design.
	//
	// (An earlier version of this comment had the PS-constant-buffer fact
	// right and the FRAMING wrong: it said imgui_impl_dx11 restores everything
	// else "around a user callback", when the save and the restore bracket the
	// whole walk and a callback gets neither. Both halves are stated above.)
	//
	// The platform_io form, not the obsolete ImDrawCallback_ResetRenderState
	// sentinel: 1.92.8 deprecated the latter in favour of this.
	ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, NULL);
}

#endif   // MT_RENDER_BACKEND_D3D11
