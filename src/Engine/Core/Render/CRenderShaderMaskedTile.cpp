#include "CRenderShaderMaskedTile.h"
#include "CRenderBackendOpenGL4.h"
#include "DBG_Log.h"
#include "SYS_Main.h"
#include "SYS_Platform.h"

CRenderShaderMaskedTile::CRenderShaderMaskedTile(CRenderBackendOpenGL4 *renderBackend)
: CRenderShaderOpenGL4(renderBackend, "MaskedTile")
{
	maskTextureId = 0;
	tilePosX = 0;
	tilePosY = 0;
	tileSizeX = 1;
	tileSizeY = 1;
}

void CRenderShaderMaskedTile::SetMaskTexture(GLuint maskTextureId)
{
	this->maskTextureId = maskTextureId;
}

void CRenderShaderMaskedTile::SetTileBounds(float tilePosX, float tilePosY, float tileSizeX, float tileSizeY)
{
	this->tilePosX = tilePosX;
	this->tilePosY = tilePosY;
	this->tileSizeX = tileSizeX;
	this->tileSizeY = tileSizeY;
}

const char *CRenderShaderMaskedTile::GetFragmentShaderSource()
{
	return R"(
		in vec2 Frag_UV;
		in vec4 Frag_Color;
		uniform sampler2D iChannel0;  // piece texture
		uniform sampler2D iChannel1;  // mask texture
		uniform vec2 iTilePos;        // tile screen position (bottom-left in OpenGL coords)
		uniform vec2 iTileSize;       // tile screen size
		layout (location = 0) out vec4 Out_Color;

		void main() {
			// Sample piece texture normally
			vec4 piece = texture(iChannel0, Frag_UV);

			// Calculate mask UV from screen position
			vec2 screenPos = gl_FragCoord.xy;
			vec2 maskUV = (screenPos - iTilePos) / iTileSize;

			// Flip Y (OpenGL has Y=0 at bottom, ImGui coords have Y=0 at top)
			maskUV.y = 1.0 - maskUV.y;

			// Sample mask (alpha channel)
			float maskAlpha = texture(iChannel1, maskUV).a;

			// Discard if outside mask bounds or mask alpha is low
			if (maskUV.x < 0.0 || maskUV.x > 1.0 ||
				maskUV.y < 0.0 || maskUV.y > 1.0 ||
				maskAlpha < 0.5) {
				discard;
			}

			Out_Color = Frag_Color * piece;
		}
	)";
}

void CRenderShaderMaskedTile::GetUniformsLocations()
{
	LOGD("CRenderShaderMaskedTile::GetUniformsLocations");

	attribLocationMask = GetUniformLocation("iChannel1");
	attribLocationTilePos = GetUniformLocation("iTilePos");
	attribLocationTileSize = GetUniformLocation("iTileSize");
}

void CRenderShaderMaskedTile::SetShaderVars()
{
	// Get DPI scale for retina displays
	float dpiScale = 1.0f;
#ifdef __APPLE__
	dpiScale = MACOS_GetBackingScaleFactor(1);
#endif

	// Bind mask texture to unit 1
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_2D, maskTextureId);
	glUniform1i(attribLocationMask, 1);
	ASSERT_OPENGL();

	// Get viewport height to convert ImGui coords to OpenGL coords
	GLint viewport[4];
	glGetIntegerv(GL_VIEWPORT, viewport);
	float viewportHeight = (float)viewport[3];

	// Convert ImGui screen position (Y=0 at top) to OpenGL (Y=0 at bottom)
	// Also apply DPI scaling since gl_FragCoord is in physical pixels
	float glPosX = tilePosX * dpiScale;
	float glPosY = (viewportHeight - (tilePosY + tileSizeY) * dpiScale);
	float glSizeX = tileSizeX * dpiScale;
	float glSizeY = tileSizeY * dpiScale;

	glUniform2f(attribLocationTilePos, glPosX, glPosY);
	ASSERT_OPENGL();
	glUniform2f(attribLocationTileSize, glSizeX, glSizeY);
	ASSERT_OPENGL();

	// Restore texture unit 0
	glActiveTexture(GL_TEXTURE0);
	ASSERT_OPENGL();
}

CRenderShaderMaskedTile::~CRenderShaderMaskedTile()
{
}
