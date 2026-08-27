#include "CSlrImageExternalTexture.h"
#include <cstdint>

CSlrImageExternalTexture::CSlrImageExternalTexture()
: CSlrImage(true, true) // "delayed load" base ctor: no file I/O, no VID_PostImageBinding
{
	// isFromAtlas = true is the load-bearing bit: it makes ~CSlrImage()
	// skip gRenderBackend->DeleteTexture(this) unconditionally (see
	// ~CSlrImage(), CSlrImage.cpp), regardless of isBound, for this
	// object's entire lifetime -- this adapter never owns the GL texture
	// it wraps and must never delete it.
	this->isFromAtlas = true;
	this->imgAtlas = nullptr;

	this->isBound = false;
	this->isActive = false;
	this->resourceState = RESOURCE_STATE_DEALLOCATED;

	this->width = 0.0f;
	this->height = 0.0f;
	this->rasterWidth = 0.0f;
	this->rasterHeight = 0.0f;
	this->origRasterWidth = 0.0f;
	this->origRasterHeight = 0.0f;
	this->widthD2 = this->heightD2 = this->widthM2 = this->heightM2 = 0.0f;

	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = 1.0f;
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = 1.0f;
}

void CSlrImageExternalTexture::SetExternalTexture(void *texture, int width, int height,
												  ERenderTextureFormat fmt)
{
	// The wrapped texture's real format, so nothing downstream has to assume
	// RGBA8. boundFormat matches residentFormat here because this adapter
	// never uploads -- the texture is already resident, owned by someone else.
	this->residentFormat = fmt;
	this->boundFormat = fmt;

	this->width = (float)width;
	this->height = (float)height;
	this->rasterWidth = (float)width;
	this->rasterHeight = (float)height;
	this->origRasterWidth = (float)width;
	this->origRasterHeight = (float)height;

	this->widthD2 = this->width / 2.0f;
	this->heightD2 = this->height / 2.0f;
	this->widthM2 = this->width * 2.0f;
	this->heightM2 = this->height * 2.0f;

	// Full [0,1] UV range -- this always wraps an exact-size upload the
	// owner (e.g. CVideoPlayer's CGLRenderTarget) controls directly, never
	// a POT-padded or atlas-packed one (mirrors CSlrImage::LoadImageForRebinding's
	// "full range" convention; see CGuiViewMovingPaneImage::SetImage's
	// defaultTex* comment).
	this->defaultTexStartX = 0.0f;
	this->defaultTexEndX = 1.0f;
	this->defaultTexStartY = 0.0f;
	this->defaultTexEndY = 1.0f;

	// Already the void* convention every backend stores into texturePtr, so
	// TexturePtr() and every ImGui AddImage()/Blit() call site (VID_Blits.cpp)
	// read it back correctly on both backends.
	this->texturePtr.store(texture, std::memory_order_release);

	// isBound gates CGuiViewMovingPaneImage::RenderImGui()'s decision to
	// Blit() at all; tie it (and isActive) to having a real texture so a
	// still-0 texture (no frame decoded yet) renders nothing rather than
	// blitting texture id 0.
	this->isBound = (texture != NULL);
	this->isActive = (texture != NULL);
	this->resourceState = (texture != NULL) ? RESOURCE_STATE_LOADED : RESOURCE_STATE_DEALLOCATED;
}
