#pragma once

#include "SYS_Defs.h"
#include "CGLRenderTarget.h"
// EYUVShaderMode moved here from below: it is a plain enum with no graphics
// dependency, and leaving it in this header -- which pulls in <OpenGL/gl3.h> --
// would drag the GL headers into every consumer of the backend-neutral video
// seam, including the Metal converter.
#include "CVideoYUVConverter.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl3w.h>
#endif

class CVideoYUVShader : public CVideoYUVConverter
{
public:
	CVideoYUVShader();
	~CVideoYUVShader();

	virtual bool Compile() override;

	// Render a quad converting YUV(A) textures to RGB
	// x,y,w,h in pixel coordinates (screen space)
	// void* handles across the seam, cast back to GLuint at the boundary below.
	// GLuint is 32-bit; a Metal texture pointer is 64, so a GLuint parameter
	// would silently truncate half of one.
	virtual void Render(void *texY, void *texU, void *texV, void *texA,
						bool hasAlpha, float alpha,
						int colorSpace, bool fullRange,
						float x, float y, float w, float h,
						float screenW, float screenH,
						const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

	// Converts one YUV(A) frame to RGBA and renders it into `target`,
	// display-oriented (rotationDegrees applied as a UV-space transform in
	// the vertex stage, not a CPU pixel remap). `target` must already be
	// sized to the *display* (post-rotation) dimensions -- CVideoPlayer's
	// RGBA output mode creates it via GetDisplayWidth()/GetDisplayHeight().
	//
	// texUVorU's meaning depends on `mode`:
	//  - YUV420_3Plane / YUV420P10: the U plane (texV is the V plane)
	//  - NV12: the single interleaved U,V plane (GL_RG8); texV is unused
	//    (may be 0)
	//
	// Shares this shader's single compiled program with Render() -- see the
	// .cpp for why a mode/rotation uniform pair was added to one program
	// rather than compiling per-mode program variants.
	// lutTexture/lutEdge (CM-E): an optional GL_TEXTURE_3D display colour LUT
	// (edge^3 RGBA16 lattice) sampled after the YUV->RGB matrix on encoded
	// R'G'B'. Pass 0/0 for no LUT -- the shader path is then bit-identical to
	// the pre-CM-E behaviour. Bound on texture unit 4.
	virtual void RenderToTarget(EYUVShaderMode mode, void *texY, void *texUVorU, void *texV,
								bool hasAlpha, void *texA, int colorSpace, bool fullRange,
								int rotationDegrees, void *lutTexture, int lutEdge,
								CRenderTarget &target,
								const SVideoHdrOutput &hdr = SVideoHdrOutput()) override;

private:
	GLuint program = 0;
	GLuint vao = 0, vbo = 0;

	GLint locTexY = -1, locTexU = -1, locTexV = -1, locTexA = -1;
	GLint locHasAlpha = -1, locAlpha = -1;
	GLint locColorSpace = -1, locFullRange = -1;
	GLint locTransform = -1;
	GLint locMode = -1, locRotation = -1;
	GLint locTexLut = -1, locUseLut = -1, locLutScale = -1, locLutOffset = -1;
	// S-5 Phase 5 HDR transfer uniforms.
	void SetHdrUniforms(const SVideoHdrOutput &hdr);

	GLint locColorTrc = -1, locFloatTarget = -1;
	GLint locSurfaceLinear = -1, locSurfaceP3 = -1, locToneMapHeadroom = -1;

	bool compiled = false;

	GLuint CompileShader(GLenum type, const char *source);
	void CreateQuadVAO();
};
