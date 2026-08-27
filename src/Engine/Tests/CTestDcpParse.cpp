#include "CTestDcpParse.h"
#include "CDcpProfile.h"
#include "PC_DcpFixtureWriter.h"
#include "RawTestFixtures.h"
#include "DBG_Log.h"

#include <cmath>
#include <cstring>
#include <cstdio>

#define DCP_ASSERT(cond, msg) \
	do { \
		bool dcpOk = (cond); \
		if (!dcpOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestDcpParse: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while (0)

static bool MatrixNear(const float a[3][3], const float *b9, float tol)
{
	for (int i = 0; i < 9; i++)
		if (std::fabs(a[i / 3][i % 3] - b9[i]) > tol)
			return false;
	return true;
}

// A fully-loaded dual-illuminant writer spec shared by several steps.
static SDcpWriterSpec FullSpec()
{
	SDcpWriterSpec s;
	s.hasColorMatrix2 = true;
	for (int i = 0; i < 9; i++)
		s.colorMatrix2[i] = s.colorMatrix1[i] * 1.1f;
	s.illuminant1 = 21;   // D65
	s.illuminant2 = 17;   // StdA
	s.hueDivisions = 6;
	s.satDivisions = 4;
	s.valDivisions = 1;
	s.hueSatData1.assign((size_t)6 * 4 * 1 * 3, 0.f);
	for (size_t i = 0; i < s.hueSatData1.size(); i += 3)
	{
		s.hueSatData1[i + 0] = 5.f;      // hue shift deg
		s.hueSatData1[i + 1] = 1.1f;     // sat scale
		s.hueSatData1[i + 2] = 1.0f;     // val scale
	}
	s.hueSatData2 = s.hueSatData1;
	for (size_t i = 0; i < s.hueSatData2.size(); i += 3)
		s.hueSatData2[i + 0] = -5.f;
	s.lookHueDivisions = 4;
	s.lookSatDivisions = 3;
	s.lookValDivisions = 1;
	s.lookData.assign((size_t)4 * 3 * 1 * 3, 0.f);
	for (size_t i = 0; i < s.lookData.size(); i += 3)
	{
		s.lookData[i + 1] = 1.f;
		s.lookData[i + 2] = 1.f;
	}
	s.hueSatMapEncoding = 0;
	s.toneCurve = { { 0.f, 0.f }, { 0.25f, 0.31f }, { 0.5f, 0.62f },
	                { 0.75f, 0.86f }, { 1.f, 1.f } };
	s.calibrationSignature = "com.photocruise.test";
	s.copyright = "PhotoCruise test fixture";
	s.embedPolicy = 3;
	s.hasBaselineExposureOffset = true;
	s.baselineExposureOffset = 0.35f;
	s.defaultBlackRender = 1;
	return s;
}

void CTestDcpParse::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// ---- full round trip ---------------------------------------------------
	SDcpProfile full;
	{
		SDcpWriterSpec s = FullSpec();
		std::vector<unsigned char> bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!bytes.empty(), "writer builds a dual-illuminant .dcp");

		std::string err;
		bool ok = CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &full, &err);
		DCP_ASSERT(ok, "full profile parses");
		DCP_ASSERT(full.hasColorMatrix1 && full.hasColorMatrix2
		           && MatrixNear(full.colorMatrix1, s.colorMatrix1, 2e-4f),
		           "colour matrices round-trip (1/10000 rational precision)");
		DCP_ASSERT(full.calibrationIlluminant1 == 21
		           && full.calibrationIlluminant2 == 17,
		           "illuminants round-trip");
		DCP_ASSERT(full.hueSatMap1.IsValid() && full.hueSatMap2.IsValid()
		           && full.hueSatMap1.hueDivisions == 6
		           && full.hueSatMap1.satDivisions == 4
		           && full.hueSatMap1.valDivisions == 1
		           && full.hueSatMap1.deltas.size() == (size_t)6 * 4 * 3
		           && full.hueSatMap1.deltas[0] == 5.f
		           && full.hueSatMap2.deltas[0] == -5.f,
		           "HueSatMap dims + deltas bit-exact (FLOAT storage)");
		DCP_ASSERT(full.lookTable.IsValid()
		           && full.lookTable.hueDivisions == 4,
		           "LookTable round-trips");
		DCP_ASSERT(full.toneCurve.size() == 5
		           && full.toneCurve[2].second == 0.62f,
		           "tone curve round-trips");
		DCP_ASSERT(full.profileName == "PC Synthetic Profile"
		           && full.uniqueCameraModel == "PhotoCruise Synthetic"
		           && full.calibrationSignature == "com.photocruise.test"
		           && full.copyright == "PhotoCruise test fixture",
		           "strings round-trip");
		DCP_ASSERT(full.embedPolicy == 3
		           && full.hasBaselineExposureOffset
		           && std::fabs(full.baselineExposureOffset - 0.35f) < 1e-4f
		           && full.defaultBlackRender == 1,
		           "policy scalars round-trip");
	}

	// ---- load-time normalisations (F1/F2) ----------------------------------
	{
		// FM = 2x an exactly-normalised matrix must come back normalised:
		// diag(PCS) itself maps (1,1,1) to the PCS white.
		SDcpWriterSpec s;
		s.hasForwardMatrix1 = true;
		const float pcs[3] = { 0.9642f, 1.0f, 0.8249f };
		for (int i = 0; i < 9; i++)
			s.forwardMatrix1[i] = 0.f;
		for (int r = 0; r < 3; r++)
			s.forwardMatrix1[r * 3 + r] = pcs[r] * 2.f;
		std::vector<unsigned char> bytes = PC_BuildDcpBytes(s);
		SDcpProfile p;
		std::string err;
		bool ok = CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err);
		DCP_ASSERT(ok && p.hasForwardMatrix1, "FM-carrying profile parses");
		bool normalised = true;
		for (int r = 0; r < 3; r++)
			if (std::fabs(p.forwardMatrix1[r][r] - pcs[r]) > 2e-3f)
				normalised = false;
		DCP_ASSERT(normalised,
		           "F1: NormalizeForwardMatrix rescaled FM*(1,1,1) to the PCS white");

		// CM scaled 3x must be normalised back (F2).
		SDcpWriterSpec s2;
		for (int i = 0; i < 9; i++)
			s2.colorMatrix1[i] *= 3.f;
		bytes = PC_BuildDcpBytes(s2);
		SDcpProfile p2;
		ok = CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p2, &err);
		SDcpWriterSpec ref;   // the unscaled matrix
		DCP_ASSERT(ok && MatrixNear(p2.colorMatrix1, ref.colorMatrix1, 5e-3f),
		           "F2: NormalizeColorMatrix undid the 3x scale");
	}

	// ---- refusals (#3.3 + #3.2) --------------------------------------------
	{
		std::string err;
		SDcpProfile p;

		SDcpWriterSpec s = FullSpec();
		s.magic = 42;
		std::vector<unsigned char> bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "TIFF magic 42 on the .dcp path refuses (naive-writer case)");

		s = FullSpec();
		s.magic = 0x1234;
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "unknown magic refuses");

		// Data2 without Data1.
		s = FullSpec();
		s.hueSatData1.clear();
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "HueSatMapData2 without Data1 refuses");

		// satDivisions below the floor.
		s = FullSpec();
		s.satDivisions = 1;
		s.hueSatData1.assign((size_t)6 * 1 * 1 * 3, 0.f);
		s.hueSatData2 = s.hueSatData1;
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "satDivisions < 2 refuses");

		// Count mismatch.
		s = FullSpec();
		s.hueSatData1.pop_back();
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "data count != hue*sat*val*3 refuses");

		// A DNG 1.6 tag.
		s = FullSpec();
		s.extraRefusalTag = 0xCD33;   // ColorMatrix3
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "a DNG 1.6+ tag refuses, never half-applies");

		// Unknown illuminant on a dual profile.
		s = FullSpec();
		s.illuminant2 = 255;   // "Other"
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "unknown CalibrationIlluminant refuses");

		// Tone curve with non-increasing x.
		s = FullSpec();
		s.toneCurve = { { 0.f, 0.f }, { 0.6f, 0.5f }, { 0.4f, 0.7f }, { 1.f, 1.f } };
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "non-increasing tone-curve x refuses");

		// No ColorMatrix1 at all.
		s = SDcpWriterSpec();
		s.hasColorMatrix1 = false;
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "no ColorMatrix1 refuses (#4.1: CM is always required)");

		// ReductionMatrix (>3 planes) -- the extra-tag knob plants it.
		s = FullSpec();
		s.extraRefusalTag = 0xC625;
		bytes = PC_BuildDcpBytes(s);
		DCP_ASSERT(!CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err),
		           "ReductionMatrix presence refuses (>3-plane camera)");
	}

	// ---- malformed sweep: truncation at every 8-byte boundary --------------
	{
		SDcpWriterSpec s = FullSpec();
		std::vector<unsigned char> bytes = PC_BuildDcpBytes(s);
		SDcpProfile p;
		std::string err;
		int refused = 0, parsed = 0;
		for (size_t cut = 0; cut < bytes.size(); cut += 8)
		{
			if (CDcpProfile::ParseDcpBytes(bytes.data(), cut, &p, &err))
				parsed++;
			else
				refused++;
		}
		// Nothing crashed; and no truncation that removes table bytes may
		// parse as the full profile. (Very long prefixes can legitimately
		// parse once every referenced value is inside the prefix.)
		DCP_ASSERT(refused > parsed,
		           "truncation sweep: overwhelmingly refuses, never crashes");
	}

	// ---- DNG-embedded path + file tags (F12) -------------------------------
	{
		SSyntheticDngSpec dng;
		SDcpWriterSpec s = FullSpec();
		PC_AppendDcpProfileTags(s, &dng.extraIfd0Tags);
		dng.extraIfd0Tags.push_back(
			PC_DngAsciiTag(0xC6F3, "com.photocruise.test"));    // file 0xC6F3
		dng.extraIfd0Tags.push_back(
			PC_DngAsciiTag(0xC6F6, "PC Synthetic Profile"));    // AsShotProfileName
		std::vector<unsigned char> bytes = PC_BuildSyntheticDng(dng);
		DCP_ASSERT(!bytes.empty(), "profile-injected DNG builds");

		CDcpProfile::SFileTags ft;
		bool ok = CDcpProfile::ReadFileTagsFromBytes(bytes.data(), bytes.size(), &ft);
		DCP_ASSERT(ok && ft.cameraCalibrationSignature == "com.photocruise.test"
		           && ft.asShotProfileName == "PC Synthetic Profile"
		           && ft.profileName == "PC Synthetic Profile",
		           "file tags read off the DNG (0xC6F3 route)");

		SDcpProfile p;
		std::string err;
		ok = CDcpProfile::ParseDngEmbedded(bytes.data(), bytes.size(), 0, &p, &err);
		DCP_ASSERT(ok && p.hasColorMatrix2 && p.hueSatMap1.IsValid()
		           && p.toneCurve.size() == 5,
		           "embedded profile parses out of the DNG's IFD0");
	}

	// ---- fingerprint pins (F10) --------------------------------------------
	{
		u8 d1[16], d2[16], d3[16];
		CDcpProfile::ComputeFingerprint(full, d1);
		CDcpProfile::ComputeFingerprint(full, d2);
		DCP_ASSERT(std::memcmp(d1, d2, 16) == 0, "fingerprint is deterministic");

		SDcpProfile other = full;
		other.toneCurve[2].second += 0.01f;
		CDcpProfile::ComputeFingerprint(other, d3);
		DCP_ASSERT(std::memcmp(d1, d3, 16) != 0,
		           "fingerprint changes when a field changes");
	}

	// ---- the illuminant table (F5) -----------------------------------------
	{
		DCP_ASSERT(DCP_IlluminantToTemperature(21) == 6500.0
		           && DCP_IlluminantToTemperature(17) == 2850.0
		           && DCP_IlluminantToTemperature(23) == 5000.0
		           && DCP_IlluminantToTemperature(255) == 0.0
		           && DCP_IlluminantToTemperature(0) == 0.0,
		           "F5: fixed illuminant->temperature table (unknown -> 0)");
	}

	TestCompleted(true, "DCP parser round-trips, refuses and survives abuse");
}
