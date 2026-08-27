#include "CImageData.h"
#include "DBG_Log.h"

#if MT_ENABLE_LIBHEIF
#include <libheif/heif.h>
#include <cstring>
#include <vector>

bool CImageData::LoadHEIF_libheif(const char *fileName)
{
    heif_context *ctx = heif_context_alloc();
    if (ctx == nullptr)
    {
        LOGError("CImageData::LoadHEIF_libheif: heif_context_alloc() returned null (OOM?) for '%s'", fileName);
        return false;
    }
    heif_error err = heif_context_read_from_file(ctx, fileName, nullptr);
    if (err.code != heif_error_Ok)
    {
        LOGError("CImageData::LoadHEIF_libheif: open failed '%s' — %s",
                 fileName, err.message);
        heif_context_free(ctx);
        return false;
    }

    heif_image_handle *handle = nullptr;
    err = heif_context_get_primary_image_handle(ctx, &handle);
    if (err.code != heif_error_Ok)
    {
        LOGError("CImageData::LoadHEIF_libheif: no primary image in '%s'", fileName);
        heif_context_free(ctx);
        return false;
    }

    heif_image *himg = nullptr;
    err = heif_decode_image(handle, &himg,
                            heif_colorspace_RGB,
                            heif_chroma_interleaved_RGBA, nullptr);
    // MUST precede heif_image_handle_release below -- the profile is read
    // through the handle. heif_error is a struct, not an enum; the file's own
    // idiom is `err.code != heif_error_Ok`.
    {
        heif_color_profile_type pt = heif_image_handle_get_color_profile_type(handle);
        if (pt == heif_color_profile_type_prof || pt == heif_color_profile_type_rICC)
        {
            size_t n = heif_image_handle_get_raw_color_profile_size(handle);
            if (n > 0)
            {
                std::vector<uint8_t> buf(n);
                heif_error perr = heif_image_handle_get_raw_color_profile(handle, buf.data());
                if (perr.code == heif_error_Ok)
                    SetIccProfile((const u8 *)buf.data(), (u32)n);
            }
        }
        // nclx-only files record nothing; CM-B's assumed profile covers them.
    }

    heif_image_handle_release(handle);
    heif_context_free(ctx);
    if (err.code != heif_error_Ok)
    {
        LOGError("CImageData::LoadHEIF_libheif: decode failed '%s' — %s",
                 fileName, err.message);
        return false;
    }

    int stride = 0;
    const uint8_t *data = heif_image_get_plane_readonly(
        himg, heif_channel_interleaved, &stride);
    int w = heif_image_get_width(himg, heif_channel_interleaved);
    int h = heif_image_get_height(himg, heif_channel_interleaved);

    this->width      = w;
    this->height     = h;
    this->type       = IMG_TYPE_RGBA;
    this->resultData = new u8[(size_t)w * h * 4];
    for (int row = 0; row < h; row++)
        memcpy(this->resultData + (size_t)row * w * 4,
               data + (size_t)row * stride, (size_t)w * 4);

    heif_image_release(himg);
    return true;
}

#else

bool CImageData::LoadHEIF_libheif(const char *fileName)
{
    LOGError("CImageData::LoadHEIF_libheif: libheif not compiled in (%s)", fileName);
    return false;
}

#endif // MT_ENABLE_LIBHEIF
