#include "CTestExifMetadataReach.h"
#include "CExifReader.h"
#include "CImageData.h"
#include "DBG_Log.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
using namespace std;

#define MR_ASSERT(cond, msg) \
	do { \
		bool mrOk = (cond); \
		if (!mrOk) { \
			char buf[256]; \
			snprintf(buf, sizeof(buf), "FAIL: %s", msg); \
			LOGD("CTestExifMetadataReach: %s", buf); \
			TestCompleted(false, buf); \
			return; \
		} \
		StepCompleted(stepNum++, true, msg); \
	} while(0)

namespace {

// ---------------------------------------------------------------------------
// A minimal TIFF writer. Hand-built because CExifBuilder deliberately cannot
// express SubIFDs, IFD1 or MakerNotes (see its header). Kept to what these
// tests need -- it must not grow into a second builder.
// ---------------------------------------------------------------------------

struct Entry
{
	uint16_t        tag  = 0;
	uint16_t        type = 0;
	uint32_t        count = 0;
	vector<uint8_t> bytes;   // already in the target byte order
};

struct Tiff
{
	bool le = true;

	void W16(vector<uint8_t> &v, uint16_t x) const
	{
		if (le) { v.push_back(x & 0xFF); v.push_back((uint8_t)(x >> 8)); }
		else    { v.push_back((uint8_t)(x >> 8)); v.push_back(x & 0xFF); }
	}
	void W32(vector<uint8_t> &v, uint32_t x) const
	{
		if (le) { v.push_back(x & 0xFF); v.push_back((x >> 8) & 0xFF);
		          v.push_back((x >> 16) & 0xFF); v.push_back((uint8_t)(x >> 24)); }
		else    { v.push_back((uint8_t)(x >> 24)); v.push_back((x >> 16) & 0xFF);
		          v.push_back((x >> 8) & 0xFF); v.push_back(x & 0xFF); }
	}
	vector<uint8_t> S(uint16_t x) const { vector<uint8_t> v; W16(v, x); return v; }
	vector<uint8_t> L(uint32_t x) const { vector<uint8_t> v; W32(v, x); return v; }
	vector<uint8_t> LA(const vector<uint32_t> &xs) const
	{ vector<uint8_t> v; for (uint32_t x : xs) W32(v, x); return v; }

	Entry E(uint16_t tag, uint16_t type, uint32_t count, vector<uint8_t> b) const
	{ Entry e; e.tag = tag; e.type = type; e.count = count; e.bytes = std::move(b); return e; }

	vector<uint8_t> Header() const
	{
		vector<uint8_t> v;
		if (le) { v.push_back('I'); v.push_back('I'); } else { v.push_back('M'); v.push_back('M'); }
		W16(v, 0x002A);
		W32(v, 8);
		return v;
	}

	// Appends a directory (entries must be tag-ascending) and returns its offset.
	// `nextIfd` is written verbatim, so a self-referential value builds the
	// cycle fixture directly.
	uint32_t AppendIfd(vector<uint8_t> &buf, const vector<Entry> &entries, uint32_t nextIfd) const
	{
		const uint32_t ifdOff = (uint32_t)buf.size();
		const uint32_t heapAt = ifdOff + 2 + (uint32_t)entries.size() * 12 + 4;
		vector<uint8_t> dir, heap;
		W16(dir, (uint16_t)entries.size());
		for (const Entry &e : entries)
		{
			W16(dir, e.tag); W16(dir, e.type); W32(dir, e.count);
			if (e.bytes.size() <= 4)
				for (size_t i = 0; i < 4; ++i) dir.push_back(i < e.bytes.size() ? e.bytes[i] : 0);
			else { W32(dir, heapAt + (uint32_t)heap.size()); heap.insert(heap.end(), e.bytes.begin(), e.bytes.end()); }
		}
		W32(dir, nextIfd);
		buf.insert(buf.end(), dir.begin(), dir.end());
		buf.insert(buf.end(), heap.begin(), heap.end());
		return ifdOff;
	}

	void PatchIfd0(vector<uint8_t> &buf, uint32_t off) const
	{ vector<uint8_t> v; W32(v, off); memcpy(&buf[4], v.data(), 4); }

	void Patch32(vector<uint8_t> &buf, uint32_t at, uint32_t x) const
	{ vector<uint8_t> v; W32(v, x); memcpy(&buf[at], v.data(), 4); }
};

const CExifTagEntry *Find(const CExifData &e, EExifIFD ifd, uint16_t tag)
{
	for (const CExifTagEntry &t : e.allTags)
		if (t.ifd == ifd && t.tag == tag) return &t;
	return nullptr;
}

size_t CountIn(const CExifData &e, EExifIFD ifd)
{
	size_t n = 0;
	for (const CExifTagEntry &t : e.allTags) if (t.ifd == ifd) ++n;
	return n;
}

// A file whose IFD0 points at one SubIFD carrying dimensions + PreviewColorSpace.
// `ptrType` selects the encoding under test: 4 (LONG) or 13 (IFD).
vector<uint8_t> BuildSubIfdFile(uint16_t ptrType, uint16_t previewCs,
                                uint32_t w, uint32_t h)
{
	Tiff t;
	vector<uint8_t> buf = t.Header();
	// IFD0 first, with a placeholder pointer patched once the SubIFD is placed.
	vector<Entry> ifd0 = {
		t.E(0x010F, 2, 6, { 'F','a','k','e','C','o' }),
		t.E(0x014A, ptrType, 1, t.L(0)),
	};
	uint32_t ifd0Off = t.AppendIfd(buf, ifd0, 0);
	t.PatchIfd0(buf, ifd0Off);

	uint32_t subOff = t.AppendIfd(buf, {
		t.E(0x0100, 4, 1, t.L(w)),
		t.E(0x0101, 4, 1, t.L(h)),
		t.E(0xC71A, 3, 1, t.S(previewCs)),
	}, 0);

	// The 0x014A value sits inline at entry+8; entry 1 of IFD0.
	t.Patch32(buf, ifd0Off + 2 + 12 * 1 + 8, subOff);
	return buf;
}

} // namespace

// ---------------------------------------------------------------------------

void CTestExifWalk::Run(ITestCallback *callback)
{
	isRunning = true;
	this->callback = callback;
	int stepNum = 0;

	// --- SubIFD reached, and BOTH pointer encodings agree -------------------
	// Type 13 (IFD) is the one a DNG may use and the reader rejected outright
	// before CM-C1. A test written only against LONG proves nothing about it:
	// the failure is silent, the tag simply vanishes.
	{
		vector<uint8_t> asLong = BuildSubIfdFile(4,  3, 1024, 680);
		vector<uint8_t> asIfd  = BuildSubIfdFile(13, 3, 1024, 680);

		CExifData a = CExifReader::Read(asLong.data(), asLong.size(), true, false, true);
		CExifData b = CExifReader::Read(asIfd.data(),  asIfd.size(),  true, false, true);

		MR_ASSERT(a.valid && b.valid, "SubIFD fixtures parse");
		MR_ASSERT(a.dngPreviewCandidates.size() == 1, "LONG-encoded SubIFD yields one candidate");
		MR_ASSERT(b.dngPreviewCandidates.size() == 1, "IFD-encoded (type 13) SubIFD yields one candidate");
		MR_ASSERT(a.dngPreviewCandidates[0].colorSpace == 3 &&
		          b.dngPreviewCandidates[0].colorSpace == 3,
		          "both encodings report PreviewColorSpace = Adobe RGB");
		MR_ASSERT(a.dngPreviewCandidates[0].width  == 1024 &&
		          a.dngPreviewCandidates[0].height == 680,
		          "candidate carries the SubIFD's declared dimensions");
		MR_ASSERT(b.dngPreviewCandidates[0].width  == 1024 &&
		          b.dngPreviewCandidates[0].height == 680,
		          "type 13 resolves to the same SubIFD, not to offset 0");
		MR_ASSERT(Find(a, EExifIFD::SubIFD, 0x0100) != nullptr,
		          "SubIFD tags are grouped under EExifIFD::SubIFD");
	}

	// --- the flag gates it --------------------------------------------------
	{
		vector<uint8_t> f = BuildSubIfdFile(4, 3, 1024, 680);
		CExifData off = CExifReader::Read(f.data(), f.size(), true, false, false);
		MR_ASSERT(off.valid, "fixture still parses with the flag off");
		MR_ASSERT(off.dngPreviewCandidates.empty(), "no DNG candidates with the flag off");
		MR_ASSERT(CountIn(off, EExifIFD::SubIFD) == 0, "no SubIFD descent with the flag off");
		// 0x014A must still be LISTED when we are not descending it, or a row
		// the panel shows today silently disappears.
		MR_ASSERT(Find(off, EExifIFD::IFD0, 0x014A) != nullptr,
		          "0x014A still appears as an ordinary tag with the flag off");
	}

	// --- IFD1 must not overwrite IFD0's typed fields ------------------------
	// The reason IFD1 is its own EExifIFD. Typed extraction is gated to
	// IFD0/Exif and assigns unconditionally, and a thumbnail directory
	// legitimately repeats Orientation/Make/DateTimeOriginal.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t ifd0Off = t.AppendIfd(buf, {
			t.E(0x010F, 2, 5, { 'R','e','a','l', 0 }),
			t.E(0x0112, 3, 1, t.S(1)),
		}, 0);
		t.PatchIfd0(buf, ifd0Off);
		uint32_t ifd1Off = t.AppendIfd(buf, {
			t.E(0x010F, 2, 6, { 'T','h','u','m','b', 0 }),
			t.E(0x0112, 3, 1, t.S(6)),
		}, 0);
		// IFD0's next-IFD link is the last 4 bytes of its directory.
		t.Patch32(buf, ifd0Off + 2 + 12 * 2, ifd1Off);

		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "IFD1 chain fixture parses");
		MR_ASSERT(CountIn(e, EExifIFD::IFD1) == 2, "IFD1 entries captured under their own kind");
		MR_ASSERT(e.orientation == 1, "IFD1's Orientation does NOT overwrite IFD0's");
		MR_ASSERT(e.make == "Real", "IFD1's Make does NOT overwrite IFD0's");
	}

	// --- a pointer tag in IFD1/SubIFD must not re-enter typed extraction ----
	// 0x8769/0x8825 are honoured only from IFD0 (and 0xA005 only from Exif).
	// Honouring them from a thumbnail directory would descend into a walk typed
	// as Exif/GPS, where extraction assigns unconditionally -- the same
	// overwrite the IFD1 design exists to stop, through a side door.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t ifd0Off = t.AppendIfd(buf, {
			t.E(0x9003, 2, 20, { 'R','e','a','l',':','d','a','t','e' }),
		}, 0);
		// pad the ASCII value to its declared count
		t.PatchIfd0(buf, ifd0Off);
		uint32_t baitOff = t.AppendIfd(buf, {
			t.E(0x9003, 2, 9, { 'B','A','D','D','A','T','E', 0 }),
		}, 0);
		uint32_t ifd1Off = t.AppendIfd(buf, {
			t.E(0x8769, 4, 1, t.L(baitOff)),   // Exif pointer FROM IFD1: non-conforming
		}, 0);
		t.Patch32(buf, ifd0Off + 2 + 12 * 1, ifd1Off);   // IFD0 -> IFD1 chain

		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "IFD1-with-Exif-pointer fixture parses");
		MR_ASSERT(e.dateTimeOriginal.find("BADDATE") == string::npos,
		          "an Exif pointer in IFD1 cannot overwrite typed fields");
	}

	// --- chains that must terminate rather than hang ------------------------
	// ParseIFD's guards are returns inside a void function, so the chain loop
	// owns termination. If it does not, these hang instead of failing.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		t.Patch32(buf, a + 2 + 12 * 1, a);          // A -> A
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "self-referential IFD chain terminates");
	}
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		uint32_t b = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, a);   // B -> A
		t.Patch32(buf, a + 2 + 12 * 1, b);                                  // A -> B
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "A->B->A chain terminates");
	}

	// --- chain bounds -------------------------------------------------------
	// Termination is not the only failure mode: the link is located from the
	// DECLARED entry count, which a truncated buffer does not contain.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		t.Patch32(buf, a + 2 + 12 * 1, 0x7FFFFF00);      // link far past the buffer
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid && e.orientation == 1, "a link past the buffer is ignored");
	}
	{
		// Directory truncated so the 4-byte link does not fit at all.
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		buf.resize(buf.size() - 4);
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "a directory with no room for a link is handled");
	}
	{
		// IFD0 offset itself past the end.
		Tiff t;
		vector<uint8_t> buf = t.Header();
		t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, 0x7FFFFF00);
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.orientation == 1, "an out-of-range IFD0 offset does not crash or corrupt");
	}

	// --- several SubIFDs, and the order candidates come back in --------------
	// A 0x014A array with more than one element was never exercised otherwise,
	// and the host's fallback rule reads this vector's order.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t ifd0Off = t.AppendIfd(buf, {
			t.E(0x014A, 4, 3, t.LA({ 0, 0, 0 })),
		}, 0);
		t.PatchIfd0(buf, ifd0Off);
		uint32_t s0 = t.AppendIfd(buf, { t.E(0x0100, 4, 1, t.L(160)),
		                                 t.E(0x0101, 4, 1, t.L(120)),
		                                 t.E(0xC71A, 3, 1, t.S(2)) }, 0);
		uint32_t s1 = t.AppendIfd(buf, { t.E(0x0100, 4, 1, t.L(1024)),
		                                 t.E(0x0101, 4, 1, t.L(680)),
		                                 t.E(0xC71A, 3, 1, t.S(3)) }, 0);
		uint32_t s2 = t.AppendIfd(buf, { t.E(0x0100, 4, 1, t.L(6000)),
		                                 t.E(0x0101, 4, 1, t.L(4000)),
		                                 t.E(0xC71A, 3, 1, t.S(4)) }, 0);
		// The array is 12 bytes, so it lives out of line, at the heap offset
		// the writer placed right after the directory.
		const uint32_t arrAt = ifd0Off + 2 + 12 * 1 + 4;
		t.Patch32(buf, arrAt + 0, s0);
		t.Patch32(buf, arrAt + 4, s1);
		t.Patch32(buf, arrAt + 8, s2);

		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.dngPreviewCandidates.size() == 3,
		          "every element of a multi-entry 0x014A array is walked");
		MR_ASSERT(e.dngPreviewCandidates[0].colorSpace == 2 &&
		          e.dngPreviewCandidates[1].colorSpace == 3 &&
		          e.dngPreviewCandidates[2].colorSpace == 4,
		          "sibling SubIFDs report their candidates in array order");
		MR_ASSERT(e.dngPreviewCandidates[1].width == 1024 &&
		          e.dngPreviewCandidates[1].height == 680,
		          "each candidate carries its OWN directory's dimensions");
	}

	// --- the entry budget actually stops a runaway --------------------------
	// The backstop the whole walk depends on. It has to be crossed to be
	// tested: allTags saturates at a LOWER cap, so counting rows proves
	// nothing about the entry counter.
	//
	// Charged-but-not-captured entries make the fixture small: an invalid TIFF
	// type is rejected by EntryData AFTER the counter has been charged, so 512
	// of them per directory cost 512 charges and no memory.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t ifd0Off = t.AppendIfd(buf, { t.E(0x014A, 4, 16, t.LA(vector<uint32_t>(16, 0))) }, 0);
		t.PatchIfd0(buf, ifd0Off);
		const uint32_t arrAt = ifd0Off + 2 + 12 * 1 + 4;

		vector<Entry> filler;
		for (int i = 0; i < 512; ++i) filler.push_back(t.E(0x1000, 250, 1, t.L(0)));
		// 16 x 512 = 8192 charged entries, i.e. exactly kMaxEntriesPerFile.
		for (int k = 0; k < 16; ++k)
		{
			uint32_t off = t.AppendIfd(buf, filler, 0);
			t.Patch32(buf, arrAt + 4 * k, off);
		}
		// A marker in a directory reached only AFTER the budget is spent.
		uint32_t markerIfd = t.AppendIfd(buf, { t.E(0xF000, 3, 1, t.S(7)) }, 0);
		t.Patch32(buf, ifd0Off + 2 + 12 * 1, markerIfd);   // IFD0's next-IFD link

		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(e.valid, "a budget-exhausting file still parses and terminates");
		MR_ASSERT(Find(e, EExifIFD::IFD1, 0xF000) == nullptr,
		          "work past the shared entry budget is refused");
	}
	{
		// Control: the same marker, reached under the budget.
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		uint32_t markerIfd = t.AppendIfd(buf, { t.E(0xF000, 3, 1, t.S(7)) }, 0);
		t.Patch32(buf, a + 2 + 12 * 1, markerIfd);
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, true);
		MR_ASSERT(Find(e, EExifIFD::IFD1, 0xF000) != nullptr,
		          "and the same marker IS reached when the budget allows it");
	}

	// --- the IFD1 chain is inert with the flag off --------------------------
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, { t.E(0x0112, 3, 1, t.S(1)) }, 0);
		t.PatchIfd0(buf, a);
		uint32_t b = t.AppendIfd(buf, { t.E(0xF000, 3, 1, t.S(7)) }, 0);
		t.Patch32(buf, a + 2 + 12 * 1, b);
		CExifData off = CExifReader::Read(buf.data(), buf.size(), true, false, false);
		MR_ASSERT(CountIn(off, EExifIFD::IFD1) == 0, "no IFD1 walk with the flag off");
	}

	TestCompleted(true, "SubIFD, type 13, IFD1 chain, bounds and budget all behave");
}

// ---------------------------------------------------------------------------

void CTestExifLeanPath::Run(ITestCallback *callback)
{
	isRunning = true;
	this->callback = callback;
	int stepNum = 0;

	// The folder scan reads every file in a directory on the scan thread. With
	// the flag off, this phase must be invisible there.
	//
	// Note allTags cannot be the observable for the real ReadFileHeader path --
	// it hard-codes captureAllTags = false, so allTags is empty either way and
	// asserting on it would assert nothing. The typed fields are what that path
	// actually produces, and what a regression would corrupt.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		vector<Entry> ifd0 = {
			t.E(0x010F, 2, 5, { 'R','e','a','l', 0 }),
			t.E(0x0112, 3, 1, t.S(3)),
			t.E(0x014A, 4, 1, t.L(0)),
		};
		uint32_t ifd0Off = t.AppendIfd(buf, ifd0, 0);
		t.PatchIfd0(buf, ifd0Off);
		uint32_t subOff = t.AppendIfd(buf, {
			t.E(0x0100, 4, 1, t.L(64)),
			t.E(0x0112, 3, 1, t.S(8)),          // would corrupt orientation if descended blindly
			t.E(0xC71A, 3, 1, t.S(2)),
		}, 0);
		t.Patch32(buf, ifd0Off + 2 + 12 * 2 + 8, subOff);

		CExifData off = CExifReader::Read(buf.data(), buf.size(), true, false, false);
		CExifData on  = CExifReader::Read(buf.data(), buf.size(), true, false, true);

		MR_ASSERT(off.orientation == 3, "typed fields unchanged with the flag off");
		MR_ASSERT(off.make == "Real",   "Make unchanged with the flag off");
		MR_ASSERT(off.dngPreviewCandidates.empty(), "no candidates with the flag off");
		MR_ASSERT(off.makerColorSpaceSource == EExifColorHintSource::None,
		          "no colour hint with the flag off");
		// And with it on, the SubIFD's Orientation still must not win.
		MR_ASSERT(on.orientation == 3, "a SubIFD's Orientation does not overwrite IFD0's");
		MR_ASSERT(on.dngPreviewCandidates.size() == 1, "the same file yields a candidate with the flag on");
	}

	// A type-13 entry is DELIBERATELY admitted regardless of the flag: it is a
	// correction to an existing path, not part of the gated reach. So it is the
	// one documented way the flag-off result differs from pre-CM-C1.
	{
		Tiff t;
		vector<uint8_t> buf = t.Header();
		uint32_t a = t.AppendIfd(buf, {
			t.E(0x0112, 3, 1, t.S(1)),
			t.E(0x8888, 13, 1, t.L(12345)),
		}, 0);
		t.PatchIfd0(buf, a);
		CExifData e = CExifReader::Read(buf.data(), buf.size(), true, false, false);
		MR_ASSERT(Find(e, EExifIFD::IFD0, 0x8888) != nullptr,
		          "a type-13 entry is admitted even with the flag off");
	}

	TestCompleted(true, "the lean scan path is unaffected by CM-C1");
}

// ---------------------------------------------------------------------------

void CTestExifPreviewHintLifecycle::Run(ITestCallback *callback)
{
	isRunning = true;
	this->callback = callback;
	int stepNum = 0;

	// The hint travels with the pixels it describes and must never outlive
	// them. iccProfile learned this the hard way -- hence the unconditional
	// reset at the top of CImageData::Load().
	//
	// allocResult = true throughout: the 3-argument constructor leaves
	// resultData NULL, and CopyDataFrom memcpys from it.
	{
		CImageData src(4, 4, IMG_TYPE_RGBA, false, true);
		src.previewColorHint       = EExifColorSpaceHint::AdobeRgb;
		src.previewColorHintSource = EExifColorHintSource::PreviewExif;

		CImageData copy(&src);
		MR_ASSERT(copy.previewColorHint == EExifColorSpaceHint::AdobeRgb &&
		          copy.previewColorHintSource == EExifColorHintSource::PreviewExif,
		          "the copy constructor carries the preview hint");
	}

	// Copying pixels FROM a source with no hint must ERASE the destination's,
	// not leave it standing -- the clearing `else` iccProfile also needs.
	{
		CImageData src(4, 4, IMG_TYPE_RGBA, false, true);
		CImageData dst(4, 4, IMG_TYPE_RGBA, false, true);
		dst.previewColorHint       = EExifColorSpaceHint::AdobeRgb;
		dst.previewColorHintSource = EExifColorHintSource::PreviewExif;

		dst.CopyDataFrom(&src);
		MR_ASSERT(dst.previewColorHint == EExifColorSpaceHint::Unknown &&
		          dst.previewColorHintSource == EExifColorHintSource::None,
		          "copying from an unhinted source CLEARS a stale hint");
	}

	// And Load() resets it BEFORE dispatching to a loader, so a file that sets
	// no hint cannot inherit the previous file's. This is the .NEF-after-.DNG
	// case, reproduced with an ordinary image: a PNG sets no preview hint, so
	// the only way the field can end up clear is the reset itself.
	{
		CImageData img;
		img.previewColorHint       = EExifColorSpaceHint::AdobeRgb;
		img.previewColorHintSource = EExifColorHintSource::PreviewExif;

		img.Load("tests/fixtures/bench_1000/img_00440.png", false);
		MR_ASSERT(img.previewColorHint == EExifColorSpaceHint::Unknown &&
		          img.previewColorHintSource == EExifColorHintSource::None,
		          "Load() resets the hint before dispatching to a loader");
	}

	TestCompleted(true, "the preview hint follows iccProfile's lifecycle at all three sites");
}

// ---------------------------------------------------------------------------

namespace {

// A JPEG carrying an Exif APP1 whose Exif IFD holds a MakerNote blob.
vector<uint8_t> BuildMakerNoteJpeg(const Tiff &t, const string &make,
                                   const vector<uint8_t> &blob)
{
	vector<uint8_t> tiff = t.Header();
	vector<Entry> ifd0 = {
		t.E(0x010F, 2, (uint32_t)make.size() + 1,
		    vector<uint8_t>(make.begin(), make.end())),
		t.E(0x8769, 4, 1, t.L(0)),
	};
	// ASCII values need their NUL; the writer above copies only the characters.
	ifd0[0].bytes.push_back(0);
	uint32_t ifd0Off = t.AppendIfd(tiff, ifd0, 0);
	t.PatchIfd0(tiff, ifd0Off);

	uint32_t exifOff = t.AppendIfd(tiff, {
		t.E(0x927C, 7, (uint32_t)blob.size(), blob),
	}, 0);
	t.Patch32(tiff, ifd0Off + 2 + 12 * 1 + 8, exifOff);

	vector<uint8_t> app1 = { 'E','x','i','f', 0, 0 };
	app1.insert(app1.end(), tiff.begin(), tiff.end());

	vector<uint8_t> j = { 0xFF, 0xD8, 0xFF, 0xE1 };
	const uint16_t segLen = (uint16_t)(app1.size() + 2);
	j.push_back((uint8_t)(segLen >> 8)); j.push_back((uint8_t)(segLen & 0xFF));
	j.insert(j.end(), app1.begin(), app1.end());
	j.push_back(0xFF); j.push_back(0xD9);
	return j;
}

// A bare IFD, for the vendors whose blob is just a directory.
vector<uint8_t> BareIfd(const Tiff &t, const vector<Entry> &entries, uint32_t prefixLen,
                        const uint8_t *prefix)
{
	vector<uint8_t> blob(prefix, prefix + prefixLen);
	// Offsets inside a self-relative blob are measured from the blob start, so
	// the directory has to be placed at a known offset within it.
	Tiff w = t;
	vector<uint8_t> body;
	w.AppendIfd(body, entries, 0);
	blob.insert(blob.end(), body.begin(), body.end());
	return blob;
}

} // namespace

void CTestExifMakerNote::Run(ITestCallback *callback)
{
	isRunning = true;
	this->callback = callback;
	int stepNum = 0;

	Tiff t;   // little-endian enclosing file throughout

	// --- Canon: no signature at all, selected by Make; file-relative offsets --
	// Canon is the reason signature matching cannot be the only rule.
	{
		// The blob IS an IFD living in the enclosing TIFF, so it must be built
		// into the file at a known offset. Easiest: give the Exif IFD a
		// MakerNote whose bytes are a directory using file-relative offsets --
		// with all values inline, no offsets are needed at all.
		vector<uint8_t> dir;
		t.AppendIfd(dir, { t.E(0x00B4, 3, 1, t.S(2)) }, 0);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Canon", dir);

		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.valid, "Canon fixture parses");
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "Canon MakerNote is detected by Make with no signature");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::AdobeRgb,
		          "Canon 0x00B4 = 2 maps to Adobe RGB");
		MR_ASSERT(e.makerColorSpaceVendor == "Canon", "vendor recorded for the readout");
	}

	// --- Canon's 65535 is ABSENCE, not a colour space -----------------------
	{
		vector<uint8_t> dir;
		t.AppendIfd(dir, { t.E(0x00B4, 3, 1, t.S(65535)) }, 0);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Canon", dir);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Unknown &&
		          e.makerColorSpaceSource == EExifColorHintSource::None,
		          "Canon 65535 (n/a) reports no signal, so a host chain falls through");
	}

	// --- Nikon: self-relative, embedded TIFF header, OPPOSITE byte order -----
	// A decoder that inherits the enclosing file's endianness produces garbage
	// tag IDs here. This is the case that catches it.
	{
		Tiff be; be.le = false;                 // embedded header is big-endian
		vector<uint8_t> inner = be.Header();    // "MM", magic, IFD0 at 8
		be.AppendIfd(inner, { be.E(0x001E, 3, 1, be.S(2)) }, 0);

		vector<uint8_t> blob = { 'N','i','k','o','n', 0, 2, 0x10, 0, 0 };  // sig + version + pad
		blob.insert(blob.end(), inner.begin(), inner.end());

		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "NIKON CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "Nikon MakerNote decoded through its embedded TIFF header");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::AdobeRgb,
		          "byte order is read from the embedded header, not inherited");
		MR_ASSERT(e.makerColorSpaceVendor == "Nikon", "Nikon recorded as the vendor");
	}

	// --- Nikon 4 = BT.2100: a NAMED space we cannot honour, not Unknown ------
	{
		Tiff be; be.le = false;
		vector<uint8_t> inner = be.Header();
		be.AppendIfd(inner, { be.E(0x001E, 3, 1, be.S(4)) }, 0);
		vector<uint8_t> blob = { 'N','i','k','o','n', 0, 2, 0x10, 0, 0 };
		blob.insert(blob.end(), inner.begin(), inner.end());
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "NIKON CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Bt2100,
		          "Nikon 4 reports BT.2100, distinguishable from 'nothing said'");
	}

	// --- Pentax: 0-BASED, where Nikon and Canon are 1-based -----------------
	// The file that catches a shared decode: value 1 must be Adobe RGB here and
	// sRGB for Nikon/Canon.
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x0037, 3, 1, t.S(1)) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX Corporation", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "Pentax MakerNote decoded");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::AdobeRgb,
		          "Pentax 0x0037 = 1 is Adobe RGB (0-based), not sRGB");
	}
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x0037, 3, 1, t.S(0)) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX Corporation", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Srgb,
		          "Pentax 0x0037 = 0 is sRGB -- the negative control for the base");
	}

	// --- OLD Olympus ("OLYMP\0"): FILE-relative, and the hoist must survive --
	// The old container is a bare directory behind a signature with NO base
	// override, so its 0x2020 pointer is relative to the enclosing TIFF header,
	// not the blob. Misclassifying it as self-relative silently killed the
	// colour hoist for the whole E-system generation -- or worse, read 0x0507
	// out of whatever bytes the wrong base happened to land on.
	{
		Tiff w;
		vector<uint8_t> body;
		uint32_t topOff = w.AppendIfd(body, { w.E(0x2020, 4, 1, w.L(0)) }, 0);
		uint32_t subOff = w.AppendIfd(body, { w.E(0x0507, 3, 1, w.S(1)) }, 0);
		vector<uint8_t> blob = { 'O','L','Y','M','P', 0, 0, 0 };
		blob.insert(blob.end(), body.begin(), body.end());

		// Assembled by hand so the blob's TIFF-space offset is known: the
		// sub-IFD pointer below must be FILE-relative.
		Tiff t2;
		vector<uint8_t> tiff = t2.Header();
		const string make = "OLYMPUS IMAGING CORP.";
		vector<Entry> ifd0 = {
			t2.E(0x010F, 2, (uint32_t)make.size() + 1,
			     vector<uint8_t>(make.begin(), make.end())),
			t2.E(0x8769, 4, 1, t2.L(0)),
		};
		ifd0[0].bytes.push_back(0);
		uint32_t ifd0Off = t2.AppendIfd(tiff, ifd0, 0);
		t2.PatchIfd0(tiff, ifd0Off);
		uint32_t exifOff = (uint32_t)tiff.size();
		// The blob is >4 bytes, so it lands in the Exif IFD's heap, which the
		// builder places immediately after the 1-entry directory: +18.
		const uint32_t blobAt = exifOff + 18;
		// Entry 0's value slot inside the blob: sig(6)+pad(2) + dir count(2) + 8.
		w.Patch32(blob, 8 + topOff + 2 + 8, blobAt + 8 + subOff);
		t2.AppendIfd(tiff, { t2.E(0x927C, 7, (uint32_t)blob.size(), blob) }, 0);
		t2.Patch32(tiff, ifd0Off + 2 + 12 * 1 + 8, exifOff);

		vector<uint8_t> app1 = { 'E','x','i','f', 0, 0 };
		app1.insert(app1.end(), tiff.begin(), tiff.end());
		vector<uint8_t> jpeg = { 0xFF, 0xD8, 0xFF, 0xE1 };
		const uint16_t segLen = (uint16_t)(app1.size() + 2);
		jpeg.push_back((uint8_t)(segLen >> 8)); jpeg.push_back((uint8_t)(segLen & 0xFF));
		jpeg.insert(jpeg.end(), app1.begin(), app1.end());
		jpeg.push_back(0xFF); jpeg.push_back(0xD9);

		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "old-Olympus (OLYMP) hoists through a FILE-relative sub-IFD pointer");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::AdobeRgb,
		          "and 0x0507 = 1 still reads as Adobe RGB there");
	}

	// --- a colour tag with a corrupted TYPE must hoist nothing --------------
	// EntryUInt's default returns 0 for ASCII/RATIONAL, and for the 0-based
	// vendors a manufactured 0 is a confident sRGB claim. Absence is the
	// correct answer.
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x0037, 2, 2, { '1', 0 }) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::None,
		          "an ASCII-typed colour tag hoists nothing, not sRGB");
	}

	// --- vendors that record no colour tag at all ---------------------------
	// Decoded for the panel, hoisting nothing. A result, not a gap.
	{
		const uint8_t sig[] = { 'F','U','J','I','F','I','L','M', 12, 0, 0, 0 };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x1210, 3, 1, t.S(1)) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "FUJIFILM", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::None,
		          "Fujifilm hoists no colour hint");
		MR_ASSERT(CountIn(e, EExifIFD::MakerNote) >= 1,
		          "Fujifilm MakerNote tags are still decoded for the panel");
	}

	// --- an unrecognised vendor is left exactly as it was -------------------
	{
		vector<uint8_t> blob = { 'W','E','I','R','D','C','A','M', 0, 1, 2, 3, 4, 5, 6, 7 };
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Unknown Brand", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.valid, "unknown-vendor fixture parses");
		MR_ASSERT(CountIn(e, EExifIFD::MakerNote) == 0, "no MakerNote rows for an unknown vendor");
		MR_ASSERT(Find(e, EExifIFD::Exif, 0x927C) != nullptr,
		          "the opaque MakerNote row survives, as before CM-C1");
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::None,
		          "and nothing is guessed from unknown bytes");
	}

	// --- malformed blobs: no hang, no over-read, container still readable ----
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		// A directory claiming far more entries than the blob can hold.
		vector<uint8_t> blob(sig, sig + sizeof(sig));
		blob.push_back(0xFF); blob.push_back(0xFF);      // count = 65535
		for (int i = 0; i < 8; ++i) blob.push_back(0xAA);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.valid, "a truncated MakerNote does not abandon the whole parse");
		MR_ASSERT(e.make == "PENTAX", "the container's own tags are still readable");
	}

	// --- Olympus: the colour tag lives one level down, in CameraSettings -----
	// The only two-level descent in the phase, and the only 0-based mapping
	// with a third value.
	{
		const uint8_t sig[] = { 'O','L','Y','M','P','U','S', 0, 'I','I', 3, 0 };
		// The sub-directory sits after the top-level one; both are inside the
		// blob, so its pointer is a blob-relative offset.
		Tiff w = t;
		vector<uint8_t> body;
		uint32_t topOff = w.AppendIfd(body, { w.E(0x2020, 4, 1, w.L(0)) }, 0);
		uint32_t subOff = w.AppendIfd(body, { w.E(0x0507, 3, 1, w.S(1)) }, 0);
		// Offsets inside the blob include the signature prefix.
		w.Patch32(body, topOff + 2 + 12 * 0 + 8, subOff + (uint32_t)sizeof(sig));
		vector<uint8_t> blob(sig, sig + sizeof(sig));
		blob.insert(blob.end(), body.begin(), body.end());

		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "OLYMPUS CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "Olympus colour is found through the CameraSettings sub-directory");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::AdobeRgb,
		          "Olympus 0x0507 = 1 is Adobe RGB (0-based like Pentax)");
		MR_ASSERT(e.makerColorSpaceVendor == "Olympus", "Olympus recorded as the vendor");
	}
	{
		// Value 2 is ProPhoto -- a named space we cannot honour, which must be
		// reported as itself rather than as "nothing said".
		const uint8_t sig[] = { 'O','L','Y','M','P','U','S', 0, 'I','I', 3, 0 };
		Tiff w = t;
		vector<uint8_t> body;
		uint32_t topOff = w.AppendIfd(body, { w.E(0x2020, 4, 1, w.L(0)) }, 0);
		uint32_t subOff = w.AppendIfd(body, { w.E(0x0507, 3, 1, w.S(2)) }, 0);
		w.Patch32(body, topOff + 2 + 8, subOff + (uint32_t)sizeof(sig));
		vector<uint8_t> blob(sig, sig + sizeof(sig));
		blob.insert(blob.end(), body.begin(), body.end());
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "OLYMPUS CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::ProPhotoRgb,
		          "Olympus 0x0507 = 2 is ProPhoto RGB");
	}
	{
		// Negative control for the descent: 0x0507 at the TOP level is not the
		// Olympus colour tag and must not be hoisted.
		const uint8_t sig[] = { 'O','L','Y','M','P','U','S', 0, 'I','I', 3, 0 };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x0507, 3, 1, t.S(1)) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "OLYMPUS CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::None,
		          "a top-level 0x0507 is NOT the Olympus colour tag");
	}

	// --- the 1-based vendors' sRGB value ------------------------------------
	// The other half of the base check: 1 must mean sRGB for Nikon and Canon
	// where it means Adobe RGB for Pentax and Olympus.
	{
		vector<uint8_t> dir;
		t.AppendIfd(dir, { t.E(0x00B4, 3, 1, t.S(1)) }, 0);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Canon", dir);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Srgb,
		          "Canon 0x00B4 = 1 is sRGB (1-based), where Pentax 1 is Adobe RGB");
	}
	{
		Tiff be; be.le = false;
		vector<uint8_t> inner = be.Header();
		be.AppendIfd(inner, { be.E(0x001E, 3, 1, be.S(1)) }, 0);
		vector<uint8_t> blob = { 'N','i','k','o','n', 0, 2, 0x10, 0, 0 };
		blob.insert(blob.end(), inner.begin(), inner.end());
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "NIKON CORPORATION", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Srgb, "Nikon 0x001E = 1 is sRGB");
	}

	// --- Panasonic also hoists nothing --------------------------------------
	{
		const uint8_t sig[] = { 'P','a','n','a','s','o','n','i','c', 0, 0, 0 };
		vector<uint8_t> blob = BareIfd(t, { t.E(0x0032, 3, 1, t.S(1)) }, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Panasonic", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.makerColorSpaceSource == EExifColorHintSource::None,
		          "Panasonic hoists no colour hint");
		MR_ASSERT(CountIn(e, EExifIFD::MakerNote) >= 1,
		          "Panasonic MakerNote tags are still decoded for the panel");
	}

	// --- MakerNote decoding is gated by the flag ----------------------------
	// The guarantee that keeps the folder scan free. Asserted on a fixture that
	// actually CONTAINS a MakerNote -- on one that does not, "no hint" holds
	// either way and proves nothing.
	{
		vector<uint8_t> dir;
		t.AppendIfd(dir, { t.E(0x00B4, 3, 1, t.S(2)) }, 0);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "Canon", dir);
		CExifData off = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, false);
		CExifData on  = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(on.makerColorSpaceSource == EExifColorHintSource::MakerNote,
		          "the same file hoists with the flag on");
		MR_ASSERT(off.makerColorSpaceSource == EExifColorHintSource::None,
		          "and hoists NOTHING with the flag off");
		MR_ASSERT(CountIn(off, EExifIFD::MakerNote) == 0, "no MakerNote rows with the flag off");
		MR_ASSERT(Find(off, EExifIFD::Exif, 0x927C) != nullptr,
		          "the opaque row is what the flag-off path still shows");
	}

	// --- a recognised but unparseable blob keeps its opaque row -------------
	// Losing it would leave strictly less information than before this phase.
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		vector<uint8_t> blob(sig, sig + sizeof(sig));
		blob.push_back(0xFF); blob.push_back(0xFF);      // count = 65535, nothing behind it
		for (int i = 0; i < 8; ++i) blob.push_back(0xAA);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.valid, "a truncated MakerNote does not abandon the whole parse");
		MR_ASSERT(e.make == "PENTAX", "the container's own tags are still readable");
		MR_ASSERT(Find(e, EExifIFD::Exif, 0x927C) != nullptr,
		          "and the opaque row survives, because nothing replaced it");
	}

	// --- MakerNote tag space must not be mistaken for Exif/GPS pointers ------
	// 0x8769/0x8825/0xA005 are ordinary vendor tag numbers inside a MakerNote.
	// Descending them would let vendor-relative garbage overwrite the real
	// Make/Model/DateTimeOriginal/Orientation.
	//
	// The pointers target a REAL directory carrying a Make and GPS coordinates,
	// so removing the guard would visibly corrupt both. Pointing them at the
	// maker IFD itself would prove nothing: the cycle guard would stop the
	// descent regardless.
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		Tiff w = t;
		vector<uint8_t> body;
		uint32_t topOff = w.AppendIfd(body, {
			t.E(0x0037, 3, 1, t.S(0)),
			t.E(0x8769, 4, 1, t.L(0)),      // looks like an Exif-IFD pointer
			t.E(0x8825, 4, 1, t.L(0)),      // looks like a GPS-IFD pointer
		}, 0);
		uint32_t bait = w.AppendIfd(body, {
			t.E(0x0001, 2, 2, { 'N', 0 }),                 // GPSLatitudeRef, if read as GPS
			t.E(0x010F, 2, 7, { 'H','I','J','A','C','K', 0 }),
		}, 0);
		const uint32_t baitAbs = bait + (uint32_t)sizeof(sig);
		w.Patch32(body, topOff + 2 + 12 * 1 + 8, baitAbs);
		w.Patch32(body, topOff + 2 + 12 * 2 + 8, baitAbs);
		vector<uint8_t> blob(sig, sig + sizeof(sig));
		blob.insert(blob.end(), body.begin(), body.end());

		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.valid, "MakerNote with pointer-shaped tags parses");
		MR_ASSERT(e.make == "PENTAX",
		          "Make is not overwritten from a MakerNote's pointer-shaped tag");
		MR_ASSERT(e.makerColorSpace == EExifColorSpaceHint::Srgb,
		          "and the real colour tag is still read");
		MR_ASSERT(!e.hasGps, "no GPS is invented from a MakerNote tag number");
	}

	// --- a MakerNote cannot fabricate a DNG preview candidate ---------------
	// 0xC71A / 0x0100 / 0x0101 are ordinary vendor tag numbers in this space,
	// and a fabricated candidate carries fabricated dimensions -- which is
	// exactly what the host matches against when choosing one.
	{
		const uint8_t sig[] = { 'A','O','C', 0, 'M','M' };
		vector<uint8_t> blob = BareIfd(t, {
			t.E(0x0100, 4, 1, t.L(4096)),
			t.E(0x0101, 4, 1, t.L(4096)),
			t.E(0xC71A, 3, 1, t.S(4)),
		}, sizeof(sig), sig);
		vector<uint8_t> jpeg = BuildMakerNoteJpeg(t, "PENTAX", blob);
		CExifData e = CExifReader::Read(jpeg.data(), jpeg.size(), true, false, true);
		MR_ASSERT(e.dngPreviewCandidates.empty(),
		          "vendor tags numbered 0xC71A do not become DNG preview candidates");
	}

	TestCompleted(true, "MakerNote shapes, per-vendor mappings, sentinels and safety all behave");
}
