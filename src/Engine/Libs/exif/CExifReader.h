#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Which IFD a captured tag came from. Also useful for grouping on display.
//
// APPEND ONLY -- hosts group tags for display by this value. CM-C1 added the
// last three. IFD1 is deliberately its OWN kind rather than being folded into
// IFD0: typed extraction is gated to IFD0/Exif and assigns unconditionally, and
// a thumbnail directory legitimately repeats Orientation, Make, Model and
// DateTimeOriginal. Walking IFD1 as IFD0 would let the thumbnail's copies
// overwrite the real ones -- the exact failure the long comment on tag 0x0112
// in ParseIFD was written about.
enum class EExifIFD : uint8_t
{ IFD0 = 0, Exif = 1, GPS = 2, Interop = 3, SubIFD = 4, MakerNote = 5, IFD1 = 6 };

// What colour space a file claims, and which signal said so (CM-C1 #6).
//
// CM-C1 SUPPLIES EVIDENCE AND DECIDES NOTHING: these are never combined here,
// and the ordering between signals belongs to the host (CM-C2's chain).
//
// Bt2100 is required -- Nikon 0x001E value 4 is a real, named space (Z8 with
// Tone Mode HLG). Collapsing it into Unknown would be wrong twice: the file did
// say what it is, and the host's "space we cannot honour" path is reachable
// only for a named space.
enum class EExifColorSpaceHint : uint8_t
{ Unknown = 0, Srgb, AdobeRgb, ProPhotoRgb, Gray22, Bt2100 };

// Unknown + None means NO SIGNAL SPOKE. A sentinel that records "nothing" --
// Canon 0x00B4 == 65535 ("n/a") -- reports exactly this, so a host chain falls
// THROUGH it rather than stopping. Embedded ICC is deliberately absent: a
// profile is bytes, not an enum, and travels on CExifData::iccProfile.
enum class EExifColorHintSource : uint8_t
{ None = 0, DngPreviewColorSpace, PreviewExif, ContainerColorSpace, InteropIndex, MakerNote };

// One DNG PreviewColorSpace (0xC71A) reading, with the declared pixel size of
// the IFD it came from.
//
// Collected, never resolved. Matching a candidate to the preview we actually
// decoded needs that preview's dimensions, which do not exist while metadata is
// being read -- the host reads EXIF before it decodes. So every candidate is
// reported and the host picks (CM-C2 #3.2).
//
// ORDER IS DEPTH-FIRST, not strictly file order: a directory's own candidate is
// appended after those of the SubIFDs it points at, because the walker emits
// when a directory ends. Candidates from sibling SubIFDs are in array order.
// A host that falls back to "the first candidate" is choosing the innermost
// one, which for a DNG is a preview rather than the full-resolution IFD --
// usually what is wanted, but it is a choice, not an accident.
struct CExifDngPreviewCandidate
{
    uint16_t colorSpace = 0;   // raw DNG value: 1 Gray2.2, 2 sRGB, 3 AdobeRGB, 4 ProPhoto
    uint32_t width      = 0;   // 0 when the IFD declared none
    uint32_t height     = 0;
};

// One raw EXIF directory entry, name-resolved and value-formatted for display.
struct CExifTagEntry
{
    uint16_t    tag   = 0;
    EExifIFD    ifd   = EExifIFD::IFD0;
    uint16_t    type  = 0;   // TIFF type 1..12
    uint32_t    count = 0;
    std::string name;        // "LensModel", or "Tag 0x1234" if unknown
    std::string value;       // human-readable (enum labels decoded)
};

struct CExifData
{
    // ---- typed fields (hoisted for display, sorting and orientation) ----
    std::string make;
    std::string model;
    std::string lensMake;
    std::string lensModel;
    std::string dateTimeOriginal;   // "YYYY:MM:DD HH:MM:SS" or empty
    std::string subSecTimeOriginal; // fractional seconds digits, e.g. "042"
    std::string offsetTimeOriginal; // "+02:00" or empty
    std::string software;
    std::string artist;
    std::string copyright;
    std::string imageDescription;
    std::string bodySerialNumber;
    std::string cameraOwnerName;
    float fNumber        = 0.f;
    float exposureTime   = 0.f;    // seconds
    int   isoSpeed       = 0;
    float focalLength    = 0.f;    // mm
    int   focalLength35mm = 0;     // mm equivalent, 0 if absent
    float exposureBias   = 0.f;    // EV
    bool  exposureBiasValid = false; // set in the 0x9204 case; distinguishes 0 EV from absent (A4 #2.3.2)
    float maxAperture    = 0.f;    // f-number (APEX-converted), 0 if absent
    float digitalZoomRatio = 0.f;
    int   orientation    = 1;      // EXIF orientation 1..8
    int   pixelWidth     = 0;      // PixelXDimension
    int   pixelHeight    = 0;      // PixelYDimension
    // enum-coded tags, -1 = absent (raw EXIF value otherwise)
    int   flash           = -1;
    int   exposureProgram = -1;
    int   meteringMode    = -1;
    int   whiteBalance    = -1;
    int   lightSource     = -1;
    int   exposureMode    = -1;
    int   sceneCaptureType = -1;
    int   colorSpace      = -1;
    // ---- GPS (decimal degrees, signed: S/W negative) ----
    bool   hasGps        = false;
    double gpsLatitude   = 0.0;
    double gpsLongitude  = 0.0;
    bool   gpsAltitudeValid = false;
    double gpsAltitude   = 0.0;    // meters; negative = below sea level
    double gpsImgDirection = -1.0; // degrees, -1 = absent
    std::string gpsDateStamp;      // "YYYY:MM:DD"
    std::string gpsTimeStamp;      // "HH:MM:SS" (UTC)
    // ---- GPS, Project B additions (fu-b #2.2) ----
    bool   gpsImgDirectionValid = false;  // 0.0 is due north; the -1.0 sentinel above stays for compat, new code tests this bool
    char   gpsImgDirectionRef   = 0;      // 'T' true / 'M' magnetic / 0 = absent
    bool   gpsSpeedValid        = false;  // separate bool: 0 km/h is a real reading, not absent
    double gpsSpeedKmh          = 0.0;    // canonical km/h, converted at finalize
    char   gpsSpeedRef          = 0;      // 'K'/'M'/'N' as recorded, 0 = absent
    // Interoperability IFD tag 0x0001: "R98" = sRGB, "R03" = Adobe RGB, "" absent.
    // The disambiguator for ColorSpace == 0xFFFF ("Uncalibrated"), which is how
    // many Adobe RGB camera JPEGs actually announce themselves.
    std::string interopIndex;
    // Reassembled ICC profile bytes; empty when the file carries none. Only
    // filled when the caller passes captureIcc -- the lean metadata paths do no
    // ICC work at all.
    std::vector<uint8_t> iccProfile;
    // ---- CM-C1: reach beyond IFD0 (all require captureMakerNotes) ----
    // Every DNG PreviewColorSpace found. Never resolved here; see the struct
    // above for the ordering, which is depth-first rather than file order.
    std::vector<CExifDngPreviewCandidate> dngPreviewCandidates;
    // The vendor MakerNote's colour space, when the vendor records one and we
    // can read it. Hoisted for Nikon/Canon/Pentax/Olympus only; Sony's is
    // model-dependent binary data we deliberately do not decode, and Fujifilm
    // and Panasonic record no such tag at all.
    EExifColorSpaceHint  makerColorSpace       = EExifColorSpaceHint::Unknown;
    EExifColorHintSource makerColorSpaceSource = EExifColorHintSource::None;
    std::string          makerColorSpaceVendor;   // "Nikon", "Canon", ... when source is MakerNote
    // ---- everything, in file order ----
    std::vector<CExifTagEntry> allTags;
    bool  valid          = false;
};

class CExifReader
{
public:
    // Parse EXIF from in-memory bytes of any supported container
    // (JPEG APP1, TIFF-based RAW, PNG eXIf, RAF, HEIC/HEIF, CR3).
    // captureAllTags=false skips filling allTags (lean path for the
    // date-sorting callers that only need dateTimeOriginal).
    // captureIcc additionally collects the file's ICC profile: JPEG APP2
    // segments (reassembled, multi-segment aware) and PNG iCCP (inflated).
    // captureMakerNotes additionally walks SubIFDs (0x014A) and the IFD1 chain
    // and decodes vendor MakerNotes. DEFAULTED OFF: the folder scan runs this
    // over every file in a directory on the scan thread, and that path must
    // keep paying nothing. Only a per-image caller should turn it on.
    static CExifData Read(const uint8_t *data, size_t size, bool captureAllTags = true,
                          bool captureIcc = false, bool captureMakerNotes = false);

    // Convenience: open file, read head (metadata always lives near the
    // start), call Read(). Reads min(fileSize, 16 MB).
    static CExifData ReadFile(const std::string &path, bool captureIcc = false,
                              bool captureMakerNotes = false);

    // Read only the first maxBytes for EXIF extraction. Used by the EXIF
    // date-sorting callers; does NOT capture allTags.
    // Bounded head read; does NOT capture allTags. This is the entry point the
    // image decoders use for ICC: a full ReadFile would read up to 16 MB and
    // build tag strings it discards, on every decode, on four worker threads.
    // 128 KB is enough -- APP2 precedes SOS and iCCP precedes IDAT by spec.
    static CExifData ReadFileHeader(const std::string &path, size_t maxBytes = 131072,
                                    bool captureIcc = false,
                                    bool captureMakerNotes = false);

    // "1/250" for 0.004s, "2.0\"" for 2s
    static std::string FormatShutter(float seconds);
    // "f/2.8"
    static std::string FormatAperture(float f);
    // "35mm"
    static std::string FormatFocalLength(float mm);
    // "+0.7 EV" ("" for 0)
    static std::string FormatExposureBias(float ev);
    // "52.22970 N, 21.01223 E" (plain ASCII, 5 decimals ~= 1m precision)
    static std::string FormatGpsCoord(double lat, double lon);
    // "113 m" / "-4 m"
    static std::string FormatGpsAltitude(double meters);
};
