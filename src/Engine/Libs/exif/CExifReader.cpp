#include "CExifReader.h"
#include "CIccProfileCodec.h"
#include "SYS_FileUtf8.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

namespace {

uint16_t read16(const uint8_t *p, bool le) { return le ? (uint16_t)(p[0] | (p[1] << 8)) : (uint16_t)((p[0] << 8) | p[1]); }
uint32_t read32(const uint8_t *p, bool le) { return le ? (uint32_t)(p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24)) : (uint32_t)((p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]); }
int32_t  reads32(const uint8_t *p, bool le) { return (int32_t)read32(p, le); }

// TIFF type sizes, index = type 1..13 (0 unused).
//
// Type 13 (IFD) is admitted for CM-C1: SubIFDs (0x014A) are defined as LONG or
// IFD, and real DNGs use both. It is a LONG in every respect except its name,
// hence the 4. Without it EntryData rejects the entry, the walker skips it with
// `continue`, and the file's preview metadata is lost SILENTLY -- while any
// test written with LONG passes.
const uint32_t kTypeSize[14] = { 0, 1, 1, 2, 4, 8, 1, 1, 2, 4, 8, 4, 8, 4 };

// Whole-file walk budget for an untrusted TIFF/EXIF block. See the rationale in
// ParseIFD: depth and per-IFD width alone bound nothing, because the branching
// factor multiplies. A real photo uses ~5 IFDs and a few hundred entries.
static const size_t   kMaxIfdsPerFile    = 64;
static const uint32_t kMaxEntriesPerFile = 8192;
static const size_t   kMaxCapturedTags   = 4096;
// CM-C1. The IFD ceiling is RAISED ONLY when the extra walks are enabled: with
// the flag off the scan thread's malformed-file worst case must not move at
// all. A real DNG uses IFD0 + Exif + Interop + 2-4 SubIFDs + IFD1, i.e. under
// ten, so 128 is still two orders of magnitude of headroom.
static const size_t   kMaxIfdsPerFileDeep = 128;
static const uint32_t kMaxSubIfdsPerTag   = 16;   // 0x014A array elements honoured
static const int      kMaxIfdChainLinks   = 16;   // IFD1 -> IFD2 -> ... hard cap
static const size_t   kMaxMakerNoteTags   = 512;  // one vendor blob must not eat kMaxCapturedTags

// A vendor MakerNote tag-name table entry. Hand-written per vendor; see the
// licensing note above the vendor tables.
struct MakerTagName { uint16_t tag; const char *name; };

struct ParseCtx
{
    const uint8_t *tiff = nullptr;
    size_t         size = 0;
    bool           le   = true;
    bool           captureAll = true;
    bool           captureMakerNotes = false;   // CM-C1: SubIFD + IFD1 + MakerNote
    CExifData     *out  = nullptr;
    int            depth = 0;
    uint32_t       entriesWalked = 0;  // whole-file total, against kMaxEntriesPerFile
    std::vector<uint32_t> visitedIfds; // cycle guard for malformed files
    // Cycle-guard base. visitedIfds stores guardBase + ifdOffset, while tiff is
    // still indexed by ifdOffset alone. A self-relative MakerNote re-points
    // `tiff` at the blob, so without a distinct base its offset 8 would alias
    // the file's own IFD0 at offset 8 and be silently skipped as a cycle.
    // Expressed in the SAME space as the existing entries -- relative to the
    // enclosing TIFF base, not the file -- because CR3 builds a fresh ParseCtx
    // per CMT box and has no file-absolute offset to offer.
    uint32_t       guardBase = 0;
    // Vendor name table for the MakerNote currently being walked. TagNameFor
    // needs this: the generic-capture block overwrites e.name unconditionally,
    // so a name the vendor walker set itself would not survive.
    const MakerTagName *vendorNames = nullptr;
    size_t              vendorNameCount = 0;
    size_t              makerTagsCaptured = 0;
    // GPS raw pieces, sign applied after the GPS IFD finishes:
    char   latRef = 0, lonRef = 0;
    double latAbs = -1000.0, lonAbs = -1000.0;
    int    altRef = 0;
    double altAbs = -1000.0;
    // GPS speed, deferred so the ref (0x000C) and value (0x000D) can arrive in any order (fu-b #2.3):
    char   speedRef = 0;
    double speedAbs = -1.0;   // negative = not seen
};

// The IFD ceiling rises only for the deep walk, so the lean scan path's
// malformed-file worst case is exactly what it was before CM-C1.
inline size_t IfdCeiling(const ParseCtx &c)
{
    return c.captureMakerNotes ? kMaxIfdsPerFileDeep : kMaxIfdsPerFile;
}

// Pointer to an entry's value bytes (inline or offset), nullptr if out of bounds.
const uint8_t *EntryData(const ParseCtx &c, const uint8_t *entry, uint16_t type, uint32_t count)
{
    if (type < 1 || type > 13) return nullptr;
    uint64_t byteLen = (uint64_t)kTypeSize[type] * count;
    if (byteLen == 0 || byteLen > c.size) return nullptr;
    if (byteLen <= 4) return entry + 8;
    uint32_t off = read32(entry + 8, c.le);
    if ((uint64_t)off + byteLen > c.size) return nullptr;
    return c.tiff + off;
}

// i-th element of an integer-typed entry as uint32 (BYTE/SHORT/LONG/SBYTE/SSHORT/SLONG).
uint32_t EntryUInt(const ParseCtx &c, const uint8_t *d, uint16_t type, uint32_t i)
{
    switch (type) {
    case 1: case 6: case 7:  return d[i];
    case 3: case 8:          return read16(d + i * 2, c.le);
    // 13 (IFD) is a LONG. It MUST be here and not only in EntryData/kTypeSize:
    // a type-13 SubIFD pointer read through the `default` below would resolve
    // to offset 0, and the walker would then parse the TIFF header itself as a
    // directory ("II" reads as 18761 entries) -- emitting garbage while losing
    // the real sub-directory. Admitting the type without decoding it is worse
    // than not admitting it at all.
    case 4: case 9: case 13: return read32(d + i * 4, c.le);
    default:                 return 0;
    }
}

double EntryRational(const ParseCtx &c, const uint8_t *d, uint32_t i, bool sign)
{
    const uint8_t *p = d + i * 8;
    if (sign) {
        int32_t n = reads32(p, c.le), den = reads32(p + 4, c.le);
        return den ? (double)n / den : 0.0;
    }
    uint32_t n = read32(p, c.le), den = read32(p + 4, c.le);
    return den ? (double)n / den : 0.0;
}

// Unsigned rational that rejects a zero denominator instead of inventing 0.0.
// B's GPS fields are the first where a manufactured zero would change matching (fu-b #2.3):
// a corrupt 0/0 must not become a valid 0 km/h / due-north / Null-Island reading.
bool EntryRationalValid(const ParseCtx &c, const uint8_t *d, uint32_t i, double *out)
{
    const uint8_t *p = d + i * 8;
    const uint32_t n = read32(p, c.le), den = read32(p + 4, c.le);
    if (den == 0) return false;
    if (out) *out = (double)n / den;
    return true;
}

std::string EntryAscii(const ParseCtx &c, const uint8_t *d, uint32_t count)
{
    (void)c;
    uint32_t n = count;
    while (n > 0 && (d[n-1] == 0 || d[n-1] == ' ')) n--;
    return std::string((const char*)d, n);
}

struct TagName { uint16_t tag; const char *name; };

const TagName kNamesTiff[] = {
    {0x0100,"ImageWidth"},{0x0101,"ImageLength"},{0x0102,"BitsPerSample"},{0x0103,"Compression"},
    {0x0106,"PhotometricInterpretation"},{0x010E,"ImageDescription"},{0x010F,"Make"},{0x0110,"Model"},
    {0x0111,"StripOffsets"},{0x0112,"Orientation"},{0x0115,"SamplesPerPixel"},{0x0116,"RowsPerStrip"},
    {0x0117,"StripByteCounts"},{0x011A,"XResolution"},{0x011B,"YResolution"},{0x011C,"PlanarConfiguration"},
    {0x0128,"ResolutionUnit"},{0x012D,"TransferFunction"},{0x0131,"Software"},{0x0132,"DateTime"},
    {0x013B,"Artist"},{0x013E,"WhitePoint"},{0x013F,"PrimaryChromaticities"},
    {0x0201,"JPEGInterchangeFormat"},{0x0202,"JPEGInterchangeFormatLength"},
    {0x0211,"YCbCrCoefficients"},{0x0212,"YCbCrSubSampling"},{0x0213,"YCbCrPositioning"},
    {0x0214,"ReferenceBlackWhite"},{0x02BC,"XMP"},{0x4746,"Rating"},{0x4749,"RatingPercent"},
    {0x83BB,"IPTC-NAA"},{0x8298,"Copyright"},{0x8769,"ExifIFDPointer"},{0x8825,"GPSIFDPointer"},
    {0xC612,"DNGVersion"},{0xC614,"UniqueCameraModel"},{0xC62F,"CameraSerialNumber"},
};

const TagName kNamesExif[] = {
    {0x829A,"ExposureTime"},{0x829D,"FNumber"},{0x8822,"ExposureProgram"},{0x8824,"SpectralSensitivity"},
    {0x8827,"ISO"},{0x8828,"OECF"},{0x8830,"SensitivityType"},{0x8831,"StandardOutputSensitivity"},
    {0x8832,"RecommendedExposureIndex"},{0x8833,"ISOSpeed"},{0x8834,"ISOSpeedLatitudeyyy"},
    {0x8835,"ISOSpeedLatitudezzz"},{0x9000,"ExifVersion"},{0x9003,"DateTimeOriginal"},
    {0x9004,"DateTimeDigitized"},{0x9010,"OffsetTime"},{0x9011,"OffsetTimeOriginal"},
    {0x9012,"OffsetTimeDigitized"},{0x9101,"ComponentsConfiguration"},{0x9102,"CompressedBitsPerPixel"},
    {0x9201,"ShutterSpeedValue"},{0x9202,"ApertureValue"},{0x9203,"BrightnessValue"},
    {0x9204,"ExposureBiasValue"},{0x9205,"MaxApertureValue"},{0x9206,"SubjectDistance"},
    {0x9207,"MeteringMode"},{0x9208,"LightSource"},{0x9209,"Flash"},{0x920A,"FocalLength"},
    {0x9214,"SubjectArea"},{0x927C,"MakerNote"},{0x9286,"UserComment"},{0x9290,"SubSecTime"},
    {0x9291,"SubSecTimeOriginal"},{0x9292,"SubSecTimeDigitized"},{0x9400,"Temperature"},
    {0x9401,"Humidity"},{0x9402,"Pressure"},{0x9403,"WaterDepth"},{0x9404,"Acceleration"},
    {0x9405,"CameraElevationAngle"},{0xA000,"FlashpixVersion"},{0xA001,"ColorSpace"},
    {0xA002,"PixelXDimension"},{0xA003,"PixelYDimension"},{0xA004,"RelatedSoundFile"},
    {0xA005,"InteropIFDPointer"},{0xA20B,"FlashEnergy"},{0xA20E,"FocalPlaneXResolution"},
    {0xA20F,"FocalPlaneYResolution"},{0xA210,"FocalPlaneResolutionUnit"},{0xA214,"SubjectLocation"},
    {0xA215,"ExposureIndex"},{0xA217,"SensingMethod"},{0xA300,"FileSource"},{0xA301,"SceneType"},
    {0xA302,"CFAPattern"},{0xA401,"CustomRendered"},{0xA402,"ExposureMode"},{0xA403,"WhiteBalance"},
    {0xA404,"DigitalZoomRatio"},{0xA405,"FocalLengthIn35mmFilm"},{0xA406,"SceneCaptureType"},
    {0xA407,"GainControl"},{0xA408,"Contrast"},{0xA409,"Saturation"},{0xA40A,"Sharpness"},
    {0xA40B,"DeviceSettingDescription"},{0xA40C,"SubjectDistanceRange"},{0xA420,"ImageUniqueID"},
    {0xA430,"CameraOwnerName"},{0xA431,"BodySerialNumber"},{0xA432,"LensSpecification"},
    {0xA433,"LensMake"},{0xA434,"LensModel"},{0xA435,"LensSerialNumber"},{0xA460,"CompositeImage"},
    {0xA461,"SourceImageNumberOfCompositeImage"},{0xA462,"SourceExposureTimesOfCompositeImage"},
    {0xA500,"Gamma"},
};

const TagName kNamesGps[] = {
    {0x0000,"GPSVersionID"},{0x0001,"GPSLatitudeRef"},{0x0002,"GPSLatitude"},{0x0003,"GPSLongitudeRef"},
    {0x0004,"GPSLongitude"},{0x0005,"GPSAltitudeRef"},{0x0006,"GPSAltitude"},{0x0007,"GPSTimeStamp"},
    {0x0008,"GPSSatellites"},{0x0009,"GPSStatus"},{0x000A,"GPSMeasureMode"},{0x000B,"GPSDOP"},
    {0x000C,"GPSSpeedRef"},{0x000D,"GPSSpeed"},{0x000E,"GPSTrackRef"},{0x000F,"GPSTrack"},
    {0x0010,"GPSImgDirectionRef"},{0x0011,"GPSImgDirection"},{0x0012,"GPSMapDatum"},
    {0x0013,"GPSDestLatitudeRef"},{0x0014,"GPSDestLatitude"},{0x0015,"GPSDestLongitudeRef"},
    {0x0016,"GPSDestLongitude"},{0x0017,"GPSDestBearingRef"},{0x0018,"GPSDestBearing"},
    {0x0019,"GPSDestDistanceRef"},{0x001A,"GPSDestDistance"},{0x001B,"GPSProcessingMethod"},
    {0x001C,"GPSAreaInformation"},{0x001D,"GPSDateStamp"},{0x001E,"GPSDifferential"},
    {0x001F,"GPSHPositioningError"},
};

const TagName kNamesInterop[] = {
    {0x0001,"InteroperabilityIndex"},{0x0002,"InteroperabilityVersion"},
    {0x1001,"RelatedImageWidth"},{0x1002,"RelatedImageLength"},
};

const char *LookupName(const TagName *table, size_t n, uint16_t tag)
{
    for (size_t i = 0; i < n; ++i) if (table[i].tag == tag) return table[i].name;
    return nullptr;
}

// Takes the ctx because MakerNote names live in a per-vendor table that is only
// known while that vendor's blob is being walked. The generic-capture block in
// ParseIFD assigns e.name = TagNameFor(...) unconditionally, so a name written
// by the vendor walker itself would simply be overwritten here.
//
// The switch is deliberately left WITHOUT a default: an exhaustive switch turns
// the next EExifIFD addition into a compiler warning instead of a silent
// "Tag 0x1234".
std::string TagNameFor(const ParseCtx &c, EExifIFD ifd, uint16_t tag)
{
    const char *n = nullptr;
    switch (ifd) {
    case EExifIFD::IFD0:
    // SubIFD and IFD1 are TIFF tag space, exactly like IFD0 -- a DNG preview
    // SubIFD carries ImageWidth/ImageLength, a thumbnail IFD1 carries
    // Compression/StripOffsets. Same fallback to Exif space for the RAWs that
    // put Exif tags in a TIFF directory.
    case EExifIFD::SubIFD:
    case EExifIFD::IFD1:
        n = LookupName(kNamesTiff, std::size(kNamesTiff), tag);
        // TIFF-based RAWs sometimes put Exif-space tags straight into IFD0:
        if (!n) n = LookupName(kNamesExif, std::size(kNamesExif), tag);
        break;
    case EExifIFD::Exif:    n = LookupName(kNamesExif, std::size(kNamesExif), tag); break;
    case EExifIFD::GPS:     n = LookupName(kNamesGps, std::size(kNamesGps), tag); break;
    case EExifIFD::Interop: n = LookupName(kNamesInterop, std::size(kNamesInterop), tag); break;
    case EExifIFD::MakerNote:
        for (size_t i = 0; i < c.vendorNameCount; ++i)
            if (c.vendorNames[i].tag == tag) { n = c.vendorNames[i].name; break; }
        break;
    }
    if (n) return n;
    char buf[16];
    snprintf(buf, sizeof(buf), "Tag 0x%04X", tag);
    return buf;
}

// Enum label decoding for the classic culling-relevant EXIF/GPS enums.
const char *EnumLabel(EExifIFD ifd, uint16_t tag, uint32_t v)
{
    if (ifd == EExifIFD::GPS)
    {
        if (tag == 0x0005) return v == 1 ? "Below sea level" : "Above sea level";
        if (tag == 0x001E) return v == 1 ? "Differential corrected" : "No correction";
        return nullptr;
    }
    if (ifd != EExifIFD::Exif && ifd != EExifIFD::IFD0) return nullptr;
    switch (tag) {
    case 0x0112: { // Orientation
        static const char *s[] = { "Horizontal", "Mirror horizontal", "Rotate 180", "Mirror vertical",
                                   "Mirror horizontal, rotate 270 CW", "Rotate 90 CW",
                                   "Mirror horizontal, rotate 90 CW", "Rotate 270 CW" };
        return (v >= 1 && v <= 8) ? s[v-1] : nullptr;
    }
    case 0x0128: case 0xA210: // ResolutionUnit / FocalPlaneResolutionUnit
        return v == 2 ? "inches" : v == 3 ? "cm" : nullptr;
    case 0x0103: // Compression
        return v == 1 ? "Uncompressed" : v == 6 ? "JPEG (old)" : v == 7 ? "JPEG" : v == 8 ? "Deflate" : nullptr;
    case 0x8822: { // ExposureProgram
        static const char *s[] = { "Not defined", "Manual", "Program AE", "Aperture priority",
                                   "Shutter priority", "Creative", "Action", "Portrait", "Landscape" };
        return v <= 8 ? s[v] : nullptr;
    }
    case 0x9207: { // MeteringMode
        static const char *s[] = { "Unknown", "Average", "Center-weighted", "Spot",
                                   "Multi-spot", "Pattern", "Partial" };
        if (v == 255) return "Other";
        return v <= 6 ? s[v] : nullptr;
    }
    case 0x9208: { // LightSource
        switch (v) { case 0: return "Unknown"; case 1: return "Daylight"; case 2: return "Fluorescent";
        case 3: return "Tungsten"; case 4: return "Flash"; case 9: return "Fine weather";
        case 10: return "Cloudy"; case 11: return "Shade"; case 17: return "Standard light A";
        case 18: return "Standard light B"; case 19: return "Standard light C";
        case 23: return "D50"; case 24: return "ISO studio tungsten"; case 255: return "Other";
        default: return nullptr; }
    }
    case 0x9209: { // Flash bitfield -> composed static labels for common values
        switch (v) { case 0x00: return "Did not fire"; case 0x01: return "Fired";
        case 0x05: return "Fired, no return"; case 0x07: return "Fired, return detected";
        case 0x08: return "On, did not fire"; case 0x09: return "Fired, compulsory";
        case 0x0D: return "Fired, compulsory, no return"; case 0x0F: return "Fired, compulsory, return";
        case 0x10: return "Off, did not fire"; case 0x18: return "Off (auto)";
        case 0x19: return "Fired (auto)"; case 0x1D: return "Fired (auto), no return";
        case 0x1F: return "Fired (auto), return"; case 0x20: return "No flash function";
        case 0x41: return "Fired, red-eye"; case 0x45: return "Fired, red-eye, no return";
        case 0x47: return "Fired, red-eye, return"; case 0x49: return "Fired, compulsory, red-eye";
        case 0x59: return "Fired (auto), red-eye"; case 0x5D: return "Fired (auto), red-eye, no return";
        case 0x5F: return "Fired (auto), red-eye, return"; default: return nullptr; }
    }
    case 0xA001: return v == 1 ? "sRGB" : v == 0xFFFF ? "Uncalibrated" : nullptr;
    case 0xA217: { // SensingMethod
        static const char *s[] = { "", "Not defined", "One-chip color area", "Two-chip color area",
                                   "Three-chip color area", "Color sequential area",
                                   "", "Trilinear", "Color sequential linear" };
        return (v >= 1 && v <= 8 && s[v][0]) ? s[v] : nullptr;
    }
    case 0xA300: return v == 3 ? "Digital camera" : nullptr;
    case 0xA301: return v == 1 ? "Directly photographed" : nullptr;
    case 0xA401: return v == 0 ? "Normal" : v == 1 ? "Custom" : nullptr;
    case 0xA402: return v == 0 ? "Auto" : v == 1 ? "Manual" : v == 2 ? "Auto bracket" : nullptr;
    case 0xA403: return v == 0 ? "Auto" : v == 1 ? "Manual" : nullptr;
    case 0xA406: { // SceneCaptureType
        static const char *s[] = { "Standard", "Landscape", "Portrait", "Night", "Other" };
        return v <= 4 ? s[v] : nullptr;
    }
    case 0xA407: { // GainControl
        static const char *s[] = { "None", "Low gain up", "High gain up", "Low gain down", "High gain down" };
        return v <= 4 ? s[v] : nullptr;
    }
    case 0xA408: return v == 0 ? "Normal" : v == 1 ? "Soft" : v == 2 ? "Hard" : nullptr;   // Contrast
    case 0xA409: return v == 0 ? "Normal" : v == 1 ? "Low" : v == 2 ? "High" : nullptr;    // Saturation
    case 0xA40A: return v == 0 ? "Normal" : v == 1 ? "Soft" : v == 2 ? "Hard" : nullptr;   // Sharpness
    case 0xA40C: { // SubjectDistanceRange
        static const char *s[] = { "Unknown", "Macro", "Close", "Distant" };
        return v <= 3 ? s[v] : nullptr;
    }
    case 0x8830: { // SensitivityType
        static const char *s[] = { "Unknown", "SOS", "REI", "ISO speed", "SOS+REI", "SOS+ISO", "REI+ISO", "SOS+REI+ISO" };
        return v <= 7 ? s[v] : nullptr;
    }
    default: return nullptr;
    }
}

std::string FormatTagValue(const ParseCtx &c, EExifIFD ifd, uint16_t tag,
                           uint16_t type, uint32_t count, const uint8_t *d)
{
    char buf[64];
    // Tag-specific pretty formats first.
    if (ifd == EExifIFD::Exif || ifd == EExifIFD::IFD0)
    {
        if (tag == 0x829A && (type == 5 || type == 10))          // ExposureTime
        { std::string s = CExifReader::FormatShutter((float)EntryRational(c, d, 0, type == 10)); return s.empty() ? "0" : s + " s"; }
        if (tag == 0x829D && type == 5)                          // FNumber
            return CExifReader::FormatAperture((float)EntryRational(c, d, 0, false));
        if (tag == 0x920A && type == 5)                          // FocalLength
            return CExifReader::FormatFocalLength((float)EntryRational(c, d, 0, false));
        if (tag == 0x9204 && (type == 5 || type == 10))          // ExposureBias
        { std::string s = CExifReader::FormatExposureBias((float)EntryRational(c, d, 0, type == 10)); return s.empty() ? "0 EV" : s; }
        if (tag == 0x9201 && (type == 10 || type == 5))          // ShutterSpeedValue (APEX)
        { double v = EntryRational(c, d, 0, type == 10); std::string s = CExifReader::FormatShutter((float)std::pow(2.0, -v)); return s + " s"; }
        if ((tag == 0x9202 || tag == 0x9205) && type == 5)       // Aperture/MaxAperture (APEX)
            return CExifReader::FormatAperture((float)std::pow(2.0, EntryRational(c, d, 0, false) / 2.0));
        if (tag == 0xA405 && count == 1)                         // FocalLengthIn35mmFilm
        { snprintf(buf, sizeof(buf), "%umm", EntryUInt(c, d, type, 0)); return buf; }
        if (tag == 0x927C)                                       // MakerNote: opaque
        { snprintf(buf, sizeof(buf), "(%u bytes)", count); return buf; }
        if ((tag == 0x9000 || tag == 0xA000) && type == 7 && count == 4) // versions "0232"
            return std::string((const char*)d, 4);
        if (tag == 0x9101 && type == 7)                          // ComponentsConfiguration
        {
            static const char *comp[] = { "-", "Y", "Cb", "Cr", "R", "G", "B" };
            std::string s;
            for (uint32_t i = 0; i < count && i < 4; ++i)
            { uint8_t x = d[i]; s += (x <= 6) ? comp[x] : "?"; }
            return s;
        }
        if (tag == 0x9286 && type == 7 && count >= 8)            // UserComment: 8-byte charset header
            return EntryAscii(c, d + 8, count - 8);
    }
    if (ifd == EExifIFD::GPS && tag == 0x0007 && type == 5 && count == 3) // GPSTimeStamp
    {
        snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                 (int)EntryRational(c, d, 0, false), (int)EntryRational(c, d, 1, false),
                 (int)EntryRational(c, d, 2, false));
        return buf;
    }
    if ((ifd == EExifIFD::GPS && (tag == 0x0002 || tag == 0x0004)) && type == 5 && count == 3)
    {   // GPSLatitude/Longitude: show unsigned decimal; sign lives in the Ref tag
        double v = EntryRational(c, d, 0, false) + EntryRational(c, d, 1, false) / 60.0
                 + EntryRational(c, d, 2, false) / 3600.0;
        snprintf(buf, sizeof(buf), "%.5f", v);
        return buf;
    }

    // Enum label?
    if (count == 1 && (type == 1 || type == 3 || type == 4))
        if (const char *lbl = EnumLabel(ifd, tag, EntryUInt(c, d, type, 0))) return lbl;

    // Generic by type.
    switch (type) {
    // ASCII is bounded like every other type here. The neighbours already
    // truncate (`count > 16` -> "(N bytes)", `count > 4` -> "(N rationals)");
    // this one did not, and `EntryData` only requires the value fit inside the
    // buffer -- so a single entry could declare a count spanning the whole
    // file. With captureAll on (the decode pool's path, one call per photo the
    // user arrows onto) a 16 MiB file declaring 512 such entries would
    // materialise 512 strings of ~16 MB, several GB from one small file.
    // Bounding the COUNT of captured tags does not bound their BYTES.
    case 2: {
        static const uint32_t kMaxAsciiChars = 4096;
        if (count > kMaxAsciiChars)
        {
            std::string s = EntryAscii(c, d, kMaxAsciiChars);
            snprintf(buf, sizeof(buf), " ... (%u bytes total)", count);
            s += buf;
            return s;
        }
        return EntryAscii(c, d, count);
    }
    case 5: case 10: {
        if (count > 4) { snprintf(buf, sizeof(buf), "(%u rationals)", count); return buf; }
        std::string s;
        for (uint32_t i = 0; i < count; ++i)
        { snprintf(buf, sizeof(buf), "%s%g", i ? " " : "", EntryRational(c, d, i, type == 10)); s += buf; }
        return s;
    }
    case 11: case 12: {
        if (count != 1) { snprintf(buf, sizeof(buf), "(%u floats)", count); return buf; }
        double v;
        if (type == 11) { float f; memcpy(&f, d, 4); v = f; }   // memcpy: value bytes
        else            { double dd; memcpy(&dd, d, 8); v = dd; } // may be unaligned
        snprintf(buf, sizeof(buf), "%g", v);
        return buf;
    }
    case 7: {
        if (count > 16) { snprintf(buf, sizeof(buf), "(%u bytes)", count); return buf; }
        std::string s;
        for (uint32_t i = 0; i < count; ++i)
        { snprintf(buf, sizeof(buf), "%s%02X", i ? " " : "", d[i]); s += buf; }
        return s;
    }
    default: { // integer types
        if (count > 16) { snprintf(buf, sizeof(buf), "(%u values)", count); return buf; }
        std::string s;
        for (uint32_t i = 0; i < count; ++i)
        {
            uint32_t u = EntryUInt(c, d, type, i);
            if (type == 6)      snprintf(buf, sizeof(buf), "%s%d", i ? " " : "", (int8_t)u);
            else if (type == 8) snprintf(buf, sizeof(buf), "%s%d", i ? " " : "", (int16_t)u);
            else if (type == 9) snprintf(buf, sizeof(buf), "%s%d", i ? " " : "", (int32_t)u);
            else                snprintf(buf, sizeof(buf), "%s%u", i ? " " : "", u);
            s += buf;
        }
        return s;
    }
    }
}

// Defined below ParseIFD (it calls back into it). Returns false when the blob is
// not a recognised vendor, in which case the caller keeps today's single opaque
// "(N bytes)" row.
bool ParseMakerNote(ParseCtx &c, uint32_t blobOffset, uint32_t blobLen);

void ParseIFD(ParseCtx &c, uint32_t ifdOffset, EExifIFD ifd)
{
    if (c.depth > 4) return;
    if ((uint64_t)ifdOffset + 2 > c.size) return;

    // TOTAL budget, not just depth and per-IFD width.
    //
    // The duplicate-offset scan below is a CYCLE guard, not a budget: it stops
    // an IFD pointing at itself, and stops nothing at all when every pointer
    // targets a DIFFERENT offset. With depth 4 and 512 entries per IFD, a file
    // whose bytes happen to parse as sub-IFD pointers everywhere expands to
    // 512^4 nodes, each of which also scanned the visited list linearly -- a
    // hang, and on the capture-all path an allocation blow-up as well. This is
    // not hypothetical input handling: ReadFileHeader runs over EVERY photo in
    // a browsed folder on the scan thread, so one such file wedges the folder
    // scan permanently, and a merely corrupt file is enough.
    //
    // A real photo uses about five IFDs (IFD0, Exif, GPS, Interop, IFD1), so
    // both caps are orders of magnitude above anything legitimate and no valid
    // file can notice them. With the count bounded, the linear scan is trivial.
    if (c.visitedIfds.size() >= IfdCeiling(c)) return;
    if (c.entriesWalked      >= kMaxEntriesPerFile) return;

    // guardBase separates "where this directory lives" from "how we index it":
    // a self-relative MakerNote re-points c.tiff, so raw offsets from two bases
    // would alias. See ParseCtx::guardBase.
    const uint32_t guardKey = c.guardBase + ifdOffset;
    for (uint32_t seen : c.visitedIfds) if (seen == guardKey) return; // cycle
    c.visitedIfds.push_back(guardKey);

    uint16_t count = read16(c.tiff + ifdOffset, c.le);
    if (count > 512) count = 512; // malformed-file sanity cap
    uint32_t pos = ifdOffset + 2;
    CExifData &out = *c.out;

    // DNG PreviewColorSpace scratch. A LOCAL, never on ParseCtx: ParseIFD
    // recurses from inside the loop below, and a SubIFD may itself carry
    // 0x014A, so ctx-level scratch would mix two directories' dimensions.
    // Emitted once after the loop, so every exit path keeps the candidate --
    // which is why the budget exits below are `break`, not `return`.
    uint16_t dngPreviewCs = 0; bool haveDngPreviewCs = false;
    uint32_t dngW = 0, dngH = 0;

    for (uint16_t i = 0; i < count && (uint64_t)pos + 12 <= c.size; ++i, pos += 12)
    {
        if (++c.entriesWalked > kMaxEntriesPerFile) break;    // whole-file budget
        const uint8_t *entry = c.tiff + pos;
        uint16_t tag  = read16(entry, c.le);
        uint16_t type = read16(entry + 2, c.le);
        uint32_t cnt  = read32(entry + 4, c.le);
        const uint8_t *d = EntryData(c, entry, type, cnt);
        if (!d) continue;

        // --- sub-IFD pointers ---
        //
        // NOT in MakerNote space. Those three tag numbers are ordinary vendor
        // tag IDs there (or corrupt bytes -- a MakerNote is the least
        // trustworthy input this reader handles), and descending would re-enter
        // Exif/GPS tag space with a vendor-relative offset, letting garbage
        // overwrite make/model/dateTimeOriginal/orientation through the typed
        // extraction below. A vendor's own sub-directories are descended by the
        // vendor walker, which knows its own offset base.
        // Each pointer is honoured only from its CONFORMING home: 0x8769/0x8825
        // live in IFD0, 0xA005 in the Exif IFD (which is also what CR3's CMT2
        // root uses). Honouring them from a SubIFD or IFD1 would let a crafted
        // thumbnail descend into a directory walked as Exif/GPS, where typed
        // extraction assigns unconditionally -- the same overwrite the IFD1
        // design locked out at the front door, re-entered through a side one.
        const bool ptrHere =
            ((tag == 0x8769 || tag == 0x8825) && ifd == EExifIFD::IFD0) ||
            (tag == 0xA005 && ifd == EExifIFD::Exif);
        if (ptrHere && (type == 4 || type == 3))
        {
            uint32_t off = EntryUInt(c, d, type, 0);
            EExifIFD sub = (tag == 0x8769) ? EExifIFD::Exif
                         : (tag == 0x8825) ? EExifIFD::GPS : EExifIFD::Interop;
            c.depth++; ParseIFD(c, off, sub); c.depth--;
            continue; // pointer tags are plumbing; don't list them in the panel
        }

        // --- SubIFDs (0x014A), an ARRAY: DNG carries the full-res image and
        // one or more previews. Gated, because pointer tags are never listed in
        // allTags -- an unconditional branch would make today's "Tag 0x014A"
        // row silently vanish for every existing caller.
        if (c.captureMakerNotes && ifd != EExifIFD::MakerNote && tag == 0x014A &&
            (type == 4 || type == 3 || type == 13))
        {
            const uint32_t n = cnt < kMaxSubIfdsPerTag ? cnt : kMaxSubIfdsPerTag;
            for (uint32_t k = 0; k < n; ++k)
            {
                // Array elements charge the SHARED counter: entriesWalked is
                // otherwise incremented once per entry, and one entry can
                // declare millions of elements.
                if (++c.entriesWalked > kMaxEntriesPerFile) break;
                c.depth++; ParseIFD(c, EntryUInt(c, d, type, k), EExifIFD::SubIFD); c.depth--;
            }
            continue;
        }

        // --- vendor MakerNote. Lives in the Exif IFD; the blob's own bytes say
        // which vendor, and each vendor says what its internal offsets are
        // relative to. On success the decoded rows replace the opaque one.
        if (c.captureMakerNotes && tag == 0x927C && ifd != EExifIFD::MakerNote &&
            d >= c.tiff && (uint64_t)(d - c.tiff) + cnt <= c.size)
        {
            if (ParseMakerNote(c, (uint32_t)(d - c.tiff), cnt))
                continue;
        }

        // --- DNG preview colour space (0xC71A) + the dimensions of the IFD it
        // sits in. Collected in any TIFF-space directory kind, because vendors
        // put preview descriptors in IFD1 as readily as in a SubIFD. Never
        // resolved here.
        //
        // NOT in MakerNote space, for the same reason the pointer tags above
        // are excluded: 0xC71A, 0x0100 and 0x0101 are ordinary vendor tag
        // numbers there. A candidate fabricated from vendor tags would carry
        // fabricated dimensions too, and the host picks the candidate whose
        // dimensions match the decoded preview -- so a fake could win that
        // match and put a wrong colour space on screen.
        if (c.captureMakerNotes && ifd != EExifIFD::MakerNote)
        {
            if (tag == 0xC71A && cnt >= 1)
            { dngPreviewCs = (uint16_t)EntryUInt(c, d, type, 0); haveDngPreviewCs = true; }
            else if (tag == 0x0100 && cnt >= 1) dngW = EntryUInt(c, d, type, 0);
            else if (tag == 0x0101 && cnt >= 1) dngH = EntryUInt(c, d, type, 0);
        }

        // --- typed extraction ---
        if (ifd == EExifIFD::IFD0 || ifd == EExifIFD::Exif)
        {
            switch (tag) {
            case 0x010F: out.make  = EntryAscii(c, d, cnt); break;
            case 0x0110: out.model = EntryAscii(c, d, cnt); break;
            // Orientation is a TIFF Rev 6.0 / IFD0 tag. It is NOT in the Exif
            // IFD's tag set, so a 0x0112 found below 0x8769 is non-conforming --
            // and honouring it was actively harmful, for two reasons:
            //
            //  1. IFD0's entries are tag-sorted, so 0x0112 is read BEFORE the
            //     0x8769 pointer above. A copy in the Exif sub-IFD therefore
            //     OVERWROTE IFD0's, i.e. the malformed value always won.
            //  2. That made this reader disagree with macOS ImageIO, which
            //     takes orientation from the TIFF/IFD0 dictionary -- and with
            //     every orientation WRITER in the host app, all of which walk
            //     IFD0 only. A host that patched IFD0 and then re-read through
            //     here saw its own write ignored: the commit reported success,
            //     the stored rotation was cleared, and the photo kept its old
            //     orientation. The rotation silently disappeared.
            //
            // Ignoring the sub-IFD copy entirely (rather than falling back to
            // it when IFD0 has none) is deliberate: a fallback would reopen the
            // same divergence for the IFD0-absent shape, because a writer
            // INSERTING into IFD0 composes from a base this reader took from
            // somewhere the writer never looks.
            case 0x0112:
                if (ifd == EExifIFD::IFD0)
                    out.orientation = (int)EntryUInt(c, d, type, 0);
                break;
            case 0x0131: out.software = EntryAscii(c, d, cnt); break;
            case 0x013B: out.artist = EntryAscii(c, d, cnt); break;
            case 0x010E: out.imageDescription = EntryAscii(c, d, cnt); break;
            case 0x8298: out.copyright = EntryAscii(c, d, cnt); break;
            case 0x9003: out.dateTimeOriginal = EntryAscii(c, d, cnt); break;
            case 0x9004: if (out.dateTimeOriginal.empty()) out.dateTimeOriginal = EntryAscii(c, d, cnt); break;
            case 0x9291: out.subSecTimeOriginal = EntryAscii(c, d, cnt); break;
            case 0x9011: out.offsetTimeOriginal = EntryAscii(c, d, cnt); break;
            case 0x829A: if (type == 5 || type == 10) out.exposureTime = (float)EntryRational(c, d, 0, type == 10); break;
            case 0x829D: if (type == 5) out.fNumber = (float)EntryRational(c, d, 0, false); break;
            case 0x8827: if (out.isoSpeed == 0) out.isoSpeed = (int)EntryUInt(c, d, type, 0); break;
            case 0x8833: if (out.isoSpeed == 0) out.isoSpeed = (int)EntryUInt(c, d, type, 0); break; // 32-bit ISOSpeed fallback
            case 0x920A: if (type == 5) out.focalLength = (float)EntryRational(c, d, 0, false); break;
            case 0xA405: out.focalLength35mm = (int)EntryUInt(c, d, type, 0); break;
            case 0x9204:
                if (type == 5 || type == 10)
                {
                    out.exposureBias      = (float)EntryRational(c, d, 0, type == 10);  // sign by type (cf. 0x829A at :394)
                    out.exposureBiasValid = true;
                }
                break;
            case 0x9205: if (type == 5) out.maxAperture = (float)std::pow(2.0, EntryRational(c, d, 0, false) / 2.0); break;
            case 0xA404: if (type == 5) out.digitalZoomRatio = (float)EntryRational(c, d, 0, false); break;
            case 0xA002: out.pixelWidth  = (int)EntryUInt(c, d, type, 0); break;
            case 0xA003: out.pixelHeight = (int)EntryUInt(c, d, type, 0); break;
            case 0xA433: out.lensMake  = EntryAscii(c, d, cnt); break;
            case 0xA434: out.lensModel = EntryAscii(c, d, cnt); break;
            case 0xA431: out.bodySerialNumber = EntryAscii(c, d, cnt); break;
            case 0xA430: out.cameraOwnerName  = EntryAscii(c, d, cnt); break;
            case 0x9209: out.flash = (int)EntryUInt(c, d, type, 0); break;
            case 0x8822: out.exposureProgram = (int)EntryUInt(c, d, type, 0); break;
            case 0x9207: out.meteringMode = (int)EntryUInt(c, d, type, 0); break;
            case 0xA403: out.whiteBalance = (int)EntryUInt(c, d, type, 0); break;
            case 0x9208: out.lightSource = (int)EntryUInt(c, d, type, 0); break;
            case 0xA402: out.exposureMode = (int)EntryUInt(c, d, type, 0); break;
            case 0xA406: out.sceneCaptureType = (int)EntryUInt(c, d, type, 0); break;
            case 0xA001: out.colorSpace = (int)EntryUInt(c, d, type, 0); break;
            default: break;
            }
        }
        else if (ifd == EExifIFD::GPS)
        {
            switch (tag) {
            case 0x0001: { std::string r = EntryAscii(c, d, cnt); if (!r.empty()) c.latRef = r[0]; break; }
            case 0x0003: { std::string r = EntryAscii(c, d, cnt); if (!r.empty()) c.lonRef = r[0]; break; }
            case 0x0002: if (type == 5 && cnt == 3) {   // reject a 0/0 component -> absent, not a valid (0,0) (fu-b #2.3)
                double dg, mn, sc;
                if (EntryRationalValid(c, d, 0, &dg) && EntryRationalValid(c, d, 1, &mn) && EntryRationalValid(c, d, 2, &sc))
                    c.latAbs = dg + mn/60.0 + sc/3600.0;
            } break;
            case 0x0004: if (type == 5 && cnt == 3) {
                double dg, mn, sc;
                if (EntryRationalValid(c, d, 0, &dg) && EntryRationalValid(c, d, 1, &mn) && EntryRationalValid(c, d, 2, &sc))
                    c.lonAbs = dg + mn/60.0 + sc/3600.0;
            } break;
            case 0x0005: c.altRef = (int)EntryUInt(c, d, type, 0); break;
            case 0x0006: if (type == 5) { double v; if (EntryRationalValid(c, d, 0, &v)) c.altAbs = v; } break;
            case 0x000C: { std::string r = EntryAscii(c, d, cnt); if (!r.empty()) c.speedRef = r[0]; break; }   // GPSSpeedRef
            case 0x000D: { double v; if (type == 5 && EntryRationalValid(c, d, 0, &v)) c.speedAbs = v; break; } // GPSSpeed
            case 0x0010: { std::string r = EntryAscii(c, d, cnt); if (!r.empty()) out.gpsImgDirectionRef = r[0]; break; } // GPSImgDirectionRef
            case 0x0011: {
                double dir;
                if (type == 5 && EntryRationalValid(c, d, 0, &dir) && dir >= 0.0 && dir < 360.0) {
                    out.gpsImgDirection      = dir;       // reject junk, do NOT wrap it (fu-b #2.3)
                    out.gpsImgDirectionValid = true;
                }
            } break;
            case 0x001D: out.gpsDateStamp = EntryAscii(c, d, cnt); break;
            case 0x0007: if (type == 5 && cnt == 3) {
                char tb[16];
                snprintf(tb, sizeof(tb), "%02d:%02d:%02d",
                         (int)EntryRational(c, d, 0, false), (int)EntryRational(c, d, 1, false),
                         (int)EntryRational(c, d, 2, false));
                out.gpsTimeStamp = tb;
            } break;
            default: break;
            }
        }
        else if (ifd == EExifIFD::Interop)
        {
            // InteropIndex. The Interop IFD is already walked (the 0xA005
            // pointer is followed above); only the typed extraction was gated
            // to IFD0/Exif, so this value never reached a CExifData field.
            // "R98" = sRGB, "R03" = Adobe RGB -- the disambiguator for
            // ColorSpace == 0xFFFF, which is how many Adobe RGB camera JPEGs
            // announce themselves.
            if (tag == 0x0001)
                out.interopIndex = EntryAscii(c, d, cnt);
        }

        // --- generic capture ---
        // Bounded independently of the walk budget: each entry here allocates a
        // name and a formatted value string, so this is the path where a
        // malformed file turns into memory rather than time. The panel that
        // consumes allTags shows a few hundred rows at most.
        // A MakerNote additionally has its own cap, so one vendor blob cannot
        // consume the whole file's capture budget.
        if (c.captureAll && out.allTags.size() < kMaxCapturedTags &&
            (ifd != EExifIFD::MakerNote || c.makerTagsCaptured < kMaxMakerNoteTags))
        {
            CExifTagEntry e;
            e.tag = tag; e.ifd = ifd; e.type = type; e.count = cnt;
            e.name  = TagNameFor(c, ifd, tag);
            e.value = FormatTagValue(c, ifd, tag, type, cnt, d);
            out.allTags.push_back(std::move(e));
            if (ifd == EExifIFD::MakerNote) ++c.makerTagsCaptured;
        }
    }

    // One emit point for the whole directory, reached by every exit above --
    // a directory that runs out of budget mid-walk still contributes what it
    // had. Only 0xC71A makes a candidate; the dimensions are optional context.
    if (haveDngPreviewCs && out.dngPreviewCandidates.size() < kMaxSubIfdsPerTag)
    {
        CExifDngPreviewCandidate cand;
        cand.colorSpace = dngPreviewCs; cand.width = dngW; cand.height = dngH;
        out.dngPreviewCandidates.push_back(cand);
    }
}

// ---------------------------------------------------------------------------
// Vendor MakerNotes (CM-C1 #4)
//
// PROVENANCE. Everything below is written from the structural description of
// each vendor's MakerNote container -- signature bytes, where the IFD starts,
// and what its internal offsets are relative to -- plus published tag numbers.
// Tag numbers, their names and their value meanings are facts about a file
// format. NO CODE, TABLE OR PrintConv BLOCK IS COPIED from ExifTool (Artistic/
// GPL) or exiv2 (GPL); neither is linked, bundled or shipped, and neither is
// licence-compatible with a store binary. The colour tag IDs and their value
// bases were verified 2026-08-07 against ExifTool master
// @ 2200871d9cef988051d2a99d67df3bda6cbb30a8, read as a reference.
//
// The name tables are deliberately modest. Reproducing a vendor's full tag
// coverage is not this phase's job.
// ---------------------------------------------------------------------------

const MakerTagName kMnNikon[] = {
    {0x0001,"MakerNoteVersion"},{0x0002,"ISOSetting"},{0x0004,"Quality"},
    {0x0005,"WhiteBalance"},{0x0006,"Sharpness"},{0x0007,"FocusMode"},
    {0x0008,"FlashSetting"},{0x0009,"FlashType"},{0x000B,"WhiteBalanceFineTune"},
    {0x001E,"ColorSpace"},{0x0084,"Lens"},{0x0087,"FlashMode"},
    {0x0088,"AFInfo"},{0x0089,"ShootingMode"},{0x008D,"ColorHue"},
    {0x0093,"ImageAdjustment"},{0x00A7,"ShutterCount"},{0x00AB,"VariProgram"},
};
const MakerTagName kMnCanon[] = {
    {0x0001,"CanonCameraSettings"},{0x0002,"CanonFocalLength"},{0x0004,"CanonShotInfo"},
    {0x0006,"CanonImageType"},{0x0007,"CanonFirmwareVersion"},{0x0009,"OwnerName"},
    {0x000C,"SerialNumber"},{0x0093,"CanonFileInfo"},{0x0095,"LensModel"},
    {0x00B4,"ColorSpace"},
};
const MakerTagName kMnPentax[] = {
    {0x0000,"PentaxVersion"},{0x0001,"PentaxModelType"},{0x0005,"PentaxModelID"},
    {0x0037,"ColorSpace"},{0x0200,"BlackPoint"},
};
const MakerTagName kMnOlympus[] = {
    {0x0200,"SpecialMode"},{0x0207,"CameraID"},{0x2010,"Equipment"},
    {0x2020,"CameraSettings"},{0x2030,"RawDevelopment"},{0x2040,"ImageProcessing"},
};
const MakerTagName kMnOlympusCs[] = {
    {0x0507,"ColorSpace"},
};
const MakerTagName kMnSony[] = {
    {0xB020,"ColorReproduction"},{0xB021,"ColorTemperature"},{0xB029,"ColorMode"},
};
const MakerTagName kMnFuji[] = {
    {0x1000,"Quality"},{0x1001,"Sharpness"},{0x1002,"WhiteBalance"},
    {0x1003,"Saturation"},{0x1004,"Contrast"},{0x1210,"ColorMode"},{0x1401,"FilmMode"},
};
const MakerTagName kMnPanasonic[] = {
    {0x0001,"ImageQuality"},{0x001A,"ImageStabilization"},{0x0028,"ColorEffect"},
    {0x0032,"ColorMode"},
};

// Per-vendor colour mapping. THE VALUE BASE DIFFERS BY VENDOR -- Nikon and
// Canon count from 1, Pentax and Olympus from 0. One shared decode would read
// every sRGB Pentax file as Adobe RGB and every Adobe RGB one as ProPhoto, so
// each vendor gets its own function and there is no default fallthrough.
EExifColorSpaceHint MapNikonColor(uint32_t v)
{
    switch (v) {
    case 1: return EExifColorSpaceHint::Srgb;
    case 2: return EExifColorSpaceHint::AdobeRgb;
    case 4: return EExifColorSpaceHint::Bt2100;   // Z8, Tone Mode HLG
    default: return EExifColorSpaceHint::Unknown;
    }
}
EExifColorSpaceHint MapCanonColor(uint32_t v)
{
    switch (v) {
    case 1: return EExifColorSpaceHint::Srgb;
    case 2: return EExifColorSpaceHint::AdobeRgb;
    // 65535 means "n/a" -- ABSENCE, not a space. Reporting Unknown lets a host
    // chain fall through to its next signal instead of stopping here.
    default: return EExifColorSpaceHint::Unknown;
    }
}
EExifColorSpaceHint MapPentaxColor(uint32_t v)
{
    switch (v) {
    case 0: return EExifColorSpaceHint::Srgb;
    case 1: return EExifColorSpaceHint::AdobeRgb;
    default: return EExifColorSpaceHint::Unknown;
    }
}
EExifColorSpaceHint MapOlympusColor(uint32_t v)
{
    switch (v) {
    case 0: return EExifColorSpaceHint::Srgb;
    case 1: return EExifColorSpaceHint::AdobeRgb;
    case 2: return EExifColorSpaceHint::ProPhotoRgb;
    default: return EExifColorSpaceHint::Unknown;
    }
}

// How a vendor's blob is laid out. `sigLen == 0` means the blob carries no
// signature at all and the vendor is selected by Make alone -- Canon's shape,
// and the reason signature-matching cannot be the only rule.
enum class MnShape : uint8_t { FileRelative, SelfRelHeader, SelfRelNoHeader };

struct MakerVendor
{
    const char *name;
    const char *sig;          size_t sigLen;
    const char *makePrefix;                    // nullptr = signature only
    MnShape     shape;
    uint32_t    ifdStartInBlob;                // SelfRelNoHeader: bytes before the IFD
    const MakerTagName *names; size_t nameCount;
    uint16_t    colorSubDir;                   // 0 = colour tag at top level
    const MakerTagName *subNames; size_t subNameCount;
    uint16_t    colorTag;                      // 0 = vendor records none
    EExifColorSpaceHint (*mapColor)(uint32_t);
};

const MakerVendor kMakerVendors[] = {
    // Nikon type 3: "Nikon\0" + 2 version bytes + 2 pad, then a COMPLETE TIFF
    // header whose byte order may differ from the enclosing file's.
    {"Nikon", "Nikon\0", 6, "NIKON", MnShape::SelfRelHeader, 10,
     kMnNikon, std::size(kMnNikon), 0, nullptr, 0, 0x001E, &MapNikonColor},
    // Canon: a bare IFD, offsets relative to the enclosing TIFF header.
    {"Canon", "", 0, "Canon", MnShape::FileRelative, 0,
     kMnCanon, std::size(kMnCanon), 0, nullptr, 0, 0x00B4, &MapCanonColor},
    {"Pentax", "AOC\0", 4, "PENTAX", MnShape::SelfRelNoHeader, 6,
     kMnPentax, std::size(kMnPentax), 0, nullptr, 0, 0x0037, &MapPentaxColor},
    {"Pentax", "PENTAX \0", 8, "PENTAX", MnShape::SelfRelNoHeader, 10,
     kMnPentax, std::size(kMnPentax), 0, nullptr, 0, 0x0037, &MapPentaxColor},
    // Olympus, three generations. The NEW container ("OLYMPUS\0II") is
    // self-relative (the format declares its base at the blob start) and so is
    // OM SYSTEM's; the OLD one ("OLYMP\0") is FILE-relative -- a bare
    // directory behind a signature, with no base override. Getting the old one
    // wrong silently killed the colour hoist for the whole E-system
    // generation. Colour lives one level down in CameraSettings (0x2020).
    {"Olympus", "OLYMPUS\0II", 10, "OLYMPUS", MnShape::SelfRelNoHeader, 12,
     kMnOlympus, std::size(kMnOlympus), 0x2020, kMnOlympusCs, std::size(kMnOlympusCs),
     0x0507, &MapOlympusColor},
    {"Olympus", "OM SYSTEM\0", 10, "OM ", MnShape::SelfRelNoHeader, 16,
     kMnOlympus, std::size(kMnOlympus), 0x2020, kMnOlympusCs, std::size(kMnOlympusCs),
     0x0507, &MapOlympusColor},
    {"Olympus", "OLYMP\0", 6, "OLYMPUS", MnShape::FileRelative, 8,
     kMnOlympus, std::size(kMnOlympus), 0x2020, kMnOlympusCs, std::size(kMnOlympusCs),
     0x0507, &MapOlympusColor},
    // Decode-only vendors. Sony's colour space is not an IFD tag at all but a
    // byte at a model-dependent offset inside a binary block, with mutually
    // incompatible value mappings between blocks -- deliberately not decoded.
    // Fujifilm and Panasonic record no colour-space tag whatsoever; both reach
    // the right answer through the container or preview EXIF instead.
    //
    // Sony and Panasonic are FILE-relative for the same reason as old
    // Olympus: their formats declare a start offset but no base override.
    {"Sony", "SONY DSC \0\0\0", 12, "SONY", MnShape::FileRelative, 12,
     kMnSony, std::size(kMnSony), 0, nullptr, 0, 0, nullptr},
    {"Fujifilm", "FUJIFILM", 8, "FUJIFILM", MnShape::SelfRelNoHeader, 12,
     kMnFuji, std::size(kMnFuji), 0, nullptr, 0, 0, nullptr},
    {"Panasonic", "Panasonic\0\0\0", 12, "Panasonic", MnShape::FileRelative, 12,
     kMnPanasonic, std::size(kMnPanasonic), 0, nullptr, 0, 0, nullptr},
};

// Find one integer tag in an IFD without recursing. Used for colour hoisting,
// which must work whether or not allTags capture is on.
bool IfdFindUInt(const ParseCtx &c, uint32_t ifdOffset, uint16_t wantTag, uint32_t *outVal)
{
    if ((uint64_t)ifdOffset + 2 > c.size) return false;
    uint16_t n = read16(c.tiff + ifdOffset, c.le);
    if (n > 512) n = 512;
    uint32_t pos = ifdOffset + 2;
    for (uint16_t i = 0; i < n && (uint64_t)pos + 12 <= c.size; ++i, pos += 12)
    {
        const uint8_t *entry = c.tiff + pos;
        if (read16(entry, c.le) != wantTag) continue;
        uint16_t type = read16(entry + 2, c.le);
        uint32_t cnt  = read32(entry + 4, c.le);
        // Integer types only. EntryUInt's default returns 0 for ASCII and
        // RATIONAL, and a manufactured 0 here is not "nothing" -- for the
        // 0-based colour vendors it is a confident sRGB claim from a corrupt
        // entry, where absence is the correct answer.
        if (!(type == 1 || type == 3 || type == 4 || type == 6 ||
              type == 7 || type == 8 || type == 9 || type == 13))
            return false;
        const uint8_t *d = EntryData(c, entry, type, cnt);
        if (!d || cnt < 1) return false;
        *outVal = EntryUInt(c, d, type, 0);
        return true;
    }
    return false;
}

bool ParseMakerNote(ParseCtx &c, uint32_t blobOffset, uint32_t blobLen)
{
    const uint8_t *blob = c.tiff + blobOffset;

    // Smaller than one count + one entry + one link cannot be a directory in
    // any shape. Without this floor an INLINE (cnt <= 4) 0x927C from a Canon
    // file would walk the enclosing Exif IFD's remaining entries as a maker
    // directory -- bounds-safe, but garbage rows that then displace the honest
    // opaque one.
    if (blobLen < 18) return false;

    // Signature first: Make strings vary by region and firmware, several
    // vendors ship more than one format under one Make, and a re-mastered file
    // can carry a rewritten Make beside an untouched MakerNote.
    const MakerVendor *v = nullptr;
    for (const MakerVendor &cand : kMakerVendors)
    {
        if (cand.sigLen == 0) continue;
        if (blobLen >= cand.sigLen && memcmp(blob, cand.sig, cand.sigLen) == 0) { v = &cand; break; }
    }
    // Only then Make, which is what makes a signature-less vendor reachable.
    if (!v)
    {
        for (const MakerVendor &cand : kMakerVendors)
        {
            if (cand.sigLen != 0 || !cand.makePrefix) continue;
            if (c.out->make.compare(0, strlen(cand.makePrefix), cand.makePrefix) == 0)
            { v = &cand; break; }
        }
    }
    if (!v) return false;   // unknown: leave today's opaque row alone

    // Save everything the walk re-points. Without restoring, the enclosing
    // directory's remaining entries would be read against the blob's base.
    const uint8_t *savedTiff = c.tiff;
    const size_t   savedSize = c.size;
    const bool     savedLe   = c.le;
    const uint32_t savedGuard = c.guardBase;
    const MakerTagName *savedNames = c.vendorNames;
    const size_t   savedNameCount  = c.vendorNameCount;

    uint32_t ifdStart = 0;
    bool ok = true;
    switch (v->shape)
    {
    case MnShape::FileRelative:
        // Offsets already resolve against the enclosing TIFF header; the blob
        // IS a directory sitting inside it, possibly behind a signature.
        // c.tiff/c.size/guardBase stay on the enclosing file -- re-pointing
        // them at the blob is exactly the misclassification that silently
        // broke Sony/Panasonic/old-Olympus out-of-line values (their
        // MakerNotes have Start offsets but NO base override in the format).
        if (blobLen <= v->ifdStartInBlob) { ok = false; break; }
        ifdStart = blobOffset + v->ifdStartInBlob;
        break;
    case MnShape::SelfRelHeader:
    {
        if (blobLen < v->ifdStartInBlob + 8) { ok = false; break; }
        const uint8_t *hdr = blob + v->ifdStartInBlob;
        bool le;
        if      (hdr[0] == 'I' && hdr[1] == 'I') le = true;
        else if (hdr[0] == 'M' && hdr[1] == 'M') le = false;
        else { ok = false; break; }
        // Byte order comes from the EMBEDDED header, not the enclosing file.
        // Inheriting it yields garbage tag IDs, and the mismatch is real.
        c.le   = le;
        c.tiff = hdr;
        c.size = blobLen - v->ifdStartInBlob;
        c.guardBase = savedGuard + blobOffset + v->ifdStartInBlob;
        ifdStart = read32(hdr + 4, le);
        break;
    }
    case MnShape::SelfRelNoHeader:
        if (blobLen <= v->ifdStartInBlob) { ok = false; break; }
        c.tiff = blob;
        c.size = blobLen;
        c.guardBase = savedGuard + blobOffset;
        ifdStart = v->ifdStartInBlob;
        break;
    }

    const size_t rowsBefore = c.out->allTags.size();
    bool hoisted = false;

    if (ok)
    {
        c.vendorNames = v->names; c.vendorNameCount = v->nameCount;
        c.depth++; ParseIFD(c, ifdStart, EExifIFD::MakerNote); c.depth--;

        // Colour hoisting, independent of allTags capture.
        if (v->colorTag && v->mapColor)
        {
            uint32_t raw = 0; bool found = false;
            if (v->colorSubDir)
            {
                // Olympus keeps ColorSpace in a sub-directory of its MakerNote.
                uint32_t subOff = 0;
                if (IfdFindUInt(c, ifdStart, v->colorSubDir, &subOff))
                    found = IfdFindUInt(c, subOff, v->colorTag, &raw);
            }
            else found = IfdFindUInt(c, ifdStart, v->colorTag, &raw);

            if (found)
            {
                EExifColorSpaceHint h = v->mapColor(raw);
                if (h != EExifColorSpaceHint::Unknown)
                {
                    c.out->makerColorSpace       = h;
                    c.out->makerColorSpaceSource = EExifColorHintSource::MakerNote;
                    c.out->makerColorSpaceVendor = v->name;
                    hoisted = true;
                }
            }
        }
    }

    c.tiff = savedTiff; c.size = savedSize; c.le = savedLe;
    c.guardBase = savedGuard;
    c.vendorNames = savedNames; c.vendorNameCount = savedNameCount;

    // "Handled" means we actually produced something. Recognising the vendor is
    // not enough: a truncated or corrupt blob can pass the shape check and then
    // yield no rows and no hint, and reporting success there would make the
    // caller drop the opaque "(N bytes)" row it would otherwise have shown --
    // strictly less information than before this phase existed.
    return ok && (hoisted || c.out->allTags.size() > rowsBefore);
}

// Apply GPS refs collected during the walk. Call once after parsing finishes.
void FinalizeGps(ParseCtx &c)
{
    CExifData &out = *c.out;
    // RANGE-validated, not just "not the sentinel". The degrees/minutes/seconds
    // rationals come straight out of the file with no bound of their own, so a
    // corrupt or crafted GPSLatitude can produce any magnitude at all. That
    // value is not merely displayed: the export path converts it with
    // `(uint32_t)absDeg`, and a double outside the destination range makes that
    // cast undefined behaviour (x86 yields 0x80000000, ARM saturates), writing
    // a garbage coordinate into the user's exported file. Out of range means
    // corrupt, so the whole fix is dropped rather than clamped -- the same
    // treatment GPSImgDirectionRef already gets below.
    if (c.latAbs > -999.0 && c.lonAbs > -999.0 &&
        c.latAbs <= 90.0 && c.lonAbs <= 180.0)
    {
        out.hasGps = true;
        out.gpsLatitude  = (c.latRef == 'S') ? -c.latAbs : c.latAbs;
        out.gpsLongitude = (c.lonRef == 'W') ? -c.lonAbs : c.lonAbs;
    }
    // Altitude gets the same range check as latitude and longitude, and for the
    // same reason: it comes from an unbounded rational, and the export path
    // converts it with `(uint32_t)lround(fabs(alt) * 100.0)` -- which is
    // unspecified on Windows, where long is 32-bit, and wraps elsewhere. A
    // corrupt 4294967295/1 became a confident "12,157,521.92 m" in the user's
    // exported file. 100 km is far above any aircraft and far below anything a
    // wrapped value produces.
    if (c.altAbs > -999.0 && c.altAbs <= 100000.0)
    {
        out.gpsAltitudeValid = true;
        out.gpsAltitude = (c.altRef == 1) ? -c.altAbs : c.altAbs;
    }
    if (c.speedAbs >= 0.0)
    {
        const char ref = c.speedRef ? c.speedRef : 'K';   // EXIF default is K when the ref is ABSENT (#2.4)
        switch (ref) {
        case 'K': out.gpsSpeedKmh = c.speedAbs;            out.gpsSpeedValid = true; break;  // km/h
        case 'M': out.gpsSpeedKmh = c.speedAbs * 1.609344; out.gpsSpeedValid = true; break;  // miles/hour
        case 'N': out.gpsSpeedKmh = c.speedAbs * 1.852;    out.gpsSpeedValid = true; break;  // knots
        default:  out.gpsSpeedValid = false; break;   // UNKNOWN ref -> corrupt -> absent, not km/h (fu-b finding r3)
        }
        if (out.gpsSpeedValid) out.gpsSpeedRef = ref;
    }
    // Direction ref, order-independent (FinalizeGps runs after all cases): a present-but-unknown
    // ImgDirectionRef (not 'T'/'M') is corrupt -> invalidate the heading. Absent ref (0) stays valid
    // (true north assumed). fu-b finding r3.
    if (out.gpsImgDirectionValid && out.gpsImgDirectionRef != 0 &&
        out.gpsImgDirectionRef != 'T' && out.gpsImgDirectionRef != 'M')
        out.gpsImgDirectionValid = false;
}

// Parse a TIFF-structured buffer (used by every container path).
// startIfd tells the walker which tag space the root IFD uses (CR3's CMT2
// stores Exif-IFD tags as a root IFD, CMT4 stores GPS tags — see Task 4).
// The 4-byte next-IFD link that terminates a directory, or 0 when there is none
// or it cannot be trusted.
//
// The BOUNDS CHECK COMES FIRST, before the count is read: ifdOffset is
// attacker-controlled at both call sites -- the first is a raw read32 from the
// TIFF header against a buffer that may be 8 bytes long, and every later one is
// an unvalidated link value, because ParseIFD merely returns on a bad offset
// without telling anyone.
//
// It also uses the RAW count. ParseIFD clamps its own copy to 512 as a
// malformed-file guard, but the link sits after however many entries the file
// DECLARES, so the clamped value would point into the middle of the directory.
// A directory declaring more than 512 entries is untrustworthy enough that its
// link is not worth following at all.
uint32_t IfdNextOffset(const ParseCtx &c, uint32_t ifdOffset)
{
    if ((uint64_t)ifdOffset + 2 > c.size) return 0;
    uint32_t rawCount = read16(c.tiff + ifdOffset, c.le);
    if (rawCount > 512) return 0;
    uint64_t linkAt = (uint64_t)ifdOffset + 2 + (uint64_t)rawCount * 12;
    if (linkAt + 4 > c.size) return 0;
    return read32(c.tiff + (uint32_t)linkAt, c.le);
}

// CR3's CMT3 box: Canon MakerNote tags stored as a root IFD in their own TIFF
// wrapper, rather than as a 0x927C blob inside the Exif IFD. It therefore never
// passes through vendor detection, so the Canon colour tag has to be hoisted
// here or a CR3 would decode its MakerNote and hoist nothing.
void ParseCr3MakerNoteBox(const uint8_t *tiff, size_t size, CExifData &out, bool captureAll);

void ParseTiffBuffer(const uint8_t *tiff, size_t size, CExifData &out, bool captureAll,
                     bool captureMakerNotes, EExifIFD startIfd = EExifIFD::IFD0)
{
    if (size < 8) return;
    bool le = (tiff[0] == 'I' && tiff[1] == 'I');
    bool be = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!le && !be) return;
    // magic 0x002A (TIFF/DNG/CR2/NEF/ARW/PEF), 0x4F52/0x5253 (ORF), 0x0055 (RW2)
    uint16_t magic = le ? (uint16_t)(tiff[2] | (tiff[3] << 8)) : (uint16_t)((tiff[2] << 8) | tiff[3]);
    if (magic != 0x002A && magic != 0x4F52 && magic != 0x5253 && magic != 0x0055) return;
    ParseCtx c;
    c.tiff = tiff; c.size = size; c.le = le; c.captureAll = captureAll; c.out = &out;
    c.captureMakerNotes = captureMakerNotes;
    uint32_t ifd0 = read32(tiff + 4, le);
    ParseIFD(c, ifd0, startIfd);

    // The IFD1 chain: thumbnail directories, and where some vendors put preview
    // descriptors. Walked as its OWN kind -- typed extraction is gated to
    // IFD0/Exif and assigns unconditionally, so walking IFD1 as IFD0 would let a
    // thumbnail's Orientation/Make/DateTimeOriginal overwrite the real ones.
    //
    // The loop, not ParseIFD, owns termination. ParseIFD's budget and cycle
    // guards are `return`s inside a void function, so a caller cannot observe
    // them: a self-referential link would spin here forever with no budget ever
    // charged. Hence both the membership test below AND the hard link cap --
    // the cap is load-bearing, because when ParseIFD bails early it returns
    // before recording the offset it was given.
    if (captureMakerNotes)
    {
        uint32_t cur = ifd0;
        for (int k = 0; k < kMaxIfdChainLinks; ++k)
        {
            uint32_t next = IfdNextOffset(c, cur);
            if (!next) break;
            bool seen = false;
            for (uint32_t s : c.visitedIfds) if (s == c.guardBase + next) { seen = true; break; }
            // Skip rather than stop: a file may list its thumbnail directory
            // both in 0x014A and on the chain, and breaking there would drop
            // every later link with it.
            if (!seen) ParseIFD(c, next, EExifIFD::IFD1);
            cur = next;
        }
    }

    FinalizeGps(c);
    out.valid = true;
}

void ParseCr3MakerNoteBox(const uint8_t *tiff, size_t size, CExifData &out, bool captureAll)
{
    if (size < 8) return;
    bool le = (tiff[0] == 'I' && tiff[1] == 'I');
    bool be = (tiff[0] == 'M' && tiff[1] == 'M');
    if (!le && !be) return;
    // Validate the magic too, exactly as ParseTiffBuffer does. Without it any
    // box body that happens to begin "II" is walked as a directory.
    uint16_t magic = le ? (uint16_t)(tiff[2] | (tiff[3] << 8)) : (uint16_t)((tiff[2] << 8) | tiff[3]);
    if (magic != 0x002A) return;

    ParseCtx c;
    c.tiff = tiff; c.size = size; c.le = le; c.captureAll = captureAll; c.out = &out;
    c.captureMakerNotes = true;
    c.vendorNames = kMnCanon; c.vendorNameCount = std::size(kMnCanon);

    uint32_t ifd0 = read32(tiff + 4, le);
    ParseIFD(c, ifd0, EExifIFD::MakerNote);

    uint32_t raw = 0;
    if (IfdFindUInt(c, ifd0, 0x00B4, &raw))
    {
        EExifColorSpaceHint h = MapCanonColor(raw);
        if (h != EExifColorSpaceHint::Unknown)
        {
            out.makerColorSpace       = h;
            out.makerColorSpaceSource = EExifColorHintSource::MakerNote;
            out.makerColorSpaceVendor = "Canon";
        }
    }
}

uint32_t beU32(const uint8_t *p) { return ((uint32_t)p[0]<<24)|((uint32_t)p[1]<<16)|((uint32_t)p[2]<<8)|p[3]; }
uint16_t beU16(const uint8_t *p) { return (uint16_t)((p[0]<<8)|p[1]); }

// Iterate ISO-BMFF boxes in [pos, end); calls fn(type4cc, bodyPtr, bodyLen).
// Handles 32-bit sizes and size==1 (64-bit largesize). Returns on malformed sizes.
template <typename F>
void WalkBoxes(const uint8_t *data, size_t pos, size_t end, F fn)
{
    while (pos + 8 <= end)
    {
        uint64_t sz = beU32(data + pos);
        const uint8_t *type = data + pos + 4;
        size_t hdr = 8;
        if (sz == 1) {
            if (pos + 16 > end) return;
            sz = ((uint64_t)beU32(data + pos + 8) << 32) | beU32(data + pos + 12);
            hdr = 16;
        }
        // Overflow-safe bound: pos + 8 <= end already held (loop condition),
        // so end - pos is a safe, non-negative size_t subtraction. Computing
        // pos + sz directly would wrap when sz is an attacker-controlled huge
        // 64-bit largesize, defeating the check.
        if (sz < hdr || sz > (uint64_t)(end - pos)) return;
        fn(type, data + pos + hdr, (size_t)(sz - hdr));
        pos += (size_t)sz;
    }
}

// HEIF/HEIC: find the 'Exif' item via meta/iinf/iloc; parse its TIFF payload.
// CR3: find Canon uuid box under moov; parse CMT1/CMT2/CMT4 as TIFF blobs.
bool ParseBmff(const uint8_t *data, size_t size, CExifData &out, bool captureAll,
               bool captureMakerNotes)
{
    static const uint8_t kCanonUuid[16] = { 0x85,0xC0,0xB6,0x87,0x82,0x0F,0x11,0xE0,
                                            0x81,0x11,0xF4,0xCE,0x46,0x2B,0x6A,0x48 };
    bool found = false;
    uint32_t exifItemId = 0;
    uint64_t exifOff = 0, exifLen = 0;

    WalkBoxes(data, 0, size, [&](const uint8_t *type, const uint8_t *body, size_t blen)
    {
        if (memcmp(type, "meta", 4) == 0 && blen > 4)
        {
            // fullbox: skip version/flags, then children
            WalkBoxes(body, 4, blen, [&](const uint8_t *t2, const uint8_t *b2, size_t l2)
            {
                if (memcmp(t2, "iinf", 4) == 0 && l2 >= 6)
                {
                    uint8_t ver = b2[0];
                    size_t p = (ver == 0) ? 6 : 8; // version0: u16 count, else u32
                    WalkBoxes(b2, p, l2, [&](const uint8_t *t3, const uint8_t *b3, size_t l3)
                    {
                        if (memcmp(t3, "infe", 4) != 0 || l3 < 12) return;
                        uint8_t iver = b3[0];
                        uint32_t itemId; size_t q;
                        if (iver == 2)      { itemId = beU16(b3 + 4); q = 8; }
                        else if (iver == 3) { itemId = beU32(b3 + 4); q = 10; }
                        else return;
                        if (l3 >= q + 4 && memcmp(b3 + q, "Exif", 4) == 0) exifItemId = itemId;
                    });
                }
                else if (memcmp(t2, "iloc", 4) == 0 && l2 >= 8)
                {
                    uint8_t ver = b2[0];
                    uint8_t offSize  = b2[4] >> 4, lenSize = b2[4] & 0xF;
                    uint8_t baseSize = b2[5] >> 4, idxSize = (ver >= 1) ? (b2[5] & 0xF) : 0;
                    size_t p = 6;
                    uint32_t itemCount;
                    if (ver < 2) { if (l2 < p + 2) return; itemCount = beU16(b2 + p); p += 2; }
                    else         { if (l2 < p + 4) return; itemCount = beU32(b2 + p); p += 4; }
                    auto rdN = [&](size_t &pp, uint8_t nBytes) -> uint64_t {
                        uint64_t v = 0;
                        for (uint8_t k = 0; k < nBytes && pp < l2; ++k) v = (v << 8) | b2[pp++];
                        return v;
                    };
                    for (uint32_t it = 0; it < itemCount && p + 4 <= l2; ++it)
                    {
                        uint32_t itemId;
                        if (ver < 2) { itemId = beU16(b2 + p); p += 2; }
                        else         { itemId = beU32(b2 + p); p += 4; }
                        uint8_t constructionMethod = 0;
                        if (ver >= 1) {
                            if (p + 2 > l2) return;
                            constructionMethod = b2[p + 1] & 0xF; // upper 12 bits reserved
                            p += 2;                            // construction_method
                        }
                        p += 2;                                // data_reference_index
                        uint64_t base = rdN(p, baseSize);
                        if (p + 2 > l2) return;
                        uint16_t extents = beU16(b2 + p); p += 2;
                        for (uint16_t ex = 0; ex < extents; ++ex)
                        {
                            rdN(p, idxSize);
                            uint64_t eo = rdN(p, offSize);
                            uint64_t el = rdN(p, lenSize);
                            // Only construction_method 0 (absolute file offset) is
                            // handled here; method 1 (idat-relative) / 2 (item-relative)
                            // would mis-slice if treated as absolute -> skip safely.
                            if (ex == 0 && itemId == exifItemId && exifItemId != 0
                                && constructionMethod == 0)
                            { exifOff = base + eo; exifLen = el; }
                        }
                    }
                }
            });
        }
        else if (memcmp(type, "moov", 4) == 0) // CR3
        {
            WalkBoxes(body, 0, blen, [&](const uint8_t *t2, const uint8_t *b2, size_t l2)
            {
                if (memcmp(t2, "uuid", 4) != 0 || l2 < 16) return;
                if (memcmp(b2, kCanonUuid, 16) != 0) return;
                WalkBoxes(b2, 16, l2, [&](const uint8_t *t3, const uint8_t *b3, size_t l3)
                {
                    if (memcmp(t3, "CMT1", 4) == 0) { ParseTiffBuffer(b3, l3, out, captureAll, captureMakerNotes, EExifIFD::IFD0); found = true; }
                    if (memcmp(t3, "CMT2", 4) == 0) { ParseTiffBuffer(b3, l3, out, captureAll, captureMakerNotes, EExifIFD::Exif); found = true; }
                    // CMT3 is Canon's MakerNote box. It reaches ParseIFD directly,
                    // bypassing the 0x927C vendor-detection path, so the Canon
                    // colour tag is hoisted separately below.
                    if (captureMakerNotes && memcmp(t3, "CMT3", 4) == 0)
                    { ParseCr3MakerNoteBox(b3, l3, out, captureAll); found = true; }
                    if (memcmp(t3, "CMT4", 4) == 0) { ParseTiffBuffer(b3, l3, out, captureAll, captureMakerNotes, EExifIFD::GPS);  found = true; }
                });
            });
        }
    });

    // Overflow-safe extent bound: exifOff and exifLen are attacker-controlled
    // 64-bit values assembled from iloc nibble-sized fields (each up to 8
    // bytes), so exifOff + exifLen can wrap. Require exifOff <= size first,
    // then bound exifLen via a safe subtraction (size - exifOff is
    // non-negative once exifOff <= size holds).
    if (!found && exifItemId != 0 && exifOff <= size && exifLen <= size - exifOff && exifLen > 4)
    {
        // Payload: u32 exif_tiff_header_offset, then (optionally "Exif\0\0" +) TIFF.
        const uint8_t *p = data + exifOff;
        uint32_t tiffHdrOff = beU32(p);
        if ((uint64_t)4 + tiffHdrOff < exifLen)
        {
            const uint8_t *t = p + 4 + tiffHdrOff;
            size_t tl = (size_t)(exifLen - 4 - tiffHdrOff);
            if (tl >= 6 && memcmp(t, "Exif\0\0", 6) == 0) { t += 6; tl -= 6; }
            ParseTiffBuffer(t, tl, out, captureAll, captureMakerNotes);
            found = true;
        }
    }
    return found;
}

} // namespace

CExifData CExifReader::Read(const uint8_t *data, size_t size, bool captureAllTags,
                            bool captureIcc, bool captureMakerNotes)
{
    CExifData out;
    if (!data || size < 4) return out;

    // Check TIFF magic (covers DNG, CR2, NEF, ARW, and other TIFF-based RAWs,
    // plus the ORF/RW2 magic variants accepted inside ParseTiffBuffer).
    bool isTiffLE = (data[0] == 0x49 && data[1] == 0x49 && data[2] == 0x2A && data[3] == 0x00);
    bool isTiffBE = (data[0] == 0x4D && data[1] == 0x4D && data[2] == 0x00 && data[3] == 0x2A);
    bool isOrfRw2 = ((data[0] == 0x49 && data[1] == 0x49) || (data[0] == 0x4D && data[1] == 0x4D)) &&
                    ((data[2] == 0x52 && data[3] == 0x4F) || (data[2] == 0x53 && data[3] == 0x52) ||
                     (data[2] == 0x55 && data[3] == 0x00));
    if (isTiffLE || isTiffBE || isOrfRw2)
    {
        if (size < 8) return out;
        ParseTiffBuffer(data, size, out, captureAllTags, captureMakerNotes);
        return out;
    }

    // ISO-BMFF (HEIC/HEIF/CR3): 'ftyp' at offset 4.
    if (size > 12 && memcmp(data + 4, "ftyp", 4) == 0)
    {
        ParseBmff(data, size, out, captureAllTags, captureMakerNotes);
        return out;
    }

    // PNG: walk chunks looking for eXIf (raw TIFF payload).
    static const uint8_t pngSig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
    if (size > 8 && memcmp(data, pngSig, 8) == 0)
    {
        size_t pos = 8;
        bool exifDone = false;
        bool iccDone = !captureIcc;
        while (pos + 8 <= size)
        {
            uint32_t len = ((uint32_t)data[pos]<<24)|((uint32_t)data[pos+1]<<16)|((uint32_t)data[pos+2]<<8)|data[pos+3];
            const uint8_t *type = data + pos + 4;
            if ((uint64_t)pos + 12 + len > size) break;
            if (!exifDone && memcmp(type, "eXIf", 4) == 0)
            {
                ParseTiffBuffer(data + pos + 8, len, out, captureAllTags, captureMakerNotes);
                exifDone = true;
                // Keep walking when ICC is still wanted: iCCP may sit either
                // side of eXIf, so returning here would drop it at random.
                if (iccDone) return out;
            }
            else if (!iccDone && memcmp(type, "iCCP", 4) == 0)
            {
                out.iccProfile = CIccProfileCodec::InflateIccp(data + pos + 8, len);
                iccDone = true;
                if (exifDone) return out;
            }
            if (memcmp(type, "IEND", 4) == 0) break;
            pos += 12 + len;
        }
        return out;
    }

    // Fujifilm RAF: embedded JPEG offset is big-endian u32 at byte 84.
    if (size > 92 && memcmp(data, "FUJIFILMCCD-RAW", 15) == 0)
    {
        static const uint32_t kRafHeaderSize = 92; // magic/header occupies bytes [0,92); JPEG must start past it
        uint32_t joff = ((uint32_t)data[84]<<24)|((uint32_t)data[85]<<16)|((uint32_t)data[86]<<8)|data[87];
        // Require the offset to land strictly past the header so the recursive Read() call is
        // always on a strictly smaller/forward slice. A joff of 0 (corrupted/truncated/zero-padded
        // RAF) would otherwise re-enter this exact branch with identical arguments -> infinite recursion.
        if (joff > kRafHeaderSize && (uint64_t)joff + 4 < size)
            // Forward captureIcc too: a RAF's embedded JPEG is the ONLY place its
            // ICC profile can live, so dropping the flag here would disable ICC
            // capture for Fujifilm alone, silently.
            // Forward EVERY flag. A RAF's embedded JPEG is the only place its
            // metadata lives, so dropping one here disables that feature for
            // Fujifilm alone, silently -- the same trap captureIcc documents.
            return Read(data + joff, size - joff, captureAllTags, captureIcc,
                        captureMakerNotes); // recurse into the JPEG
        return out;
    }

    // Check JPEG SOI
    if (data[0] != 0xFF || data[1] != 0xD8) return out;

    size_t pos = 2;
    bool exifDone = false;
    std::vector<std::vector<uint8_t> > app2Segments;
    while (pos + 4 <= size) {
        if (data[pos] != 0xFF) break;
        uint8_t marker = data[pos + 1];
        if (marker == 0xD9) break; // EOI
        if (marker == 0xDA) break; // SOS: metadata all precedes the scan

        if (pos + 4 > size) break;
        uint16_t segLen = (uint16_t)((data[pos+2] << 8) | data[pos+3]);
        if (segLen < 2 || pos + 2 + segLen > size) break;

        if (!exifDone && marker == 0xE1 && segLen >= 8) {
            // Check "Exif\0\0"
            const uint8_t *seg = data + pos + 4;
            if (memcmp(seg, "Exif\0\0", 6) == 0) {
                const uint8_t *tiff = seg + 6;
                size_t tiffSize     = segLen - 2 - 6;
                // A malformed Exif payload ends our interest in EXIF, but it
                // must NOT end the walk: ICC lives in later APP2 segments, and
                // abandoning here would drop a perfectly good profile because
                // some other metadata block was broken.
                if (tiffSize >= 8)
                    ParseTiffBuffer(tiff, tiffSize, out, captureAllTags, captureMakerNotes);
                exifDone = true;
                if (!captureIcc) break;
            }
        }
        else if (captureIcc && marker == 0xE2 && segLen > 2) {
            const uint8_t *seg = data + pos + 4;
            app2Segments.push_back(std::vector<uint8_t>(seg, seg + (segLen - 2)));
        }
        pos += 2 + segLen;
    }
    if (captureIcc && !app2Segments.empty())
        out.iccProfile = CIccProfileCodec::JoinApp2(app2Segments);
    return out;
}

CExifData CExifReader::ReadFile(const std::string &path, bool captureIcc,
                                bool captureMakerNotes)
{
    // SYS_Utf8ToFsPath, not the raw std::string: `path` is UTF-8, and
    // std::ifstream's narrow constructor decodes it with the process ANSI code
    // page on Windows -- a photo under a non-ASCII folder or filename would
    // silently fail to open. See SYS_FileUtf8.h.
    std::ifstream f(SYS_Utf8ToFsPath(path), std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    size_t readSz = (size_t)std::min((std::streampos)(16 * 1024 * 1024), sz);
    f.seekg(0);
    std::vector<uint8_t> buf(readSz);
    f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)readSz);
    return Read(buf.data(), buf.size(), true, captureIcc, captureMakerNotes);
}

CExifData CExifReader::ReadFileHeader(const std::string &path, size_t maxBytes,
                                      bool captureIcc, bool captureMakerNotes)
{
    // SYS_Utf8ToFsPath, not the raw std::string: `path` is UTF-8, and
    // std::ifstream's narrow constructor decodes it with the process ANSI code
    // page on Windows -- a photo under a non-ASCII folder or filename would
    // silently fail to open. See SYS_FileUtf8.h.
    std::ifstream f(SYS_Utf8ToFsPath(path), std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto sz = f.tellg();
    if (sz <= 0) return {};
    size_t readSz = (size_t)std::min((std::streampos)maxBytes, sz);
    f.seekg(0);
    std::vector<uint8_t> buf(readSz);
    f.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)readSz);
    return Read(buf.data(), buf.size(), false, captureIcc, captureMakerNotes);
}

std::string CExifReader::FormatShutter(float seconds)
{
    if (seconds <= 0.f) return "";
    if (seconds >= 1.f) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.1f\"", (double)seconds);
        return buf;
    }
    int denom = (int)std::round(1.f / seconds);
    char buf[32];
    snprintf(buf, sizeof(buf), "1/%d", denom);
    return buf;
}

std::string CExifReader::FormatAperture(float f)
{
    if (f <= 0.f) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "f/%.1f", (double)f);
    return buf;
}

std::string CExifReader::FormatFocalLength(float mm)
{
    if (mm <= 0.f) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0fmm", (double)mm);
    return buf;
}

std::string CExifReader::FormatExposureBias(float ev)
{
    if (ev == 0.f) return "";
    char buf[32];
    snprintf(buf, sizeof(buf), "%+.1f EV", (double)ev);
    return buf;
}

std::string CExifReader::FormatGpsCoord(double lat, double lon)
{
    char buf[64];
    snprintf(buf, sizeof(buf), "%.5f %c, %.5f %c",
             std::fabs(lat), lat >= 0 ? 'N' : 'S',
             std::fabs(lon), lon >= 0 ? 'E' : 'W');
    return buf;
}

std::string CExifReader::FormatGpsAltitude(double meters)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "%.0f m", meters);
    return buf;
}
