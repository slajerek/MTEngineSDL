#ifndef MT_JPEG_TEST_HELPERS_H
#define MT_JPEG_TEST_HELPERS_H

// Byte-level JPEG probes shared by the engine's and the apps' tests. Header
// only: every function is inline, every name std::-qualified (the test files
// that include this one may or may not say `using namespace std`).
//
// Neither function decodes anything -- they walk the marker segments between
// SOI and the start of scan data, so a test can ask what a WRITER put in a
// file without trusting a reader that might normalise it away.

#include <cstddef>
#include <utility>
#include <vector>

// Sampling factors of component 0, read straight out of the SOF0/SOF1
// segment: len(2) precision(1) height(2) width(2) nComp(1) then per component
// id(1) sampling(1: h<<4 | v) qtable(1). False when there is no SOF0/SOF1
// before SOS (progressive, arithmetic or a damaged file). (Moved out of
// CTestJpegMarkers.cpp unchanged, libjpeg-turbo plan Task 3.)
inline bool LumaSampling(const std::vector<unsigned char> &j, int &h, int &v)
{
	std::size_t i = 2;
	while (i + 3 < j.size() && j[i] == 0xFF)
	{
		unsigned char m = j[i + 1];
		std::size_t len = ((std::size_t)j[i + 2] << 8) | j[i + 3];
		if (m == 0xC0 || m == 0xC1)                 // SOF0 / SOF1
		{
			// From the 0xFF at i: len(2) precision(1) height(2) width(2)
			// nComp(1) = 8 bytes, so component 0 begins at i+2+8. Its second
			// byte packs the sampling factors as h<<4 | v.
			std::size_t comp0 = i + 2 + 8;
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

// Every marker segment after SOI up to (not including) SOS/EOI, in file order:
// its marker byte and its payload (the bytes after the 2-byte length). Fill
// bytes (0xFF 0xFF) are skipped. EMPTY when the file does not start with SOI
// or a segment is malformed (no 0xFF where a marker must be, a length under 2,
// or a segment running past the end) -- a caller comparing two files' lists
// must not be handed half a list that happens to match.
inline std::vector<std::pair<unsigned char, std::vector<unsigned char>>>
JpegMarkerSegments(const std::vector<unsigned char> &j)
{
	std::vector<std::pair<unsigned char, std::vector<unsigned char>>> out;
	if (j.size() < 4 || j[0] != 0xFF || j[1] != 0xD8) return out;
	std::size_t p = 2;
	while (p + 4 <= j.size())
	{
		if (j[p] != 0xFF) return {};
		const unsigned char m = j[p + 1];
		if (m == 0xFF) { ++p; continue; }            // fill byte
		if (m == 0xDA || m == 0xD9) break;            // SOS / EOI: scan data follows
		const std::size_t len = ((std::size_t)j[p + 2] << 8) | j[p + 3];
		if (len < 2 || p + 2 + len > j.size()) return {};
		out.emplace_back(m, std::vector<unsigned char>(j.begin() + (std::ptrdiff_t)(p + 4),
		                                               j.begin() + (std::ptrdiff_t)(p + 2 + len)));
		p += 2 + len;
	}
	return out;
}

#endif
