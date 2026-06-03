#pragma once

#include "SYS_Defs.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

class CVideoYUVShader
{
public:
	CVideoYUVShader();
	~CVideoYUVShader();

	bool Compile();

	// Render a quad converting YUV(A) textures to RGB
	// x,y,w,h in pixel coordinates (screen space)
	void Render(GLuint texY, GLuint texU, GLuint texV, GLuint texA,
				bool hasAlpha, float alpha,
				int colorSpace, bool fullRange,
				float x, float y, float w, float h,
				float screenW, float screenH);

private:
	GLuint program = 0;
	GLuint vao = 0, vbo = 0;

	GLint locTexY = -1, locTexU = -1, locTexV = -1, locTexA = -1;
	GLint locHasAlpha = -1, locAlpha = -1;
	GLint locColorSpace = -1, locFullRange = -1;
	GLint locTransform = -1;

	bool compiled = false;

	GLuint CompileShader(GLenum type, const char *source);
	void CreateQuadVAO();
};
