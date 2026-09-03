#ifndef _CRenderShaderMaskedTileD3D11_h_
#define _CRenderShaderMaskedTileD3D11_h_

#include "CMaskedTileShader.h"

#if defined(MT_RENDER_BACKEND_D3D11)

#include "CMaskedTileBoundsQueue.h"

class CRenderBackendD3D11;

// The game app's hex-grid tile mask on D3D11: the piece texture sampled
// normally, the mask sampled by SCREEN position against the tile rectangle, and
// everything outside the rectangle or under a low mask alpha discarded.
//
// The D3D11 twin of CRenderShaderMaskedTileMetal and CRenderShaderMaskedTile
// (OpenGL). See Shaders/MaskedTile.hlsl for the maths and for the one place the
// three deliberately differ -- D3D's SV_Position is top-left, like Metal's
// [[position]] and unlike gl_FragCoord, so the GLSL version's Y flip must not
// be carried across.
//
// IT SUBCLASSES CMaskedTileShader DIRECTLY, not CRenderShader. Consumers store
// the QUEUED variant and call BeginBatch()/PushTileBounds(), neither of which
// exists on CRenderShader -- that is the whole reason the backend-neutral
// interface exists, and the OpenGL backend reaches the same shape through a
// CMaskedTileShaderGL adapter it defines inline. One class covers both variants
// here because the queued behaviour is four lines; `queued` selects it.
//
// WHY THE CONSTANT BUFFER IS DYNAMIC, where FlatColor's is IMMUTABLE: the tile
// rectangle changes for every tile in a frame. It is written inside the draw
// CALLBACK, not at Push time, because ImGui callbacks are deferred -- see
// UseShaderProgram().
class CRenderShaderMaskedTileD3D11 : public CMaskedTileShader
{
public:
	CRenderShaderMaskedTileD3D11(CRenderBackendD3D11 *renderBackend, bool queued);
	virtual ~CRenderShaderMaskedTileD3D11();

	virtual void CompileShaders() override;
	virtual void UseShaderProgram() override;
	virtual void ResetState() override;

	virtual void SetMaskTexture(void *maskTexture) override;
	virtual void SetTileBounds(float tilePosX, float tilePosY, float tileSizeX, float tileSizeY) override;

	virtual void BeginBatch() override;
	virtual void PushTileBounds(void *maskTexture, float tilePosX, float tilePosY,
								float tileSizeX, float tileSizeY) override;

	virtual bool IsUsable() const override { return isCompiled; }

	// Called from inside the ImGui draw callback with the live device context,
	// already null-checked. Pops the next queued rectangle when this is the
	// queued variant, then binds shader, constants and the mask at t1.
	void BindTo(void *deviceContext);

private:
	CRenderBackendD3D11 *renderBackend;
	void *pixelShaderPtr;     // ID3D11PixelShader *, retained
	void *constantBufferPtr;  // ID3D11Buffer *,      retained, DYNAMIC
	void *maskTexturePtr;     // ID3D11ShaderResourceView *, NOT retained (owned by CSlrImage)

	float tilePosX, tilePosY, tileSizeX, tileSizeY;

	bool queued;
	CMaskedTileBoundsQueue boundsQueue;

	bool isCompiled;
	// Latch. Without it a failed creation is retried every frame, which turns
	// one diagnostic into a scrolling wall of them.
	bool compileFailed;
};

#endif   // MT_RENDER_BACKEND_D3D11
#endif
