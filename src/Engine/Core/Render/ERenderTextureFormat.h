#ifndef _ERenderTextureFormat_h_
#define _ERenderTextureFormat_h_

// What an uploaded texture is made of -- the RESIDENT format.
//
// Its own header, beside EImageGpuFormat.h and for the same reason: this is
// consumed by CSlrImage.h, which today includes neither <SDL3/SDL.h> nor
// imgui.h. CRenderBackend.h includes both and says so itself ("there are many
// plain-C++ translation units"), so declaring a two-value enum there would drag
// the whole of SDL and ImGui into every translation unit that merely wants to
// name an image's format.
//
// There are exactly TWO, and that is a deliberate cap (S-5): a decoder may
// produce any decode-time type, but one funnel -- CSlrImage -- decides the
// resident format and it has only two answers, so conversions stay a short list
// rather than a matrix.
enum ERenderTextureFormat
{
	// The default everywhere. 8 bits per channel, 4 bytes per pixel.
	RENDER_TEXTURE_RGBA8 = 0,

	// IEEE half per channel, 8 bytes per pixel. Twice the memory, so it is
	// spent only on sources that can actually carry above-white data and only
	// when a display can show it. Note there is no _sRGB hardware-decode
	// variant of this format on either backend (only 8-bit formats have one),
	// so whatever encoding the texels carry is what the surface receives.
	RENDER_TEXTURE_RGBA16F = 1,
};

#endif
