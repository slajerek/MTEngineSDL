#include "CTestRawPreview.h"
#include "RawTestFixtures.h"
#include "CImageData.h"
#include "ICC_SRGBProfile.h"
#include "DBG_Log.h"
#include <cstdio>
#include <filesystem>
#include <string>

#define RP_ASSERT(cond, msg) \
	do { \
		bool rpOk = (cond); \
		if (!rpOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestRawPreview: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while (0)

void CTestRawPreview::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	// Case 2 (design #3, the CM-A hook): a preview whose APP2 carries an ICC
	// profile surfaces it through LoadRAWPreview. GENERATED, so it runs on
	// every machine: a synthetic DNG embedding a builder JPEG with a real
	// ICC v2 sRGB profile in its APP2 -- the plan's splice-into-a-real-RAW
	// route is unnecessary once the DNG builder can carry a thumb IFD.
	{
		std::vector<uint8_t> icc = ICC_BuildSRGBProfileV2();
		std::vector<unsigned char> jpeg =
			PC_BuildJpegWithIcc(64, 48, std::vector<unsigned char>(icc.begin(), icc.end()));
		if (jpeg.empty())
		{
			TestCompleted(false, "FAIL: PC_BuildJpegWithIcc produced no bytes");
			return;
		}
		StepCompleted(stepNum++, true, "ICC-carrying preview JPEG built");
		SSyntheticDngSpec spec;
		spec.thumbJpeg = &jpeg;
		spec.thumbWidth = 64;
		spec.thumbHeight = 48;
		std::vector<unsigned char> dng = PC_BuildSyntheticDng(spec);
		std::string path = PC_WriteTempFile(".dng", dng);
		if (path.empty())
		{
			TestCompleted(false, "FAIL: thumb-carrying synthetic DNG not written");
			return;
		}
		CImageData img;
		bool ok = img.LoadRAWPreview(path.c_str());
		remove(path.c_str());
		RP_ASSERT(ok && img.width == 64 && img.height == 48,
				  "synthetic DNG's embedded preview decodes at 64x48");
		RP_ASSERT(img.iccProfile != NULL && img.iccProfileSize > 100,
				  "APP2 ICC profile surfaced through LoadRAWPreview (CM-A hook)");
	}

	std::string dir = PC_RawFixtureDir();
	if (dir.empty())
	{
		TestCompleted(true, "generated ICC-preview case passed; real-RAW cases skipped (PC_RAW_FIXTURE_DIR not set)");
		return;
	}
	std::string rawPath = PC_FindFixture(dir,
		{ ".cr2", ".cr3", ".nef", ".arw", ".rw2", ".orf", ".dng", ".raf" });
	if (rawPath.empty())
	{
		TestSkipped("no RAW fixture in PC_RAW_FIXTURE_DIR -- the RAW preview safety net did NOT run");
		return;
	}

	// Case 1 (design #3): real RAW with an embedded thumb decodes as RGBA
	// with plausible dimensions.
	{
		CImageData img;
		bool ok = img.LoadRAWPreview(rawPath.c_str());
		RP_ASSERT(ok, "real RAW: LoadRAWPreview succeeds");
		RP_ASSERT(img.width > 0 && img.height > 0,
				  "real RAW: preview has nonzero dimensions");
		RP_ASSERT(img.getImageType() == IMG_TYPE_RGBA,
				  "real RAW: preview type is RGBA");
		RP_ASSERT(img.resultData != NULL, "real RAW: preview buffer is non-NULL");
	}

	// Case 3 (design #3): corrupted thumb bytes -- the contract is "returns
	// false OR a valid image; never crashes, never returns true with a
	// NULL/garbage buffer". Corrupt two windows at different depths so at
	// least one lands inside the embedded JPEG for typical layouts.
	{
		std::error_code ec;
		size_t sz = (size_t)std::filesystem::file_size(
			std::filesystem::path(std::u8string(rawPath.begin(), rawPath.end())), ec);
		RP_ASSERT(!ec && sz > 4096, "real RAW: fixture is readable and non-trivial");

		const double fractions[2] = { 0.25, 0.50 };
		for (int i = 0; i < 2; i++)
		{
			std::string bad =
				PC_MakeCorruptedCopy(rawPath, (size_t)(sz * fractions[i]), 64);
			RP_ASSERT(!bad.empty(), "corrupted copy created");
			CImageData img;
			bool ok = img.LoadRAWPreview(bad.c_str());
			bool contract = !ok || (img.resultData != NULL && img.width > 0
									&& img.height > 0);
			remove(bad.c_str());
			RP_ASSERT(contract,
					  "corrupted thumb: fails cleanly or returns a valid image");
		}
	}

	// Case 4 (design #3): non-RAW bytes behind a RAW extension fail cleanly.
	{
		std::string fake = PC_MakeFilledTempFile(".cr2", 4096, 0xAB);
		RP_ASSERT(!fake.empty(), "fake RAW file created");
		CImageData img;
		bool ok = img.LoadRAWPreview(fake.c_str());
		remove(fake.c_str());
		RP_ASSERT(ok == false, "non-RAW file with RAW extension: returns false");
	}

	// Case 2 (design #3, ICC in the thumb's APP2) is added in RD-A task 12 --
	// it needs the APP2 splice builder and the safety net must not wait for it.

	TestCompleted(true, "RAW preview safety net passed");
}
