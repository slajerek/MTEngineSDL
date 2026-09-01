#include "CTestRawDecode.h"
#include "RawTestFixtures.h"
#include "CRawDecoder.h"
#include "DBG_Log.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

#ifdef __APPLE__
#include <mach/mach.h>
// CURRENT physical footprint (not lifetime peak): ru_maxrss is useless
// in-suite because the app's cache tests push the lifetime peak past this
// decode's whole demand long before this test runs (verified: the delta
// read 0.0 in-suite regardless of registration order). Sampled by a polling
// thread while the decode runs, this measures THIS decode's scratch.
static long long RawCurrentFootprintBytes()
{
	task_vm_info_data_t info;
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info,
				  &count) != KERN_SUCCESS)
		return -1;
	return (long long)info.phys_footprint;
}
#endif

// Lifetime peak RSS in bytes (ru_maxrss is bytes on macOS, KB on Linux;
// PeakWorkingSetSize on Windows). Good enough for a 2x-regression detector.
static long long RawPeakRssBytes()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS pmc;
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		return -1;
	return (long long)pmc.PeakWorkingSetSize;
#else
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return -1;
#ifdef __APPLE__
	return (long long)ru.ru_maxrss;
#else
	return (long long)ru.ru_maxrss * 1024;
#endif
#endif
}

#define RD_ASSERT(cond, msg) \
	do { \
		bool rdOk = (cond); \
		if (!rdOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestRawDecode: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while (0)

// Shared skip guard: false is "unavailable in this build", never an error
// (design #5.0). The skip is REPORTED so a skipped run is never mistaken for
// a passing one.
#define RD_SKIP_IF_UNAVAILABLE() \
	do { \
		if (!CRawDecoder::IsAvailable()) { \
			TestCompleted(true, "CRawDecoder unavailable in this build -- skipped"); \
			return; \
		} \
	} while (0)

// True when the struct is field-for-field equal to a default-constructed one
// (the failure-path contract: `out` zeroed on every false return).
static bool ResultIsZeroed(const SRawDecodeResult &r)
{
	if (r.linear != NULL || r.owner != NULL || r.width != 0 || r.height != 0)
		return false;
	if (r.hasMatrix || r.hasDefaultCrop || r.defaultCropApplied
		|| r.hasBaselineExposure || r.hasAsShotNeutral)
		return false;
	if (r.dngFields[0] != 0 || r.dngFields[1] != 0)
		return false;
	for (int i = 0; i < 4; i++)
		if (r.asShotWB[i] != 0.f || r.cblackPre[i] != 0)
			return false;
	// The fields a mid-decode failure would have written before RawFail's
	// reset: timings, make/model and the matrix are the witnesses that the
	// reset actually ran (review finding 6 -- msOpen is written before every
	// open-failure return).
	if (r.msOpen != 0.f || r.msUnpack != 0.f || r.msProcess != 0.f)
		return false;
	if (r.cameraMake[0] != 0 || r.cameraModel[0] != 0)
		return false;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			if (r.camToXYZ[i][j] != 0.f)
				return false;
	if (r.blackLevelPre != 0 || r.whiteLevelPre != 0)
		return false;
	return true;
}

void CTestRawDecodeSynthetic::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	SRawDecodeResult r;
	std::string err;
	CRawDecoder::SOptions opt;

	// 64x64 flat neutral field at ~1/8 range: comfortably below any clip
	// point under highlight=1's largest-multiplier normalisation (#5.4).
	std::vector<unsigned char> bayer = PC_MakeBayerRGGB(64, 64, 8000);
	bool ok = CRawDecoder::DecodeBayer(bayer.data(), (unsigned)bayer.size(),
									   64, 64, opt, &r, &err);
	RD_ASSERT(ok, "synthetic RGGB decodes");
	RD_ASSERT(r.width == 64 && r.height == 64,
			  "output dimensions match the mosaic");
	RD_ASSERT(r.colors == 3 && r.linear != NULL,
			  "3-channel interleaved buffer present");

	// Flat neutral patch stays flat and neutral over the interior (the border
	// carries demosaic edge effects by construction). "Neutral" here means
	// R==G==B within 1% of full scale after WB; "flat" means each channel's
	// min/max stay within 1% of each other.
	{
		int lo = 8, hi = 56;
		unsigned minC[3] = { 65535u, 65535u, 65535u };
		unsigned maxC[3] = { 0, 0, 0 };
		for (int y = lo; y < hi; y++)
		{
			for (int x = lo; x < hi; x++)
			{
				const u16 *px = r.linear + ((size_t)y * r.width + x) * 3;
				for (int c = 0; c < 3; c++)
				{
					if (px[c] < minC[c]) minC[c] = px[c];
					if (px[c] > maxC[c]) maxC[c] = px[c];
				}
			}
		}
		const float tol = 655.f;   // 1% of 16-bit full scale
		bool flat = true, neutral = true;
		for (int c = 0; c < 3; c++)
			if ((float)(maxC[c] - minC[c]) > tol)
				flat = false;
		for (int c = 1; c < 3; c++)
			if (fabsf((float)maxC[c] - (float)maxC[0]) > tol)
				neutral = false;
		char msg[160];
		snprintf(msg, sizeof(msg),
				 "flat neutral patch stays flat and neutral (R %u..%u G %u..%u B %u..%u)",
				 minC[0], maxC[0], minC[1], maxC[1], minC[2], maxC[2]);
		RD_ASSERT(flat && neutral, msg);
	}

	// A hard vertical edge gains no colour fringe beyond threshold: left half
	// dark, right half bright, both neutral -- any strong R/B divergence from
	// G along the edge column is demosaic fringing.
	{
		std::vector<unsigned char> edge((size_t)64 * 64 * 2);
		for (unsigned y = 0; y < 64; y++)
			for (unsigned x = 0; x < 64; x++)
			{
				unsigned short v = (x < 32) ? 4000 : 16000;
				size_t i = ((size_t)y * 64 + x) * 2;
				edge[i] = (unsigned char)(v & 0xFF);
				edge[i + 1] = (unsigned char)(v >> 8);
			}
		SRawDecodeResult re;
		bool okE = CRawDecoder::DecodeBayer(edge.data(), (unsigned)edge.size(),
											64, 64, opt, &re, &err);
		RD_ASSERT(okE, "hard-edge mosaic decodes");
		// Sample two columns straddling the edge, interior rows only.
		float worst = 0.f;
		for (int y = 8; y < 56; y++)
			for (int x = 30; x < 34; x++)
			{
				const u16 *px = re.linear + ((size_t)y * re.width + x) * 3;
				float g = (float)px[1];
				if (g < 1.f) g = 1.f;
				float dr = fabsf((float)px[0] - g) / 65535.f;
				float db = fabsf((float)px[2] - g) / 65535.f;
				if (dr > worst) worst = dr;
				if (db > worst) worst = db;
			}
		char msg[96];
		snprintf(msg, sizeof(msg),
				 "edge colour fringe bounded (worst %.3f of full scale)", worst);
		RD_ASSERT(worst < 0.10f, msg);
		CRawDecoder::FreeResult(&re);
	}

	CRawDecoder::FreeResult(&r);
	RD_ASSERT(r.linear == NULL && r.width == 0,
			  "FreeResult zeroes the struct");

	TestCompleted(true, "synthetic RAW decode tests passed");
}

void CTestRawLinearity::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// Two exposures differing by exactly 3x, both BELOW the clip point:
	// highlight=1 normalises by the LARGEST multiplier so whiteLevel does not
	// map to 65535 (#5.4) -- staying under ~1/4 range keeps every channel
	// clear of clipping under any multiplier.
	//
	// DO NOT LOOSEN THE TOLERANCE. This is the test that catches
	// adjust_maximum_thr regressing (output scale derived from the picture's
	// own content) and the auto-WB daylight fallback regressing (each
	// exposure renormalised independently) -- both of which present as
	// intermittent small ratio errors, i.e. exactly what a "loosen it"
	// fix would hide (#10.3, #5.3 trap 1).
	CRawDecoder::SOptions opt;
	std::vector<unsigned char> lo = PC_MakeBayerRGGB(64, 64, 4000);
	std::vector<unsigned char> hi = PC_MakeBayerRGGB(64, 64, 12000);

	SRawDecodeResult rLo, rHi;
	std::string err;
	bool okLo = CRawDecoder::DecodeBayer(lo.data(), (unsigned)lo.size(),
										 64, 64, opt, &rLo, &err);
	RD_ASSERT(okLo, "low exposure decodes");
	bool okHi = CRawDecoder::DecodeBayer(hi.data(), (unsigned)hi.size(),
										 64, 64, opt, &rHi, &err);
	RD_ASSERT(okHi, "high exposure (3x) decodes");
	RD_ASSERT(rLo.width == rHi.width && rLo.height == rHi.height,
			  "both exposures share dimensions");

	// Per-channel interior means.
	double sumLo[3] = {}, sumHi[3] = {};
	long n = 0;
	for (int y = 8; y < 56; y++)
	{
		for (int x = 8; x < 56; x++)
		{
			const u16 *pl = rLo.linear + ((size_t)y * rLo.width + x) * 3;
			const u16 *ph = rHi.linear + ((size_t)y * rHi.width + x) * 3;
			for (int c = 0; c < 3; c++)
			{
				sumLo[c] += pl[c];
				sumHi[c] += ph[c];
			}
			n++;
		}
	}
	bool linear = true;
	char msg[192];
	float ratios[3] = {};
	for (int c = 0; c < 3; c++)
	{
		double meanLo = sumLo[c] / n, meanHi = sumHi[c] / n;
		if (meanLo < 1.0) { linear = false; break; }
		ratios[c] = (float)(meanHi / meanLo);
		if (fabsf(ratios[c] - 3.0f) > 0.015f)
			linear = false;
	}
	snprintf(msg, sizeof(msg),
			 "3x exposure yields 3x output within 0.5%% (R %.4f G %.4f B %.4f)",
			 ratios[0], ratios[1], ratios[2]);
	CRawDecoder::FreeResult(&rLo);
	CRawDecoder::FreeResult(&rHi);
	RD_ASSERT(linear, msg);

	// Second pair HIGH in range (20000 / 60000): 60000 exceeds
	// adjust_maximum's default 0.75*65535 threshold, so if the
	// adjust_maximum_thr=0 pin ever regresses, the high exposure's output
	// scale gets derived from the picture's own content and this ratio moves
	// to ~3.28. The first pair alone cannot catch that (verified: with the
	// pin removed it still passed) -- this pair is the actual detector.
	{
		std::vector<unsigned char> lo2 = PC_MakeBayerRGGB(64, 64, 20000);
		std::vector<unsigned char> hi2 = PC_MakeBayerRGGB(64, 64, 60000);
		SRawDecodeResult a, b;
		RD_ASSERT(CRawDecoder::DecodeBayer(lo2.data(), (unsigned)lo2.size(),
										   64, 64, opt, &a, &err),
				  "high-range low exposure decodes");
		RD_ASSERT(CRawDecoder::DecodeBayer(hi2.data(), (unsigned)hi2.size(),
										   64, 64, opt, &b, &err),
				  "high-range 3x exposure decodes");
		double sLo = 0, sHi = 0;
		long m = 0;
		for (int y = 8; y < 56; y++)
			for (int x = 8; x < 56; x++)
			{
				sLo += a.linear[((size_t)y * a.width + x) * 3 + 1];
				sHi += b.linear[((size_t)y * b.width + x) * 3 + 1];
				m++;
			}
		float ratio = (float)((sHi / m) / (sLo / m));
		CRawDecoder::FreeResult(&a);
		CRawDecoder::FreeResult(&b);
		char m2[128];
		snprintf(m2, sizeof(m2),
				 "high-range 3x exposure stays 3x (got %.4f) -- adjust_maximum stayed off",
				 ratio);
		RD_ASSERT(fabsf(ratio - 3.0f) < 0.015f, m2);
	}

	// WB reproducibility: a TINTED field keeps its tint. If the
	// CAMERAWB_FALLBACK_TO_DAYLIGHT bit regresses, LibRaw's greybox auto-WB
	// scan overwrites the fixture's unity user_mul with content-derived
	// multipliers that neutralise the tint -- a flat NEUTRAL field cannot see
	// that (auto-WB of neutral is unity), which is why this case exists.
	{
		std::vector<unsigned char> tinted =
			PC_MakeBayerRGGB(64, 64, 16000, 0.5f, 1.0f, 1.0f);
		SRawDecodeResult t;
		RD_ASSERT(CRawDecoder::DecodeBayer(tinted.data(), (unsigned)tinted.size(),
										   64, 64, opt, &t, &err),
				  "tinted field decodes");
		double sr = 0, sg = 0;
		long m = 0;
		for (int y = 8; y < 56; y++)
			for (int x = 8; x < 56; x++)
			{
				const u16 *px = t.linear + ((size_t)y * t.width + x) * 3;
				sr += px[0];
				sg += px[1];
				m++;
			}
		float rg = (float)((sr / m) / (sg / m));
		CRawDecoder::FreeResult(&t);
		char m3[128];
		snprintf(m3, sizeof(m3),
				 "tinted field keeps its tint (R/G %.4f, want ~0.5) -- auto-WB did not fire",
				 rg);
		RD_ASSERT(fabsf(rg - 0.5f) < 0.02f, m3);
	}

	TestCompleted(true, "RAW linearity invariant holds");
}

void CTestRawMissingMatrix::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// open_bayer runs initdata() without identify(): rgb_cam stays ALL ZEROS
	// (identity is set inside identify(), which never runs) and raw_color
	// stays 1. So this fixture must decode fine and report hasMatrix ==
	// false with camToXYZ untouched -- the "successful decode, no matrix is
	// not an error" half of the #6.2 contract. CTestRawMatrixD65 (task 9)
	// asserts the true half on the DNG path; only the pair proves the
	// capture point works.
	CRawDecoder::SOptions opt;
	std::vector<unsigned char> bayer = PC_MakeBayerRGGB(64, 64, 8000);
	SRawDecodeResult r;
	std::string err;
	bool ok = CRawDecoder::DecodeBayer(bayer.data(), (unsigned)bayer.size(),
									   64, 64, opt, &r, &err);
	RD_ASSERT(ok, "open_bayer fixture decodes successfully");
	RD_ASSERT(r.hasMatrix == false, "hasMatrix is false for a matrix-less input");
	bool zeroMatrix = true;
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			if (r.camToXYZ[i][j] != 0.f)
				zeroMatrix = false;
	RD_ASSERT(zeroMatrix, "camToXYZ stays zeroed when hasMatrix is false");
	CRawDecoder::FreeResult(&r);

	TestCompleted(true, "missing-matrix contract holds");
}

// Maps camera neutral (1,1,1) through camToXYZ and asserts the chromaticity
// is D65 within tol. Returns via the assert macro, so callers pass stepNum.
static bool RawNeutralIsD65(const SRawDecodeResult &r, float tol,
							float *outX, float *outY)
{
	float X = r.camToXYZ[0][0] + r.camToXYZ[0][1] + r.camToXYZ[0][2];
	float Y = r.camToXYZ[1][0] + r.camToXYZ[1][1] + r.camToXYZ[1][2];
	float Z = r.camToXYZ[2][0] + r.camToXYZ[2][1] + r.camToXYZ[2][2];
	float sum = X + Y + Z;
	if (sum <= 0.f)
		return false;
	float x = X / sum, y = Y / sum;
	if (outX) *outX = x;
	if (outY) *outY = y;
	// D65 = (0.3127, 0.3290); illuminant E = (0.3333, 0.3333); distance
	// ~0.021. 0.006 rejects E with 3x margin while allowing float rounding.
	return fabsf(x - 0.3127f) < tol && fabsf(y - 0.3290f) < tol;
}

void CTestRawMatrixD65::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// ---- 4a: builder-generated synthetic DNG. No skip path.
	SSyntheticDngSpec spec;
	std::vector<unsigned char> dng = PC_BuildSyntheticDng(spec);
	RD_ASSERT(!dng.empty(), "synthetic DNG builder produced bytes");
	std::string path = PC_WriteTempFile(".dng", dng);
	RD_ASSERT(!path.empty(), "synthetic DNG written to temp");

	// Both applyCameraWB settings run: the option changes WHICH matrix
	// LibRaw installs on non-DNG files (#5.3 trap 3), and pinning it here
	// keeps that coupling exercised on the DNG path too.
	for (int wb = 0; wb <= 1; wb++)
	{
		CRawDecoder::SOptions opt;
		opt.applyCameraWB = (wb == 1);
		SRawDecodeResult r;
		std::string err;
		bool ok = CRawDecoder::Decode(path.c_str(), opt, &r, &err);
		char m0[128];
		snprintf(m0, sizeof(m0),
				 "synthetic DNG decodes via the DNG matrix path (wb=%d): %s",
				 wb, ok ? "ok" : err.c_str());
		if (!ok)
		{
			remove(path.c_str());
			RD_ASSERT(false, m0);
		}
		StepCompleted(stepNum++, true, m0);
		if (!r.hasMatrix)
		{
			CRawDecoder::FreeResult(&r);
			remove(path.c_str());
			RD_ASSERT(false, "synthetic DNG yields hasMatrix == true");
		}
		StepCompleted(stepNum++, true, "synthetic DNG yields hasMatrix == true");
		float x = 0, y = 0;
		bool d65 = RawNeutralIsD65(r, 0.006f, &x, &y);
		CRawDecoder::FreeResult(&r);
		char m1[160];
		snprintf(m1, sizeof(m1),
				 "neutral maps to D65 not E (wb=%d: x=%.4f y=%.4f; D65=0.3127,0.3290 E=0.3333,0.3333)",
				 wb, x, y);
		if (!d65)
		{
			remove(path.c_str());
			RD_ASSERT(false, m1);
		}
		StepCompleted(stepNum++, true, m1);
	}
	remove(path.c_str());

	// ---- 4b: the same assertion against a real RAW -- exercises the
	// adobe_coeff table path 4a cannot reach. Skip-reported when absent.
	std::string dir = PC_RawFixtureDir();
	std::string rawPath = dir.empty() ? "" :
		PC_FindFixture(dir, { ".cr2", ".cr3", ".nef", ".arw", ".rw2",
							  ".orf", ".raf" });
	if (rawPath.empty())
	{
		TestCompleted(true,
			"D65 invariant holds on the synthetic DNG (4b real-RAW variant skipped: no PC_RAW_FIXTURE_DIR fixture)");
		return;
	}
	for (int wb = 0; wb <= 1; wb++)
	{
		CRawDecoder::SOptions opt;
		opt.applyCameraWB = (wb == 1);
		SRawDecodeResult r;
		std::string err;
		bool ok = CRawDecoder::Decode(rawPath.c_str(), opt, &r, &err);
		RD_ASSERT(ok, "real RAW decodes for the 4b matrix check");
		RD_ASSERT(r.hasMatrix, "real RAW yields hasMatrix == true");
		float x = 0, y = 0;
		bool d65 = RawNeutralIsD65(r, 0.006f, &x, &y);
		CRawDecoder::FreeResult(&r);
		char m1[160];
		snprintf(m1, sizeof(m1),
				 "real RAW: neutral maps to D65 not E (wb=%d: x=%.4f y=%.4f)",
				 wb, x, y);
		RD_ASSERT(d65, m1);
	}

	TestCompleted(true, "D65 matrix invariant holds (synthetic DNG + real RAW)");
}

void CTestRawDefaultCrop::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	std::string err;

	// A 32x32 DNG whose DefaultCrop is strictly inside the frame (LibRaw
	// only promotes the tag when origin+size fits STRICTLY inside --
	// identify.cpp:1439-1441 -- so a full-frame crop would never arrive).
	SSyntheticDngSpec spec;
	spec.hasDefaultCrop = true;
	spec.defaultCropOrigin[0] = 4;
	spec.defaultCropOrigin[1] = 4;
	spec.defaultCropSize[0] = 24;
	spec.defaultCropSize[1] = 24;
	std::vector<unsigned char> dng = PC_BuildSyntheticDng(spec);
	RD_ASSERT(!dng.empty(), "cropped synthetic DNG built");
	std::string path = PC_WriteTempFile(".dng", dng);
	RD_ASSERT(!path.empty(), "cropped synthetic DNG written");

	// Default options: decodes at the CROPPED dimensions with provenance.
	{
		CRawDecoder::SOptions opt;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(path.c_str(), opt, &r, &err);
		RD_ASSERT(ok, "cropped DNG decodes");
		char m[160];
		snprintf(m, sizeof(m),
				 "decodes at cropped dimensions (got %dx%d, want 24x24)",
				 r.width, r.height);
		bool dims = (r.width == 24 && r.height == 24);
		bool prov = r.hasDefaultCrop && r.defaultCropApplied
					&& r.defaultCropRequested[0] == 4
					&& r.defaultCropRequested[1] == 4
					&& r.defaultCropRequested[2] == 24
					&& r.defaultCropRequested[3] == 24;
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(dims, m);
		RD_ASSERT(prov, "hasDefaultCrop + defaultCropApplied + requested rectangle recorded");
	}

	// applyDefaultCrop = false: decodes LARGER, tag presence still reported.
	{
		CRawDecoder::SOptions opt;
		opt.applyDefaultCrop = false;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(path.c_str(), opt, &r, &err);
		RD_ASSERT(ok, "cropped DNG decodes with applyDefaultCrop=false");
		bool shape = (r.width == 32 && r.height == 32)
					 && r.hasDefaultCrop && !r.defaultCropApplied;
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(shape, "opt-out decodes full-size; hasDefaultCrop stays true, applied false");
	}
	remove(path.c_str());

	// No DefaultCrop tag: unaffected.
	{
		SSyntheticDngSpec plain;
		std::vector<unsigned char> d2 = PC_BuildSyntheticDng(plain);
		std::string p2 = PC_WriteTempFile(".dng", d2);
		RD_ASSERT(!p2.empty(), "plain synthetic DNG written");
		CRawDecoder::SOptions opt;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(p2.c_str(), opt, &r, &err);
		remove(p2.c_str());
		RD_ASSERT(ok, "plain DNG decodes");
		bool shape = (r.width == 32 && r.height == 32)
					 && !r.hasDefaultCrop && !r.defaultCropApplied;
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(shape, "no tag: hasDefaultCrop false, full dimensions");
	}

	// Malformed crop (origin 28, size 24 -- runs off the frame): LibRaw's
	// promotion gate refuses it, so the decode SUCCEEDS uncropped and no
	// BAD_CROP whole-decode failure occurs.
	{
		SSyntheticDngSpec bad;
		bad.hasDefaultCrop = true;
		bad.defaultCropOrigin[0] = 28;
		bad.defaultCropOrigin[1] = 28;
		bad.defaultCropSize[0] = 24;
		bad.defaultCropSize[1] = 24;
		std::vector<unsigned char> d3 = PC_BuildSyntheticDng(bad);
		std::string p3 = PC_WriteTempFile(".dng", d3);
		RD_ASSERT(!p3.empty(), "malformed-crop DNG written");
		CRawDecoder::SOptions opt;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(p3.c_str(), opt, &r, &err);
		remove(p3.c_str());
		RD_ASSERT(ok, "malformed-crop DNG still decodes (no BAD_CROP)");
		bool shape = (r.width == 32 && r.height == 32) && !r.defaultCropApplied;
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(shape, "malformed crop refused upstream: full-size, not applied");
	}

	TestCompleted(true, "DefaultCrop contract holds");
}

// LIBRAW_DNGFM_* values mirrored locally (libraw_const.h:110-117): this
// file must compile in builds without the image-codecs bundle, so it cannot
// include libraw headers. CTestRawCalibrationProvenance failing on a LibRaw
// bump is exactly what these being wrong would look like -- recheck there
// first.
static const unsigned RD_DNGFM_FORWARDMATRIX = 1u << 0;
static const unsigned RD_DNGFM_ILLUMINANT    = 1u << 1;
static const unsigned RD_DNGFM_COLORMATRIX   = 1u << 2;
static const unsigned RD_DNGFM_CALIBRATION   = 1u << 3;

static bool RawMat3Close(const float got[3][3], const float want[9], float tol)
{
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 3; c++)
			if (fabsf(got[r][c] - want[r * 3 + c]) > tol)
				return false;
	return true;
}

void CTestRawCalibrationProvenance::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// A dual-illuminant DNG with calibration + analog balance. Values are
	// arbitrary but distinct so a copy from the wrong field cannot pass.
	SSyntheticDngSpec spec;
	spec.hasColorMatrix2 = true;
	static const float CM2[9] = { 0.7188f, -0.1641f, -0.0546f,
								  -0.5391f, 1.2891f, 0.2812f,
								  -0.1015f, 0.2422f, 0.5859f };
	memcpy(spec.colorMatrix2, CM2, sizeof(CM2));
	spec.calibrationIlluminant2 = 17;   // StdA
	spec.hasCalibration1 = true;
	static const float CC1[9] = { 1.02f, 0.f, 0.f,
								  0.f, 0.99f, 0.f,
								  0.f, 0.f, 1.01f };
	memcpy(spec.cameraCalibration1, CC1, sizeof(CC1));
	spec.hasAnalogBalance = true;
	spec.analogBalance[0] = 1.10f;
	spec.analogBalance[1] = 1.00f;
	spec.analogBalance[2] = 0.95f;

	std::vector<unsigned char> dng = PC_BuildSyntheticDng(spec);
	RD_ASSERT(!dng.empty(), "calibration DNG built");
	std::string path = PC_WriteTempFile(".dng", dng);
	RD_ASSERT(!path.empty(), "calibration DNG written");

	CRawDecoder::SOptions opt;
	SRawDecodeResult r;
	std::string err;
	bool ok = CRawDecoder::Decode(path.c_str(), opt, &r, &err);
	remove(path.c_str());
	RD_ASSERT(ok, "calibration DNG decodes");

	// SRATIONAL encoding is 1/10000 -- tolerance 2e-4 catches wrong-field
	// copies while allowing the quantisation.
	const float tol = 2e-4f;
	RD_ASSERT((r.dngFields[0] & RD_DNGFM_COLORMATRIX) != 0
			  && RawMat3Close(r.dngColorMatrix[0], spec.colorMatrix1, tol),
			  "ColorMatrix1 surfaced with its parsedfields bit");
	RD_ASSERT((r.dngFields[1] & RD_DNGFM_COLORMATRIX) != 0
			  && RawMat3Close(r.dngColorMatrix[1], CM2, tol),
			  "ColorMatrix2 surfaced with its parsedfields bit");
	RD_ASSERT((r.dngFields[0] & RD_DNGFM_CALIBRATION) != 0
			  && RawMat3Close(r.dngCalibration[0], CC1, tol),
			  "CameraCalibration1 surfaced with its parsedfields bit");
	RD_ASSERT((r.dngFields[0] & RD_DNGFM_ILLUMINANT) != 0
			  && (r.dngFields[1] & RD_DNGFM_ILLUMINANT) != 0
			  && r.dngIlluminant[0] == 21 && r.dngIlluminant[1] == 17,
			  "CalibrationIlluminant1/2 surfaced (D65, StdA)");
	RD_ASSERT((r.dngFields[0] & RD_DNGFM_FORWARDMATRIX) == 0,
			  "no ForwardMatrix bit when the tag is absent");
	RD_ASSERT(fabsf(r.dngAnalogBalance[0] - 1.10f) < tol
			  && fabsf(r.dngAnalogBalance[1] - 1.00f) < tol
			  && fabsf(r.dngAnalogBalance[2] - 0.95f) < tol,
			  "AnalogBalance surfaced");
	RD_ASSERT(r.hasAsShotNeutral
			  && fabsf(r.dngAsShotNeutral[0] - 0.6f) < tol
			  && fabsf(r.dngAsShotNeutral[1] - 1.0f) < tol
			  && fabsf(r.dngAsShotNeutral[2] - 0.7f) < tol,
			  "AsShotNeutral surfaced (NOT cam_mul)");
	RD_ASSERT(strcmp(r.uniqueCameraModel, "MTEngine Synthetic") == 0,
			  "UniqueCameraModel surfaced -- the DCP match key");
	CRawDecoder::FreeResult(&r);

	// Non-DNG input: the whole block zeroed, dngFields == 0, analog balance
	// at its {1,1,1} initialiser -- RD-D relies on exactly this.
	{
		std::vector<unsigned char> bayer = PC_MakeBayerRGGB(64, 64, 8000);
		SRawDecodeResult rb;
		bool okB = CRawDecoder::DecodeBayer(bayer.data(), (unsigned)bayer.size(),
											64, 64, opt, &rb, &err);
		RD_ASSERT(okB, "non-DNG input decodes");
		bool clean = rb.dngFields[0] == 0 && rb.dngFields[1] == 0
					 && !rb.hasAsShotNeutral
					 && rb.dngAnalogBalance[0] == 1.f
					 && rb.dngAnalogBalance[1] == 1.f
					 && rb.dngAnalogBalance[2] == 1.f;
		bool zeroed = true;
		for (int i = 0; i < 3 && zeroed; i++)
			for (int j = 0; j < 3; j++)
				if (rb.dngColorMatrix[0][i][j] != 0.f
					|| rb.dngCalibration[0][i][j] != 0.f)
					{ zeroed = false; break; }
		CRawDecoder::FreeResult(&rb);
		RD_ASSERT(clean && zeroed,
				  "non-DNG: dngFields == 0, block zeroed, analog balance at identity");
	}

	TestCompleted(true, "DNG calibration provenance intact for RD-D");
}

void CTestRawNoRotation::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	std::string err;

	// Generated half: 40x28 (non-square -- a swap is VISIBLE, which a square
	// fixture could never show) with Orientation = 6 (90 degrees CW).
	{
		SSyntheticDngSpec spec;
		spec.width = 40;
		spec.height = 28;
		spec.orientation = 6;
		std::vector<unsigned char> dng = PC_BuildSyntheticDng(spec);
		RD_ASSERT(!dng.empty(), "oriented synthetic DNG built");
		std::string path = PC_WriteTempFile(".dng", dng);
		RD_ASSERT(!path.empty(), "oriented synthetic DNG written");
		CRawDecoder::SOptions opt;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(path.c_str(), opt, &r, &err);
		remove(path.c_str());
		RD_ASSERT(ok, "oriented DNG decodes");
		char m[128];
		snprintf(m, sizeof(m),
				 "Orientation=6 does NOT rotate the buffer (got %dx%d, want 40x28)",
				 r.width, r.height);
		bool unrotated = (r.width == 40 && r.height == 28);
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(unrotated, m);
	}

	// Real half: a portrait-orientation RAW, when the maintainer has one in
	// PC_RAW_FIXTURE_DIR/portrait/.
	std::string dir = PC_RawFixtureDir();
	std::string rawPath = dir.empty() ? "" :
		PC_FindFixtureIn(dir, "portrait",
						 { ".cr2", ".cr3", ".nef", ".arw", ".rw2", ".orf",
						   ".dng", ".raf" });
	if (rawPath.empty())
	{
		ReportRequiredGap("real portrait RAW did not run -- add a portrait-orientation "
		                  "RAW under tests/raws/portrait/; the synthetic DNG cannot "
		                  "reach the user_flip=0 double-rotation case");
		TestCompleted(true,
			"no-rotation invariant holds on the oriented synthetic DNG -- the real portrait RAW did NOT run");
		return;
	}
	{
		CRawDecoder::SOptions opt;
		SRawDecodeResult r;
		bool ok = CRawDecoder::Decode(rawPath.c_str(), opt, &r, &err);
		RD_ASSERT(ok, "portrait RAW decodes");
		// Sensors are landscape; a portrait shot's buffer must still come
		// back landscape (visible-area semantics, never raw_width x
		// raw_height and never swapped).
		bool landscape = r.width > r.height;
		CRawDecoder::FreeResult(&r);
		RD_ASSERT(landscape, "portrait RAW decodes UNROTATED (landscape buffer)");
	}

	TestCompleted(true, "no-rotation contract holds");
}

void CTestRawCodecCapability::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// Unconditional: the build-config probe. On any platform where the
	// image-codecs bundle predates the RD-A jpeg/zlib wiring this fails --
	// which is the point (the stale-bundle failure shape from RD-F #3.1).
	bool hasLibRaw = false, hasJpeg = false, hasZlib = false;
	CRawDecoder::GetBuildCapabilities(&hasLibRaw, &hasJpeg, &hasZlib);
	RD_ASSERT(hasLibRaw, "LibRaw present");
	RD_ASSERT(hasJpeg, "LIBRAW_CAPS_JPEG set -- lossy-JPEG DNG/KDC decodable");
	RD_ASSERT(hasZlib, "LIBRAW_CAPS_ZLIB set -- deflate/FP DNG decodable");

	// Fixture-gated: the two converter DNGs decode END TO END.
	std::string dir = PC_RawFixtureDir();
	std::string lossy = dir.empty() ? "" :
		PC_FindFixtureIn(dir, "codec", { ".dng" });
	if (dir.empty() || lossy.empty())
	{
		// DoD 8 calls this present-and-passing MANDATORY on all three machines.
		// Recording it as a required gap is what turns that from a sentence in
		// a summary string into something --require-fixtures can fail on.
		ReportRequiredGap("codec DNG decode did not run -- author codec/lossy.dng "
		                  "and codec/deflate.dng (Adobe DNG Converter, Lossy JPEG "
		                  "and Deflate); DoD 8 requires this on all three machines");
		TestCompleted(true,
			"capabilities verified -- codec DNG decode did NOT run (no codec/ fixtures)");
		return;
	}
	// Decode every DNG actually in codec/ -- any converter output there
	// must decode; the lossy.dng/deflate.dng names are convention, not
	// contract (review finding 3: probing only literal names would fail
	// loudly over a filename while the real fixture sits right there).
	{
		CRawDecoder::SOptions opt;
		int decoded = 0;
		bool sawLossyJpeg = false, sawDeflate = false, sawJpegXL = false;
		namespace fs = std::filesystem;
		std::error_code ec;
		for (const auto &entry : fs::directory_iterator(
				 fs::path(std::u8string(dir.begin(), dir.end())) / "codec", ec))
		{
			if (!entry.is_regular_file(ec))
				continue;
			std::string ext = entry.path().extension().string();
			if (ext != ".dng" && ext != ".DNG")
				continue;
			std::u8string u8 = entry.path().u8string();
			std::string p(u8.begin(), u8.end());
			SRawDecodeResult r;
			std::string err;
			bool ok = CRawDecoder::Decode(p.c_str(), opt, &r, &err);
			// On failure, name the COMPRESSION. LibRaw returns a bare -2
			// (LIBRAW_FILE_UNSUPPORTED) for every variant it cannot handle,
			// which is indistinguishable from a corrupt file -- so a fixture
			// authored with the wrong Adobe DNG Converter setting looks exactly
			// like a decoder bug. It read "unpack failed: -2" once and cost real
			// time; the file was JPEG XL, which this LibRaw cannot decode.
			char m[320];
			if (ok)
			{
				snprintf(m, sizeof(m), "codec DNG '%s' decodes: ok",
						 entry.path().filename().string().c_str());
			}
			else
			{
				int comp = PC_DngMainCompression(p);
				snprintf(m, sizeof(m),
						 "codec DNG '%s' decodes: %s [main image Compression=%d, %s]",
						 entry.path().filename().string().c_str(), err.c_str(),
						 comp, PC_DngCompressionName(comp));
			}
			bool shape = ok && r.width > 0 && r.height > 0 && r.linear != NULL;
			CRawDecoder::FreeResult(&r);
			RD_ASSERT(shape, m);

			int comp = PC_DngMainCompression(p);
			switch (comp)
			{
				case 34892: sawLossyJpeg = true; break;
				case 8:     sawDeflate = true;   break;
				case 52546: sawJpegXL = true;    break;
				default:    break;
			}

			// TILE ASSEMBLY. "It decoded" is not the assertion that matters for
			// a tiled codec: the most likely defect in a tile loop is placing
			// tiles at the wrong origin, and a scrambled grid still returns a
			// full-size, non-black, perfectly valid-looking buffer.
			//
			// A correctly assembled image is continuous across tile seams, so
			// the mean absolute difference between adjacent columns SPANNING a
			// seam should look like the difference anywhere else. If tiles are
			// misplaced the seams become discontinuities and the ratio blows up.
			// Re-decoded here because Decode() frees its result above.
			if (comp == 52546)
			{
				SRawDecodeResult rr;
				std::string e2;
				if (CRawDecoder::Decode(p.c_str(), opt, &rr, &e2) &&
					rr.linear != NULL && rr.width > 900 && rr.height > 900)
				{
					const int TILE = 416;   // this fixture's TileWidth
					double seamSum = 0.0, interiorSum = 0.0;
					int seamN = 0, interiorN = 0;
					const int y0 = rr.height / 4, y1 = rr.height * 3 / 4;
					for (int y = y0; y < y1; y += 7)
					{
						for (int x = 1; x < rr.width; x++)
						{
							const u16 *a = rr.linear + ((size_t)y * rr.width + x - 1) * rr.colors;
							const u16 *b = rr.linear + ((size_t)y * rr.width + x) * rr.colors;
							double d = 0.0;
							for (int c = 0; c < rr.colors; c++)
								d += fabs((double)b[c] - (double)a[c]);
							if (x % TILE == 0) { seamSum += d; seamN++; }
							else               { interiorSum += d; interiorN++; }
						}
					}
					double seam = seamN ? seamSum / seamN : 0.0;
					double interior = interiorN ? interiorSum / interiorN : 0.0;
					char m3[220];
					snprintf(m3, sizeof(m3),
							 "JPEG XL tiles assemble continuously (seam delta %.1f vs "
							 "interior %.1f over %d seam / %d interior samples)",
							 seam, interior, seamN, interiorN);
					// 4x is generous -- a misplaced tile grid produces orders of
					// magnitude, not a factor of four -- while leaving room for
					// genuine detail happening to sit on a seam.
					bool continuous = (interior > 0.0) && (seam < interior * 4.0);
					CRawDecoder::FreeResult(&rr);
					RD_ASSERT(continuous, m3);
				}
			}
			decoded++;
		}
		char m2[96];
		snprintf(m2, sizeof(m2), "%d codec DNG(s) decoded end to end", decoded);
		RD_ASSERT(decoded > 0, m2);

		// "Some DNG decoded" is not the claim DoD 8 makes. The point is that
		// the build's LIBRAW_CAPS_JPEG and LIBRAW_CAPS_ZLIB flags -- which are
		// compile-time claims -- are true END TO END, and that needs one fixture
		// of each compression. Without this, a directory holding only a lossless
		// JPEG DNG passes while proving neither, which is exactly what the first
		// attempt at these fixtures produced.
		if (!sawLossyJpeg)
			ReportRequiredGap("no Lossy JPEG (Compression 34892) fixture in codec/ -- "
			                  "LIBRAW_CAPS_JPEG is claimed but unproven. Adobe DNG "
			                  "Converter with Compatibility DNG 1.6 or EARLIER; 1.7+ "
			                  "produces JPEG XL instead");
		// JPEG XL (52546) is decoded by our patched LibRaw via libjxl, not by
		// the Adobe DNG SDK. Tracked separately: a JXL fixture proves the libjxl
		// path and says nothing about LIBRAW_CAPS_JPEG, and vice versa.
		if (!sawJpegXL)
			ReportRequiredGap("no JPEG XL (Compression 52546) fixture in codec/ -- "
			                  "the libjxl decode path is unproven");
		if (!sawDeflate)
			ReportRequiredGap("no Deflate (Compression 8) fixture in codec/ -- "
			                  "LIBRAW_CAPS_ZLIB is claimed but unproven");
	}

	TestCompleted(true, "codec capability verified");
}

void CTestRawConcurrent::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	// Two different synthetic DNGs decoded on two threads simultaneously.
	// Separate LibRaw instances are documented safe; RD-E will do exactly
	// this from its develop lane, so the claim gets exercised here (and
	// under TSan in the standalone probe -- see the task-12 commit message).
	SSyntheticDngSpec a;
	SSyntheticDngSpec b;
	b.width = 40;
	b.height = 28;
	b.fillValue = 12000;
	std::vector<unsigned char> dngA = PC_BuildSyntheticDng(a);
	std::vector<unsigned char> dngB = PC_BuildSyntheticDng(b);
	std::string pathA = PC_WriteTempFile(".dng", dngA);
	std::string pathB = PC_WriteTempFile(".dng", dngB);
	RD_ASSERT(!pathA.empty() && !pathB.empty(), "two synthetic DNGs written");

	bool okA = false, okB = false;
	SRawDecodeResult rA, rB;
	std::string errA, errB;
	CRawDecoder::SOptions opt;
	std::thread tA([&]() { okA = CRawDecoder::Decode(pathA.c_str(), opt, &rA, &errA); });
	std::thread tB([&]() { okB = CRawDecoder::Decode(pathB.c_str(), opt, &rB, &errB); });
	tA.join();
	tB.join();
	remove(pathA.c_str());
	remove(pathB.c_str());

	bool shapes = okA && okB && rA.width == 32 && rA.height == 32
				  && rB.width == 40 && rB.height == 28;
	CRawDecoder::FreeResult(&rA);
	CRawDecoder::FreeResult(&rB);
	RD_ASSERT(shapes, "both concurrent decodes succeed with the right shapes");

	TestCompleted(true, "concurrent decode smoke test passed");
}

void CTestRawBudget::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	std::string dir = PC_RawFixtureDir();
	std::string rawPath = dir.empty() ? "" :
		PC_FindFixture(dir, { ".cr2", ".cr3", ".nef", ".arw", ".rw2",
							  ".orf", ".dng", ".raf" });
	if (rawPath.empty())
	{
		TestCompleted(true,
			"scratch-budget check skipped (no PC_RAW_FIXTURE_DIR fixture -- a 32x32 synthetic proves nothing about the budget)");
		return;
	}

	CRawDecoder::SOptions opt;
	SRawDecodeResult r;
	std::string err;
	bool ok;
	long long peakDeltaMeasured = -1;
#ifdef __APPLE__
	{
		long long before = RawCurrentFootprintBytes();
		std::atomic<bool> done(false);
		std::atomic<long long> maxSeen(before);
		std::thread sampler([&]() {
			while (!done.load())
			{
				long long now = RawCurrentFootprintBytes();
				if (now > maxSeen.load())
					maxSeen.store(now);
				std::this_thread::sleep_for(std::chrono::milliseconds(2));
			}
		});
		ok = CRawDecoder::Decode(rawPath.c_str(), opt, &r, &err);
		done.store(true);
		sampler.join();
		if (before >= 0 && maxSeen.load() >= before)
			peakDeltaMeasured = maxSeen.load() - before;
	}
#else
	{
		long long before = RawPeakRssBytes();
		ok = CRawDecoder::Decode(rawPath.c_str(), opt, &r, &err);
		long long after = RawPeakRssBytes();
		if (before >= 0 && after >= 0)
			peakDeltaMeasured = after - before;
	}
#endif
	RD_ASSERT(ok && r.width > 0, "real RAW decodes for the budget check");

	// The estimate wants FULL-SENSOR dimensions; width/height are the
	// (possibly cropped) output. The crop is a few percent -- inside the 50%
	// margin -- so the output dims are a fine proxy here.
	size_t estimate = CRawDecoder::EstimateScratchBytes(r.width, r.height, false);
	long long peakDelta = peakDeltaMeasured;
	CRawDecoder::FreeResult(&r);

	char msg[224];
	snprintf(msg, sizeof(msg),
			 "peak RSS delta %.1f MB vs estimate %.1f MB (+50%% margin)",
			 peakDelta / 1048576.0, estimate / 1048576.0);
	if (peakDelta < 0)
	{
		// getrusage/GetProcessMemoryInfo failed -- report, do not fail the
		// suite over a broken meter (review finding 2b).
		TestSkipped("peak-RSS query unavailable -- the scratch-budget ceiling was never checked");
		return;
	}
#ifdef __APPLE__
	// Asserted here only: the machine the number was measured on. Both
	// bounds asserted: an upper (estimate + 50% -- a 2x scratch regression
	// fails) and a sanity lower (the sampler must have SEEN the decode --
	// at least the output buffer's worth -- or the measurement itself broke
	// and the assert would be vacuously green forever, which is how the
	// first two versions of this test failed their job).
	RD_ASSERT((size_t)peakDelta < estimate + estimate / 2
			  && (size_t)peakDelta > estimate / 8, msg);
#else
	// Report-only on Windows/Linux: peak RSS is noisy across allocators.
	StepCompleted(stepNum++, true, msg);
#endif

	TestCompleted(true, "scratch budget within the published constant");
}

void CTestRawFailurePaths::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	RD_SKIP_IF_UNAVAILABLE();

	CRawDecoder::SOptions opt;

	// Run the whole set twice: a leak- or state-corruption-shaped failure
	// often only shows on the second pass.
	for (int pass = 0; pass < 2; pass++)
	{
		// Non-RAW bytes behind a RAW extension.
		{
			std::string fake = PC_MakeFilledTempFile(".nef", 4096, 0xAB);
			RD_ASSERT(!fake.empty(), "fake .nef created");
			SRawDecodeResult r;
			std::string err;
			bool ok = CRawDecoder::Decode(fake.c_str(), opt, &r, &err);
			remove(fake.c_str());
			RD_ASSERT(!ok && !err.empty(), "non-RAW file: false with a reason");
			RD_ASSERT(ResultIsZeroed(r), "non-RAW file: result stays zeroed");
		}

		// Truncated file: a plausible TIFF header then nothing.
		{
			std::vector<unsigned char> stub = { 0x49, 0x49, 0x2A, 0x00,
												0x08, 0x00, 0x00, 0x00 };
			std::string trunc = PC_WriteTempFile(".dng", stub);
			RD_ASSERT(!trunc.empty(), "truncated .dng created");
			SRawDecodeResult r;
			std::string err;
			bool ok = CRawDecoder::Decode(trunc.c_str(), opt, &r, &err);
			remove(trunc.c_str());
			RD_ASSERT(!ok && !err.empty(), "truncated file: false with a reason");
			RD_ASSERT(ResultIsZeroed(r), "truncated file: result stays zeroed");
		}

		// Missing file.
		{
			SRawDecodeResult r;
			std::string err;
			bool ok = CRawDecoder::Decode("no_such_file_pc_rd_a.cr2", opt, &r, &err);
			RD_ASSERT(!ok && !err.empty(), "missing file: false with a reason");
			RD_ASSERT(ResultIsZeroed(r), "missing file: result stays zeroed");
		}

		// Bayer buffer whose datalen fits no supported bit depth
		// (tiff_bps = datalen*8/(w*h) must land on 8/10/12/14/16, #10).
		{
			std::vector<unsigned char> junk((size_t)64 * 64 * 2 - 100, 0x55);
			SRawDecodeResult r;
			std::string err;
			bool ok = CRawDecoder::DecodeBayer(junk.data(), (unsigned)junk.size(),
											   64, 64, opt, &r, &err);
			RD_ASSERT(!ok && !err.empty(),
					  "wrong-size Bayer buffer: false with a reason");
			RD_ASSERT(ResultIsZeroed(r), "wrong-size Bayer: result stays zeroed");
		}
	}

	TestCompleted(true, "RAW failure-path tests passed");
}
