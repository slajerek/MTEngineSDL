#include "CTestKtx2Transcode.h"

#include "CKTX2Loader.h"
#include "DBG_Log.h"
#include "basisu_transcoder.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

// Compiled-in KTX2 support is also asserted at compile time in CImageData.cpp.
// Restated here because this test is the RUNTIME half of the same guarantee: a
// static_assert proves the macro, this proves the code behind it actually runs.
static_assert(BASISD_SUPPORT_KTX2 == 1, "CTestKtx2Transcode requires BASISD_SUPPORT_KTX2=1");

CTestKtx2Transcode::CTestKtx2Transcode() {}
CTestKtx2Transcode::~CTestKtx2Transcode() {}
void CTestKtx2Transcode::Cancel() { isRunning = false; }

// Apps that ship a KTX2 fixture put it at one of these, PROJECT-RELATIVE.
// Resolved through CTest::ResolveProjectPath, never opened raw: the binary runs
// from the git root for a development build and from platform/<P>/prod/<arch>/
// for a final one, and a literal path is right in exactly one of them.
static const char *kFixtureCandidates[] = {
	"tests/testdata/ktx2-decode-64.ktx2",
	"tests/fixtures/formats/ktx2/uastc-64.ktx2",
	NULL
};

void CTestKtx2Transcode::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

	basist::basisu_transcoder_init();
	StepCompleted(1, true, "basisu_transcoder_init() returned");

	// ---- Step 2: the preflight must REJECT a container that is not KTX2 ----
	//
	// The dispatch in CImageData::LoadKTX2 decides between the BasisLZ/UASTC
	// transcoder and the concrete-format software path purely from this header
	// read. If it accepted garbage, both paths would be handed a buffer neither
	// can parse, and the failure would surface as a decode error somewhere far
	// from the actual cause.
	{
		u8 notKtx2[128];
		memset(notKtx2, 0xAB, sizeof(notKtx2));

		KTX2HeaderInfo hdr;
		if (KTX2_ReadHeaderForDispatch(notKtx2, sizeof(notKtx2), hdr))
		{
			TestCompleted(false, "KTX2_ReadHeaderForDispatch accepted a non-KTX2 buffer");
			return;
		}
	}
	StepCompleted(2, true, "Preflight rejects a non-KTX2 buffer");

	// ---- Step 3: the preflight must REJECT a truncated container ----
	//
	// A valid 12-byte identifier followed by nothing. This is the shape a
	// half-written or half-downloaded asset actually has, so it is the case a
	// length check is most likely to get wrong.
	{
		const u8 ktx2Identifier[12] = {
			0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32, 0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A
		};
		KTX2HeaderInfo hdr;
		if (KTX2_ReadHeaderForDispatch(ktx2Identifier, sizeof(ktx2Identifier), hdr))
		{
			TestCompleted(false, "KTX2_ReadHeaderForDispatch accepted a header-only truncated file");
			return;
		}
	}
	StepCompleted(3, true, "Preflight rejects a truncated container");

	// ---- Step 4: the transcoder must REJECT the same garbage ----
	//
	// ktx2_transcoder::init() is the second gate. It is tested separately from
	// the preflight because the two are independent: a container can pass the
	// engine's own header read and still be something basis will not open.
	{
		u8 notKtx2[256];
		memset(notKtx2, 0x5C, sizeof(notKtx2));

		basist::ktx2_transcoder transcoder;
		if (transcoder.init(notKtx2, (uint32_t)sizeof(notKtx2)))
		{
			TestCompleted(false, "basist::ktx2_transcoder::init() accepted a non-KTX2 buffer");
			return;
		}
	}
	StepCompleted(4, true, "ktx2_transcoder::init() rejects a non-KTX2 buffer");

	// ---- Step 5: transcode a real fixture, if this app ships one ----
	//
	// No project root is a FAILURE, not a skip. A bad working directory that
	// silently downgraded this test to its fixture-free half would be exactly
	// the hollow green ResolveProjectPath exists to remove.
	if (CTest::ProjectRootPath().empty())
	{
		TestCompleted(false, "no project root: cannot resolve the KTX2 fixture, and a skip here would hide a bad working directory");
		return;
	}

	std::string fixturePathStr;
	for (int i = 0; kFixtureCandidates[i] != NULL; i++)
	{
		std::string candidate = CTest::ResolveProjectPath(kFixtureCandidates[i]);
		if (!candidate.empty() && std::filesystem::exists(candidate))
		{
			fixturePathStr = candidate;
			break;
		}
	}
	const char *fixturePath = fixturePathStr.empty() ? NULL : fixturePathStr.c_str();

	if (fixturePath == NULL)
	{
		// Honest skip, not a pass: steps 1-4 proved the transcoder is present
		// and refuses bad input, but nothing here proved it can DECODE.
		TestSkipped("no KTX2 fixture in this app, so a real UASTC transcode was never exercised");
		return;
	}

	std::vector<u8> bytes;
	{
		FILE *fp = fopen(fixturePath, "rb");
		if (fp == NULL)
		{
			TestCompleted(false, "fixture exists but could not be opened");
			return;
		}
		fseek(fp, 0, SEEK_END);
		long size = ftell(fp);
		fseek(fp, 0, SEEK_SET);
		if (size <= 0)
		{
			fclose(fp);
			TestCompleted(false, "fixture is empty");
			return;
		}
		bytes.resize((size_t)size);
		size_t rd = fread(bytes.data(), 1, (size_t)size, fp);
		fclose(fp);
		if (rd != (size_t)size)
		{
			TestCompleted(false, "short read on the KTX2 fixture");
			return;
		}
	}

	KTX2HeaderInfo hdr;
	if (!KTX2_ReadHeaderForDispatch(bytes.data(), bytes.size(), hdr))
	{
		TestCompleted(false, "preflight rejected a fixture it should accept");
		return;
	}
	StepCompleted(5, true, "Preflight accepts the fixture");

	const bool isBasis = (hdr.supercompressionScheme == 1 || hdr.vkFormat == 0);
	if (!isBasis)
	{
		// A concrete-vkFormat fixture goes down CKTX2Loader's software path,
		// which is not what this test covers.
		TestSkipped("fixture is a concrete-vkFormat KTX2, so the UASTC transcoder path was never exercised");
		return;
	}

	basist::ktx2_transcoder transcoder;
	if (!transcoder.init(bytes.data(), (uint32_t)bytes.size()))
	{
		TestCompleted(false, "ktx2_transcoder::init() rejected a valid UASTC fixture");
		return;
	}
	if (!transcoder.start_transcoding())
	{
		TestCompleted(false, "ktx2_transcoder::start_transcoding() failed on a valid fixture");
		return;
	}
	StepCompleted(6, true, "Transcoder opened the fixture");

	const uint32_t levels = transcoder.get_levels();
	const uint32_t w = transcoder.get_width();
	const uint32_t h = transcoder.get_height();
	if (levels < 1 || w < 1 || h < 1)
	{
		char msg[160];
		snprintf(msg, sizeof(msg), "degenerate fixture geometry: %ux%u, %u level(s)", w, h, levels);
		TestCompleted(false, msg);
		return;
	}

	// Transcode mip 0 to RGBA32. Chosen deliberately over BC7/ASTC: RGBA32 is
	// the one target every build can decode regardless of which compressed
	// formats the host GPU reports, so this step tests the TRANSCODER rather
	// than the machine it happens to run on.
	basist::ktx2_image_level_info li;
	if (!transcoder.get_image_level_info(li, 0, 0, 0))
	{
		TestCompleted(false, "get_image_level_info() failed for mip 0");
		return;
	}

	std::vector<uint32_t> rgba((size_t)li.m_orig_width * li.m_orig_height);
	if (!transcoder.transcode_image_level(0, 0, 0,
										  rgba.data(),
										  (uint32_t)rgba.size(),
										  basist::transcoder_texture_format::cTFRGBA32))
	{
		TestCompleted(false, "transcode_image_level() to RGBA32 failed on a valid fixture");
		return;
	}

	// A transcode that "succeeds" into an untouched buffer is the failure this
	// guards: assert the output is not uniformly zero.
	bool anyNonZero = false;
	for (size_t i = 0; i < rgba.size(); i++)
	{
		if (rgba[i] != 0) { anyNonZero = true; break; }
	}
	if (!anyNonZero)
	{
		TestCompleted(false, "transcode reported success but produced an all-zero image");
		return;
	}

	char summary[192];
	snprintf(summary, sizeof(summary),
			 "KTX2 transcode: rejects malformed input, decoded %ux%u UASTC fixture (%u mip level(s)) to RGBA32",
			 w, h, levels);
	TestCompleted(true, summary);
}
