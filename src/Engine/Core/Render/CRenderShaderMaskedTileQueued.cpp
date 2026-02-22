#include "CRenderShaderMaskedTileQueued.h"

CRenderShaderMaskedTileQueued::CRenderShaderMaskedTileQueued(CRenderBackendOpenGL4 *renderBackend)
: CRenderShaderMaskedTile(renderBackend)
{
	readIndex = 0;
}

void CRenderShaderMaskedTileQueued::BeginBatch()
{
	boundsQueue.clear();
	readIndex = 0;
}

void CRenderShaderMaskedTileQueued::PushTileBounds(GLuint maskTexId, float px, float py, float sx, float sy)
{
	boundsQueue.push_back({maskTexId, px, py, sx, sy});
}

void CRenderShaderMaskedTileQueued::SetShaderVars()
{
	// Pop next entry from queue if available, otherwise fall back to parent
	// behavior (reads member variables set via SetMaskTexture/SetTileBounds)
	if (readIndex < (int)boundsQueue.size())
	{
		auto& b = boundsQueue[readIndex++];
		maskTextureId = b.maskTexId;
		tilePosX = b.px;
		tilePosY = b.py;
		tileSizeX = b.sx;
		tileSizeY = b.sy;
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
