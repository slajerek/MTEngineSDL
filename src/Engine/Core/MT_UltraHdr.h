#pragma once

// libultrahdr (Google; Apache-2.0 AND MIT) behind the engine's own types: the
// ISO 21496-1 gain-map JPEG ("Ultra HDR"). Built into the image-codec bundle
// by platform/*/build-image_codecs.* when MT_ENABLE_LIBULTRAHDR is on -- which
// follows MT_CAP_PHOTO_CODECS. With it off every call below fails and
// MT_UltraHdrIsAvailable() says false; the functions still EXIST, so a caller
// asks at RUNTIME and needs no preprocessor flag of its own.
//
// ultrahdr_api.h is included by MT_UltraHdr.cpp only: the codec's types never
// reach a consumer, and a consumer never needs the image-codec include path.

#include "SYS_Defs.h"

#include <cstddef>
#include <string>
#include <vector>

class CImageData;

bool MT_UltraHdrIsAvailable();

// What a base image's pixels are in. Two values because those are the two a
// photo app writes; the codec also knows BT.2100, which the decode handles.
enum class EMTUltraHdrGamut : int { Bt709 = 0, DisplayP3 = 1 };

struct SMTUltraHdrEncodeRequest
{
	// The SDR rendition as a COMPLETE JPEG. It becomes the file's primary image:
	// its DQT/SOF/DHT and SOS..EOI bytes are kept as they are (libultrahdr's
	// "API-3"), so a reader that ignores the gain map sees exactly these pixels.
	// Its EXIF (APP1) and ICC (APP2) are carried; every other APPn segment (XMP,
	// IPTC) is dropped by libultrahdr's segment reorder. Its transfer function
	// MUST be the sRGB curve -- libultrahdr linearises it with that curve on both
	// sides of the map.
	const u8 *baseJpeg = nullptr;
	size_t    baseJpegSize = 0;
	// What the base's pixels are in. With an embedded ICC the codec reads the
	// gamut from it and REFUSES a mismatch; with none, it writes its own profile
	// for this gamut.
	EMTUltraHdrGamut baseGamut = EMTUltraHdrGamut::DisplayP3;
	// The HDR intent: RGBA half floats, tightly packed (stride = width), LINEAR,
	// the primaries named by hdrGamut (DisplayP3 by default), 1.0 = SDR reference white. The codec COPIES it at
	// the call, so the caller's buffer and its copy coexist for the encode.
	const u16 *hdrRgbaHalf = nullptr;
	int width = 0, height = 0;           // must equal the base's
	// The HDR intent's largest linear component. Sets the metadata's HDR
	// capacity (203 * clamp(peak, 2, 10000/203) nits), so that a reader applies
	// the WHOLE map on a display with at least this headroom. The codec's own
	// default for linear input is 49x, which a 4x display would weight to ~36 %.
	float hdrPeak = 2.f;
	int   gainMapScale   = 2;            // the map is (w/scale) x (h/scale)
	int   gainMapQuality = 95;
	// The measured gain range is clamped to these (linear). Unclamped, near-black
	// pixels set BOTH ends: an 8-bit base code of 1 over an HDR value of ~0 is a
	// gain of ~-8 stops, and libultrahdr caps a dark pixel's POSITIVE gain at
	// +2.3 stops but keeps it (gainmapmath.cpp:773-782) -- a ~4.9x maximum
	// whatever the highlights are. Either widens the map's range and coarsens
	// its highlight steps.
	//   minContentBoost: 2^-2 admits every legitimate negative gain the photo
	//     app's SDR pass produces (Stage 16 pulls weak channels of a saturated
	//     highlight toward the mean by up to 10 %: -0.66 stop for a channel at
	//     5 % of the maximum, -1.6 at 5 % of the mean).
	//   maxContentBoost: 0 = clamp(1.15 * hdrPeak, 2, 128) -- the brightest
	//     channel's legitimate gain is peak over its SDR value, at most ~1.075 x
	//     peak after Stage 16's desaturation, plus margin.
	float minContentBoost = 0.25f;       // 2^-2
	float maxContentBoost = 0.0f;        // 0 = from hdrPeak
	// true = today's renders (three-channel maps); false = a single-channel map
	// (fixtures, Review Focus 3).
	bool  multiChannelGainMap = true;
	// 1 = today's renders; other values for fixtures only.
	float gainMapGamma = 1.0f;
	// The HDR intent's primaries. DisplayP3 = today's renders; Bt709 for fixtures
	// only -- over a P3 base libultrahdr keeps use_base_cg and writes a BT.709 ICC
	// into the map (jpegr.cpp:608-611), the case option C declines (R-2).
	EMTUltraHdrGamut hdrGamut = EMTUltraHdrGamut::DisplayP3;
};

// Returns false with a reason in `err` (libultrahdr's own detail when it has one).
bool MT_UltraHdrEncodeJpeg(const SMTUltraHdrEncodeRequest &req, std::vector<u8> *out,
                           std::string *err);

struct SMTUltraHdrInfo
{
	int   width = 0, height = 0, gainMapWidth = 0, gainMapHeight = 0;
	float maxContentBoost[3] = { 0.f, 0.f, 0.f }, minContentBoost[3] = { 0.f, 0.f, 0.f };
	float gamma[3] = { 0.f, 0.f, 0.f };
	float offsetSdr[3] = { 0.f, 0.f, 0.f }, offsetHdr[3] = { 0.f, 0.f, 0.f };
	float hdrCapacityMin = 0.f, hdrCapacityMax = 0.f;
	bool  useBaseColorSpace = true;
};

// True for a gain-map JPEG this build can read. Parses; never decodes pixels.
bool MT_UltraHdrProbe(const u8 *data, size_t size, SMTUltraHdrInfo *out);

// An OUTPUT TRANSFORM for CImageData::LoadUltraHdrLinear: the host's surface
// encoding, applied in the decode's one copy pass instead of a second pass over
// the whole image afterwards (measured: that second pass cost as much as
// the decode). Every member is optional; the steps run in this order:
//   1. matrixFromSrgb -- LINEAR sRGB -> the output's linear primaries (row-
//      major). Composed with the decode's own base-gamut -> sRGB matrix, so a
//      Display P3 base on a Display P3 output is no matrix at all: the gamut
//      round trip collapses (a composed matrix within 1e-5 of identity is
//      treated as identity -- closer to the file than the round trip it skips).
//   2. a TOE: when a matrix ran, a value with |v| <= toeKnee is written as
//      FloatToHalf(v * toeSlope) instead of through the LUT -- the exact linear
//      segment of a piecewise curve (sRGB: 0.0031308, 12.92), where rounding
//      the input to half before the LUT would cost up to several half ULPs.
//      toeKnee = 0 turns it off.
//   3. lut -- 65,536 entries, half bits -> half bits, per channel (RGB; alpha
//      stays 1.0). nullptr = none. The caller keeps it alive for the call.
// With no matrix and no LUT the pass is a row memcpy. The pixels do not depend
// on copyThreads.
struct SMTUltraHdrOutputTransform
{
	bool        hasMatrix = false;
	float       matrixFromSrgb[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };
	float       toeKnee  = 0.f;
	float       toeSlope = 0.f;
	const u16  *lut      = nullptr;
	// The copy pass in this many row bands at once (capped by the hardware
	// concurrency and the height). For the host's ON-SCREEN decode only: its
	// background decodes already keep the cores busy, and libultrahdr's own
	// threads have finished before the copy starts. 1 = on the calling thread.
	int         copyThreads = 1;
};

// ---------------------------------------------------------------------------
// Option C (2026-10-07, gain-map-resident HDR): a gain-map JPEG kept
// RESIDENT as its 8-bit SDR base plus its 8-bit map, the map applied by a shader
// at draw time instead of by libultrahdr at decode time. Decoded by
// CImageData::LoadUltraHdrBaseAndMap; the members below are what that shader
// needs. Per channel c the shader computes (Design steps 1-6):
//   1. g      = bilinear map sample at the position rule below (clamp-to-edge)
//   2. g      = pow(g, invGamma[c])                                  (when != 1)
//   3. boost  = exp2(mix(logMinBoost[c], logMaxBoost[c], g))
//   4. hdr[c] = clamp((sRGB-decode(base[c]) + offsetSdr[c]) * boost
//               - offsetHdr[c], 0, 10000/203)
//   5. out    = matrix * hdr                                      (when hasMatrix)
//   6. out    = sRGB-encode(out)                                  (when encodeSrgb)
// ---------------------------------------------------------------------------
struct SMTGainMapApply
{
	float logMinBoost[3] = { 0.f, 0.f, 0.f };   // Design step 3: log2(min_content_boost); single-channel map: all = channel 0
	float logMaxBoost[3] = { 0.f, 0.f, 0.f };   // Design step 3: log2(max_content_boost)
	float invGamma[3]    = { 1.f, 1.f, 1.f };   // Design step 2: 1 / the map's gamma
	float offsetSdr[3]   = { 0.f, 0.f, 0.f };   // Design step 4
	float offsetHdr[3]   = { 0.f, 0.f, 0.f };   // Design step 4
	bool  hasMatrix = false;                     // Design step 5: base gamut -> sRGB, composed with the output's matrix
	float matrix[3][3] = { { 1.f, 0.f, 0.f }, { 0.f, 1.f, 0.f }, { 0.f, 0.f, 1.f } };   // Design step 5, row-major
	bool  encodeSrgb = false;                    // Design step 6: the output had a LUT (the sRGB-extended encode)
	int   mapChannels = 1;                       // Design step 1: 1 or 3 (a 1-channel map is stored replicated)
	// The map's POSITION (Design, "Map reconstruction" / "The map's position"): map
	// coordinate = (pixel - mapOffset) / mapScale on each axis, texel centres at integers.
	float mapScale = 1.f;                        // libultrahdr's ONE scale, baseW / mapW of the SOURCE frame
	float mapOffset[2] = { 0.f, 0.f };           // 0 in the source frame; set by MT_GainMapOrient only
};

// Exact twin (oracle): the shader's maths on the CPU, one output pixel per
// step x step block (at the block's top-left pixel). Writes surface-encoded
// half RGBA (alpha 1.0); returns the LINEAR peak (after step 5, before step 6).
float MT_GainMapEvaluate(const CImageData &base, const CImageData &map, const SMTGainMapApply &a,
                         int step, std::vector<u16> *outRgbaHalf, int *outW, int *outH);

// Fast measure: one histogram sample per MAP texel, table-driven; writes
// surface-encoded half RGBA (map-sized) for the histogram; returns an UPPER
// BOUND on the LINEAR peak (never below MT_GainMapEvaluate's at step 1).
// threads > 1 runs the texel rows in bands on that many threads (clamped to the
// hardware and the row count, as LoadUltraHdrLinear's copyThreads); the result is
// bit-identical for every thread count.
float MT_GainMapMeasure(const CImageData &base, const CImageData &map, const SMTGainMapApply &a,
                        std::vector<u16> *outRgbaHalf, int *outW, int *outH, int threads = 1);

// After PC_ApplyExifOrientation turned base and map: carry the position rule
// into the turned frame. Called once per decode, on an apply whose offsets are
// still 0. Orientation 1 (or out of range) leaves it untouched.
void MT_GainMapOrient(SMTGainMapApply *a, int orientation, int turnedBaseW, int turnedBaseH,
                      int turnedMapW, int turnedMapH);

// Decision R-2's STRICT byte form (the decode uses the gamut form below,
// MT_GainMapIccGamutMatchesBase): true when the map JPEG carries no ICC profile, or one
// byte-identical to the base's (both as RAW profile bytes, APP2 "ICC_PROFILE"
// chunks reassembled). baseIcc NULL / size 0 = the base has none. False for a
// malformed chunk set in the map (bad length, duplicate or missing sequence).
bool MT_GainMapIccMatchesBase(const u8 *baseIcc, size_t baseIccSize,
                              const u8 *mapJpeg, size_t mapJpegSize);

// An ICC profile's gamut exactly as libultrahdr classifies it
// (IccHelper::readIccColorGamut, icc.cpp): a 'cicp' tag naming sRGB / P3 /
// BT.2020 primaries wins; otherwise the rXYZ/gXYZ/bXYZ colorants within 0.001
// of libultrahdr's own tables; anything else is Unspecified. `profile` is the
// RAW profile (no "ICC_PROFILE" chunk header). Pure byte parsing, every build.
enum class EMTIccGamut : int { Unspecified = -1, Bt709 = 0, DisplayP3 = 1, Bt2100 = 2 };
EMTIccGamut MT_IccProfileGamut(const u8 *profile, size_t size);

// Decision R-2 AS LIBULTRAHDR APPLIES IT -- the predicate LoadUltraHdrBaseAndMap
// uses. With use_base_cg the codec converts the applied result from the base's
// gamut to the MAP's gamut (jpegr.cpp:1616-1623): hdr_cg = the map ICC's gamut,
// or the base's when the map's is Unspecified; the base's is BT.709 when
// Unspecified. Not reproduced, so true only when that conversion is the
// identity: the map has no ICC, or its gamut is Unspecified or equal to
// `baseGamut` (the decoded base's, libultrahdr's own reading). The map's gamut
// is classified on exactly the block the codec classifies: the FIRST APP2 ICC
// marker in stream order, after its 14-byte chunk header. False for a malformed
// chunk set in the map (stricter than the codec: a decline, never a guess).
// MT_GainMapIccMatchesBase's byte identity is the stricter form: every map
// libultrahdr 2.0.2 writes with UHDR_WRITE_XMP off (our build) carries an ICC of
// its own (compressGainMap, jpegr.cpp:520-528) that differs in BYTES from any
// base profile it did not write itself, so byte identity declines them all.
bool MT_GainMapIccGamutMatchesBase(EMTIccGamut baseGamut, const u8 *mapJpeg, size_t mapJpegSize);
