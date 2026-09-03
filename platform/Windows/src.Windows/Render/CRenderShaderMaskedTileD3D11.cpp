#include "CRenderShaderMaskedTileD3D11.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "CRenderBackendD3D11.h"
#include "DBG_Log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"

#include <d3d11.h>
#include <cstring>

#include "Generated/MaskedTilePSBytecode.h"

// Mirrors cbuffer MaskedTileConstants in Shaders/MaskedTile.hlsl: two float2 in
// physical framebuffer pixels, y down. 16 bytes, one constant register.
struct MaskedTileConstantsCPU
{
	float tilePosX, tilePosY;
	float tileSizeX, tileSizeY;
};

CRenderShaderMaskedTileD3D11::CRenderShaderMaskedTileD3D11(CRenderBackendD3D11 *renderBackend, bool queued)
: renderBackend(renderBackend), pixelShaderPtr(NULL), constantBufferPtr(NULL),
  maskTexturePtr(NULL), tilePosX(0), tilePosY(0), tileSizeX(1), tileSizeY(1),
  queued(queued), isCompiled(false), compileFailed(false)
{
}

CRenderShaderMaskedTileD3D11::~CRenderShaderMaskedTileD3D11()
{
	if (constantBufferPtr != NULL) { ((ID3D11Buffer *)constantBufferPtr)->Release(); constantBufferPtr = NULL; }
	if (pixelShaderPtr != NULL)    { ((ID3D11PixelShader *)pixelShaderPtr)->Release(); pixelShaderPtr = NULL; }
	// maskTexturePtr is NOT released: the SRV belongs to the CSlrImage that
	// handed it over, exactly as on the other two backends.
}

void CRenderShaderMaskedTileD3D11::CompileShaders()
{
	if (isCompiled || compileFailed)
		return;

	ID3D11Device *device = (ID3D11Device *)(renderBackend ? renderBackend->GetD3DDevice() : NULL);
	if (device == NULL)
	{
		LOGError("CRenderShaderMaskedTileD3D11: no D3D device yet");
		compileFailed = true;
		return;
	}

	// A PLACEHOLDER BYTECODE HEADER CARRIES LENGTH 0 -- the .hlsl was authored
	// on a machine with no HLSL compiler. Refuse loudly rather than creating a
	// shader from nothing; IsUsable() then stays false and every caller draws
	// its unshaded fallback, which is the same outcome as this backend having
	// no masked-tile shader at all, but said out loud.
	if (kMaskedTilePSBytecodeLength == 0)
	{
		LOGError("CRenderShaderMaskedTileD3D11: the embedded MaskedTile bytecode is a PLACEHOLDER "
				 "(length 0). Run tools/embed-hlsl-shaders.ps1 with fxc.exe on PATH and commit "
				 "the generated headers.");
		compileFailed = true;
		return;
	}

	ID3D11PixelShader *ps = NULL;
	HRESULT hr = device->CreatePixelShader(kMaskedTilePSBytecodeData,
										   (SIZE_T)kMaskedTilePSBytecodeLength, NULL, &ps);
	if (FAILED(hr))
	{
		LOGError("CRenderShaderMaskedTileD3D11: CreatePixelShader failed (hr=0x%08x)", (unsigned)hr);
		compileFailed = true;
		return;
	}

	// DYNAMIC, where FlatColor's is IMMUTABLE: the tile rectangle changes for
	// every tile in a frame, and it is written inside the draw callback.
	D3D11_BUFFER_DESC bd = {};
	bd.ByteWidth = sizeof(MaskedTileConstantsCPU);
	bd.Usage = D3D11_USAGE_DYNAMIC;
	bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
	bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

	ID3D11Buffer *cb = NULL;
	hr = device->CreateBuffer(&bd, NULL, &cb);
	if (FAILED(hr))
	{
		LOGError("CRenderShaderMaskedTileD3D11: constant buffer creation failed (hr=0x%08x)", (unsigned)hr);
		ps->Release();
		compileFailed = true;
		return;
	}

	pixelShaderPtr = ps;
	constantBufferPtr = cb;
	isCompiled = true;
}

void CRenderShaderMaskedTileD3D11::SetMaskTexture(void *maskTexture)
{
	this->maskTexturePtr = maskTexture;
}

void CRenderShaderMaskedTileD3D11::SetTileBounds(float tilePosX, float tilePosY,
												 float tileSizeX, float tileSizeY)
{
	this->tilePosX = tilePosX;
	this->tilePosY = tilePosY;
	this->tileSizeX = tileSizeX;
	this->tileSizeY = tileSizeY;
}

void CRenderShaderMaskedTileD3D11::BeginBatch()
{
	if (queued)
		boundsQueue.Clear();
}

void CRenderShaderMaskedTileD3D11::PushTileBounds(void *maskTexture, float px, float py, float sx, float sy)
{
	if (queued)
		boundsQueue.Push(maskTexture, px, py, sx, sy);
}

void CRenderShaderMaskedTileD3D11::BindTo(void *deviceContext)
{
	ID3D11DeviceContext *ctx = (ID3D11DeviceContext *)deviceContext;
	if (ctx == NULL || pixelShaderPtr == NULL || constantBufferPtr == NULL)
		return;

	// POP HERE, NOT AT PUSH TIME. This is the counterpart of the GL version's
	// SetShaderVars(): ImGui draw callbacks are deferred, so the queue is filled
	// during the frame's blits and drained during the draw walk, one entry per
	// callback, in the order they were pushed. An exhausted queue falls back to
	// the member variables set through SetMaskTexture/SetTileBounds, which is
	// the single-tile path -- identical to CRenderShaderMaskedTileQueued.
	void *maskTexture = maskTexturePtr;
	MaskedTileConstantsCPU c = { tilePosX, tilePosY, tileSizeX, tileSizeY };
	if (queued)
	{
		const CMaskedTileBounds *b = boundsQueue.Pop();
		if (b != NULL)
		{
			maskTexture = b->maskTexture;
			c.tilePosX = b->px;
			c.tilePosY = b->py;
			c.tileSizeX = b->sx;
			c.tileSizeY = b->sy;
		}
	}

	// A zero extent would divide the mask UV by zero. The GL and Metal versions
	// inherit the same guard from their callers never pushing one; here it is
	// cheap to be explicit, and a NaN UV samples unpredictably rather than
	// discarding.
	if (c.tileSizeX == 0.0f || c.tileSizeY == 0.0f)
		return;

	D3D11_MAPPED_SUBRESOURCE mapped;
	ID3D11Buffer *cb = (ID3D11Buffer *)constantBufferPtr;
	if (FAILED(ctx->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
		return;
	memcpy(mapped.pData, &c, sizeof(c));
	ctx->Unmap(cb, 0);

	ctx->PSSetShader((ID3D11PixelShader *)pixelShaderPtr, NULL, 0);
	// b0 OF THE PIXEL STAGE. ImGui's b0 is on the VERTEX stage (its projection
	// matrix) and its own pixel shader reads no constant buffer, so the two do
	// not collide -- the binding tables are separate.
	ctx->PSSetConstantBuffers(0, 1, &cb);

	// t1 ONLY. t0 is the draw command's own texture, which imgui_impl_dx11 has
	// already bound to the piece we are drawing, and s0 is ImGui's sampler; the
	// mask is the one thing it knows nothing about. A CSlrImage's texture handle
	// IS an ID3D11ShaderResourceView* on this backend (CRenderBackendD3D11.cpp
	// says so at CreateTexture), so it needs no translation.
	ID3D11ShaderResourceView *maskSRV = (ID3D11ShaderResourceView *)maskTexture;
	ctx->PSSetShaderResources(1, 1, &maskSRV);
}

void CRenderShaderMaskedTileD3D11::UseShaderProgram()
{
	if (!isCompiled)
		return;

	ImGui::GetWindowDrawList()->AddCallback([](const ImDrawList *, const ImDrawCmd *cmd)
	{
		CRenderShaderMaskedTileD3D11 *shader = (CRenderShaderMaskedTileD3D11 *)cmd->UserCallbackData;
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

void CRenderShaderMaskedTileD3D11::ResetState()
{
	// GIVE ImGui ITS PIPELINE BACK. imgui_impl_dx11 captures BACKUP_DX11_STATE
	// once before the whole draw walk and restores it once after; inside the
	// walk a user callback gets no save and no restore at all, so everything
	// BindTo() touched survives until this runs. See the longer version of this
	// note in CRenderShaderFlatColorD3D11::ResetState.
	//
	// The queue is DELIBERATELY not cleared here. Callbacks are deferred and
	// have not consumed their entries yet when ResetState runs; BeginBatch()
	// owns the clear, at the start of the next frame. Getting this wrong drains
	// the queue before the draw walk reads it and every tile falls back to the
	// single-tile member variables -- the same trap CMaskedTileBoundsQueue's
	// header calls out.
	ImGui::GetWindowDrawList()->AddCallback(ImGui::GetPlatformIO().DrawCallback_ResetRenderState, NULL);
}

#endif   // MT_RENDER_BACKEND_D3D11
