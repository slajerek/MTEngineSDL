#ifndef _CRenderBackend_h_
#define _CRenderBackend_h_

#include "SYS_Defs.h"
#include <SDL.h>
#include "imgui.h"
#include "EImageGpuFormat.h"

class CSlrImage;

// this is generic class to wrap ImGui's render backends

class CRenderBackend
{
public:
	CRenderBackend(const char *name);
	SDL_Window *mainWindow;
	
	virtual SDL_Window *CreateSDLWindow(const char *title, int x, int y, int w, int h, bool maximized);
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

	// Compressed texture format this backend/device can upload, or
	// IMG_GPU_UNCOMPRESSED if none. Default: none (safe for any backend
	// that does not override this).
	virtual EImageGpuFormat GetPreferredCompressedFormat() { return IMG_GPU_UNCOMPRESSED; }

	virtual ~CRenderBackend();
	
	const char *name;
};

#endif
