#include "CTestJpegMarkers.h"
#include "DBG_Log.h"
#include "JPEGWriter.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
using namespace std;

#define JM_ASSERT(cond, msg) \
	do { \
		bool jmOk = (cond); \
		if (!jmOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestJpegMarkers: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

namespace {

const unsigned kW = 32, kH = 16;

// A deterministic RGB test image with enough high-frequency content that
// chroma subsampling changes the encoded size.
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
	for (unsigned y = 0; y < kH; y++) rows[y] = &px[y * kW * 3];
	return rows;
}

vector<unsigned char> ReadFileBytes(const string &path)
{
	vector<unsigned char> out;
	FILE *f = fopen(path.c_str(), "rb");
	if (!f) return out;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	if (n > 0) { out.resize((size_t)n); size_t rd = fread(&out[0], 1, (size_t)n, f); out.resize(rd); }
	fclose(f);
	return out;
}

// Walk the JPEG marker segments in order, returning the marker bytes found
// after SOI (0xFFD8) up to the start of scan data.
vector<unsigned char> MarkerSequence(const vector<unsigned char> &j)
{
	vector<unsigned char> seq;
	size_t i = 2;                                   // skip SOI
	while (i + 3 < j.size() && j[i] == 0xFF)
	{
		unsigned char m = j[i + 1];
		if (m == 0xDA || m == 0xD9) break;          // SOS / EOI: scan data follows
		seq.push_back(m);
		size_t len = ((size_t)j[i + 2] << 8) | j[i + 3];
		i += 2 + len;
	}
	return seq;
}

// Sampling factors of component 0, read straight out of the SOF0 segment:
// len(2) precision(1) height(2) width(2) nComp(1) then per component
// id(1) sampling(1: h<<4 | v) qtable(1).
bool LumaSampling(const vector<unsigned char> &j, int &h, int &v)
{
	size_t i = 2;
	while (i + 3 < j.size() && j[i] == 0xFF)
	{
		unsigned char m = j[i + 1];
		size_t len = ((size_t)j[i + 2] << 8) | j[i + 3];
		if (m == 0xC0 || m == 0xC1)                 // SOF0 / SOF1
		{
			// From the 0xFF at i: len(2) precision(1) height(2) width(2)
			// nComp(1) = 8 bytes, so component 0 begins at i+2+8. Its second
			// byte packs the sampling factors as h<<4 | v.
			size_t comp0 = i + 2 + 8;
			if (comp0 + 2 >= j.size()) return false;
			h = (j[comp0 + 1] >> 4) & 0x0F;
			v =  j[comp0 + 1]       & 0x0F;
			return true;
		}
		if (m == 0xDA || m == 0xD9) break;
		i += 2 + len;
	}
	return false;
}

string TempPath(const char *name)
{
	std::filesystem::path dir = std::filesystem::temp_directory_path();
	std::filesystem::path full = dir / name;
	std::string result = full.string();
	return result;
}

} // namespace

CTestJpegMarkers::CTestJpegMarkers() {}
CTestJpegMarkers::~CTestJpegMarkers() {}

void CTestJpegMarkers::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	LOGD("CTestJpegMarkers: Starting JPEGWriter marker tests");

	vector<unsigned char> px = MakeImage();

	// -----------------------------------------------------------------------
	// 1. APP1 then APP2 appear in call order, immediately after SOI, and the
	//    image data still decodes.
	// -----------------------------------------------------------------------
	const string p1 = TempPath("mtengine_markers_order.jpg");
	{
		vector<unsigned char> app1;
		const char *id = "Exif";
		app1.insert(app1.end(), id, id + 4);
		app1.push_back(0); app1.push_back(0);
		for (int i = 0; i < 40; i++) app1.push_back((unsigned char)i);

		vector<unsigned char> app2(200, 0x5A);

		vector<unsigned char *> rows = RowPointers(px);
		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(90);
			w.beginWrite(p1);
			w.writeMarker(0xE1, &app1[0], (unsigned)app1.size());
			w.writeMarker(0xE2, &app2[0], (unsigned)app2.size());
			w.writeRows(rows.begin());
			w.endWrite();
		}

		vector<unsigned char> bytes = ReadFileBytes(p1);
		JM_ASSERT(bytes.size() > 4, "marker file written");
		JM_ASSERT(bytes[0] == 0xFF && bytes[1] == 0xD8, "starts with SOI");

		// libjpeg emits its own JFIF APP0 first (jcmarker.c:537), so what
		// writeMarker guarantees is call ORDER among the caller's markers, not
		// absolute position. Position is controlled separately -- see step 2.
		vector<unsigned char> seq = MarkerSequence(bytes);
		int iApp1 = -1, iApp2 = -1;
		for (size_t i = 0; i < seq.size(); i++)
		{
			if (seq[i] == 0xE1 && iApp1 < 0) iApp1 = (int)i;
			if (seq[i] == 0xE2 && iApp2 < 0) iApp2 = (int)i;
		}
		JM_ASSERT(iApp1 >= 0, "APP1 present");
		JM_ASSERT(iApp2 >= 0, "APP2 present");
		JM_ASSERT(iApp1 < iApp2, "markers emitted in call order (APP1 before APP2)");

		bool sawEOI = bytes.size() > 2 &&
		              bytes[bytes.size() - 2] == 0xFF && bytes[bytes.size() - 1] == 0xD9;
		JM_ASSERT(sawEOI, "image data completes with EOI");
	}

	// -----------------------------------------------------------------------
	// 1b. With JFIF suppressed, APP1 lands immediately after SOI -- which is
	//     what the Exif specification requires and what an Exif-writing caller
	//     must be able to produce.
	// -----------------------------------------------------------------------
	{
		const string pExif = TempPath("mtengine_markers_exif_first.jpg");
		vector<unsigned char> app1;
		const char *id = "Exif";
		app1.insert(app1.end(), id, id + 4);
		app1.push_back(0); app1.push_back(0);
		for (int i = 0; i < 40; i++) app1.push_back((unsigned char)i);

		vector<unsigned char *> rows = RowPointers(px);
		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(90);
			w.setWriteJfifHeader(false);
			w.beginWrite(pExif);
			w.writeMarker(0xE1, &app1[0], (unsigned)app1.size());
			w.writeRows(rows.begin());
			w.endWrite();
		}

		vector<unsigned char> bytes = ReadFileBytes(pExif);
		vector<unsigned char> seq = MarkerSequence(bytes);
		JM_ASSERT(!seq.empty(), "markers present with JFIF suppressed");
		JM_ASSERT(seq[0] == 0xE1, "APP1 is FIRST after SOI when JFIF is suppressed");
		for (size_t i = 0; i < seq.size(); i++)
			JM_ASSERT(seq[i] != 0xE0, "no JFIF APP0 when suppressed");

		std::error_code ec;
		std::filesystem::remove(pExif, ec);
	}

	// -----------------------------------------------------------------------
	// 2. The 65533-byte cap: exactly at the limit writes, one over throws, and
	//    the rejected write leaves no partial marker behind.
	// -----------------------------------------------------------------------
	{
		const string pOk = TempPath("mtengine_markers_cap_ok.jpg");
		vector<unsigned char> atCap(65533, 0x7E);
		vector<unsigned char *> rows = RowPointers(px);
		bool threw = false;
		try
		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(80);
			w.beginWrite(pOk);
			w.writeMarker(0xE2, &atCap[0], (unsigned)atCap.size());
			w.writeRows(rows.begin());
			w.endWrite();
		}
		catch (...) { threw = true; }
		JM_ASSERT(!threw, "a 65533-byte payload is accepted");

		const string pBad = TempPath("mtengine_markers_cap_over.jpg");
		std::filesystem::remove(pBad);
		vector<unsigned char> overCap(65534, 0x7E);
		threw = false;
		try
		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(80);
			w.beginWrite(pBad);
			w.writeMarker(0xE2, &overCap[0], (unsigned)overCap.size());
			w.writeRows(rows.begin());
			w.endWrite();
		}
		catch (...) { threw = true; }
		JM_ASSERT(threw, "a 65534-byte payload throws rather than chunking");

		// Silent truncation is the one unacceptable outcome: whatever is on
		// disk must not contain a partial APP2.
		vector<unsigned char> bad = ReadFileBytes(pBad);
		bool hasPartial = false;
		for (size_t i = 0; i + 1 < bad.size(); i++)
			if (bad[i] == 0xFF && bad[i + 1] == 0xE2) { hasPartial = true; break; }
		JM_ASSERT(!hasPartial, "rejected write leaves no partial APP2 in the file");
	}

	// -----------------------------------------------------------------------
	// 3. setChromaSubsampling, in the window it requires: after header(),
	//    before beginWrite(). Asserted on the SOF0 sampling factors, so the
	//    timing fix cannot regress silently.
	// -----------------------------------------------------------------------
	{
		const string p444 = TempPath("mtengine_markers_444.jpg");
		const string p420 = TempPath("mtengine_markers_420.jpg");
		vector<unsigned char *> rows = RowPointers(px);

		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(90);
			w.setChromaSubsampling(0);                 // 4:4:4
			w.beginWrite(p444);
			w.writeRows(rows.begin());
			w.endWrite();
		}
		{
			JPEGWriter w;
			w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(90);
			w.setChromaSubsampling(1);                 // 4:2:0
			w.beginWrite(p420);
			w.writeRows(rows.begin());
			w.endWrite();
		}

		int h = 0, v = 0;
		vector<unsigned char> b444 = ReadFileBytes(p444);
		JM_ASSERT(LumaSampling(b444, h, v), "SOF0 found in the 4:4:4 file");
		JM_ASSERT(h == 1 && v == 1, "mode 0 yields 1x1 luma sampling (4:4:4)");

		vector<unsigned char> b420 = ReadFileBytes(p420);
		JM_ASSERT(LumaSampling(b420, h, v), "SOF0 found in the 4:2:0 file");
		JM_ASSERT(h == 2 && v == 2, "mode 1 yields 2x2 luma sampling (4:2:0)");
	}

	// -----------------------------------------------------------------------
	// 4. setQuality belongs in the same window. jpeg_set_defaults() calls
	//    jpeg_set_quality(75) itself, so a call placed before header() is
	//    silently reset -- no crash, just every file at the wrong quality.
	// -----------------------------------------------------------------------
	{
		const string pLo = TempPath("mtengine_markers_q20.jpg");
		const string pHi = TempPath("mtengine_markers_q95.jpg");
		vector<unsigned char *> rows = RowPointers(px);

		{
			JPEGWriter w; w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(20); w.write(pLo, rows.begin());
		}
		{
			JPEGWriter w; w.header(kW, kH, 3, JPEG::COLOR_RGB);
			w.setQuality(95); w.write(pHi, rows.begin());
		}

		size_t lo = ReadFileBytes(pLo).size();
		size_t hi = ReadFileBytes(pHi).size();
		JM_ASSERT(lo > 0 && hi > 0, "both quality files written");
		JM_ASSERT(hi > lo, "quality set after header() takes effect (q95 > q20)");
	}

	// Clean up.
	const char *tmps[] = { "mtengine_markers_order.jpg", "mtengine_markers_cap_ok.jpg",
	                       "mtengine_markers_cap_over.jpg", "mtengine_markers_444.jpg",
	                       "mtengine_markers_420.jpg", "mtengine_markers_q20.jpg",
	                       "mtengine_markers_q95.jpg" };
	for (size_t i = 0; i < sizeof(tmps) / sizeof(tmps[0]); i++)
	{
		std::error_code ec;
		std::filesystem::remove(TempPath(tmps[i]), ec);
	}

	LOGD("CTestJpegMarkers: All JPEG marker tests passed");
	TestCompleted(true, "APP marker order, 65533 cap, subsampling and quality window verified");
}

void CTestJpegMarkers::Cancel()
{
	isRunning = false;
}
