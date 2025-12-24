#ifndef _CRenderBackendOpenGL4_h_
#define _CRenderBackendOpenGL4_h_

#include "CRenderBackend.h"

class CRenderBackendOpenGL4 : public CRenderBackend
{
public:
	CRenderBackendOpenGL4();

	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized);

	SDL_GLContext glContext;
	virtual void CreateRenderContext();
	virtual void InitRenderPipeline();
	virtual void CreateFontsTexture();
	virtual void NewFrame(ImVec4 clearColor);
	virtual void PresentFrameBuffer(ImVec4 clearColor);
	virtual void Shutdown();

	virtual void CreateTexture(CSlrImage *image);
	virtual void UpdateTextureLinearScaling(CSlrImage *image);
	virtual void ReBindTexture(CSlrImage *image);
	virtual void DeleteTexture(CSlrImage *image);

	void SetupGlSlVersion();
	const char *glslVersionString;
	const char *GetGlSlVersion();
	
	virtual ~CRenderBackendOpenGL4();
	
	static CRenderBackendOpenGL4 *GetRenderBackendOpenGL4();
	static bool CheckOpenGLError();
};

#define ASSERT_OPENGL()  { if (CRenderBackendOpenGL4::CheckOpenGLError() == true) { SYS_FatalExit("OpenGL error"); } }
//#define ASSERT_OPENGL()  { CRenderBackendOpenGL4::CheckOpenGLError(); }

#endif

