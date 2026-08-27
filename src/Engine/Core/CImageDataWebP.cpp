#include "CImageData.h"
#include "DBG_Log.h"
#include "SYS_FileUtf8.h"   // SYS_FopenUtf8: UTF-8 names on a non-UTF-8 ANSI code page

#if MT_ENABLE_LIBWEBP
#include <webp/decode.h>
#include <webp/demux.h>
#include <cstdio>
#include <cstring>


namespace
{
// Both WebP paths need the profile, and only the animated one already builds a
// demuxer. WebPDemux() does NOT copy its input and chunk.bytes points straight
// into it, so this must run while the file buffer is still alive -- hence a
// self-contained helper called before either `delete[] fileData`.
void ExtractWebPIccProfile(CImageData *img, const uint8_t *fileData, size_t fileSize)
{
    WebPData webpData = { fileData, fileSize };
    WebPDemuxer *demux = WebPDemux(&webpData);
    if (!demux)
        return;
    WebPChunkIterator chunkIter;
    if (WebPDemuxGetChunk(demux, "ICCP", 1, &chunkIter))
    {
        if (chunkIter.chunk.bytes != NULL && chunkIter.chunk.size > 0)
            img->SetIccProfile((const u8 *)chunkIter.chunk.bytes, (u32)chunkIter.chunk.size);
        WebPDemuxReleaseChunkIterator(&chunkIter);
    }
    WebPDemuxDelete(demux);
}
} // namespace

bool CImageData::LoadWebP(const char *fileName)
{
    FILE *f = SYS_FopenUtf8(fileName, "rb");
    if (!f)
    {
        LOGError("CImageData::LoadWebP: cannot open '%s'", fileName);
        return false;
    }
    fseek(f, 0, SEEK_END);
    long fileSize = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (fileSize <= 0)
    {
        LOGError("CImageData::LoadWebP: empty file '%s'", fileName);
        fclose(f);
        return false;
    }

    uint8_t *fileData = new uint8_t[(size_t)fileSize];
    if ((long)fread(fileData, 1, (size_t)fileSize, f) != fileSize)
    {
        LOGError("CImageData::LoadWebP: read error '%s'", fileName);
        fclose(f);
        delete[] fileData;
        return false;
    }
    fclose(f);

    // Detect animated WebP — display first frame only.
    WebPBitstreamFeatures features;
    if (WebPGetFeatures(fileData, (size_t)fileSize, &features) == VP8_STATUS_OK
        && features.has_animation)
    {
        LOGM("CImageData::LoadWebP: animated WebP '%s' — first frame only", fileName);
        WebPData webpData = { fileData, (size_t)fileSize };
        WebPDemuxer *demux = WebPDemux(&webpData);
        bool ok = false;
        if (demux)
        {
            WebPIterator iter;
            if (WebPDemuxGetFrame(demux, 1, &iter))
            {
                int w = 0, h = 0;
                uint8_t *decoded = WebPDecodeRGBA(
                    iter.fragment.bytes, iter.fragment.size, &w, &h);
                if (decoded)
                {
                    this->width  = w;
                    this->height = h;
                    this->type   = IMG_TYPE_RGBA;
                    size_t sz = (size_t)w * h * 4;
                    this->resultData = new u8[sz];
                    memcpy(this->resultData, decoded, sz);
                    WebPFree(decoded);
                    ok = true;
                }
                WebPDemuxReleaseIterator(&iter);
            }
            WebPDemuxDelete(demux);
        }
        ExtractWebPIccProfile(this, fileData, (size_t)fileSize);
        delete[] fileData;
        return ok;
    }

    // Still WebP.
    int w = 0, h = 0;
    uint8_t *decoded = WebPDecodeRGBA(fileData, (size_t)fileSize, &w, &h);
    ExtractWebPIccProfile(this, fileData, (size_t)fileSize);
    delete[] fileData;
    if (!decoded)
    {
        LOGError("CImageData::LoadWebP: decode failed '%s'", fileName);
        return false;
    }

    this->width  = w;
    this->height = h;
    this->type   = IMG_TYPE_RGBA;
    size_t sz = (size_t)w * h * 4;
    this->resultData = new u8[sz];
    memcpy(this->resultData, decoded, sz);
    WebPFree(decoded);
    return true;
}

#else

bool CImageData::LoadWebP(const char *fileName)
{
    LOGError("CImageData::LoadWebP: WebP not supported on this build (%s)", fileName);
    return false;
}

#endif // MT_ENABLE_LIBWEBP
