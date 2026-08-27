#include "CImageData.h"
#include "DBG_Log.h"

#if MT_ENABLE_LIBAVIF
#include <avif/avif.h>
#include <cstring>

bool CImageData::LoadAVIF(const char *fileName)
{
    avifDecoder *dec = avifDecoderCreate();
    if (!dec)
    {
        LOGError("CImageData::LoadAVIF: avifDecoderCreate failed (%s)", fileName);
        return false;
    }
    dec->maxThreads = 2;

    avifResult result = avifDecoderSetIOFile(dec, fileName);
    if (result != AVIF_RESULT_OK)
    {
        LOGError("CImageData::LoadAVIF: cannot open '%s' — %s",
                 fileName, avifResultToString(result));
        avifDecoderDestroy(dec);
        return false;
    }

    result = avifDecoderParse(dec);
    if (result != AVIF_RESULT_OK)
    {
        LOGError("CImageData::LoadAVIF: parse failed '%s' — %s",
                 fileName, avifResultToString(result));
        avifDecoderDestroy(dec);
        return false;
    }

    // Copy the profile before decoding the frame: it belongs to the parsed
    // image and SetIccProfile copies, so ordering is not critical, but doing
    // it here keeps it beside the parse that produced it.
    if (dec->image != NULL && dec->image->icc.data != NULL && dec->image->icc.size > 0)
        SetIccProfile((const u8 *)dec->image->icc.data, (u32)dec->image->icc.size);

    result = avifDecoderNextImage(dec);
    if (result != AVIF_RESULT_OK)
    {
        LOGError("CImageData::LoadAVIF: decode failed '%s' — %s",
                 fileName, avifResultToString(result));
        avifDecoderDestroy(dec);
        return false;
    }

    avifRGBImage rgb;
    memset(&rgb, 0, sizeof(rgb));
    avifRGBImageSetDefaults(&rgb, dec->image);
    rgb.format = AVIF_RGB_FORMAT_RGBA;
    rgb.depth  = 8;

    result = avifRGBImageAllocatePixels(&rgb);
    if (result != AVIF_RESULT_OK)
    {
        LOGError("CImageData::LoadAVIF: pixel alloc failed '%s' — %s",
                 fileName, avifResultToString(result));
        avifDecoderDestroy(dec);
        return false;
    }

    result = avifImageYUVToRGB(dec->image, &rgb);
    if (result != AVIF_RESULT_OK)
    {
        LOGError("CImageData::LoadAVIF: YUV->RGB failed '%s' — %s",
                 fileName, avifResultToString(result));
        avifRGBImageFreePixels(&rgb);
        avifDecoderDestroy(dec);
        return false;
    }

    this->width      = (int)rgb.width;
    this->height     = (int)rgb.height;
    this->type       = IMG_TYPE_RGBA;
    size_t sz        = (size_t)rgb.width * rgb.height * 4;
    this->resultData = new u8[sz];
    memcpy(this->resultData, rgb.pixels, sz);

    avifRGBImageFreePixels(&rgb);
    avifDecoderDestroy(dec);
    return true;
}

#else

bool CImageData::LoadAVIF(const char *fileName)
{
    LOGError("CImageData::LoadAVIF: AVIF not supported on this build (%s)", fileName);
    return false;
}

#endif // MT_ENABLE_LIBAVIF
