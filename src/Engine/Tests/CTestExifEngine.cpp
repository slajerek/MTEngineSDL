#include "CTestExifEngine.h"
#include "CExifBuilder.h"
#include "CExifReader.h"
#include "DBG_Log.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace std;

#define EX_ASSERT(cond, msg) \
	do { \
		bool exOk = (cond); \
		if (!exOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestExifEngine: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

namespace {

// Wrap an APP1 payload in a minimal JPEG so CExifReader can consume it:
// SOI + APP1(length + payload) + EOI. This is the same shape
// JPEGWriter::writeMarker produces, and lets the builder be validated by a
// real parser rather than by its own expectations.
vector<uint8_t> WrapApp1(const vector<uint8_t> &app1)
{
	vector<uint8_t> j = { 0xFF, 0xD8, 0xFF, 0xE1 };
	const uint16_t segLen = (uint16_t)(app1.size() + 2);   // length includes itself
	j.push_back((uint8_t)(segLen >> 8));
	j.push_back((uint8_t)(segLen & 0xFF));
	j.insert(j.end(), app1.begin(), app1.end());
	j.push_back(0xFF); j.push_back(0xD9);
	return j;
}

// Locate a captured raw entry, so tests can assert the TIFF *type* and not
// merely the decoded value.
const CExifTagEntry *FindTag(const CExifData &e, EExifIFD ifd, uint16_t tag)
{
	for (size_t i = 0; i < e.allTags.size(); i++)
	{
		if (e.allTags[i].ifd == ifd && e.allTags[i].tag == tag)
			return &e.allTags[i];
	}
	return NULL;
}

bool HasTag(const vector<uint16_t> &v, uint16_t tag)
{
	for (size_t i = 0; i < v.size(); i++) if (v[i] == tag) return true;
	return false;
}

} // namespace

CTestExifEngine::CTestExifEngine() {}
CTestExifEngine::~CTestExifEngine() {}

void CTestExifEngine::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;
	int stepNum = 1;

	LOGD("CTestExifEngine: Starting CExifBuilder / CExifReader tests");

	// -----------------------------------------------------------------------
	// 1. An empty builder yields an empty payload.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		EX_ASSERT(b.IsEmpty(), "fresh builder is empty");
		EX_ASSERT(b.BuildApp1().empty(), "empty builder -> empty APP1");
	}

	// -----------------------------------------------------------------------
	// 2. Little-endian round-trip of the typed fields A3 retains.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetAscii   (CExifBuilder::Ifd::Primary, 0x013B, "Ansel Adams");   // Artist
		b.SetAscii   (CExifBuilder::Ifd::Primary, 0x8298, "(c) 2026");      // Copyright
		b.SetShort   (CExifBuilder::Ifd::Primary, 0x0112, 1);               // Orientation
		b.SetAscii   (CExifBuilder::Ifd::Exif,    0x9003, "2026:07:20 11:22:33");
		b.SetRational(CExifBuilder::Ifd::Exif,    0x829A, 1, 250);          // ExposureTime
		b.SetRational(CExifBuilder::Ifd::Exif,    0x829D, 28, 10);          // FNumber
		b.SetShort   (CExifBuilder::Ifd::Exif,    0x8827, 200);             // ISO
		b.SetRational(CExifBuilder::Ifd::Exif,    0x920A, 85, 1);           // FocalLength
		b.SetSRational(CExifBuilder::Ifd::Exif,   0x9204, 7, 10);           // ExposureBias

		vector<uint8_t> app1 = b.BuildApp1(true);
		EX_ASSERT(!app1.empty(), "LE builder produced a payload");
		EX_ASSERT(app1.size() > 6 && memcmp(&app1[0], "Exif\0\0", 6) == 0,
		          "payload starts with the Exif identifier plus two NULs");
		EX_ASSERT(app1[6] == 'I' && app1[7] == 'I', "LE payload carries the II byte order mark");

		vector<uint8_t> jpg = WrapApp1(app1);
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.valid, "LE payload parses");
		EX_ASSERT(e.artist == "Ansel Adams", "Artist round-trips");
		EX_ASSERT(e.copyright == "(c) 2026", "Copyright round-trips");
		EX_ASSERT(e.orientation == 1, "Orientation round-trips");
		EX_ASSERT(e.dateTimeOriginal == "2026:07:20 11:22:33", "DateTimeOriginal round-trips");
		EX_ASSERT(fabs(e.exposureTime - 0.004f) < 1e-6f, "ExposureTime round-trips");
		EX_ASSERT(fabs(e.fNumber - 2.8f) < 1e-6f, "FNumber round-trips");
		EX_ASSERT(e.isoSpeed == 200, "ISO round-trips");
		EX_ASSERT(fabs(e.focalLength - 85.f) < 1e-6f, "FocalLength round-trips");
		EX_ASSERT(fabs(e.exposureBias - 0.7f) < 1e-6f, "ExposureBias (SRATIONAL) round-trips");
	}

	// -----------------------------------------------------------------------
	// 3. Big-endian round-trip. The promoted fixture builder only ever emitted
	//    little-endian, so this is the genuinely new serialisation path.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetAscii (CExifBuilder::Ifd::Primary, 0x013B, "Big Endian");
		b.SetShort (CExifBuilder::Ifd::Primary, 0x0112, 1);
		b.SetLong  (CExifBuilder::Ifd::Exif,    0xA002, 8192);        // PixelXDimension
		b.SetRational(CExifBuilder::Ifd::Exif,  0x829D, 40, 10);

		vector<uint8_t> app1 = b.BuildApp1(false);
		EX_ASSERT(app1.size() > 8 && app1[6] == 'M' && app1[7] == 'M',
		          "BE payload carries the MM byte order mark");

		vector<uint8_t> jpg = WrapApp1(app1);
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.valid, "BE payload parses");
		EX_ASSERT(e.artist == "Big Endian", "BE Artist round-trips");
		EX_ASSERT(e.pixelWidth == 8192, "BE LONG round-trips");
		EX_ASSERT(fabs(e.fNumber - 4.0f) < 1e-6f, "BE RATIONAL round-trips");
	}

	// -----------------------------------------------------------------------
	// 4. A value over 4 bytes goes to the data area with a correct offset;
	//    a value of 4 or fewer is packed inline. Both must read back.
	// -----------------------------------------------------------------------
	{
		const string longStr = "A string comfortably longer than four bytes";
		CExifBuilder b;
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, longStr);
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x8298, "abc");   // 4 bytes with NUL: inline
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);

		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.artist == longStr, "long ASCII lands in the data area and reads back");
		EX_ASSERT(e.copyright == "abc", "short ASCII packs inline and reads back");
	}

	// -----------------------------------------------------------------------
	// 5. Sub-IFD pointer tags appear only when that IFD has entries.
	// -----------------------------------------------------------------------
	{
		CExifBuilder onlyPrimary;
		onlyPrimary.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		vector<uint8_t> jpgP = WrapApp1(onlyPrimary.BuildApp1());
		CExifData eP = CExifReader::Read(&jpgP[0], jpgP.size());
		EX_ASSERT(FindTag(eP, EExifIFD::IFD0, 0x8769) == NULL,
		          "no ExifIFD pointer when the Exif IFD is empty");
		EX_ASSERT(FindTag(eP, EExifIFD::IFD0, 0x8825) == NULL,
		          "no GPSIFD pointer when the GPS IFD is empty");

		CExifBuilder withSubs;
		withSubs.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		withSubs.SetShort(CExifBuilder::Ifd::Exif, 0x8827, 100);
		withSubs.SetAscii(CExifBuilder::Ifd::Gps,  0x0001, "N");
		vector<uint8_t> jpgS = WrapApp1(withSubs.BuildApp1());
		CExifData eS = CExifReader::Read(&jpgS[0], jpgS.size());
		EX_ASSERT(eS.isoSpeed == 100, "Exif sub-IFD is reachable via its pointer");
		EX_ASSERT(FindTag(eS, EExifIFD::GPS, 0x0001) != NULL,
		          "GPS sub-IFD is reachable via its pointer");
	}

	// -----------------------------------------------------------------------
	// 6. GPS: count-3 RATIONALs, and the BYTE altitude reference that carries
	//    the sign. The original A3 API could express neither.
	// -----------------------------------------------------------------------
	{
		vector<pair<uint32_t, uint32_t> > lat;   // 52 deg 13' 46.92" N
		lat.push_back(make_pair(52u, 1u));
		lat.push_back(make_pair(13u, 1u));
		lat.push_back(make_pair(4692u, 100u));
		vector<pair<uint32_t, uint32_t> > lon;   // 21 deg 0' 44.03" E
		lon.push_back(make_pair(21u, 1u));
		lon.push_back(make_pair(0u, 1u));
		lon.push_back(make_pair(4403u, 100u));

		CExifBuilder b;
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		b.SetAscii(CExifBuilder::Ifd::Gps, 0x0001, "N");
		b.SetRationalArray(CExifBuilder::Ifd::Gps, 0x0002, lat);
		b.SetAscii(CExifBuilder::Ifd::Gps, 0x0003, "E");
		b.SetRationalArray(CExifBuilder::Ifd::Gps, 0x0004, lon);
		b.SetByte(CExifBuilder::Ifd::Gps, 0x0005, 1);              // below sea level
		b.SetRational(CExifBuilder::Ifd::Gps, 0x0006, 42, 10);     // 4.2 m

		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.hasGps, "GPS present");
		EX_ASSERT(fabs(e.gpsLatitude - 52.2297) < 1e-4, "count-3 RATIONAL latitude round-trips");
		EX_ASSERT(fabs(e.gpsLongitude - 21.01223) < 1e-4, "count-3 RATIONAL longitude round-trips");
		EX_ASSERT(e.gpsAltitudeValid && fabs(e.gpsAltitude - (-4.2)) < 1e-6,
		          "below-sea-level altitude round-trips NEGATIVE");

		// Value-only assertions cannot catch a SetByte implemented as SetShort:
		// the reader consumes this tag with a type-agnostic accessor, so the
		// wrong type still yields the right number. Assert the TIFF type.
		const CExifTagEntry *ref = FindTag(e, EExifIFD::GPS, 0x0005);
		EX_ASSERT(ref != NULL, "GPSAltitudeRef captured");
		EX_ASSERT(ref->type == 1, "GPSAltitudeRef emitted as TIFF type 1 (BYTE), not SHORT");

		const CExifTagEntry *latE = FindTag(e, EExifIFD::GPS, 0x0002);
		EX_ASSERT(latE != NULL && latE->type == 5 && latE->count == 3,
		          "GPSLatitude emitted as RATIONAL count 3");
	}

	// -----------------------------------------------------------------------
	// 7. A UTF-8 Artist survives byte-for-byte. EXIF types this field 7-bit
	//    ASCII, so a strict builder would drop it -- silently destroying the
	//    attribution the field exists to carry, for exactly the users least
	//    likely to notice. Bytes are written as given.
	// -----------------------------------------------------------------------
	{
		// Split literals: a hex escape consumes every following hex digit, so
		// "\xC3\xADa" would parse as 0xADA and fail to compile.
		const string utf8Name = "Jos\xC3\xA9 Garc\xC3\xAD" "a";   // Jose Garcia, accented
		const string utf8Copy = "\xC2\xA9 2026 Jos\xC3\xA9";      // (c) 2026 Jose
		CExifBuilder b;
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, utf8Name);
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x8298, utf8Copy);
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);

		EX_ASSERT(b.Dropped().empty(), "non-ASCII is NOT dropped");

		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.artist == utf8Name, "UTF-8 Artist round-trips byte-for-byte");
		EX_ASSERT(e.copyright == utf8Copy, "UTF-8 Copyright round-trips byte-for-byte");
	}

	// -----------------------------------------------------------------------
	// 8. Dropped(): refused values are absent from the output AND reported.
	//    Asserted in both directions, so a silent half-write fails.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		b.SetRational(CExifBuilder::Ifd::Exif, 0x829A, 1, 0);        // zero denominator
		string withNul = "trunc";
		withNul[2] = '\0';                                          // interior NUL
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, withNul);

		EX_ASSERT(HasTag(b.Dropped(), 0x829A), "zero-denominator rational reported dropped");
		EX_ASSERT(HasTag(b.Dropped(), 0x013B), "ASCII with an interior NUL reported dropped");

		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.valid, "payload with dropped tags still parses");
		EX_ASSERT(e.artist.empty(), "dropped ASCII is absent from the output");
		EX_ASSERT(fabs(e.exposureTime) < 1e-9f, "dropped rational is absent from the output");
		EX_ASSERT(e.orientation == 1, "surviving tags are unaffected by a drop");
	}

	// -----------------------------------------------------------------------
	// 9. A payload past what one APP1 segment can carry yields nothing, rather
	//    than the builder choosing which of the caller's tags to sacrifice.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		const string huge(70000, 'x');
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, huge);
		EX_ASSERT(b.BuildApp1().empty(), "over-cap payload yields an empty vector");
	}

	// -----------------------------------------------------------------------
	// 9b. A tag that fails and is then set successfully must NOT still be
	//     reported dropped -- Dropped() would name a tag that is in the output,
	//     and a caller reporting "these fields were lost" would lie about it.
	//     Symmetrically, one tag failing twice is reported once.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		b.SetRational(CExifBuilder::Ifd::Exif, 0x829A, 1, 0);        // fails
		EX_ASSERT(HasTag(b.Dropped(), 0x829A), "tag reported dropped after the failure");
		b.SetRational(CExifBuilder::Ifd::Exif, 0x829A, 1, 125);      // then succeeds
		EX_ASSERT(!HasTag(b.Dropped(), 0x829A), "retry clears the earlier drop report");

		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(fabs(e.exposureTime - 0.008f) < 1e-6f, "the retried value is what lands");

		CExifBuilder c;
		c.SetRational(CExifBuilder::Ifd::Exif, 0x829D, 1, 0);
		c.SetRational(CExifBuilder::Ifd::Exif, 0x829D, 2, 0);
		int count = 0;
		for (size_t i = 0; i < c.Dropped().size(); i++)
			if (c.Dropped()[i] == 0x829D) count++;
		EX_ASSERT(count == 1, "a tag failing twice is reported once, not twice");
	}

	// -----------------------------------------------------------------------
	// 10. Last write wins for a repeated tag, and the payload stays valid.
	// -----------------------------------------------------------------------
	{
		CExifBuilder b;
		b.SetShort(CExifBuilder::Ifd::Primary, 0x0112, 1);
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, "first");
		b.SetAscii(CExifBuilder::Ifd::Primary, 0x013B, "second");
		vector<uint8_t> jpg = WrapApp1(b.BuildApp1());
		CExifData e = CExifReader::Read(&jpg[0], jpg.size());
		EX_ASSERT(e.artist == "second", "repeated tag: last write wins");
	}

	LOGD("CTestExifEngine: All EXIF builder/reader tests passed");
	// ---------------------------------------------------- InteropIndex (CM-A)
	// The Interop IFD was already followed (the 0xA005 pointer), but typed
	// extraction was gated to IFD0/Exif, so InteropIndex never reached a field.
	// It is the disambiguator for ColorSpace == 0xFFFF ("Uncalibrated"), which
	// is how many Adobe RGB camera JPEGs announce themselves.
	{
		// Little-endian TIFF: IFD0 -> 0x8769 Exif IFD -> 0xA001 ColorSpace 0xFFFF
		// + 0xA005 Interop IFD -> 0x0001 InteropIndex "R03".
		std::vector<uint8_t> t;
		auto u16 = [&](std::vector<uint8_t> &v, uint16_t x){ v.push_back((uint8_t)(x & 0xFF)); v.push_back((uint8_t)(x >> 8)); };
		auto u32 = [&](std::vector<uint8_t> &v, uint32_t x){ for (int i=0;i<4;i++) v.push_back((uint8_t)((x >> (8*i)) & 0xFF)); };
		t.push_back('I'); t.push_back('I'); u16(t, 42); u32(t, 8);
		// IFD0 at 8: one entry (Exif pointer), next=0
		u16(t, 1);
		u16(t, 0x8769); u16(t, 4); u32(t, 1); u32(t, 26);
		u32(t, 0);
		// Exif IFD at 26: two entries, next=0
		u16(t, 2);
		u16(t, 0xA001); u16(t, 3); u32(t, 1); u16(t, 0xFFFF); u16(t, 0);
		u16(t, 0xA005); u16(t, 4); u32(t, 1); u32(t, 56);
		u32(t, 0);
		// Interop IFD at 56: one entry, next=0
		u16(t, 1);
		u16(t, 0x0001); u16(t, 2); u32(t, 4);
		t.push_back('R'); t.push_back('0'); t.push_back('3'); t.push_back(0);
		u32(t, 0);

		CExifData ex = CExifReader::Read(&t[0], t.size());
		EX_ASSERT(ex.interopIndex == "R03", "InteropIndex R03 is hoisted from the Interop IFD");
		EX_ASSERT(ex.colorSpace == 0xFFFF, "ColorSpace 0xFFFF (Uncalibrated) is captured unsigned");
	}

	TestCompleted(true, "All CExifBuilder round-trip and CExifReader tests passed");
}

void CTestExifEngine::Cancel()
{
	isRunning = false;
}
