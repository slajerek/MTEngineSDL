#pragma once

#include "CSlrImage.h"

#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl3w.h>
#endif

// CSlrImage-compatible adapter that wraps a GL texture owned by someone
// else -- e.g. CVideoPlayer's RGBA render-to-texture output (Task 10's
// CGLRenderTarget) -- so it can be handed to
// CGuiViewMovingPaneImage::SetImage() and rendered via ImGui AddImage()/
// CSlrImage::TexturePtr() exactly like any file-backed CSlrImage.
//
// CSlrImage::TexturePtr() is NOT virtual, so this adapter cannot override
// it. Instead SetExternalTexture() writes directly into the inherited
// texturePtr/width/height/etc. state that TexturePtr() and
// CGuiViewMovingPaneImage::SetImage() read.
//
// Ownership: NEVER uploads, binds, or frees the wrapped GL texture.
//  - The constructor delegates to CSlrImage's lightest ("delayed load")
//    base constructor, which does none of CSlrImage's usual file-load /
//    VID_PostImageBinding work (see CSlrImage::CSlrImage(bool, bool),
//    CSlrImage.cpp).
//  - It then sets isFromAtlas = true. That is the same flag CSlrImage's
//    image-atlas sub-image constructor uses to mean "I don't own this
//    texture, don't glDeleteTextures it on destruction"
//    (CSlrImage::CSlrImage(CSlrImage *imgAtlas, ...)). ~CSlrImage() checks
//    isFromAtlas unconditionally -- independent of isBound -- before
//    calling gRenderBackend->DeleteTexture(this), so this holds for the
//    adapter's entire lifetime, including if SetExternalTexture() is never
//    called or is called with texture == 0.
//  - It never calls VID_PostImageBinding()/VID_PostImageDealloc()/
//    VID_PostImageDestroy(), so it never enters VID_ImageBinding.cpp's
//    async imageBindings queue -- nothing will ever BindImage()/
//    Deallocate()/glDeleteTextures it from that path either.
//  - cacheKey and resourcePath are never set (both stay 0/NULL for the
//    adapter's whole life), so CSlrImage::TexturePtr()'s
//    RES_CacheGetImage() reload-on-deallocated-resource branch can never
//    trigger and silently replace the external texture.
//
// RED LINE — the one UNGUARDED free path: CSlrImage::Deallocate() calls
// gRenderBackend->DeleteTexture(this) REGARDLESS of isFromAtlas (unlike
// ~CSlrImage(), which checks it). Deallocate() is reachable via
// ResourceDeactivate() and via VID_PostImageDealloc()'s
// BINDING_MODE_DEALLOC dispatch. The adapter is safe only because it never
// enters those paths on its own — so consumers must NEVER:
//  - call Deallocate() (or ResourceDeactivate()) on this adapter,
//  - call VID_PostImageDealloc()/VID_PostImageDestroy() with it,
//  - pass it to CGuiViewMovingPaneImage::SetImage()/SetImageData() while
//    the view's shouldDeallocImage is true (SetImage() then
//    VID_PostImageDealloc()s the PREVIOUS image — if that previous image
//    is this adapter, its wrapped external texture gets glDeleteTextures'd
//    out from under its real owner).
// Any of these deletes the external GL texture the adapter does not own.
//
// Not thread-safe with itself: SetExternalTexture() writes render-thread
// GL-identifying state (an atomic texturePtr, plus plain float/bool
// fields) and must be called from the render thread, matching
// CGuiViewMovingPaneImage's existing render-thread usage of the image it
// is given.
class CSlrImageExternalTexture : public CSlrImage
{
public:
	CSlrImageExternalTexture();

	// (Re)points this adapter at an external backend texture. Call from the
	// render thread. `texture` == NULL means "no frame yet": TexturePtr() will
	// report NULL and isBound stays false so
	// CGuiViewMovingPaneImage::RenderImGui() skips drawing instead of blitting
	// a null texture.
	//
	// void*, not GLuint: GLuint is 32-bit and would silently truncate half of an
	// id<MTLTexture>, which compiles, runs, and samples garbage.
	// `fmt` is the wrapped texture's RESIDENT format (S-5 Phase 5). It matters
	// because the video render target is RGBA16F for an HDR clip with the
	// session gate open, and anything downstream that reasons about the
	// image's format -- or its above-white content -- would otherwise assume
	// 8-bit. Defaulted, so every existing caller keeps today's answer.
	void SetExternalTexture(void *texture, int width, int height,
							ERenderTextureFormat fmt = RENDER_TEXTURE_RGBA8);
};
