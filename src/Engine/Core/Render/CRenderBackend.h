#ifndef _CRenderBackend_h_
#define _CRenderBackend_h_

#include "SYS_Defs.h"
#include <SDL.h>
#include "imgui.h"

class CSlrImage;

// this is generic class to wrap ImGui's render backends

class CRenderBackend
{
public:
	CRenderBackend(const char *name);
	SDL_Window *mainWindow;
	
	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h);
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
	
	virtual ~CRenderBackend();
	
	const char *name;
};

#endif
