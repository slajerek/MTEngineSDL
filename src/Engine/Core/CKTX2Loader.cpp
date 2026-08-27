// MT_CAP_KTX2. Value style -- `#if !defined(X) || (X)` -- because it is the one
// that survives a `-D X=0`: the #ifdef family defaults OFF when undefined and
// this defaults ON, which is what a standalone engine build needs.
//
// The idiom, from CImageDataTIFF.cpp: guard the BODY, leave the header
// unconditional, leave the file in all three build lists, degrade to a stub.
// Capability gating then needs no file-list churn.
//
// This file needed the guard WRITING. Unlike every other capability in this
// programme there was no existing off-path: it had no preprocessor guard at all
// and CImageData.cpp consumes it unconditionally, so there was nothing to switch
// on.
#if !defined(MT_ENABLE_KTX2) || (MT_ENABLE_KTX2)

#include "CKTX2Loader.h"
#include "DBG_Log.h"
#include <cstring>
#include <cstdint>
#include <cmath>

#define BCDEC_IMPLEMENTATION
#include "../Libs/bcdec.h"

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wshorten-64-to-32"
#define ETCDEC_IMPLEMENTATION
#include "../Libs/etcdec.h"
#pragma clang diagnostic pop

// basisu_astc_helpers.h requires basisu::vector (via basisu_containers.h) and assert().
// The implementations of both are compiled in basisu_transcoder.cpp — we only need declarations here.
#include <cassert>
#include "../Libs/basis_universal/transcoder/basisu_containers.h"
#include "../Libs/basis_universal/transcoder/basisu_astc_helpers.h"

// ============================================================
// VkFormat constants used in this file
// ============================================================
enum {
    VK_FORMAT_R8G8B8_UNORM  = 23,
    VK_FORMAT_R8G8B8_SRGB   = 29,
    VK_FORMAT_R8G8B8A8_UNORM = 37,
    VK_FORMAT_R8G8B8A8_SRGB  = 43,
    VK_FORMAT_R16G16B16A16_SFLOAT    = 97,
    VK_FORMAT_R32G32B32A32_SFLOAT    = 109,
    VK_FORMAT_B10G11R11_UFLOAT_PACK32 = 122,
    VK_FORMAT_E5B9G9R9_UFLOAT_PACK32  = 123,
    VK_FORMAT_BC1_RGB_UNORM_BLOCK  = 131,
    VK_FORMAT_BC1_RGB_SRGB_BLOCK   = 132,
    VK_FORMAT_BC1_RGBA_UNORM_BLOCK = 133,
    VK_FORMAT_BC1_RGBA_SRGB_BLOCK  = 134,
    VK_FORMAT_BC2_UNORM_BLOCK = 135,
    VK_FORMAT_BC2_SRGB_BLOCK  = 136,
    VK_FORMAT_BC3_UNORM_BLOCK = 137,
    VK_FORMAT_BC3_SRGB_BLOCK  = 138,
    VK_FORMAT_BC4_UNORM_BLOCK = 139,
    VK_FORMAT_BC4_SNORM_BLOCK = 140,
    VK_FORMAT_BC5_UNORM_BLOCK = 141,
    VK_FORMAT_BC5_SNORM_BLOCK = 142,
    VK_FORMAT_BC6H_UFLOAT_BLOCK = 143,
    VK_FORMAT_BC6H_SFLOAT_BLOCK = 144,
    VK_FORMAT_BC7_UNORM_BLOCK   = 145,
    VK_FORMAT_BC7_SRGB_BLOCK    = 146,
    VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK   = 147,
    VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK    = 148,
    VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK = 149,
    VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK  = 150,
    VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK = 151,
    VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK  = 152,
    VK_FORMAT_EAC_R11_UNORM_BLOCK  = 153,
    VK_FORMAT_EAC_R11_SNORM_BLOCK  = 154,
    VK_FORMAT_EAC_R11G11_UNORM_BLOCK = 155,
    VK_FORMAT_EAC_R11G11_SNORM_BLOCK = 156,
    VK_FORMAT_ASTC_4x4_UNORM_BLOCK = 157,
    VK_FORMAT_ASTC_4x4_SRGB_BLOCK  = 158,
};

// ============================================================
// KTX2 container reader
// ============================================================

static const uint8_t KTX2_ID[12] = {
    0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
};

bool KTX2_ReadHeaderForDispatch(const u8 *fileBytes, size_t fileSize,
                                KTX2HeaderInfo &out)
{
    // Minimum: 80-byte header + 24-byte level[0] index entry
    if (!fileBytes || fileSize < 104)
        return false;
    if (memcmp(fileBytes, KTX2_ID, 12) != 0)
        return false;

    uint32_t vkFormat, pixelWidth, pixelHeight, pixelDepth;
    uint32_t layerCount, faceCount, levelCount, supercomp;
    memcpy(&vkFormat,    fileBytes + 12, 4);
    memcpy(&pixelWidth,  fileBytes + 20, 4);
    memcpy(&pixelHeight, fileBytes + 24, 4);
    memcpy(&pixelDepth,  fileBytes + 28, 4);
    memcpy(&layerCount,  fileBytes + 32, 4);
    memcpy(&faceCount,   fileBytes + 36, 4);
    memcpy(&levelCount,  fileBytes + 40, 4);
    memcpy(&supercomp,   fileBytes + 44, 4);

    if (levelCount == 0 || pixelWidth == 0)
        return false;

    // Verify the full level index fits in the file
    uint64_t levelIndexEnd = (uint64_t)80 + (uint64_t)levelCount * 24;
    if (levelIndexEnd > (uint64_t)fileSize)
        return false;

    out.vkFormat               = vkFormat;
    out.supercompressionScheme = supercomp;
    out.pixelWidth             = pixelWidth;
    out.pixelHeight            = (pixelHeight == 0) ? 1 : pixelHeight;
    out.pixelDepth             = pixelDepth;
    out.layerCount             = layerCount;
    out.faceCount              = (faceCount == 0) ? 1 : faceCount;
    return true;
}

// ============================================================
// sRGB OETF: linear [0,1] float -> sRGB uint8
// ============================================================
static uint8_t LinearToSRGB8(float c)
{
    if (c <= 0.0f) return 0;
    if (c >= 1.0f) return 255;
    float s = (c <= 0.0031308f)
              ? 12.92f * c
              : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)(s * 255.0f + 0.5f);
}

// ============================================================
// HDR TRANSITION: CollapseLinearFloatToRGBA8
// Input:  linear float RGBA, w*h*4 floats (values nominally [0,1])
// Output: sRGB RGBA8, w*h*4 bytes
//
// To transition to HDR display: keep the float buffer, skip this collapse,
// upload a float texture to the EDR surface.  See specs/claude/architecture/
// ktx2-hdr-transition.md for the full checklist.
// ============================================================
static void CollapseLinearFloatToRGBA8(const float *src, uint32_t w, uint32_t h,
                                                        u8 *dst)
{
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        dst[i * 4 + 0] = LinearToSRGB8(src[i * 4 + 0]);
        dst[i * 4 + 1] = LinearToSRGB8(src[i * 4 + 1]);
        dst[i * 4 + 2] = LinearToSRGB8(src[i * 4 + 2]);
        float a = src[i * 4 + 3];
        dst[i * 4 + 3] = (a <= 0.0f) ? 0 : (a >= 1.0f) ? 255 : (uint8_t)(a * 255.0f + 0.5f);
    }
}

// ============================================================
// Raw 8-bit format decode: R8G8B8 and R8G8B8A8 (UNORM and SRGB)
// Output: RGBA8 sRGB (always 4 bytes/pixel)
// ============================================================
static bool DecodeRaw8(const u8 *src, size_t srcLen, uint32_t vkFmt,
                       uint32_t w, uint32_t h, u8 *dst)
{
    size_t n = (size_t)w * h;
    bool isSRGB  = (vkFmt == VK_FORMAT_R8G8B8_SRGB  || vkFmt == VK_FORMAT_R8G8B8A8_SRGB);
    bool hasAlpha = (vkFmt == VK_FORMAT_R8G8B8A8_UNORM || vkFmt == VK_FORMAT_R8G8B8A8_SRGB);

    if (hasAlpha) {
        if (srcLen < n * 4) return false;
        if (isSRGB) {
            memcpy(dst, src, n * 4);
        } else {
            // UNORM: linear bytes -> sRGB OETF
            for (size_t i = 0; i < n; i++) {
                dst[i*4+0] = LinearToSRGB8(src[i*4+0] / 255.0f);
                dst[i*4+1] = LinearToSRGB8(src[i*4+1] / 255.0f);
                dst[i*4+2] = LinearToSRGB8(src[i*4+2] / 255.0f);
                dst[i*4+3] = src[i*4+3]; // alpha: linear pass-through
            }
        }
    } else {
        // RGB: expand to RGBA
        if (srcLen < n * 3) return false;
        for (size_t i = 0; i < n; i++) {
            uint8_t r = src[i*3+0], g = src[i*3+1], b = src[i*3+2];
            if (!isSRGB) {
                r = LinearToSRGB8(r / 255.0f);
                g = LinearToSRGB8(g / 255.0f);
                b = LinearToSRGB8(b / 255.0f);
            }
            dst[i*4+0] = r;
            dst[i*4+1] = g;
            dst[i*4+2] = b;
            dst[i*4+3] = 255;
        }
    }
    return true;
}

// ============================================================
// BC block-compressed format decoders
// ============================================================

// Generic BC decoder that outputs 4 bytes/pixel (RGBA8) per block
// isLinear=true: decoded bytes are UNORM (linear) — apply sRGB OETF on RGB
// isLinear=false: decoded bytes are already sRGB-encoded — blit as-is
static bool DecodeBCRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                         u8 *dst, int blockBytes,
                         void (*decode_fn)(const void *, void *, int),
                         bool isLinear)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * blockBytes)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            uint8_t tmp[4 * 4 * 4]; // 4x4 RGBA8
            decode_fn(src + ((size_t)(y * bx + x) * blockBytes), tmp, 4 * 4);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    const u8 *t = tmp + (py * 4 + px) * 4;
                    u8 *d = dst + ((size_t)(dy * w + dx) * 4);
                    if (isLinear) {
                        d[0] = LinearToSRGB8(t[0] / 255.0f);
                        d[1] = LinearToSRGB8(t[1] / 255.0f);
                        d[2] = LinearToSRGB8(t[2] / 255.0f);
                        d[3] = t[3];
                    } else {
                        memcpy(d, t, 4);
                    }
                }
            }
        }
    }
    return true;
}

// BC4: R8 per pixel → expand to RGBA grayscale, apply sRGB OETF (both UNORM and SNORM are linear-origin)
static bool DecodeBC4toRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                             u8 *dst, bool isSnorm)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * BCDEC_BC4_BLOCK_SIZE)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            uint8_t tmp[4 * 4]; // 4x4 R8
            bcdec_bc4(src + (size_t)(y * bx + x) * BCDEC_BC4_BLOCK_SIZE, tmp, 4);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    uint8_t v = tmp[py * 4 + px];
                    uint8_t r;
                    if (isSnorm) {
                        uint8_t u = (uint8_t)std::clamp((int)(int8_t)v + 128, 0, 255);
                        r = LinearToSRGB8(u / 255.0f);
                    } else {
                        r = LinearToSRGB8(v / 255.0f);
                    }
                    u8 *p = dst + (size_t)(dy * w + dx) * 4;
                    p[0] = r; p[1] = r; p[2] = r; p[3] = 255;
                }
            }
        }
    }
    return true;
}

// BC5: RG8 per pixel (2 bytes) → RGBA with B=0, A=255, apply sRGB OETF (linear-origin)
static bool DecodeBC5toRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                             u8 *dst, bool isSnorm)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * BCDEC_BC5_BLOCK_SIZE)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            uint8_t tmp[4 * 4 * 2]; // 4x4 RG8
            bcdec_bc5(src + (size_t)(y * bx + x) * BCDEC_BC5_BLOCK_SIZE, tmp, 4 * 2);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    uint8_t rv = tmp[(py * 4 + px) * 2 + 0];
                    uint8_t gv = tmp[(py * 4 + px) * 2 + 1];
                    uint8_t r, g;
                    if (isSnorm) {
                        r = LinearToSRGB8((uint8_t)std::clamp((int)(int8_t)rv + 128, 0, 255) / 255.0f);
                        g = LinearToSRGB8((uint8_t)std::clamp((int)(int8_t)gv + 128, 0, 255) / 255.0f);
                    } else {
                        r = LinearToSRGB8(rv / 255.0f);
                        g = LinearToSRGB8(gv / 255.0f);
                    }
                    u8 *p = dst + (size_t)(dy * w + dx) * 4;
                    p[0] = r; p[1] = g; p[2] = 0; p[3] = 255;
                }
            }
        }
    }
    return true;
}

// BC6H: 3 floats/pixel (RGB) → linear float4 → CollapseLinearFloatToRGBA8
static bool DecodeBC6HtoRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                              u8 *dst, int isSigned)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * BCDEC_BC6H_BLOCK_SIZE)
        return false;
    // Decode to linear float RGBA (pad alpha=1)
    uint64_t floatBufBytes = (uint64_t)w * h * 4 * sizeof(float);
    float *fbuf = new float[(size_t)floatBufBytes / sizeof(float)];
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            float tmp[4 * 4 * 3]; // 4x4 RGB float
            bcdec_bc6h_float(src + (size_t)(y * bx + x) * BCDEC_BC6H_BLOCK_SIZE,
                             tmp, 4 * 3, isSigned);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    const float *s = tmp + (py * 4 + px) * 3;
                    float *d = fbuf + (size_t)(dy * w + dx) * 4;
                    d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d[3] = 1.0f;
                }
            }
        }
    }
    CollapseLinearFloatToRGBA8(fbuf, w, h, dst);
    delete[] fbuf;
    return true;
}

// ============================================================
// ETC1/ETC2/EAC block-compressed format decoders
// ============================================================

// ETC1/ETC2 RGB(A): etcdec outputs RGBA8 directly
static bool DecodeETCRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                          u8 *dst, int blockBytes,
                          void (*decode_fn)(const void *, void *, int))
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * blockBytes)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            uint8_t tmp[4 * 4 * 4];
            decode_fn(src + (size_t)(y * bx + x) * blockBytes, tmp, 4 * 4);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    memcpy(dst + (size_t)(dy * w + dx) * 4,
                           tmp + (py * 4 + px) * 4, 4);
                }
            }
        }
    }
    return true;
}

// Apply sRGB OETF to R,G,B channels in-place (A unchanged)
static void ApplySRGBOETF(u8 *rgba8, uint32_t w, uint32_t h)
{
    size_t n = (size_t)w * h;
    for (size_t i = 0; i < n; i++) {
        rgba8[i*4+0] = LinearToSRGB8(rgba8[i*4+0] / 255.0f);
        rgba8[i*4+1] = LinearToSRGB8(rgba8[i*4+1] / 255.0f);
        rgba8[i*4+2] = LinearToSRGB8(rgba8[i*4+2] / 255.0f);
    }
}

// EAC R11: single-channel float/pixel → grayscale RGBA
static bool DecodeEACR11toRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                                u8 *dst, int isSigned)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * ETCDEC_EAC_R11_BLOCK_SIZE)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            float tmp[4 * 4]; // one float per pixel
            etcdec_eac_r11_float(src + (size_t)(y * bx + x) * ETCDEC_EAC_R11_BLOCK_SIZE,
                                 tmp, 4 * (int)sizeof(float), isSigned);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    float v = tmp[py * 4 + px];
                    if (isSigned) v = v * 0.5f + 0.5f; // remap [-1,1] to [0,1]
                    uint8_t c = LinearToSRGB8(v);
                    u8 *p = dst + (size_t)(dy * w + dx) * 4;
                    p[0] = c; p[1] = c; p[2] = c; p[3] = 255;
                }
            }
        }
    }
    return true;
}

// EAC RG11: two-channel float/pixel → (R,G,0,255) RGBA
static bool DecodeEACRG11toRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                                 u8 *dst, int isSigned)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    if (srcLen < (size_t)bx * by * ETCDEC_EAC_RG11_BLOCK_SIZE)
        return false;
    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            float tmp[4 * 4 * 2]; // two floats per pixel
            etcdec_eac_rg11_float(src + (size_t)(y * bx + x) * ETCDEC_EAC_RG11_BLOCK_SIZE,
                                  tmp, 4 * 2 * (int)sizeof(float), isSigned);
            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    float r = tmp[(py * 4 + px) * 2 + 0];
                    float g = tmp[(py * 4 + px) * 2 + 1];
                    if (isSigned) { r = r * 0.5f + 0.5f; g = g * 0.5f + 0.5f; }
                    u8 *p = dst + (size_t)(dy * w + dx) * 4;
                    p[0] = LinearToSRGB8(r);
                    p[1] = LinearToSRGB8(g);
                    p[2] = 0; p[3] = 255;
                }
            }
        }
    }
    return true;
}

static bool DecodeASTC4x4toRGBA(const u8 *src, size_t srcLen, uint32_t w, uint32_t h,
                                 u8 *dst)
{
    int bx = ((int)w + 3) / 4;
    int by = ((int)h + 3) / 4;
    size_t bytesPerBlock = 16; // ASTC blocks are always 16 bytes
    if (srcLen < (size_t)bx * by * bytesPerBlock)
        return false;

    for (int y = 0; y < by; y++) {
        for (int x = 0; x < bx; x++) {
            const void *blk = src + (size_t)(y * bx + x) * bytesPerBlock;
            astc_helpers::log_astc_block logBlk{};
            if (!astc_helpers::unpack_block(blk, logBlk, 4, 4))
                return false; // malformed block

            uint8_t tmp[4 * 4 * 4]; // 4x4 RGBA8 (cDecodeModeLDR8 returns uint8_t)
            if (!astc_helpers::decode_block(logBlk, tmp, 4, 4,
                                            astc_helpers::cDecodeModeLDR8))
                return false;

            for (int py = 0; py < 4; py++) {
                int dy = y * 4 + py;
                if (dy >= (int)h) break;
                for (int px = 0; px < 4; px++) {
                    int dx = x * 4 + px;
                    if (dx >= (int)w) break;
                    memcpy(dst + (size_t)(dy * w + dx) * 4,
                           tmp + (py * 4 + px) * 4, 4);
                }
            }
        }
    }
    return true;
}

// ============================================================
// HDR float format decoders
// ============================================================

// RGBA16F: 4 x half-float per pixel
static float HalfToFloat(uint16_t h)
{
    uint32_t exp  = (h >> 10) & 0x1F;
    uint32_t mant = h & 0x3FF;
    uint32_t sign = (h >> 15) & 1;
    uint32_t f;
    if (exp == 0) {
        f = (mant == 0) ? 0 : (sign << 31) | (((127 - 15 - 1) + 1) << 23) | (mant << 13);
    } else if (exp == 31) {
        f = (sign << 31) | (0xFF << 23) | (mant << 13); // inf or NaN
    } else {
        f = (sign << 31) | ((exp + (127 - 15)) << 23) | (mant << 13);
    }
    float result;
    memcpy(&result, &f, 4);
    return result;
}

static bool DecodeRGBA16FtoRGBA8(const u8 *src, size_t srcLen,
                                  uint32_t w, uint32_t h, u8 *dst)
{
    size_t n = (size_t)w * h;
    if (srcLen < n * 8) return false; // 4 x uint16 per pixel
    float *fbuf = new float[n * 4];
    for (size_t i = 0; i < n; i++) {
        const uint16_t *p = reinterpret_cast<const uint16_t *>(src) + i * 4;
        fbuf[i*4+0] = HalfToFloat(p[0]);
        fbuf[i*4+1] = HalfToFloat(p[1]);
        fbuf[i*4+2] = HalfToFloat(p[2]);
        fbuf[i*4+3] = HalfToFloat(p[3]);
    }
    // HDR TRANSITION: when EDR surface available, keep fbuf and skip collapse.
    // See specs/claude/architecture/ktx2-hdr-transition.md.
    CollapseLinearFloatToRGBA8(fbuf, w, h, dst);
    delete[] fbuf;
    return true;
}

static bool DecodeRGBA32FtoRGBA8(const u8 *src, size_t srcLen,
                                  uint32_t w, uint32_t h, u8 *dst)
{
    size_t n = (size_t)w * h;
    if (srcLen < n * 16) return false; // 4 x float32 per pixel
    const float *fdata = reinterpret_cast<const float *>(src);
    // HDR TRANSITION: when EDR surface available, skip collapse and return float buffer.
    CollapseLinearFloatToRGBA8(fdata, w, h, dst);
    return true;
}

// B10G11R11_UFLOAT_PACK32: packed 32-bit, 11-bit R, 11-bit G, 10-bit B (unsigned floats)
// Vulkan spec: bits[10:0]=R(11), bits[21:11]=G(11), bits[31:22]=B(10)
static float Unpack11BitUFloat(uint32_t v)
{
    // 11-bit: 5 exponent bits, 6 mantissa bits, no sign bit
    uint32_t exp  = (v >> 6) & 0x1F;
    uint32_t mant = v & 0x3F;
    if (exp == 0)  return mant == 0 ? 0.0f : (float)mant / (64.0f * (1 << 14));
    if (exp == 31) return mant ? 0.0f : 65504.0f; // inf/NaN → clamp
    uint32_t f32 = ((exp + (127 - 15)) << 23) | (mant << 17);
    float r; memcpy(&r, &f32, 4); return r;
}
static float Unpack10BitUFloat(uint32_t v)
{
    // 10-bit: 5 exponent bits, 5 mantissa bits, no sign bit
    uint32_t exp  = (v >> 5) & 0x1F;
    uint32_t mant = v & 0x1F;
    if (exp == 0)  return mant == 0 ? 0.0f : (float)mant / (32.0f * (1 << 14));
    if (exp == 31) return mant ? 0.0f : 65504.0f;
    uint32_t f32 = ((exp + (127 - 15)) << 23) | (mant << 18);
    float r; memcpy(&r, &f32, 4); return r;
}

static bool DecodeB10G11R11toRGBA8(const u8 *src, size_t srcLen,
                                    uint32_t w, uint32_t h, u8 *dst)
{
    size_t n = (size_t)w * h;
    if (srcLen < n * 4) return false;
    float *fbuf = new float[n * 4];
    for (size_t i = 0; i < n; i++) {
        uint32_t p; memcpy(&p, src + i * 4, 4);
        fbuf[i*4+0] = Unpack11BitUFloat(p & 0x7FF);           // R: bits[10:0]
        fbuf[i*4+1] = Unpack11BitUFloat((p >> 11) & 0x7FF);   // G: bits[21:11]
        fbuf[i*4+2] = Unpack10BitUFloat((p >> 22) & 0x3FF);   // B: bits[31:22]
        fbuf[i*4+3] = 1.0f;
    }
    // HDR TRANSITION: when EDR surface available, skip collapse.
    CollapseLinearFloatToRGBA8(fbuf, w, h, dst);
    delete[] fbuf;
    return true;
}

// E5B9G9R9_UFLOAT_PACK32: RGB9E5 shared exponent
// Vulkan spec: bits[31:27]=exp, bits[26:18]=B, bits[17:9]=G, bits[8:0]=R
static bool DecodeRGB9E5toRGBA8(const u8 *src, size_t srcLen,
                                 uint32_t w, uint32_t h, u8 *dst)
{
    size_t n = (size_t)w * h;
    if (srcLen < n * 4) return false;
    float *fbuf = new float[n * 4];
    for (size_t i = 0; i < n; i++) {
        uint32_t p; memcpy(&p, src + i * 4, 4);
        uint32_t exp = (p >> 27) & 0x1F;
        uint32_t br  = (p >> 18) & 0x1FF; // B mantissa
        uint32_t gr  = (p >>  9) & 0x1FF; // G mantissa
        uint32_t rr  =  p        & 0x1FF; // R mantissa
        float scale = ldexpf(1.0f, (int)exp - 24); // 2^(exp - 15 - 9)
        fbuf[i*4+0] = (float)rr * scale;
        fbuf[i*4+1] = (float)gr * scale;
        fbuf[i*4+2] = (float)br * scale;
        fbuf[i*4+3] = 1.0f;
    }
    // HDR TRANSITION: when EDR surface available, skip collapse.
    CollapseLinearFloatToRGBA8(fbuf, w, h, dst);
    delete[] fbuf;
    return true;
}

// ============================================================
// Main decode entry point
// ============================================================
bool KTX2_DecodeToRGBA8(const u8 *fileBytes, size_t fileSize,
                        KTX2DecodeResult &out)
{
    KTX2HeaderInfo hdr;
    if (!KTX2_ReadHeaderForDispatch(fileBytes, fileSize, hdr))
        return false;

    // Only concrete-format with no supercompression
    if (hdr.vkFormat == 0 || hdr.supercompressionScheme != 0)
        return false;

    // Read level-0 byte offset and length from the level index at byte 80
    uint64_t byteOffset, byteLength;
    memcpy(&byteOffset, fileBytes + 80, 8);
    memcpy(&byteLength, fileBytes + 88, 8);

    if (byteLength == 0)
        return false;
    if (byteOffset > (uint64_t)fileSize)
        return false;
    if (byteLength > (uint64_t)fileSize - byteOffset)
        return false;

    uint32_t w = hdr.pixelWidth;
    uint32_t h = hdr.pixelHeight;   // already normalized to >= 1

    if (w > 65536 || h > 65536)
        return false;

    uint64_t outBytes = (uint64_t)w * h * 4;
    if (outBytes == 0 || outBytes > (uint64_t)512 * 1024 * 1024)
        return false;

    const u8 *levelData = fileBytes + (size_t)byteOffset;

    out.rgba8 = new u8[(size_t)outBytes];
    out.width  = (int)w;
    out.height = (int)h;

    bool ok = false;
    switch (hdr.vkFormat) {
    case VK_FORMAT_R8G8B8_UNORM:
    case VK_FORMAT_R8G8B8_SRGB:
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        ok = DecodeRaw8(levelData, (size_t)byteLength, hdr.vkFormat, w, h, out.rgba8);
        break;

    case VK_FORMAT_BC1_RGB_UNORM_BLOCK:
    case VK_FORMAT_BC1_RGBA_UNORM_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC1_BLOCK_SIZE, bcdec_bc1, /*isLinear=*/true);
        // BC1_RGB: force alpha=255 (no meaningful alpha in BC1_RGB)
        if (ok && hdr.vkFormat == VK_FORMAT_BC1_RGB_UNORM_BLOCK) {
            for (size_t i = 0; i < (size_t)w * h; i++)
                out.rgba8[i * 4 + 3] = 255;
        }
        break;

    case VK_FORMAT_BC1_RGB_SRGB_BLOCK:
    case VK_FORMAT_BC1_RGBA_SRGB_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC1_BLOCK_SIZE, bcdec_bc1, /*isLinear=*/false);
        // BC1_RGB: force alpha=255
        if (ok && hdr.vkFormat == VK_FORMAT_BC1_RGB_SRGB_BLOCK) {
            for (size_t i = 0; i < (size_t)w * h; i++)
                out.rgba8[i * 4 + 3] = 255;
        }
        break;

    case VK_FORMAT_BC2_UNORM_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC2_BLOCK_SIZE, bcdec_bc2, /*isLinear=*/true);
        break;

    case VK_FORMAT_BC2_SRGB_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC2_BLOCK_SIZE, bcdec_bc2, /*isLinear=*/false);
        break;

    case VK_FORMAT_BC3_UNORM_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC3_BLOCK_SIZE, bcdec_bc3, /*isLinear=*/true);
        break;

    case VK_FORMAT_BC3_SRGB_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC3_BLOCK_SIZE, bcdec_bc3, /*isLinear=*/false);
        break;

    case VK_FORMAT_BC4_UNORM_BLOCK:
        ok = DecodeBC4toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, /*isSnorm=*/false);
        break;

    case VK_FORMAT_BC4_SNORM_BLOCK:
        ok = DecodeBC4toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, /*isSnorm=*/true);
        break;

    case VK_FORMAT_BC5_UNORM_BLOCK:
        ok = DecodeBC5toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, /*isSnorm=*/false);
        break;

    case VK_FORMAT_BC5_SNORM_BLOCK:
        ok = DecodeBC5toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, /*isSnorm=*/true);
        break;

    case VK_FORMAT_BC6H_UFLOAT_BLOCK:
        ok = DecodeBC6HtoRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 0);
        break;

    case VK_FORMAT_BC6H_SFLOAT_BLOCK:
        ok = DecodeBC6HtoRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 1);
        break;

    case VK_FORMAT_BC7_UNORM_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC7_BLOCK_SIZE, bcdec_bc7, /*isLinear=*/true);
        break;

    case VK_FORMAT_BC7_SRGB_BLOCK:
        ok = DecodeBCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                          BCDEC_BC7_BLOCK_SIZE, bcdec_bc7, /*isLinear=*/false);
        break;

    case VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8_SRGB_BLOCK:
        ok = DecodeETCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                           ETCDEC_ETC_RGB_BLOCK_SIZE, etcdec_etc_rgb);
        if (ok && hdr.vkFormat == VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK)
            ApplySRGBOETF(out.rgba8, w, h);
        break;

    case VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A1_SRGB_BLOCK:
        ok = DecodeETCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                           ETCDEC_ETC_RGB_A1_BLOCK_SIZE, etcdec_etc_rgb_a1);
        if (ok && hdr.vkFormat == VK_FORMAT_ETC2_R8G8B8A1_UNORM_BLOCK)
            ApplySRGBOETF(out.rgba8, w, h);
        break;

    case VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK:
    case VK_FORMAT_ETC2_R8G8B8A8_SRGB_BLOCK:
        ok = DecodeETCRGBA(levelData, (size_t)byteLength, w, h, out.rgba8,
                           ETCDEC_EAC_RGBA_BLOCK_SIZE, etcdec_eac_rgba);
        if (ok && hdr.vkFormat == VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK)
            ApplySRGBOETF(out.rgba8, w, h);
        break;

    case VK_FORMAT_EAC_R11_UNORM_BLOCK:
        ok = DecodeEACR11toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 0);
        break;
    case VK_FORMAT_EAC_R11_SNORM_BLOCK:
        ok = DecodeEACR11toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 1);
        break;
    case VK_FORMAT_EAC_R11G11_UNORM_BLOCK:
        ok = DecodeEACRG11toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 0);
        break;
    case VK_FORMAT_EAC_R11G11_SNORM_BLOCK:
        ok = DecodeEACRG11toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8, 1);
        break;

    case VK_FORMAT_ASTC_4x4_UNORM_BLOCK:
    case VK_FORMAT_ASTC_4x4_SRGB_BLOCK:
        ok = DecodeASTC4x4toRGBA(levelData, (size_t)byteLength, w, h, out.rgba8);
        break;

    case VK_FORMAT_R16G16B16A16_SFLOAT:
        ok = DecodeRGBA16FtoRGBA8(levelData, (size_t)byteLength, w, h, out.rgba8);
        break;

    case VK_FORMAT_R32G32B32A32_SFLOAT:
        ok = DecodeRGBA32FtoRGBA8(levelData, (size_t)byteLength, w, h, out.rgba8);
        break;

    case VK_FORMAT_B10G11R11_UFLOAT_PACK32:
        ok = DecodeB10G11R11toRGBA8(levelData, (size_t)byteLength, w, h, out.rgba8);
        break;

    case VK_FORMAT_E5B9G9R9_UFLOAT_PACK32:
        ok = DecodeRGB9E5toRGBA8(levelData, (size_t)byteLength, w, h, out.rgba8);
        break;

    default:
        LOGError("CKTX2Loader: vkFormat %u not yet implemented", hdr.vkFormat);
        break;
    }

    if (!ok) {
        delete[] out.rgba8;
        out.rgba8 = nullptr;
    }
    return ok;
}

#else // MT_ENABLE_KTX2 == 0

// Stub. Both entry points already return false for an unsupported format or a
// bounds error, and every caller handles that -- so "KTX2 is not compiled in"
// reaches CImageData through a path it already exercises, rather than through a
// new one.
#include "CKTX2Loader.h"

bool KTX2_ReadHeaderForDispatch(const u8 *, size_t, KTX2HeaderInfo &)
{
	return false;
}

bool KTX2_DecodeToRGBA8(const u8 *, size_t, KTX2DecodeResult &)
{
	return false;
}

#endif // MT_ENABLE_KTX2
