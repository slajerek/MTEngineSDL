#include "CRenderBackend.h"

CRenderBackend::CRenderBackend(const char *name)
{
	this->name = name;
	this->mainWindow = NULL;
}

SDL_Window *CRenderBackend::CreateSDLWindow(const char *title, int x, int y, int w, int h)
{
	return NULL;
}

void CRenderBackend::CreateRenderContext()
{
}

void CRenderBackend::InitRenderPipeline()
{
}

void CRenderBackend::CreateFontsTexture()
{
}

void CRenderBackend::NewFrame(ImVec4 clearColor)
{
}

void CRenderBackend::PresentFrameBuffer(ImVec4 clearColor)
{
}

void CRenderBackend::Shutdown()
{
}

void CRenderBackend::CreateTexture(CSlrImage *image)
{
}

void CRenderBackend::UpdateTextureLinearScaling(CSlrImage *image)
{
}

void CRenderBackend::ReBindTexture(CSlrImage *image)
{
}

void CRenderBackend::DeleteTexture(CSlrImage *image)
{
}

CRenderBackend::~CRenderBackend()
{
}
