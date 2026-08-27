#pragma once

// RD-D #3: the DCP container parser -- a TYPED, BINARY, bounds-checked
// TIFF-shaped IFD reader (deliberately NOT CExifReader, whose values are
// human-readable strings and whose float arrays are elided, #3.1). A `.dcp`
// carries magic 0x4352 ('CR'), NOT TIFF's 42 (dng_tag_values.h
// magicExtendedProfile, spec F11); the DNG-embedded path uses 42.
//
// DCPs are UNTRUSTED INPUT: every offset is range-checked, no allocation is
// sized from an unchecked file-supplied count, and stated maxima cap the
// tables. Parse failures REFUSE with a reason string -- refusal is never
// modal (#3.3): the caller renders baseline + badge.

#include "SYS_Defs.h"

#include <string>
#include <vector>
#include <utility>

// A HueSatMap / LookTable: deltas stored EXACTLY as the container does --
// value slowest, then hue, then saturation fastest (#3.3), triples of
// (hueShiftDeg, satScale, valScale).
struct SDcpHueSatMap
{
	u32 hueDivisions = 0;
	u32 satDivisions = 0;
	u32 valDivisions = 0;
	std::vector<float> deltas;   // hue*sat*val*3 floats

	bool IsValid() const { return !deltas.empty(); }
};

struct SDcpProfile
{
	// XYZ -> camera, row-major, NormalizeColorMatrix applied at load (F2:
	// scaled so MaxEntry(CM * PCSwhiteXYZ) == 1 when outside [0.99, 1.01]).
	float colorMatrix1[3][3] = {};  bool hasColorMatrix1 = false;
	float colorMatrix2[3][3] = {};  bool hasColorMatrix2 = false;

	// camera -> XYZ(D50), NormalizeForwardMatrix applied at load (F1: the
	// DIAGONAL white rescale diag(PCS) * diag(1/(FM*1)) * FM -- not
	// Bradford).
	float forwardMatrix1[3][3] = {};  bool hasForwardMatrix1 = false;
	float forwardMatrix2[3][3] = {};  bool hasForwardMatrix2 = false;

	// EXIF LightSource enums (#3.2) -- temperatures via
	// DCP_IlluminantToTemperature (F5's fixed table).
	int calibrationIlluminant1 = 0;
	int calibrationIlluminant2 = 0;

	SDcpHueSatMap hueSatMap1;      // ProfileHueSatMapData1
	SDcpHueSatMap hueSatMap2;      // ProfileHueSatMapData2 (never alone)
	SDcpHueSatMap lookTable;       // ProfileLookTableData (singular, #4.2)
	int hueSatMapEncoding = 0;     // 0 linear (default) / 1 sRGB
	int lookTableEncoding = 0;

	// ProfileToneCurve: validated pairs (>=2, strictly increasing x,
	// x0 == 0, xN == 1). Empty = the profile carries no curve (LEGAL --
	// #4.4's rule: the baseline approximation stays ON, seam = nullptr).
	std::vector<std::pair<float, float>> toneCurve;

	std::string profileName;
	std::string uniqueCameraModel;       // the profile's camera restriction
	std::string localizedCameraModel;    // display only
	std::string calibrationSignature;    // ProfileCalibrationSignature
	std::string copyright;
	u32 embedPolicy = 0;                 // #7.3; parsed + recorded, no
	                                     // consumer yet (we never embed DCPs)
	float baselineExposureOffset = 0.f;  // #4.5 -- sums at RD-C stage 1
	bool  hasBaselineExposureOffset = false;
	int   defaultBlackRender = 0;        // parsed + exposed; currently a
	                                     // NO-OP (plan R2: RD-C applies no
	                                     // default black to suppress)
};

class CDcpProfile
{
public:
	// Parse a standalone .dcp (magic 0x4352). False => `outError` says why;
	// the caller badges + renders baseline (#3.3).
	static bool ParseDcpFile(const char *utf8Path, SDcpProfile *out,
	                         std::string *outError);
	static bool ParseDcpBytes(const u8 *data, size_t len, SDcpProfile *out,
	                          std::string *outError);

	// Parse the profile carried in a DNG's IFD (magic 42). `ifdOffset` == 0
	// means the first IFD (IFD0's own profile tags); an ExtraCameraProfiles
	// entry supplies non-zero offsets for the additional profiles (#5.1).
	static bool ParseDngEmbedded(const u8 *data, size_t len, u32 ifdOffset,
	                             SDcpProfile *out, std::string *outError);
	static bool ParseDngEmbeddedFromFile(const char *utf8Path, u32 ifdOffset,
	                                     SDcpProfile *out, std::string *outError);

	// IFD0-only scan -- the cheap read the index (#9) and the file-side
	// 0xC6F3 route (F12) use. Works on .dcp and DNG/TIFF files alike.
	// Never loads a table.
	struct SFileTags
	{
		std::string uniqueCameraModel;
		std::string profileName;
		std::string cameraCalibrationSignature;   // FILE tag 0xC6F3
		std::string asShotProfileName;            // 0xC6F6
		std::vector<u32> extraProfileOffsets;     // 0xC6F5
	};
	static bool ReadFileTags(const char *utf8Path, SFileTags *out);
	static bool ReadFileTagsFromBytes(const u8 *data, size_t len,
	                                  SFileTags *out);

	// dng_sdk's CalculateFingerprint, byte-for-byte (spec F10). A
	// VERIFICATION key only (#5.3): primary matching is name + camera.
	static void ComputeFingerprint(const SDcpProfile &p, u8 outDigest[16]);
};

// F5: dng_camera_profile::IlluminantToTemperature's FIXED table. Returns
// 0.0 for unknown codes (including 255 "Other") -- the caller refuses.
double DCP_IlluminantToTemperature(int lightSourceCode);
