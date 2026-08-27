#include "Generated/MTMaskedTileMetallib.h"
#include "CRenderShaderMaskedTileMetal.h"
#include "CRenderBackendMetal.h"
#include "DBG_Log.h"
#include "VID_Main.h"
#include "SYS_Platform.h"

#import <Metal/Metal.h>
#include <SDL3/SDL.h>

CRenderShaderMaskedTileMetal::CRenderShaderMaskedTileMetal(CRenderBackendMetal *renderBackend, bool queued)
: CRenderShaderMetal(renderBackend, "MaskedTile")
{
	this->isQueued = queued;
	this->maskTexture = NULL;
	this->tilePosX = 0;
	this->tilePosY = 0;
	this->tileSizeX = 1;
	this->tileSizeY = 1;
}

CRenderShaderMaskedTileMetal::~CRenderShaderMaskedTileMetal()
{
}

void CRenderShaderMaskedTileMetal::SetMaskTexture(void *maskTexture)
{
	this->maskTexture = maskTexture;
}

void CRenderShaderMaskedTileMetal::SetTileBounds(float px, float py, float sx, float sy)
{
	this->tilePosX = px;
	this->tilePosY = py;
	this->tileSizeX = sx;
	this->tileSizeY = sy;
}

void CRenderShaderMaskedTileMetal::BeginBatch()
{
	if (isQueued)
		boundsQueue.Clear();
}

void CRenderShaderMaskedTileMetal::PushTileBounds(void *maskTexture, float px, float py, float sx, float sy)
{
	if (isQueued)
		boundsQueue.Push(maskTexture, px, py, sx, sy);
}

// The GLSL this ports, and the one deliberate difference.
//
// GL samples the piece texture at Frag_UV, derives the mask UV from
// gl_FragCoord, and discards outside [0,1] or where the mask alpha is below
// 0.5. The maths there is contorted by OpenGL's bottom-left origin: SetShaderVars
// passes iTilePos as (tilePosX*dpi, viewportHeight - (tilePosY + tileSizeY)*dpi)
// and the shader then flips maskUV.y a second time.
//
// Metal's [[position]] is already top-down and matches ImGui's own coordinate
// sense, so BOTH flips disappear: the tile position goes in as plain
// (tilePosX*dpi, tilePosY*dpi) and the mask UV is the direct ratio. That is not
// a simplification of the maths, it is the same maths with the two
// compensating inversions removed -- a fragment at the top edge of the tile
// lands on maskUV.y = 0 under either backend, which is what makes the two
// renderings comparable pixel for pixel.
const char *CRenderShaderMaskedTileMetal::GetMetalShaderSource()
{
	return kMTMaskedTileMetalSource;
}

const void *CRenderShaderMaskedTileMetal::GetEmbeddedLibraryData(unsigned long *outLength)
{
	if (outLength != NULL)
		*outLength = kMTMaskedTileMetallibLength;
	return kMTMaskedTileMetallibData;
}

void CRenderShaderMaskedTileMetal::SetShaderVars(void *encoder)
{
	id<MTLRenderCommandEncoder> enc = (__bridge id<MTLRenderCommandEncoder>)encoder;

	// Pop the next queued entry if there is one, otherwise use the members set
	// through SetMaskTexture/SetTileBounds -- the single-tile path. Same
	// fallback rule as CRenderShaderMaskedTileQueued::SetShaderVars().
	void *tileMask = this->maskTexture;
	float px = tilePosX, py = tilePosY, sx = tileSizeX, sy = tileSizeY;
	if (isQueued)
	{
		const CMaskedTileBounds *b = boundsQueue.Pop();
		if (b != NULL)
		{
			tileMask = b->maskTexture;
			px = b->px; py = b->py; sx = b->sx; sy = b->sy;
		}
	}

	// gl_FragCoord and Metal's [[position]] are both in PHYSICAL pixels, so the
	// DPI scale is needed on both backends for the same reason.
	float dpiScale = SDL_GetWindowPixelDensity(VID_GetMainSDLWindow());
	if (dpiScale <= 0.0f)
		dpiScale = 1.0f;

	float tileUniforms[4] = { px * dpiScale, py * dpiScale, sx * dpiScale, sy * dpiScale };
	[enc setFragmentBytes:tileUniforms length:sizeof(tileUniforms) atIndex:0];

	id<MTLTexture> mask = (__bridge id<MTLTexture>)tileMask;
	// nil is legal here and renders as a zero mask, i.e. everything discarded.
	// That is the correct degradation: an unset mask must not paint an unmasked
	// tile over the board.
	[enc setFragmentTexture:mask atIndex:1];
}
