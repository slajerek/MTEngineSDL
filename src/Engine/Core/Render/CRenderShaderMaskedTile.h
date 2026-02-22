#ifndef _CRenderShaderMaskedTile_h_
#define _CRenderShaderMaskedTile_h_

#include "CRenderShaderOpenGL4.h"

class CRenderShaderMaskedTile : public CRenderShaderOpenGL4
{
public:
	CRenderShaderMaskedTile(CRenderBackendOpenGL4 *renderBackend);
	virtual ~CRenderShaderMaskedTile();

	// Set before UseShaderProgram()
	void SetMaskTexture(GLuint maskTextureId);
	void SetTileBounds(float tilePosX, float tilePosY, float tileSizeX, float tileSizeY);

protected:
	GLuint maskTextureId;
	float tilePosX, tilePosY, tileSizeX, tileSizeY;

	GLint attribLocationMask;      // iChannel1
	GLint attribLocationTilePos;   // iTilePos
	GLint attribLocationTileSize;  // iTileSize

	virtual const char *GetFragmentShaderSource() override;
	virtual void GetUniformsLocations() override;
	virtual void SetShaderVars() override;
};

#endif
