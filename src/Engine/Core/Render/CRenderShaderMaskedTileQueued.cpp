#include "CRenderShaderMaskedTileQueued.h"

CRenderShaderMaskedTileQueued::CRenderShaderMaskedTileQueued(CRenderBackendOpenGL4 *renderBackend)
: CRenderShaderMaskedTile(renderBackend)
{
}

void CRenderShaderMaskedTileQueued::BeginBatch()
{
	boundsQueue.Clear();
}

void CRenderShaderMaskedTileQueued::PushTileBounds(GLuint maskTexId, float px, float py, float sx, float sy)
{
	boundsQueue.Push((void *)(uintptr_t)maskTexId, px, py, sx, sy);
}

void CRenderShaderMaskedTileQueued::SetShaderVars()
{
	// Pop next entry from queue if available, otherwise fall back to parent
	// behavior (reads member variables set via SetMaskTexture/SetTileBounds)
	const CMaskedTileBounds *b = boundsQueue.Pop();
	if (b != NULL)
	{
		maskTextureId = (GLuint)(uintptr_t)b->maskTexture;
		tilePosX = b->px;
		tilePosY = b->py;
		tileSizeX = b->sx;
		tileSizeY = b->sy;
	}

	CRenderShaderMaskedTile::SetShaderVars();
}

void CRenderShaderMaskedTileQueued::ResetState()
{
	// Don't clear the queue here — callbacks are deferred and haven't
	// consumed the entries yet. Queue is cleared by BeginBatch().
	CRenderShaderMaskedTile::ResetState();
}

CRenderShaderMaskedTileQueued::~CRenderShaderMaskedTileQueued()
{
}
