#ifndef _CMaskedTileShader_h_
#define _CMaskedTileShader_h_

// Backend-neutral interface for the masked-tile shader (LightHeroes' hex grid).
//
// This is deliberately NOT `CRenderShader *`: consumers store the QUEUED variant
// and call BeginBatch()/PushTileBounds() on it, neither of which exists on
// CRenderShader, so returning the base class from the backend factory would
// force a downcast to a GL-specific type and defeat the point of the factory.
//
// Texture handles are the `void *` convention CSlrImage already uses -- never
// GLuint, which is 32-bit and would truncate an id<MTLTexture>.
class CMaskedTileShader
{
public:
	virtual ~CMaskedTileShader() {}

	virtual void CompileShaders() = 0;
	virtual void UseShaderProgram() = 0;
	virtual void ResetState() = 0;

	virtual void SetMaskTexture(void *maskTexture) = 0;
	virtual void SetTileBounds(float tilePosX, float tilePosY, float tileSizeX, float tileSizeY) = 0;

	// Queued variant only. No-ops on the non-queued one so callers never need a
	// capability test for the common path.
	virtual void BeginBatch() {}
	virtual void PushTileBounds(void *maskTexture, float tilePosX, float tilePosY,
								float tileSizeX, float tileSizeY) {}

	// False when the shader failed to compile. Callers draw their unshaded
	// fallback rather than issuing draws that would silently render nothing.
	virtual bool IsUsable() const = 0;
};

#endif
