#include "CImageData.h"
#include "DBG_Log.h"

#if MT_ENABLE_LIBTIFF
#include <tiffio.h>
#include <cstring>

bool CImageData::LoadTIFF(const char *fileName)
{
    // Suppress libtiff's default stderr warnings — they go through our log instead.
    TIFFSetWarningHandler(nullptr);
    TIFFSetErrorHandler(nullptr);

    TIFF *tif = TIFFOpen(fileName, "r");
    if (!tif)
    {
        LOGError("CImageData::LoadTIFF: cannot open '%s'", fileName);
        return false;
    }

    uint32_t w = 0, h = 0;
    TIFFGetField(tif, TIFFTAG_IMAGEWIDTH, &w);
    TIFFGetField(tif, TIFFTAG_IMAGELENGTH, &h);
    if (w == 0 || h == 0)
    {
        LOGError("CImageData::LoadTIFF: zero dimensions in '%s'", fileName);
        TIFFClose(tif);
        return false;
    }

    size_t npixels = (size_t)w * h;
    uint32_t *raster = (uint32_t *)_TIFFmalloc((tmsize_t)(npixels * sizeof(uint32_t)));
    if (!raster)
    {
        LOGError("CImageData::LoadTIFF: out of memory for '%s'", fileName);
        TIFFClose(tif);
        return false;
    }

    // ORIENTATION_TOPLEFT: row 0 is top, column 0 is left.
    if (!TIFFReadRGBAImageOriented(tif, w, h, raster, ORIENTATION_TOPLEFT, 0))
    {
        LOGError("CImageData::LoadTIFF: decode failed for '%s'", fileName);
        _TIFFfree(raster);
        TIFFClose(tif);
        return false;
    }
    // ICC lives in TIFFTAG_ICCPROFILE (34675). libtiff owns the returned
    // buffer and frees it with the handle, so grab it before TIFFClose --
    // SetIccProfile copies.
    {
        uint32 iccLen = 0;
        void *iccData = NULL;
        if (TIFFGetField(tif, TIFFTAG_ICCPROFILE, &iccLen, &iccData) && iccData != NULL && iccLen > 0)
            SetIccProfile((const u8 *)iccData, (u32)iccLen);
    }

    TIFFClose(tif);

    // TIFFGetR/G/B/A macros unpack the uint32 ABGR value into components.
    this->width  = (int)w;
    this->height = (int)h;
    this->type   = IMG_TYPE_RGBA;
    this->resultData = new u8[npixels * 4];

    for (size_t i = 0; i < npixels; i++)
    {
        uint32_t px = raster[i];
        this->resultData[i * 4 + 0] = (u8)TIFFGetR(px);
        this->resultData[i * 4 + 1] = (u8)TIFFGetG(px);
        this->resultData[i * 4 + 2] = (u8)TIFFGetB(px);
        this->resultData[i * 4 + 3] = (u8)TIFFGetA(px);
    }
    _TIFFfree(raster);
    return true;
}

#else

bool CImageData::LoadTIFF(const char *fileName)
{
    LOGError("CImageData::LoadTIFF: TIFF not supported on this build (%s)", fileName);
    return false;
}

#endif // MT_ENABLE_LIBTIFF
