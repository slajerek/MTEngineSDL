#ifndef _CRenderShaderMaskedTileQueued_h_
#define _CRenderShaderMaskedTileQueued_h_

#include "CRenderShaderMaskedTile.h"
#include "CMaskedTileBoundsQueue.h"

// Queued version of CRenderShaderMaskedTile for rendering multiple tiles per frame.
// Each PushTileBounds() call stores tile bounds that are consumed in order by
// deferred ImGui callbacks. Without queued bounds, falls back to parent behavior
// (reads member variables set via SetMaskTexture/SetTileBounds).
//
// Usage for multiple tiles:
//   BeginBatch();
//   for each tile:
//     PushTileBounds(maskTexId, px, py, w, h);
//     UseShaderProgram();   // queues callback that will pop from queue
//     Blit(image, ...);
//     ResetState();         // queues GL reset (safe to draw non-masked items after)
//
class CRenderShaderMaskedTileQueued : public CRenderShaderMaskedTile
{
public:
	CRenderShaderMaskedTileQueued(CRenderBackendOpenGL4 *renderBackend);
	virtual ~CRenderShaderMaskedTileQueued();

	// Clear queue for a new batch of tiles
	void BeginBatch();

	// Queue tile bounds for the next UseShaderProgram() callback
	void PushTileBounds(GLuint maskTextureId, float tilePosX, float tilePosY, float tileSizeX, float tileSizeY);

	virtual void SetShaderVars() override;
	virtual void ResetState() override;

private:
	// SHARED with the Metal port (CRenderShaderMaskedTileMetal) rather than
	// duplicated. The push/pop and the deferred-callback lifetime rule are
	// entirely backend-independent, and the lifetime rule is the part that is
	// easy to get wrong twice.
	CMaskedTileBoundsQueue boundsQueue;
};

#endif
