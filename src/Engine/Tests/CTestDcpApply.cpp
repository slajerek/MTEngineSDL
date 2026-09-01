#include "CTestDcpApply.h"
#include "CDcpApply.h"
#include "CDcpProfile.h"
#include "CDcpTemperature.h"
#include "MT_DcpFixtureWriter.h"
#include "DevelopMath.h"
#include "DevelopSeams.h"
#include "DBG_Log.h"

#include <cmath>
#include <cstdio>

#define DCA_ASSERT(cond, msg) \
	do { \
		bool dcaOk = (cond); \
		if (!dcaOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestDcpApply: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while (0)

static SDcpProfile ParseSpec(const SDcpWriterSpec &s)
{
	SDcpProfile p;
	std::string err;
	std::vector<unsigned char> bytes = PC_BuildDcpBytes(s);
	CDcpProfile::ParseDcpBytes(bytes.data(), bytes.size(), &p, &err);
	return p;
}

static SRawDngTags PlainFile()
{
	SRawDngTags f;
	f.hasAsShotNeutral = true;
	f.dngAsShotNeutral[0] = 0.6f;
	f.dngAsShotNeutral[1] = 1.0f;
	f.dngAsShotNeutral[2] = 0.7f;
	return f;
}

// A normalised forward matrix: rows scaled so FM * (1,1,1) = PCS white --
// what NormalizeForwardMatrix leaves untouched, so writer round trips are
// exact up to the 1/10000 rational quantisation.
static void MakeNormalisedFm(float base[9], float mix)
{
	// diag(PCS) blended toward a mild cross-coupling, then row-normalised.
	const float pcs[3] = { 0.9642f, 1.0f, 0.8249f };
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			base[r * 3 + c] = (r == c) ? 1.f - 2.f * mix : mix;
	for (int r = 0; r < 3; r++)
	{
		const float sum = base[r * 3] + base[r * 3 + 1] + base[r * 3 + 2];
		for (int c = 0; c < 3; c++)
			base[r * 3 + c] *= pcs[r] / sum;
	}
}

void CTestDcpApply::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// ---- mired interpolation over the FM path (#8.2 row 1) ----------------
	{
		SDcpWriterSpec s;
		s.hasColorMatrix2 = true;
		for (int i = 0; i < 9; i++)
			s.colorMatrix2[i] = s.colorMatrix1[i] * 1.15f;
		s.illuminant1 = 21;   // D65 (6500) -- deliberately OUT OF ORDER
		s.illuminant2 = 17;   // StdA (2850)
		s.hasForwardMatrix1 = true;
		s.hasForwardMatrix2 = true;
		MakeNormalisedFm(s.forwardMatrix1, 0.05f);   // the D65 set
		MakeNormalisedFm(s.forwardMatrix2, 0.15f);   // the StdA set

		SDcpProfile p = ParseSpec(s);
		CDcpSeamSet seams(p, "");
		SRawDngTags file = PlainFile();

		auto matrixAtKelvin = [&](float kelvin, float outM[3][3]) -> float
		{
			SWhiteBalanceRequest req;
			req.mode = SWhiteBalanceRequest::TempTint;
			req.temperature = kelvin;
			req.tint = 0.f;
			req.temperatureIsKelvin = true;
			SCameraResolution res;
			bool ok = seams.Resolve(file, req, &res);
			if (!ok)
				return -1.f;
			seams.GetMatrix(outM);
			return seams.ResolvedWeight();
		};

		float mLow[3][3], mHigh[3][3], mMidA[3][3], mMidB[3][3];
		const float gLow = matrixAtKelvin(2850.f, mLow);
		const float gHigh = matrixAtKelvin(6500.f, mHigh);
		DCA_ASSERT(gLow >= 0.99f && gHigh <= 0.01f,
		           "weight 1 at the LOWER temperature, 0 at the higher "
		           "(out-of-order illuminants sorted)");
		// At the endpoints the matrix is that set's FM (normalised).
		DCA_ASSERT(std::fabs(mLow[0][0] - p.forwardMatrix2[0][0]) < 5e-3f
		           && std::fabs(mHigh[0][0] - p.forwardMatrix1[0][0]) < 5e-3f,
		           "endpoint matrices reproduce each illuminant's FM exactly");

		// Two intermediates: mired-linear, NOT kelvin-linear.
		const float gA = matrixAtKelvin(3500.f, mMidA);
		const float gB = matrixAtKelvin(5000.f, mMidB);
		auto expectG = [](double T) {
			return (1.0 / T - 1.0 / 6500.0) / (1.0 / 2850.0 - 1.0 / 6500.0);
		};
		DCA_ASSERT(std::fabs(gA - (float)expectG(3500)) < 5e-3f
		           && std::fabs(gB - (float)expectG(5000)) < 5e-3f,
		           "interpolation weight is linear in MIREDS at two intermediates");
		bool blendOk = true;
		for (int r = 0; r < 3 && blendOk; r++)
			for (int c = 0; c < 3 && blendOk; c++)
			{
				const float want = gA * mLow[r][c] + (1.f - gA) * mHigh[r][c];
				if (std::fabs(mMidA[r][c] - want) > 5e-3f)
					blendOk = false;
			}
		DCA_ASSERT(blendOk, "intermediate matrix is the mired blend of the endpoint FMs");
	}

	// ---- signature gate, both ways (#2.1) ----------------------------------
	{
		SDcpWriterSpec s;
		s.calibrationSignature = "com.pc.sig";
		SDcpProfile p = ParseSpec(s);

		SRawDngTags file = PlainFile();
		file.dngCalibration[0][0][0] = 1.2f;
		file.dngCalibration[0][1][1] = 1.0f;
		file.dngCalibration[0][2][2] = 0.9f;
		file.dngCalibration[1][0][0] = 1.2f;
		file.dngCalibration[1][1][1] = 1.0f;
		file.dngCalibration[1][2][2] = 0.9f;
		// dngFields is AUTHORITATIVE for presence (the tracker's standing
		// RD-D rule) -- a matrix without its mask bit must be IGNORED.
		file.dngFields[0] = 1u << 3;   // LIBRAW_DNGFM_CALIBRATION
		file.dngFields[1] = 1u << 3;

		SWhiteBalanceRequest req;   // As Shot
		SCameraResolution res;

		CDcpSeamSet match(p, "com.pc.sig");
		DCA_ASSERT(match.Resolve(file, req, &res)
		           && res.calibrationSignatureMatches
		           && std::fabs(res.calibration[0][0] - 1.2f) < 1e-5f,
		           "matching signature: the file's CC comes through");

		CDcpSeamSet mismatch(p, "some.other.body");
		DCA_ASSERT(mismatch.Resolve(file, req, &res)
		           && !res.calibrationSignatureMatches
		           && res.calibration[0][0] == 1.f
		           && res.calibration[1][1] == 1.f,
		           "mismatched signature: calibration handed back IDENTITY, "
		           "flag is diagnostics only");

		// The mask rule itself: values present, mask bit ABSENT -> identity
		// (presence is never re-derived from a non-zero matrix).
		SRawDngTags unmasked = file;
		unmasked.dngFields[0] = 0;
		unmasked.dngFields[1] = 0;
		CDcpSeamSet masked(p, "com.pc.sig");
		DCA_ASSERT(masked.Resolve(unmasked, req, &res)
		           && res.calibration[0][0] == 1.f,
		           "dngFields is authoritative: no mask bit, no calibration");
	}

	// ---- the NeutralToXY fixed point (#4.2/F4) ------------------------------
	{
		SDcpWriterSpec s;   // single-illuminant, CM-only
		SDcpProfile p = ParseSpec(s);
		CDcpSeamSet seams(p, "");
		SRawDngTags file = PlainFile();

		SWhiteBalanceRequest req;   // As Shot
		SCameraResolution res;
		DCA_ASSERT(seams.Resolve(file, req, &res), "as-shot Resolve converges");
		DCA_ASSERT(res.correlatedColorTemp > 1500.f
		           && res.correlatedColorTemp < 25000.f,
		           "resolved CCT is a plausible temperature");
		// The fixed point's defining property: the reference neutral
		// reproduces the (max-normalised) as-shot neutral.
		DCA_ASSERT(std::fabs(res.referenceNeutral[0] - 0.6f) < 2e-3f
		           && std::fabs(res.referenceNeutral[1] - 1.0f) < 2e-3f
		           && std::fabs(res.referenceNeutral[2] - 0.7f) < 2e-3f,
		           "referenceNeutral reproduces the as-shot neutral "
		           "(the NeutralToXY fixed point closed)");
	}

	// ---- HueSatMap sampler against hand-computed triples (F8) --------------
	{
		// Uniform +30 degree hue shift, 2.5-D table.
		SDcpWriterSpec s;
		s.hueDivisions = 6;
		s.satDivisions = 2;
		s.valDivisions = 1;
		s.hueSatData1.assign((size_t)6 * 2 * 1 * 3, 0.f);
		for (size_t i = 0; i < s.hueSatData1.size(); i += 3)
		{
			s.hueSatData1[i + 0] = 30.f;
			s.hueSatData1[i + 1] = 1.f;
			s.hueSatData1[i + 2] = 1.f;
		}
		SDcpProfile p = ParseSpec(s);
		CDcpSeamSet seams(p, "");
		SRawDngTags file = PlainFile();
		SWhiteBalanceRequest req;
		SCameraResolution res;
		DCA_ASSERT(seams.Resolve(file, req, &res) && !seams.IsIdentity(),
		           "hue-shift profile resolves with a live map");

		float rgb[3] = { 1.f, 0.f, 0.f };   // pure red, h = 0
		seams.ApplyPixel(rgb);
		DCA_ASSERT(std::fabs(rgb[0] - 1.f) < 1e-4f
		           && std::fabs(rgb[1] - 0.5f) < 1e-4f
		           && std::fabs(rgb[2] - 0.f) < 1e-4f,
		           "+30deg on pure red gives exactly (1, 0.5, 0)");

		// V > 1: the documented deviation -- valScale applies UNPINNED.
		SDcpWriterSpec sv = s;
		for (size_t i = 0; i < sv.hueSatData1.size(); i += 3)
		{
			sv.hueSatData1[i + 0] = 0.f;
			sv.hueSatData1[i + 2] = 0.8f;
		}
		SDcpProfile pv = ParseSpec(sv);
		CDcpSeamSet seamsV(pv, "");
		DCA_ASSERT(seamsV.Resolve(file, req, &res), "valScale profile resolves");
		float hot[3] = { 2.f, 2.f, 2.f };
		// neutral: s = 0, hue undefined -- valScale still applies.
		seamsV.ApplyPixel(hot);
		DCA_ASSERT(std::fabs(hot[0] - 1.6f) < 1e-3f,
		           "V=2 * valScale 0.8 = 1.6 UNPINNED (the scene-referred deviation)");

		// Encoding tag: same 3-D table sampled at linear vs sRGB-encoded V
		// indices must differ (#8.2 row 4).
		SDcpWriterSpec se;
		se.hueDivisions = 6;
		se.satDivisions = 2;
		se.valDivisions = 2;
		se.hueSatData1.assign((size_t)6 * 2 * 2 * 3, 0.f);
		const size_t half = se.hueSatData1.size() / 2;   // val-slowest storage
		for (size_t i = 0; i < se.hueSatData1.size(); i += 3)
		{
			se.hueSatData1[i + 0] = (i < half) ? 0.f : 60.f;
			se.hueSatData1[i + 1] = 1.f;
			se.hueSatData1[i + 2] = 1.f;
		}
		se.hueSatMapEncoding = 0;
		SDcpProfile pe0 = ParseSpec(se);
		se.hueSatMapEncoding = 1;
		SDcpProfile pe1 = ParseSpec(se);
		CDcpSeamSet s0(pe0, ""), s1(pe1, "");
		DCA_ASSERT(s0.Resolve(file, req, &res) && s1.Resolve(file, req, &res),
		           "encoding-pair profiles resolve");
		float a[3] = { 0.25f, 0.05f, 0.05f };
		float b[3] = { 0.25f, 0.05f, 0.05f };
		s0.ApplyPixel(a);
		s1.ApplyPixel(b);
		DCA_ASSERT(std::fabs(a[1] - b[1]) > 1e-4f || std::fabs(a[2] - b[2]) > 1e-4f,
		           "Encoding 0 vs 1 produce different output (tag not ignored)");
	}

	// ---- RGBTone vs per-channel (F7, #8.2 row 6) ----------------------------
	{
		SDevLut1D lut;
		lut.Build([](float x) { return x <= 0.f ? 0.f : std::sqrt(x); });

		float rgb[3] = { 0.64f, 0.16f, 0.04f };
		PC_DevRgbTone(rgb, lut);
		// max: sqrt(0.64) = 0.8; min: sqrt(0.04) = 0.2;
		// mid: 0.2 + 0.6 * (0.16-0.04)/(0.64-0.04) = 0.32 -- NOT sqrt(0.16)=0.4.
		DCA_ASSERT(std::fabs(rgb[0] - 0.8f) < 1e-3f
		           && std::fabs(rgb[1] - 0.32f) < 1e-3f
		           && std::fabs(rgb[2] - 0.2f) < 1e-3f,
		           "RGBTone: max/min through the curve, middle by ratio (0.32 != 0.4)");

		float tie[3] = { 0.5f, 0.25f, 0.25f };
		PC_DevRgbTone(tie, lut);
		DCA_ASSERT(std::fabs(tie[0] - std::sqrt(0.5f)) < 1e-3f
		           && std::fabs(tie[1] - 0.5f) < 1e-3f
		           && tie[1] == tie[2],
		           "RGBTone tie case r >= g == b evaluates both through the table");

		float neutral[3] = { 0.3f, 0.3f, 0.3f };
		PC_DevRgbTone(neutral, lut);
		DCA_ASSERT(std::fabs(neutral[0] - neutral[1]) < 1e-6f
		           && std::fabs(neutral[1] - neutral[2]) < 1e-6f,
		           "RGBTone keeps neutrals neutral");
	}

	// ---- MapWhiteMatrix (F6) ------------------------------------------------
	{
		float m[3][3];
		DCP_MapWhiteMatrix(DCP_kD50XY, DCP_kD50XY, m);
		bool ident = true;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				if (std::fabs(m[r][c] - (r == c ? 1.f : 0.f)) > 1e-4f)
					ident = false;
		DCA_ASSERT(ident, "MapWhiteMatrix(D50 -> D50) is identity");

		const float extreme[2] = { 0.9f, 0.05f };
		DCP_MapWhiteMatrix(DCP_kD50XY, extreme, m);
		bool finite = true;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
				if (!std::isfinite(m[r][c]))
					finite = false;
		DCA_ASSERT(finite, "MapWhiteMatrix stays finite at a degenerate white (pins)");
	}

	// ---- tone-curve seam + BEO (F13/#4.5) -----------------------------------
	{
		SDcpWriterSpec s;
		s.hasBaselineExposureOffset = true;
		s.baselineExposureOffset = 0.35f;
		SDcpProfile p = ParseSpec(s);
		CDcpSeamSet seams(p, "");
		DCA_ASSERT(seams.ToneCurveSeam() == nullptr,
		           "no ProfileToneCurve -> nullptr seam (baseline approximation stays ON)");
		SRawDngTags file = PlainFile();
		SWhiteBalanceRequest req;
		SCameraResolution res;
		DCA_ASSERT(seams.Resolve(file, req, &res)
		           && std::fabs(res.baselineExposureOffset - 0.35f) < 1e-4f,
		           "BaselineExposureOffset rides the resolution (RD-C stage 1)");

		SDcpWriterSpec sc = s;
		sc.toneCurve = { { 0.f, 0.f }, { 0.5f, 0.7f }, { 1.f, 1.f } };
		SDcpProfile pc = ParseSpec(sc);
		CDcpSeamSet seamsC(pc, "");
		DCA_ASSERT(seamsC.ToneCurveSeam() != nullptr, "curve-carrying profile supplies the seam");
		auto curve = seamsC.GetLinearCurve();
		DCA_ASSERT(curve && std::fabs(curve(0.5f) - 0.7f) < 1e-4f
		           && std::fabs(curve(0.f)) < 1e-4f
		           && std::fabs(curve(1.f) - 1.f) < 1e-4f,
		           "the spline reproduces the curve's knots");
	}

	TestCompleted(true, "DCP application arithmetic follows the pinned source");
}
