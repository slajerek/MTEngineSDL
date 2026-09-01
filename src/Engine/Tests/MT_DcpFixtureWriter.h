#pragma once

// RD-D #8.1: the profile-tag writer -- synthetic `.dcp` files AND
// DNG-IFD0 injection (via SSyntheticDngSpec::extraIfd0Tags), because #7.2
// forbids committing any real Adobe profile. Absorbs the "DcpTagPatcher"
// duty RD-C's plan named but its tests never needed (recorded in the
// tracker). Deterministic bytes, little-endian, magic configurable so the
// wrong-magic refusal case (#8.1 -- including TIFF's 42, what a naive
// writer would emit) is one field away.

#include "RawTestFixtures.h"

#include <string>
#include <vector>
#include <utility>

struct SDcpWriterSpec
{
	// XYZ -> camera. Defaults: the same plausible daylight matrix the
	// synthetic DNG uses.
	bool hasColorMatrix1 = true;
	float colorMatrix1[9] = { 0.6446f, -0.0366f, -0.0468f,
	                          -0.4358f, 1.2071f, 0.2564f,
	                          -0.0587f, 0.1316f, 0.5452f };
	bool hasColorMatrix2 = false;
	float colorMatrix2[9] = {};
	int illuminant1 = 21;              // D65
	int illuminant2 = 17;              // StdA
	bool hasForwardMatrix1 = false;
	float forwardMatrix1[9] = {};
	bool hasForwardMatrix2 = false;
	float forwardMatrix2[9] = {};

	// HueSatMap: dims emitted when hueDivisions > 0; data1/data2 emitted
	// when non-empty (tests craft the invalid combinations directly).
	unsigned hueDivisions = 0, satDivisions = 0, valDivisions = 0;
	std::vector<float> hueSatData1;
	std::vector<float> hueSatData2;
	unsigned lookHueDivisions = 0, lookSatDivisions = 0, lookValDivisions = 0;
	std::vector<float> lookData;
	int hueSatMapEncoding = -1;        // -1 = omit tag
	int lookTableEncoding = -1;

	std::vector<std::pair<float, float>> toneCurve;   // empty = omit

	std::string profileName = "PC Synthetic Profile";
	std::string uniqueCameraModel = "MTEngine Synthetic";
	std::string calibrationSignature;  // empty = omit
	std::string copyright;             // empty = omit
	int embedPolicy = -1;              // -1 = omit
	bool hasBaselineExposureOffset = false;
	float baselineExposureOffset = 0.f;
	int defaultBlackRender = -1;       // -1 = omit

	// Malformed-input knobs (#8.1).
	unsigned short magic = 0x4352;     // set 42 for the naive-writer case
	unsigned short extraRefusalTag = 0;   // emit this tag (LONG 0) if != 0
};

// A standalone .dcp byte stream.
std::vector<unsigned char> PC_BuildDcpBytes(const SDcpWriterSpec &spec);

// The same profile as packed IFD0 tags for SSyntheticDngSpec::extraIfd0Tags
// (the DNG-embedded path, #5.1).
void PC_AppendDcpProfileTags(const SDcpWriterSpec &spec,
                             std::vector<SDngRawTag> *out);

// Generic packed-tag makers for FILE-side tags tests need
// (CameraCalibrationSignature 0xC6F3, AsShotProfileName 0xC6F6, ...).
SDngRawTag PC_DngAsciiTag(unsigned short tag, const std::string &value);
SDngRawTag PC_DngLongTag(unsigned short tag, unsigned value);
