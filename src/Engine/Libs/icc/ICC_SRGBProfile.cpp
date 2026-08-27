#include "ICC_SRGBProfile.h"

#include <cstring>

// Minimal ICC v2 matrix/TRC sRGB profile builder. Portable by construction:
// pure <cstdint>/<vector>/<cstring> byte assembly, no platform #ifdefs, no
// clock/random -- the output is byte-for-byte identical on every platform
// and every call. Layout follows ICC.1:2001-04 (ICC v2):
//   [0..127]    128-byte profile header
//   [128..)     tag table: uint32 count, then count * (sig, offset, size)
//   [.. end)    tag data, referenced by the tag table
//
// Colorimetry (decision #2, published IEC 61966-2.1 constants):
//   - PCS illuminant and white point: D50 (the required ICC PCS adaptation
//     white, not sRGB's native D65 -- this is standard practice; every
//     bundled sRGB v2 profile chromatically adapts D65-native primaries to
//     D50 for the profile connection space).
//   - rXYZ/gXYZ/bXYZ: the D50-adapted sRGB primaries used by widely shipped
//     sRGB v2 profiles.
//   - TRC: single-gamma curv, 2.2 encoded as u8Fixed8 0x0233 (563/256 =
//     2.19921875). NOT 0x0132 (1.195) -- a prior review caught that
//     transposed-looking but wrong value; the shared curv payload is the one
//     numeric fact a purely structural test cannot catch, so the app-side
//     test asserts it explicitly.

namespace
{
    void PutU32(std::vector<uint8_t> &buf, uint32_t v)
    {
        buf.push_back(uint8_t((v >> 24) & 0xFF));
        buf.push_back(uint8_t((v >> 16) & 0xFF));
        buf.push_back(uint8_t((v >> 8) & 0xFF));
        buf.push_back(uint8_t(v & 0xFF));
    }

    void PutU16(std::vector<uint8_t> &buf, uint16_t v)
    {
        buf.push_back(uint8_t((v >> 8) & 0xFF));
        buf.push_back(uint8_t(v & 0xFF));
    }

    void PutSig(std::vector<uint8_t> &buf, const char *sig4)
    {
        buf.insert(buf.end(), sig4, sig4 + 4);
    }

    void PutBytes(std::vector<uint8_t> &buf, const void *data, size_t n)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        buf.insert(buf.end(), p, p + n);
    }

    // Zero-pad the buffer up to the next multiple of 4 bytes (ICC tag data
    // must be 4-byte aligned).
    void Pad4(std::vector<uint8_t> &buf)
    {
        while (buf.size() % 4 != 0) buf.push_back(0);
    }

    // s15Fixed16Number: signed 16.16 fixed point, big-endian.
    void PutS15Fixed16(std::vector<uint8_t> &buf, double value)
    {
        int32_t fixed = (int32_t)(value * 65536.0 + (value >= 0.0 ? 0.5 : -0.5));
        PutU32(buf, (uint32_t)fixed);
    }

    // XYZType tag ("XYZ "): sig + reserved(4) + one XYZ triplet. 20 bytes,
    // already 4-byte aligned.
    std::vector<uint8_t> BuildXYZTag(double x, double y, double z)
    {
        std::vector<uint8_t> t;
        PutSig(t, "XYZ ");
        PutU32(t, 0); // reserved
        PutS15Fixed16(t, x);
        PutS15Fixed16(t, y);
        PutS15Fixed16(t, z);
        return t;
    }

    // textDescriptionType ("desc"), ICC v2 6.5.17: sig + reserved(4) +
    // asciiCount(4) + ascii bytes (incl NUL) + unicode lang(4) + uniCount(4)
    // [+ unicode chars] + scriptcode code(2) + mac count(1) + 67-byte mac
    // field. We emit ASCII-only (unicode/mac fields zeroed/empty).
    std::vector<uint8_t> BuildDescTag(const char *asciiText)
    {
        std::vector<uint8_t> t;
        PutSig(t, "desc");
        PutU32(t, 0); // reserved
        const size_t asciiLen = std::strlen(asciiText) + 1; // incl NUL
        PutU32(t, (uint32_t)asciiLen);
        PutBytes(t, asciiText, asciiLen);
        PutU32(t, 0); // unicode language code
        PutU32(t, 0); // unicode count (none)
        PutU16(t, 0); // scriptcode code
        t.push_back(0); // Macintosh description count
        for (int i = 0; i < 67; i++) t.push_back(0); // fixed 67-byte mac field
        Pad4(t);
        return t;
    }

    // textType ("text"): sig + reserved(4) + NUL-terminated ASCII string.
    std::vector<uint8_t> BuildTextTag(const char *asciiText)
    {
        std::vector<uint8_t> t;
        PutSig(t, "text");
        PutU32(t, 0); // reserved
        const size_t len = std::strlen(asciiText) + 1; // incl NUL
        PutBytes(t, asciiText, len);
        Pad4(t);
        return t;
    }

    // curveType ("curv") with a single-entry table: sig + reserved(4) +
    // count(4) + count * u16. count=1 encodes a simple gamma value in
    // u8Fixed8 (value/256).
    std::vector<uint8_t> BuildCurveTag(uint16_t gammaU8Fixed8)
    {
        std::vector<uint8_t> t;
        PutSig(t, "curv");
        PutU32(t, 0); // reserved
        PutU32(t, 1); // count = 1 (single gamma entry)
        PutU16(t, gammaU8Fixed8);
        Pad4(t);
        return t;
    }
}

// Both built-in profiles are the same minimal matrix/TRC shape and differ only
// in their description and primaries, so the assembly lives here once. The
// sRGB caller passes exactly the values it used before this was factored out,
// which keeps its bytes -- and therefore its content digest, which the export
// path and the transform cache both depend on -- unchanged.
static std::vector<uint8_t> BuildMatrixTrcProfile(const char *description,
                                                  double rX, double rY, double rZ,
                                                  double gX, double gY, double gZ,
                                                  double bX, double bY, double bZ,
                                                  uint16_t gammaU8Fixed8)
{
    // ---------------------------------------------------------------- tags
    // Built once; rTRC/gTRC/bTRC all reference the SAME curv blob (minimal
    // single-gamma profile -- one shared curve, not three copies).
    const std::vector<uint8_t> descTag = BuildDescTag(description);
    const std::vector<uint8_t> wtptTag = BuildXYZTag(0.9642, 1.0000, 0.8249); // D50 white point
    const std::vector<uint8_t> rXYZTag = BuildXYZTag(rX, rY, rZ);
    const std::vector<uint8_t> gXYZTag = BuildXYZTag(gX, gY, gZ);
    const std::vector<uint8_t> bXYZTag = BuildXYZTag(bX, bY, bZ);
    const std::vector<uint8_t> curvTag = BuildCurveTag(gammaU8Fixed8);
    const std::vector<uint8_t> cprtTag = BuildTextTag("No copyright, use freely");

    struct TagRef { const char *sig; const std::vector<uint8_t> *data; };
    const TagRef tags[] = {
        { "desc", &descTag },
        { "wtpt", &wtptTag },
        { "rXYZ", &rXYZTag },
        { "gXYZ", &gXYZTag },
        { "bXYZ", &bXYZTag },
        { "rTRC", &curvTag },
        { "gTRC", &curvTag },
        { "bTRC", &curvTag },
        { "cprt", &cprtTag },
    };
    const size_t tagCountN = sizeof(tags) / sizeof(tags[0]); // 9
    const uint32_t tagCount = (uint32_t)tagCountN;

    // ---------------------------------------- lay out tag data, offsets
    // Identical data pointers (the shared curv blob) collapse to ONE offset
    // so rTRC/gTRC/bTRC point at the same bytes instead of being duplicated.
    const size_t headerSize = 128;
    const size_t tagTableSize = 4 + (size_t)tagCount * 12;
    size_t cursor = headerSize + tagTableSize;

    std::vector<uint32_t> offsets(tagCountN, 0);
    std::vector<uint32_t> sizes(tagCountN, 0);
    std::vector<const std::vector<uint8_t> *> seenPtrs;
    std::vector<uint32_t> seenOffsets;
    std::vector<uint8_t> tagDataBlob;

    for (size_t i = 0; i < tagCountN; i++)
    {
        const std::vector<uint8_t> *d = tags[i].data;
        int foundIdx = -1;
        for (size_t j = 0; j < seenPtrs.size(); j++)
        {
            if (seenPtrs[j] == d) { foundIdx = (int)j; break; }
        }
        if (foundIdx >= 0)
        {
            offsets[i] = seenOffsets[(size_t)foundIdx];
            sizes[i] = (uint32_t)d->size();
        }
        else
        {
            offsets[i] = (uint32_t)cursor;
            sizes[i] = (uint32_t)d->size();
            tagDataBlob.insert(tagDataBlob.end(), d->begin(), d->end());
            cursor += d->size();
            seenPtrs.push_back(d);
            seenOffsets.push_back(offsets[i]);
        }
    }

    const uint32_t totalSize = (uint32_t)cursor;

    // ---------------------------------------------------------------- header
    std::vector<uint8_t> p;
    p.reserve(totalSize);

    PutU32(p, totalSize);                // 0: profile size
    PutU32(p, 0);                        // 4: CMM type (none)
    PutU32(p, 0x02100000);               // 8: profile version 2.1.0
    PutSig(p, "mntr");                   // 12: device class = display
    PutSig(p, "RGB ");                   // 16: colour space = RGB
    PutSig(p, "XYZ ");                   // 20: PCS = XYZ
    // 24..35: date/time, fixed (deterministic, no clock): 2026-01-01T00:00:00
    PutU16(p, 2026); PutU16(p, 1); PutU16(p, 1);
    PutU16(p, 0);    PutU16(p, 0); PutU16(p, 0);
    PutSig(p, "acsp");                   // 36: profile file signature
    PutU32(p, 0);                        // 40: primary platform (none)
    PutU32(p, 0);                        // 44: profile flags
    PutU32(p, 0);                        // 48: device manufacturer
    PutU32(p, 0);                        // 52: device model
    PutU32(p, 0); PutU32(p, 0);          // 56..63: device attributes (8 bytes)
    PutU32(p, 0);                        // 64: rendering intent (perceptual)
    // 68..79: PCS illuminant = D50, s15Fixed16 XYZ
    PutU32(p, 0x0000F6D6);
    PutU32(p, 0x00010000);
    PutU32(p, 0x0000D32D);
    PutU32(p, 0);                        // 80: profile creator
    for (int i = 0; i < 44; i++) p.push_back(0); // 84..127: reserved

    // ------------------------------------------------------------ tag table
    PutU32(p, tagCount);
    for (size_t i = 0; i < tagCountN; i++)
    {
        PutSig(p, tags[i].sig);
        PutU32(p, offsets[i]);
        PutU32(p, sizes[i]);
    }

    // ------------------------------------------------------------- tag data
    p.insert(p.end(), tagDataBlob.begin(), tagDataBlob.end());

    return p;
}


std::vector<uint8_t> ICC_BuildSRGBProfileV2()
{
    // D50-adapted sRGB primaries, gamma 2.2 as u8Fixed8 0x0233 (563/256) --
    // NOT 0x0132. See the notes at the top of this file.
    return BuildMatrixTrcProfile("sRGB",
                                 0.43607, 0.22249, 0.01392,
                                 0.38515, 0.71687, 0.09708,
                                 0.14307, 0.06061, 0.71410,
                                 0x0233);
}

std::vector<uint8_t> ICC_BuildAdobeRGBProfileV2()
{
    // D50-adapted Adobe RGB (1998) primaries, from the published colorimetry.
    // They sum to the D50 white point exactly (X 0.96421, Y 1.00000,
    // Z 0.82491), which is the arithmetic check that the adaptation is right.
    //
    // The 563/256 gamma is EXACT for Adobe RGB (1998), unlike sRGB where the
    // same encoding is an approximation of a piecewise curve.
    //
    // The description deliberately reads "Compatible with Adobe RGB (1998)"
    // rather than naming the profile outright: these are our own bytes built
    // from published colorimetry, no Adobe-distributed file is shipped, and
    // this binary goes to the App Store and the Microsoft Store.
    return BuildMatrixTrcProfile("Compatible with Adobe RGB (1998)",
                                 0.60974, 0.31111, 0.01947,
                                 0.20528, 0.62567, 0.06087,
                                 0.14919, 0.06322, 0.74457,
                                 0x0233);
}

std::vector<uint8_t> ICC_BuildLinearProPhotoProfileV2()
{
    // RD-C #7.1. Native-D50 primaries written UNADAPTED -- see the header
    // comment for the by-analogy trap this deliberately avoids. Columns are
    // the published ISO 22028-2 ROMM matrix; column sums are exactly the ICC
    // PCS D50 (X 0.9642, Y 1.0000, Z 0.8249). Gamma 1.0 = u8Fixed8 0x0100,
    // exactly representable (no quantisation caveat like the sRGB 0x0233).
    return BuildMatrixTrcProfile("Linear ProPhoto RGB",
                                 0.7977, 0.2880, 0.0000,
                                 0.1352, 0.7119, 0.0000,
                                 0.0313, 0.0001, 0.8249,
                                 0x0100);
}

std::vector<uint8_t> ICC_BuildDisplayP3ProfileV2()
{
    // CM-F: D50-adapted (Bradford) Display P3 colorants -- P3 primaries
    // (R .680/.320, G .265/.690, B .150/.060), D65 white -- matching Apple's
    // canonical profile's values. They sum to the D50 white point (X 0.96420,
    // Y 1.00002, Z 0.82491), the same arithmetic check the siblings satisfy;
    // rZ is legitimately (slightly) negative and s15Fixed16 encodes it fine.
    //
    // TRC: the same 0x0233 single-gamma approximation as the sRGB builtin,
    // NOT an attempt at the piecewise curve -- Display P3's transfer function
    // is sRGB's, and approximating both identically makes the error cancel in
    // sRGB<->P3 round trips through our own profiles (spec #8).
    return BuildMatrixTrcProfile("Display P3",
                                 0.51512, 0.24120, -0.00105,
                                 0.29198, 0.69225,  0.04189,
                                 0.15710, 0.06657,  0.78407,
                                 0x0233);
}

std::vector<uint8_t> ICC_BuildRec709ProfileV2()
{
    // CM-E: Rec.709 shares sRGB's primaries and white point exactly -- the
    // matrix columns are byte-identical to ICC_BuildSRGBProfileV2()'s. Only
    // the TRC differs: single-gamma curv 2.4, the universal LUT-workflow
    // approximation of the BT.1886 reference EOTF (whose black-level term is
    // display-dependent and cannot live in a deterministic profile).
    //
    // Gamma quantisation: u8Fixed8 0x0266 = 614/256 = 2.3984375, the nearest
    // encodable value to 2.4 (2.4 * 256 = 614.4) -- recorded here so a
    // round-trip test doesn't "discover" 2.398 != 2.4.
    return BuildMatrixTrcProfile("Rec. 709 (gamma 2.4)",
                                 0.43607, 0.22249, 0.01392,
                                 0.38515, 0.71687, 0.09708,
                                 0.14307, 0.06061, 0.71410,
                                 0x0266);
}
