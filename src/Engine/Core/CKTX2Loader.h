#pragma once
#include "SYS_Defs.h"
#include <stddef.h>
#include <stdint.h>

struct KTX2HeaderInfo {
    uint32_t vkFormat               = 0;
    uint32_t supercompressionScheme = 0;
    uint32_t pixelWidth             = 0;
    uint32_t pixelHeight            = 0;
    uint32_t pixelDepth             = 0;
    uint32_t layerCount             = 0;
    uint32_t faceCount              = 0;
};

struct KTX2DecodeResult {
    int   width  = 0;
    int   height = 0;
    u8   *rgba8  = nullptr;   // new[]; caller frees with delete[]

    // HDR TRANSITION (future): when an EDR display surface exists and the engine
    // gains a float texture upload path, expose linear float data here instead of
    // collapsing to SDR. See specs/claude/architecture/ktx2-hdr-transition.md.
    // Always null in the current SDR build.
    float *linearFloat = nullptr;
};

// Parse identifier, header, and level-0 index.  Returns false on any
// format/bounds error.  Safe to call on any byte buffer.
bool KTX2_ReadHeaderForDispatch(const u8 *fileBytes, size_t fileSize,
                                KTX2HeaderInfo &out);

// Decode mip-0, layer-0, face-0 (depth-slice-0) of a concrete-format KTX2
// file to RGBA8.  Caller must free out.rgba8 with delete[].  Returns false
// (no allocation) on any unsupported format or bounds error.
bool KTX2_DecodeToRGBA8(const u8 *fileBytes, size_t fileSize,
                        KTX2DecodeResult &out);
