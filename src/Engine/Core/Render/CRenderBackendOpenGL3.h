#ifndef _CRenderBackendOpenGL3_h_
#define _CRenderBackendOpenGL3_h_

#include "CRenderBackend.h"

class CRenderBackendOpenGL3 : public CRenderBackend
{
public:
	CRenderBackendOpenGL3();

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

	void GetGlSlVersion();
	const char *glslVersionString;
	
	virtual ~CRenderBackendOpenGL3();
};

#endif

