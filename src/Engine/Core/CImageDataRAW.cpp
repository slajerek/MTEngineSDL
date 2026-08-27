#include "CImageData.h"
#include "CRawDecoder.h"
#include "DBG_Log.h"

#if MT_ENABLE_LIBRAW
#include <libraw/libraw.h>
#include "stb_image.h"
#include "CExifReader.h"
#include <cstring>
#include <vector>
#if defined(_WIN32) || defined(WIN32)
#include "SYS_FileUtf8.h"
#endif
#include <cstring>

// The LibRaw half of LoadRAWPreview, hoisted so it can run on the 8 MB stack.
// It copies the preview JPEG out and releases LibRaw's buffer, so everything
// after it -- the stb decode and the EXIF/ICC parse -- runs on the caller's
// stack against plain bytes.
struct SRawPreviewCtx
{
    const char        *path = nullptr;
    std::vector<u8>    jpeg;
    bool               ok = false;
};

static void LoadRAWPreviewBody(void *p)
{
    SRawPreviewCtx *ctx = (SRawPreviewCtx *)p;
    LibRaw raw;
    raw.imgdata.params.use_camera_wb = 1;

#if defined(_WIN32) || defined(WIN32)
    // Wide overload. The narrow one is ANSI and fails outright on any
    // non-ASCII name -- so a Windows user with RAWs under "Zdjęcia" or a CJK
    // folder got no preview at all. This was diagnosed and written down twice
    // in this very file ("the LoadRAWPreview defect above; do not copy it")
    // without ever being fixed here.
    std::wstring wide = SYS_Utf8ToWide(ctx->path);
    int rc = raw.open_file(wide.c_str());
#else
    int rc = raw.open_file(ctx->path);
#endif
    if (rc != LIBRAW_SUCCESS)
    {
        LOGError("CImageData::LoadRAWPreview: cannot open '%s'", ctx->path);
        return;
    }

    if (raw.unpack_thumb() != LIBRAW_SUCCESS)
    {
        LOGError("CImageData::LoadRAWPreview: no embedded preview in '%s'", ctx->path);
        return;
    }

    libraw_processed_image_t *thumb = raw.dcraw_make_mem_thumb(nullptr);
    if (!thumb)
    {
        LOGError("CImageData::LoadRAWPreview: dcraw_make_mem_thumb failed for '%s'", ctx->path);
        return;
    }

    if (thumb->type != LIBRAW_IMAGE_JPEG)
    {
        LOGError("CImageData::LoadRAWPreview: non-JPEG preview (type=%d) in '%s'",
                 thumb->type, ctx->path);
        LibRaw::dcraw_clear_mem(thumb);
        return;
    }

    // Copy before clear: thumb->data belongs to LibRaw and dies with it, and
    // `raw` cannot outlive this frame. One memcpy of a few hundred KB, which
    // is noise beside the JPEG decode that follows.
    ctx->jpeg.assign(thumb->data, thumb->data + thumb->data_size);
    LibRaw::dcraw_clear_mem(thumb);
    ctx->ok = true;
}

bool CImageData::LoadRAWPreview(const char *fileName)
{
    // EVERY LibRaw entry point needs the 8 MB stack -- identify() alone puts
    // multi-hundred-KB frames on it, and the decode pool's workers do not have
    // that much. Decode()/DecodeBayer always guarded themselves and
    // ReadEmbeddedXmp was fixed after it hit the same SIGBUS; this entry point
    // was simply never covered.
    SRawPreviewCtx ctx;
    ctx.path = fileName;
    if (!CRawDecoder::RunWithBigStack(LoadRAWPreviewBody, &ctx) || !ctx.ok)
        return false;

    struct SThumbView { const u8 *data; size_t data_size; };
    const SThumbView thumbView{ ctx.jpeg.data(), ctx.jpeg.size() };
    const SThumbView *thumb = &thumbView;

    int w = 0, h = 0;
    // Decode the embedded JPEG from memory using stb_image.
    u8 *rgba = stbi_load_from_memory(
        thumb->data, (int)thumb->data_size, &w, &h, nullptr, 4);

    // Parse the preview's own APP2. Camera previews usually carry no profile
    // at all -- that gap is exactly what CM-C exists to fill -- but when one
    // is present it is authoritative and must win. (The ordering worry this
    // comment used to carry is gone: the bytes are ours now, copied out
    // before LibRaw's buffer was released.)
    //
    // Declared out here so the colour hint below can use it: that assignment
    // has to wait until the decode is known to have SUCCEEDED, or a corrupt
    // preview would leave the object describing pixels it never produced.
    CExifData ex = CExifReader::Read(thumb->data, thumb->data_size, false, true);
    if (!ex.iccProfile.empty())
        SetIccProfile(&ex.iccProfile[0], (u32)ex.iccProfile.size());

    if (!rgba)
    {
        LOGError("CImageData::LoadRAWPreview: JPEG decode failed for '%s'", fileName);
        return false;
    }

    // CM-C1: keep what the preview says about ITSELF. This parse already
    // happened for the ICC above, and typed extraction is not gated on
    // captureAllTags, so colorSpace/interopIndex cost nothing extra.
    //
    // Only meaningful when the preview brought no profile of its own --
    // bytes outrank a hint. Most camera previews carry neither, which is
    // precisely why the container-level signals exist; RAF is the format
    // where this path pays off, its preview being a full JPEG with EXIF.
    if (ex.iccProfile.empty() && ex.valid)
    {
        if (ex.colorSpace == 1)
        {
            previewColorHint       = EExifColorSpaceHint::Srgb;
            previewColorHintSource = EExifColorHintSource::PreviewExif;
        }
        else if (ex.colorSpace == 2)
        {
            previewColorHint       = EExifColorSpaceHint::AdobeRgb;
            previewColorHintSource = EExifColorHintSource::PreviewExif;
        }
        // 0xFFFF ("Uncalibrated") deliberately does NOT match above: it
        // means "not sRGB, look elsewhere", and InteropIndex is where the
        // Adobe RGB cameras that write it actually say so.
        else if (ex.interopIndex == "R03")
        {
            previewColorHint       = EExifColorSpaceHint::AdobeRgb;
            previewColorHintSource = EExifColorHintSource::InteropIndex;
        }
        else if (ex.interopIndex == "R98")
        {
            previewColorHint       = EExifColorSpaceHint::Srgb;
            previewColorHintSource = EExifColorHintSource::InteropIndex;
        }
    }

    this->width      = w;
    this->height     = h;
    this->type       = IMG_TYPE_RGBA;
    size_t sz        = (size_t)w * h * 4;
    this->resultData = new u8[sz];
    memcpy(this->resultData, rgba, sz);
    stbi_image_free(rgba);
    return true;
}

struct SReadXmpCtx
{
    const char *path;
    std::string *out;
    bool ok;
};

static void ReadEmbeddedXmpBody(void *p);

bool CImageData::ReadEmbeddedXmp(const char *utf8FileName, std::string *outXmp)
{
    if (utf8FileName == NULL || outXmp == NULL)
        return false;
    outXmp->clear();

    // LibRaw overflows 512 KB secondary-thread stacks (the RD-A SIGBUS);
    // the decode guards itself internally, and THIS entry point must too --
    // RD-E's develop lane was the first caller to hit it, exactly as the
    // CRawDecoder comment predicted.
    SReadXmpCtx ctx{ utf8FileName, outXmp, false };
    if (!CRawDecoder::RunWithBigStack(ReadEmbeddedXmpBody, &ctx))
        return false;
    return ctx.ok;
}

static void ReadEmbeddedXmpBody(void *p)
{
    SReadXmpCtx *ctx = (SReadXmpCtx *)p;
    const char *utf8FileName = ctx->path;
    std::string *outXmp = ctx->out;

    LibRaw raw;
#if defined(_WIN32) || defined(WIN32)
    // Wide overload: the narrow one is ANSI and fails on non-ASCII names --
    // the LoadRAWPreview defect above; do not copy it (RD-B #4.2 hazard 4).
    std::wstring wide = SYS_Utf8ToWide(utf8FileName);
    int rc = raw.open_file(wide.c_str());
#else
    int rc = raw.open_file(utf8FileName);
#endif
    // Hazard 2: LIBRAW_FILE_UNSUPPORTED returns after `final:` WITHOUT
    // recycle(), so the packet survives -- a plain non-raw TIFF and the
    // missing-libjpeg DNG both land there. The exception paths DO free it.
    if (rc != LIBRAW_SUCCESS && rc != LIBRAW_FILE_UNSUPPORTED)
        return;
    if (raw.imgdata.idata.xmpdata == NULL || raw.imgdata.idata.xmplen == 0)
        return;
    // Hazard 1: xmplen is not a length you can trust (the three fill sites
    // disagree about NUL inclusion, and a short read leaves it describing
    // bytes never written). strnlen bounds it.
    size_t len = strnlen(raw.imgdata.idata.xmpdata,
                         (size_t)raw.imgdata.idata.xmplen);
    if (len == 0)
        return;
    outXmp->assign(raw.imgdata.idata.xmpdata, len);
    ctx->ok = true;
}

#else

bool CImageData::LoadRAWPreview(const char *fileName)
{
    LOGError("CImageData::LoadRAWPreview: RAW not supported on this build (%s)", fileName);
    return false;
}

bool CImageData::ReadEmbeddedXmp(const char *utf8FileName, std::string *outXmp)
{
    (void)utf8FileName;
    if (outXmp != NULL)
        outXmp->clear();
    return false;
}

#endif // MT_ENABLE_LIBRAW
