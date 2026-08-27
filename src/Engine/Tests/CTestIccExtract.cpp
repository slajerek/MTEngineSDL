#include "CTestIccExtract.h"
#include "CTestIccHelpers.h"

#include "CExifReader.h"
#include "CExifBuilder.h"
#include "CIccProfileCodec.h"
#include "ICC_SRGBProfile.h"
#include "CImageData.h"
#include "JPEGWriter.h"
#include "DBG_Log.h"
#include "zlib.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace std;
using IccTestFixtures::MakeSyntheticProfile;

#define IX_ASSERT(cond, msg) \
	do { \
		bool ixOk = (cond); \
		if (!ixOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestIccExtract: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

namespace
{

const unsigned kW = 32, kH = 16;

string TempPath(const char *name)
{
	std::filesystem::path full = std::filesystem::temp_directory_path() / name;
	return full.string();
}

vector<unsigned char> MakeImage()
{
	vector<unsigned char> px(kW * kH * 3);
	for (unsigned y = 0; y < kH; y++)
		for (unsigned x = 0; x < kW; x++)
		{
			unsigned char *p = &px[(y * kW + x) * 3];
			p[0] = (unsigned char)((x * 8) & 0xFF);
			p[1] = (unsigned char)(((x ^ y) * 16) & 0xFF);
			p[2] = (unsigned char)((y * 16) & 0xFF);
		}
	return px;
}

vector<unsigned char *> RowPointers(vector<unsigned char> &px)
{
	vector<unsigned char *> rows(kH);
	for (unsigned y = 0; y < kH; y++)
		rows[y] = &px[y * kW * 3];
	return rows;
}

// Write a JPEG carrying the given APP1 (may be empty) and APP2 payloads, in
// the order supplied -- the order matters for the out-of-order test.
void WriteJpeg(const string &path,
               const vector<uint8_t> &app1,
               const vector<vector<uint8_t> > &app2s)
{
	vector<unsigned char> px = MakeImage();
	vector<unsigned char *> rows = RowPointers(px);
	JPEGWriter w;
	w.header(kW, kH, 3, JPEG::COLOR_RGB);
	w.setQuality(90);
	if (!app1.empty())
		w.setWriteJfifHeader(false);   // Exif wants APP1 immediately after SOI
	w.beginWrite(path);
	if (!app1.empty())
		w.writeMarker(0xE1, &app1[0], (unsigned)app1.size());
	for (size_t i = 0; i < app2s.size(); i++)
		w.writeMarker(0xE2, &app2s[i][0], (unsigned)app2s[i].size());
	w.writeRows(rows.begin());
	w.endWrite();
}

vector<uint8_t> Deflate(const vector<uint8_t> &in)
{
	uLongf bound = compressBound((uLong)in.size());
	vector<uint8_t> out(bound);
	if (compress2(&out[0], &bound, in.empty() ? NULL : &in[0], (uLong)in.size(), 9) != Z_OK)
		return vector<uint8_t>();
	out.resize(bound);
	return out;
}

void PutU32BE(vector<uint8_t> &v, uint32_t x)
{
	v.push_back((uint8_t)(x >> 24)); v.push_back((uint8_t)(x >> 16));
	v.push_back((uint8_t)(x >> 8));  v.push_back((uint8_t)x);
}

// One PNG chunk: length, type, data, CRC32 over (type + data).
void AppendChunk(vector<uint8_t> &png, const char type[4], const vector<uint8_t> &data)
{
	PutU32BE(png, (uint32_t)data.size());
	size_t crcStart = png.size();
	png.insert(png.end(), type, type + 4);
	png.insert(png.end(), data.begin(), data.end());
	uLong crc = crc32(0L, Z_NULL, 0);
	crc = crc32(crc, &png[crcStart], (uInt)(4 + data.size()));
	PutU32BE(png, (uint32_t)crc);
}

// A PNG that CExifReader can walk. It only reads chunk headers, never pixels,
// so IHDR + the chunk under test + IEND is enough.
vector<uint8_t> MakePng(const vector<uint8_t> &iccpPayload, bool withEXif)
{
	static const uint8_t sig[8] = { 0x89,'P','N','G',0x0D,0x0A,0x1A,0x0A };
	vector<uint8_t> png(sig, sig + 8);

	vector<uint8_t> ihdr;
	PutU32BE(ihdr, 4); PutU32BE(ihdr, 4);
	ihdr.push_back(8); ihdr.push_back(6); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);
	AppendChunk(png, "IHDR", ihdr);

	if (withEXif)
	{
		// A minimal little-endian TIFF: header + zero-entry IFD0.
		vector<uint8_t> tiff;
		tiff.push_back('I'); tiff.push_back('I'); tiff.push_back(0x2A); tiff.push_back(0x00);
		tiff.push_back(8); tiff.push_back(0); tiff.push_back(0); tiff.push_back(0);
		tiff.push_back(0); tiff.push_back(0);                       // 0 entries
		tiff.push_back(0); tiff.push_back(0); tiff.push_back(0); tiff.push_back(0);
		AppendChunk(png, "eXIf", tiff);
	}
	if (!iccpPayload.empty())
		AppendChunk(png, "iCCP", iccpPayload);

	AppendChunk(png, "IEND", vector<uint8_t>());
	return png;
}

vector<uint8_t> MakeIccpPayload(const vector<uint8_t> &profile, uint8_t method)
{
	vector<uint8_t> payload;
	const char *name = "icc";
	payload.insert(payload.end(), name, name + strlen(name));
	payload.push_back(0);
	payload.push_back(method);
	vector<uint8_t> z = Deflate(profile);
	payload.insert(payload.end(), z.begin(), z.end());
	return payload;
}

bool SameBytes(const vector<uint8_t> &a, const vector<uint8_t> &b)
{
	return a.size() == b.size() && (a.empty() || memcmp(&a[0], &b[0], a.size()) == 0);
}

} // namespace

// =====================================================================  JPEG

CTestIccExtractJpeg::CTestIccExtractJpeg() {}
CTestIccExtractJpeg::~CTestIccExtractJpeg() {}

void CTestIccExtractJpeg::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	const vector<uint8_t> smallProfile = MakeSyntheticProfile(1000);
	const vector<uint8_t> big = MakeSyntheticProfile(70000);
	vector<string> written;

	// 1. single-segment
	{
		const string p = TempPath("mt_icc_single.jpg");
		written.push_back(p);
		WriteJpeg(p, vector<uint8_t>(), CIccProfileCodec::SplitApp2(&smallProfile[0], (uint32_t)smallProfile.size()));
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(SameBytes(ex.iccProfile, smallProfile), "single-segment APP2 profile extracted byte-for-byte");
	}

	// 2. multi-segment
	{
		const string p = TempPath("mt_icc_multi.jpg");
		written.push_back(p);
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&big[0], (uint32_t)big.size());
		IX_ASSERT(segs.size() == 2, "70000-byte profile needs two APP2 segments");
		WriteJpeg(p, vector<uint8_t>(), segs);
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(SameBytes(ex.iccProfile, big), "multi-segment APP2 profile reassembled byte-for-byte");
	}

	// 3. segments written out of order
	{
		const string p = TempPath("mt_icc_swapped.jpg");
		written.push_back(p);
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&big[0], (uint32_t)big.size());
		vector<vector<uint8_t> > swapped;
		swapped.push_back(segs[1]);
		swapped.push_back(segs[0]);
		WriteJpeg(p, vector<uint8_t>(), swapped);
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(SameBytes(ex.iccProfile, big), "out-of-order APP2 segments reassemble correctly");
	}

	// 4. a gap yields nothing, never a partial profile
	{
		const string p = TempPath("mt_icc_gap.jpg");
		written.push_back(p);
		vector<uint8_t> huge = MakeSyntheticProfile(140000);
		vector<vector<uint8_t> > segs = CIccProfileCodec::SplitApp2(&huge[0], (uint32_t)huge.size());
		IX_ASSERT(segs.size() == 3, "140000-byte profile needs three APP2 segments");
		vector<vector<uint8_t> > gap;
		gap.push_back(segs[0]);
		gap.push_back(segs[2]);
		WriteJpeg(p, vector<uint8_t>(), gap);
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(ex.iccProfile.empty(), "a missing APP2 segment yields no profile, not a partial one");
	}

	// 5. real Exif APP1 alongside ICC: BOTH must land
	{
		const string p = TempPath("mt_icc_exif.jpg");
		written.push_back(p);
		CExifBuilder b;
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x010F, "TestCam");
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x0110, "IccModel");
		vector<uint8_t> app1 = b.BuildApp1(true);
		IX_ASSERT(!app1.empty(), "CExifBuilder produced an APP1 payload");
		WriteJpeg(p, app1, CIccProfileCodec::SplitApp2(&smallProfile[0], (uint32_t)smallProfile.size()));
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(ex.make == "TestCam" && ex.model == "IccModel",
		          "EXIF fields still parse when ICC capture is on");
		IX_ASSERT(SameBytes(ex.iccProfile, smallProfile),
		          "ICC is collected past the APP1 (the walk no longer stops at Exif)");
	}

	// 6. the lean path does no ICC work
	{
		const string p = TempPath("mt_icc_single.jpg");
		CExifData ex = CExifReader::ReadFile(p);          // captureIcc defaults false
		IX_ASSERT(ex.iccProfile.empty(), "captureIcc=false leaves iccProfile empty");
	}

	// 7. malformed APP1 before a valid APP2 must not abandon the walk
	{
		const string p = TempPath("mt_icc_badexif.jpg");
		written.push_back(p);
		// "Exif\0\0" + only 4 bytes of TIFF: too short to parse (tiffSize < 8).
		vector<uint8_t> app1;
		const char *id = "Exif";
		app1.insert(app1.end(), id, id + 4);
		app1.push_back(0); app1.push_back(0);
		for (int i = 0; i < 4; i++) app1.push_back((uint8_t)i);
		WriteJpeg(p, app1, CIccProfileCodec::SplitApp2(&smallProfile[0], (uint32_t)smallProfile.size()));
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(SameBytes(ex.iccProfile, smallProfile),
		          "a malformed Exif APP1 does not prevent ICC collection");
	}

	// 8. an unrelated APP2 mixed in is ignored
	{
		const string p = TempPath("mt_icc_noise.jpg");
		written.push_back(p);
		vector<vector<uint8_t> > segs;
		segs.push_back(vector<uint8_t>(200, 0x5A));     // not ICC_PROFILE
		vector<vector<uint8_t> > icc = CIccProfileCodec::SplitApp2(&smallProfile[0], (uint32_t)smallProfile.size());
		segs.insert(segs.end(), icc.begin(), icc.end());
		WriteJpeg(p, vector<uint8_t>(), segs);
		CExifData ex = CExifReader::ReadFile(p, true);
		IX_ASSERT(SameBytes(ex.iccProfile, smallProfile), "a non-ICC APP2 alongside is ignored");
	}

	for (size_t i = 0; i < written.size(); i++)
	{
		std::error_code ec;
		std::filesystem::remove(written[i], ec);
	}

	TestCompleted(true, "JPEG APP2 ICC extraction: single, multi, out-of-order, gap, with-Exif, malformed-Exif");
}

void CTestIccExtractJpeg::Cancel() { isRunning = false; }

// ======================================================================  PNG

CTestIccExtractPng::CTestIccExtractPng() {}
CTestIccExtractPng::~CTestIccExtractPng() {}

void CTestIccExtractPng::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	const vector<uint8_t> srgb = ICC_BuildSRGBProfileV2();

	{
		vector<uint8_t> png = MakePng(MakeIccpPayload(srgb, 0), false);
		CExifData ex = CExifReader::Read(&png[0], png.size(), true, true);
		IX_ASSERT(SameBytes(ex.iccProfile, srgb), "iCCP profile inflated and extracted byte-for-byte");
	}
	{
		vector<uint8_t> png = MakePng(MakeIccpPayload(srgb, 1), false);
		CExifData ex = CExifReader::Read(&png[0], png.size(), true, true);
		IX_ASSERT(ex.iccProfile.empty(), "iCCP with an unknown compression method yields nothing");
	}
	{
		vector<uint8_t> png = MakePng(MakeIccpPayload(srgb, 0), false);
		CExifData ex = CExifReader::Read(&png[0], png.size(), true, false);
		IX_ASSERT(ex.iccProfile.empty(), "captureIcc=false does no iCCP work");
	}
	{
		// eXIf BEFORE iCCP: the walk used to return at eXIf, which would have
		// dropped the profile depending purely on chunk order.
		vector<uint8_t> png = MakePng(MakeIccpPayload(srgb, 0), true);
		CExifData ex = CExifReader::Read(&png[0], png.size(), true, true);
		IX_ASSERT(SameBytes(ex.iccProfile, srgb), "iCCP after eXIf is still collected");
	}
	{
		vector<uint8_t> png = MakePng(vector<uint8_t>(), false);
		CExifData ex = CExifReader::Read(&png[0], png.size(), true, true);
		IX_ASSERT(ex.iccProfile.empty(), "a PNG with no iCCP reports no profile");
	}

	TestCompleted(true, "PNG iCCP ICC extraction verified");
}

void CTestIccExtractPng::Cancel() { isRunning = false; }

// ============================================================  CImageData

CTestIccExtractFormats::CTestIccExtractFormats() {}
CTestIccExtractFormats::~CTestIccExtractFormats() {}

void CTestIccExtractFormats::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	const vector<uint8_t> big = MakeSyntheticProfile(70000);
	const vector<uint8_t> smallProfile = MakeSyntheticProfile(1000);
	vector<string> written;

	// 1. tagged JPEG through the stb branch
	const string tagged = TempPath("mt_iccimg_tagged.jpg");
	written.push_back(tagged);
	WriteJpeg(tagged, vector<uint8_t>(), CIccProfileCodec::SplitApp2(&big[0], (uint32_t)big.size()));
	{
		CImageData img;
		IX_ASSERT(img.Load(tagged.c_str(), true), "tagged JPEG loads");
		IX_ASSERT(img.resultData != NULL, "tagged JPEG decoded pixels");
		IX_ASSERT(img.iccProfile != NULL && img.iccProfileSize == 70000,
		          "CImageData carries the 70000-byte profile from a multi-segment APP2");
		IX_ASSERT(memcmp(img.iccProfile, &big[0], big.size()) == 0,
		          "the carried profile is byte-identical to the embedded one");
	}

	// 2. untagged JPEG: no false positives
	const string untagged = TempPath("mt_iccimg_untagged.jpg");
	written.push_back(untagged);
	WriteJpeg(untagged, vector<uint8_t>(), vector<vector<uint8_t> >());
	{
		CImageData img;
		IX_ASSERT(img.Load(untagged.c_str(), true), "untagged JPEG loads");
		IX_ASSERT(img.iccProfile == NULL && img.iccProfileSize == 0,
		          "an untagged JPEG reports no profile");
	}

	// 3. malformed profile must be refused, not stored
	{
		const string bad = TempPath("mt_iccimg_bad.jpg");
		written.push_back(bad);
		vector<uint8_t> junk(2000, 0xAB);            // no 'acsp', no valid header
		vector<vector<uint8_t> > segs;
		vector<uint8_t> seg;
		const char *id = "ICC_PROFILE";
		seg.insert(seg.end(), id, id + 11);
		seg.push_back(0);
		seg.push_back(1); seg.push_back(1);
		seg.insert(seg.end(), junk.begin(), junk.end());
		segs.push_back(seg);
		WriteJpeg(bad, vector<uint8_t>(), segs);
		CImageData img;
		IX_ASSERT(img.Load(bad.c_str(), true), "JPEG with a malformed profile still loads");
		IX_ASSERT(img.iccProfile == NULL, "a malformed profile is refused, leaving the image untagged");
	}

	// 4. ownership across a reload: the specialized loaders return before the
	//    dealloc block, so this is the case the top-of-Load reset exists for
	{
		CImageData img;
		IX_ASSERT(img.Load(tagged.c_str(), true), "reload case: tagged first");
		IX_ASSERT(img.iccProfile != NULL, "reload case: profile present after the tagged load");
		IX_ASSERT(img.Load(untagged.c_str(), true), "reload case: untagged second");
		IX_ASSERT(img.iccProfile == NULL && img.iccProfileSize == 0,
		          "reloading an untagged file clears the previous profile");
	}

	// 5. copies carry the profile
	{
		CImageData src;
		IX_ASSERT(src.Load(tagged.c_str(), true), "copy case: source loads");
		CImageData clone(&src);
		IX_ASSERT(clone.iccProfile != NULL && clone.iccProfileSize == src.iccProfileSize,
		          "the copy constructor deep-copies the profile");
		IX_ASSERT(clone.iccProfile != src.iccProfile,
		          "the copy owns its own bytes (not an aliased pointer)");
		IX_ASSERT(memcmp(clone.iccProfile, &big[0], big.size()) == 0,
		          "the copied profile is byte-identical");

		// CopyDataFrom does not copy `type`, and GetDataLength() fatals on an
		// unknown type -- so the target must be shaped first. Pre-existing
		// contract; noted here because it is not obvious from the name.
		CImageData target(src.width, src.height, IMG_TYPE_RGBA);
		target.CopyDataFrom(&src);
		IX_ASSERT(target.iccProfile != NULL && target.iccProfileSize == src.iccProfileSize,
		          "CopyDataFrom carries the profile too");
	}

	// 6. soft-skip fixtures for the container formats we cannot synthesise here
	{
		const char *fixtures[] = {
			"tests/fixtures/formats/icc/tagged.webp",
			"tests/fixtures/formats/icc/tagged.avif",
			"tests/fixtures/formats/icc/p3.heic",
			NULL
		};
		for (int i = 0; fixtures[i]; i++)
		{
			if (!std::filesystem::exists(fixtures[i]))
			{
				LOGM("CTestIccExtractFormats: SKIP %s (fixture absent)", fixtures[i]);
				StepCompleted(stepNum++, true, "fixture absent -- skipped");
				continue;
			}
			CImageData img;
			IX_ASSERT(img.Load(fixtures[i], true), "fixture loads");
			IX_ASSERT(img.iccProfile != NULL &&
			          CIccProfileCodec::ValidateHeader(img.iccProfile, img.iccProfileSize),
			          "fixture yields a structurally valid ICC profile");
		}
	}

	for (size_t i = 0; i < written.size(); i++)
	{
		std::error_code ec;
		std::filesystem::remove(written[i], ec);
	}

	TestCompleted(true, "CImageData ICC: extraction, refusal, reload ownership and copies verified");
}

void CTestIccExtractFormats::Cancel() { isRunning = false; }
