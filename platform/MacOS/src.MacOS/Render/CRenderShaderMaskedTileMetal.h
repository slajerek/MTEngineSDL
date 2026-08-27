#ifndef _CRenderShaderMaskedTileMetal_h_
#define _CRenderShaderMaskedTileMetal_h_

#include "CRenderShaderMetal.h"
#include "Core/Render/CMaskedTileShader.h"
#include "Core/Render/CMaskedTileBoundsQueue.h"

// Metal port of CRenderShaderMaskedTile / CRenderShaderMaskedTileQueued.
//
// Unlike the GL side this is ONE class implementing both the backend-neutral
// CMaskedTileShader interface and the Metal shader, rather than a shader plus an
// adapter: there is no pre-existing Metal shader class to adapt, and the queued
// variant differs only in whether the bounds queue has entries. `queued` decides
// whether BeginBatch()/PushTileBounds() do anything, exactly as the GL pair does.
class CRenderShaderMaskedTileMetal : public CRenderShaderMetal, public CMaskedTileShader
{
public:
	CRenderShaderMaskedTileMetal(CRenderBackendMetal *renderBackend, bool queued);
	virtual ~CRenderShaderMaskedTileMetal();

	virtual const char *GetMetalShaderSource() override;
	virtual const void *GetEmbeddedLibraryData(unsigned long *outLength) override;
	virtual void SetShaderVars(void *encoder) override;

	// CMaskedTileShader. Both bases declare these, so each needs an explicit
	// forwarding override -- naming which base is meant is the point.
	virtual void CompileShaders() override   { CRenderShaderMetal::CompileShaders(); }
	virtual void UseShaderProgram() override { CRenderShaderMetal::UseShaderProgram(); }
	virtual void ResetState() override       { CRenderShaderMetal::ResetState(); }
	virtual bool IsUsable() const override   { return isCompiled; }

	virtual void SetMaskTexture(void *maskTexture) override;
	virtual void SetTileBounds(float px, float py, float sx, float sy) override;
	virtual void BeginBatch() override;
	virtual void PushTileBounds(void *maskTexture, float px, float py, float sx, float sy) override;

private:
	bool isQueued;
	CMaskedTileBoundsQueue boundsQueue;

	void *maskTexture;
	float tilePosX, tilePosY, tileSizeX, tileSizeY;
};

#endif
