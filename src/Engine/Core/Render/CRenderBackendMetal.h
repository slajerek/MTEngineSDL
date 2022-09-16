#ifndef _CRenderBackendMetal_h_
#define _CRenderBackendMetal_h_

#include "CRenderBackend.h"

class CRenderBackendMetal : public CRenderBackend
{
public:
	CRenderBackendMetal();

	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h);

	SDL_Renderer* renderer;
	
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

	virtual ~CRenderBackendMetal();
};

#endif
