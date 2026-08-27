#include "RawTestFixtures.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <vector>

// The engine's vendored jpeg-9a (compiled into the engine target on every
// platform), for PC_BuildJpegWithIcc. jconfig.h/jmorecfg.h come with it.
#include <cstddef>
extern "C" {
#include "jpeglib.h"
}

#ifdef _WIN32
#include <process.h>
#define PC_GETPID _getpid
#else
#include <unistd.h>
#define PC_GETPID getpid
#endif

namespace fs = std::filesystem;

// C++20: path::u8string() returns std::u8string (char8_t) and u8path is
// deprecated. These two keep the API surface plain std::string (UTF-8) on
// every platform, including Windows where path's char constructor would use
// the ANSI code page instead.
static fs::path PathFromUtf8(const std::string &s)
{
	return fs::path(std::u8string(s.begin(), s.end()));
}

static std::string Utf8FromPath(const fs::path &p)
{
	std::u8string u8 = p.u8string();
	return std::string(u8.begin(), u8.end());
}

std::string PC_RawFixtureDir()
{
	std::error_code ec;

	// PC_RAW_FIXTURE_DIR wins: it is how a machine points at a fixture set kept
	// outside the tree.
	const char *dir = getenv("PC_RAW_FIXTURE_DIR");
	if (dir != NULL && dir[0] != 0)
	{
		if (fs::is_directory(PathFromUtf8(dir), ec))
			return dir;
		return "";   // named but wrong: do not silently fall back and hide the typo
	}

	// FALL BACK TO THE DOCUMENTED DEFAULT, which the docs already promised and
	// this function did not honour: raw-decode.md says the location is
	// `tests/raws` locally. Requiring the environment variable as well meant six
	// real-RAW tests skipped on every machine -- for months, with the fixtures
	// sitting in tests/raws the whole time -- because the one extra step is
	// invisible when forgotten.
	//
	// Relative to the working directory, never an absolute path: tests run from
	// the project root (tests/run_test.sh cds there), and hardcoding a path is
	// forbidden outright. Apps without such a directory get "" exactly as
	// before, so this is inert for c64d and LightHeroes.
	const fs::path fallback = fs::path("tests") / "raws";
	if (fs::is_directory(fallback, ec))
		return fallback.string();

	return "";
}

const char *PC_DngCompressionName(int compression)
{
	switch (compression)
	{
		case 1:     return "uncompressed";
		case 7:     return "JPEG (lossless)";
		case 8:     return "Deflate/ZIP";
		case 34892: return "Lossy JPEG";
		case 52546: return "JPEG XL -- DNG 1.7+, NOT decodable by this LibRaw build";
		default:    return "unknown";
	}
}

namespace
{
	// Minimal TIFF walker: enough to reach the main image's Compression tag and
	// nothing more. Deliberately not a DNG parser -- it only has to make a
	// failure message specific.
	struct TiffReader
	{
		const unsigned char *d = NULL;
		size_t len = 0;
		bool little = true;
		int best = -1;
		unsigned int bestPixels = 0;

		unsigned int U16(size_t o) const
		{
			if (o + 2 > len) return 0;
			return little ? (unsigned int)(d[o] | (d[o + 1] << 8))
						  : (unsigned int)((d[o] << 8) | d[o + 1]);
		}
		unsigned int U32(size_t o) const
		{
			if (o + 4 > len) return 0;
			return little ? (unsigned int)(d[o] | (d[o+1] << 8) | (d[o+2] << 16) | ((unsigned int)d[o+3] << 24))
						  : (unsigned int)(((unsigned int)d[o] << 24) | (d[o+1] << 16) | (d[o+2] << 8) | d[o+3]);
		}

		void Ifd(size_t off, int depth)
		{
			// Bounded: malformed files must not spin. Real DNGs nest one level.
			if (off == 0 || off >= len || depth > 4)
				return;
			unsigned int n = U16(off);
			if (n == 0 || off + 2 + (size_t)n * 12 + 4 > len)
				return;

			int compression = -1;
			unsigned int w = 0, h = 0;
			std::vector<unsigned int> subs;

			for (unsigned int i = 0; i < n; i++)
			{
				size_t e = off + 2 + (size_t)i * 12;
				unsigned int tag = U16(e), typ = U16(e + 2), cnt = U32(e + 4);
				size_t vo = e + 8;
				unsigned int v = (typ == 3) ? U16(vo) : U32(vo);
				if (tag == 259 && cnt == 1) compression = (int)v;
				else if (tag == 256) w = v;
				else if (tag == 257) h = v;
				else if (tag == 330)
				{
					if (cnt == 1) subs.push_back(v);
					else
					{
						unsigned int base = U32(vo);
						for (unsigned int k = 0; k < cnt && k < 16; k++)
							subs.push_back(U32(base + 4 * k));
					}
				}
			}

			// The MAIN image is simply the largest one carrying a Compression
			// tag -- more robust than trusting NewSubFileType, which converters
			// set inconsistently across versions.
			if (compression >= 0 && w && h && w * h > bestPixels)
			{
				bestPixels = w * h;
				best = compression;
			}

			for (size_t i = 0; i < subs.size(); i++)
				Ifd(subs[i], depth + 1);
			Ifd(U32(off + 2 + (size_t)n * 12), depth);
		}
	};
}

int PC_DngMainCompression(const std::string &path)
{
	std::ifstream f(PathFromUtf8(path), std::ios::binary);
	if (!f)
		return -1;
	std::vector<unsigned char> buf((std::istreambuf_iterator<char>(f)),
								   std::istreambuf_iterator<char>());
	if (buf.size() < 8)
		return -1;
	if (!((buf[0] == 'I' && buf[1] == 'I') || (buf[0] == 'M' && buf[1] == 'M')))
		return -1;

	TiffReader r;
	r.d = buf.data();
	r.len = buf.size();
	r.little = (buf[0] == 'I');
	r.Ifd(r.U32(4), 0);
	return r.best;
}

static bool ExtensionMatches(const fs::path &p,
							 std::initializer_list<const char *> exts)
{
	std::string e = p.extension().string();
	std::transform(e.begin(), e.end(), e.begin(),
				   [](unsigned char c) { return (char)tolower(c); });
	for (const char *want : exts)
		if (e == want)
			return true;
	return false;
}

static std::string FindFixtureInPath(const fs::path &dir,
									 std::initializer_list<const char *> exts)
{
	std::error_code ec;
	if (!fs::is_directory(dir, ec))
		return "";
	std::vector<std::string> matches;
	for (const auto &entry : fs::directory_iterator(dir, ec))
	{
		if (!entry.is_regular_file(ec))
			continue;
		if (ExtensionMatches(entry.path(), exts))
			matches.push_back(Utf8FromPath(entry.path()));
	}
	if (matches.empty())
		return "";
	// Lexicographic minimum so every run picks the same fixture.
	return *std::min_element(matches.begin(), matches.end());
}

std::string PC_FindFixture(const std::string &dir,
						   std::initializer_list<const char *> exts)
{
	return FindFixtureInPath(PathFromUtf8(dir), exts);
}

std::string PC_FindFixtureIn(const std::string &dir, const char *sub,
							 std::initializer_list<const char *> exts)
{
	return FindFixtureInPath(PathFromUtf8(dir) / sub, exts);
}

static fs::path TempFilePath(const char *extension)
{
	static unsigned counter = 0;
	char name[128];
	snprintf(name, sizeof(name), "pc_raw_fixture_%d_%u%s",
			 (int)PC_GETPID(), counter++, extension);
	return fs::temp_directory_path() / name;
}

std::string PC_MakeCorruptedCopy(const std::string &src, size_t offset,
								 size_t count)
{
	std::ifstream in(PathFromUtf8(src), std::ios::binary);
	if (!in)
		return "";
	std::vector<char> bytes((std::istreambuf_iterator<char>(in)),
							std::istreambuf_iterator<char>());
	if (bytes.empty())
		return "";
	offset = std::min(offset, bytes.size() - 1);
	count = std::min(count, bytes.size() - offset);
	for (size_t i = 0; i < count; i++)
		bytes[offset + i] ^= (char)0xA5;

	fs::path out = TempFilePath(PathFromUtf8(src).extension().string().c_str());
	std::ofstream o(out, std::ios::binary | std::ios::trunc);
	if (!o)
		return "";
	o.write(bytes.data(), (std::streamsize)bytes.size());
	o.close();
	return o.good() ? Utf8FromPath(out) : "";
}

std::string PC_MakeFilledTempFile(const char *extension, size_t size,
								  unsigned char fill)
{
	std::vector<unsigned char> bytes(size, fill);
	return PC_WriteTempFile(extension, bytes);
}

std::string PC_WriteTempFile(const char *extension,
							 const std::vector<unsigned char> &bytes)
{
	fs::path out = TempFilePath(extension);
	std::ofstream o(out, std::ios::binary | std::ios::trunc);
	if (!o)
		return "";
	o.write((const char *)bytes.data(), (std::streamsize)bytes.size());
	o.close();
	return o.good() ? Utf8FromPath(out) : "";
}

std::vector<unsigned char> PC_MakeBayerRGGB(unsigned short width,
											unsigned short height,
											unsigned short value,
											float rMul, float gMul, float bMul)
{
	std::vector<unsigned char> out((size_t)width * height * 2);
	for (unsigned y = 0; y < height; y++)
	{
		for (unsigned x = 0; x < width; x++)
		{
			// RGGB 2x2: (0,0)=R (1,0)=G (0,1)=G (1,1)=B
			float mul;
			if ((y & 1) == 0)
				mul = ((x & 1) == 0) ? rMul : gMul;
			else
				mul = ((x & 1) == 0) ? gMul : bMul;
			float v = value * mul;
			if (v < 0.f) v = 0.f;
			if (v > 65535.f) v = 65535.f;
			unsigned short px = (unsigned short)(v + 0.5f);
			size_t i = ((size_t)y * width + x) * 2;
			out[i] = (unsigned char)(px & 0xFF);
			out[i + 1] = (unsigned char)(px >> 8);
		}
	}
	return out;
}

// ---- Synthetic DNG builder --------------------------------------------
//
// Minimal little-endian TIFF writer. Entries must be emitted in ascending
// tag order (TIFF requirement); out-of-line values land after the IFD, the
// CFA strip last. Everything is deterministic.

namespace {

enum
{
	TT_BYTE = 1, TT_ASCII = 2, TT_SHORT = 3, TT_LONG = 4,
	TT_RATIONAL = 5, TT_SRATIONAL = 10
};

struct STiffEntry
{
	unsigned short tag;
	unsigned short type;
	unsigned count;
	std::vector<unsigned char> value;   // packed value bytes (pre-offset)
};

void Put16(std::vector<unsigned char> &v, unsigned short x)
{
	v.push_back((unsigned char)(x & 0xFF));
	v.push_back((unsigned char)(x >> 8));
}

void Put32(std::vector<unsigned char> &v, unsigned x)
{
	v.push_back((unsigned char)(x & 0xFF));
	v.push_back((unsigned char)((x >> 8) & 0xFF));
	v.push_back((unsigned char)((x >> 16) & 0xFF));
	v.push_back((unsigned char)((x >> 24) & 0xFF));
}

STiffEntry ShortEntry(unsigned short tag, std::initializer_list<unsigned short> vals)
{
	STiffEntry e{ tag, TT_SHORT, (unsigned)vals.size(), {} };
	for (unsigned short v : vals)
		Put16(e.value, v);
	return e;
}

STiffEntry LongEntry(unsigned short tag, unsigned val)
{
	STiffEntry e{ tag, TT_LONG, 1, {} };
	Put32(e.value, val);
	return e;
}

STiffEntry ByteEntry(unsigned short tag, std::initializer_list<unsigned char> vals)
{
	STiffEntry e{ tag, TT_BYTE, (unsigned)vals.size(), {} };
	for (unsigned char v : vals)
		e.value.push_back(v);
	return e;
}

STiffEntry AsciiEntry(unsigned short tag, const char *str)
{
	STiffEntry e{ tag, TT_ASCII, 0, {} };
	for (const char *p = str; *p; p++)
		e.value.push_back((unsigned char)*p);
	e.value.push_back(0);
	e.count = (unsigned)e.value.size();
	return e;
}

// Signed rationals at 1/10000 precision -- plenty for camera matrices.
STiffEntry SRationalEntry(unsigned short tag, const float *vals, unsigned n)
{
	STiffEntry e{ tag, TT_SRATIONAL, n, {} };
	for (unsigned i = 0; i < n; i++)
	{
		int num = (int)((double)vals[i] * 10000.0 + (vals[i] >= 0 ? 0.5 : -0.5));
		Put32(e.value, (unsigned)num);
		Put32(e.value, 10000u);
	}
	return e;
}

STiffEntry RationalEntry(unsigned short tag, const float *vals, unsigned n)
{
	STiffEntry e{ tag, TT_RATIONAL, n, {} };
	for (unsigned i = 0; i < n; i++)
	{
		unsigned num = (unsigned)((double)vals[i] * 10000.0 + 0.5);
		Put32(e.value, num);
		Put32(e.value, 10000u);
	}
	return e;
}

} // namespace

std::vector<unsigned char> PC_BuildSyntheticDng(const SSyntheticDngSpec &spec)
{
	// LibRaw rejects width/height < 22 outright (identify.cpp:1104), so a
	// spec below that floor would build bytes that can never open.
	if (spec.width < 22 || spec.height < 22
		|| (spec.width & 1) || (spec.height & 1))
		return {};
	if (spec.thumbJpeg != NULL && spec.thumbJpeg->empty())
		return {};

	const unsigned stripBytes = (unsigned)spec.width * spec.height * 2;

	// ---- IFD0: the raw CFA image + DNG colour tags
	std::vector<STiffEntry> ifd0;
	ifd0.push_back(LongEntry(254, 0));                        // NewSubfileType: full-res
	ifd0.push_back(LongEntry(256, spec.width));               // ImageWidth
	ifd0.push_back(LongEntry(257, spec.height));              // ImageLength
	ifd0.push_back(ShortEntry(258, { 16 }));                  // BitsPerSample
	ifd0.push_back(ShortEntry(259, { 1 }));                   // Compression: none
	ifd0.push_back(ShortEntry(262, { 32803 }));               // Photometric: CFA
	ifd0.push_back(AsciiEntry(271, "PhotoCruise"));           // Make
	ifd0.push_back(AsciiEntry(272, "SyntheticDNG"));          // Model
	ifd0.push_back(LongEntry(273, 0));                        // StripOffsets (patched)
	const size_t stripOffsetsIndex = ifd0.size() - 1;
	if (spec.orientation != 0)
		ifd0.push_back(ShortEntry(274, { spec.orientation }));
	if (!spec.xmpPacket.empty())
	{
		STiffEntry e{ 700, TT_BYTE, (unsigned)spec.xmpPacket.size(), {} };
		e.value.assign(spec.xmpPacket.begin(), spec.xmpPacket.end());
		ifd0.push_back(std::move(e));
	}
	ifd0.push_back(ShortEntry(277, { 1 }));                   // SamplesPerPixel
	ifd0.push_back(LongEntry(278, spec.height));              // RowsPerStrip
	ifd0.push_back(LongEntry(279, stripBytes));               // StripByteCounts
	ifd0.push_back(ShortEntry(284, { 1 }));                   // PlanarConfiguration
	ifd0.push_back(ShortEntry(33421, { 2, 2 }));              // CFARepeatPatternDim
	ifd0.push_back(ByteEntry(33422, { 0, 1, 1, 2 }));         // CFAPattern: RGGB
	ifd0.push_back(ByteEntry(50706, { 1, 4, 0, 0 }));         // DNGVersion
	ifd0.push_back(AsciiEntry(50708, spec.uniqueCameraModel)); // UniqueCameraModel
	ifd0.push_back(ShortEntry(50714, { 0 }));                 // BlackLevel
	ifd0.push_back(ShortEntry(50717, { 65535 }));             // WhiteLevel
	if (spec.hasDefaultCrop)
	{
		ifd0.push_back(ShortEntry(50719, { spec.defaultCropOrigin[0],
										   spec.defaultCropOrigin[1] }));
		ifd0.push_back(ShortEntry(50720, { spec.defaultCropSize[0],
										   spec.defaultCropSize[1] }));
	}
	ifd0.push_back(SRationalEntry(50721, spec.colorMatrix1, 9));
	if (spec.hasColorMatrix2)
		ifd0.push_back(SRationalEntry(50722, spec.colorMatrix2, 9));
	if (spec.hasCalibration1)
		ifd0.push_back(SRationalEntry(50723, spec.cameraCalibration1, 9));
	if (spec.hasCalibration2)
		ifd0.push_back(SRationalEntry(50724, spec.cameraCalibration2, 9));
	if (spec.hasAnalogBalance)
		ifd0.push_back(RationalEntry(50727, spec.analogBalance, 3));
	ifd0.push_back(RationalEntry(50728, spec.asShotNeutral, 3));
	if (spec.hasBaselineExposure)
		ifd0.push_back(SRationalEntry(50730, &spec.baselineExposure, 1));
	ifd0.push_back(ShortEntry(50778, { (unsigned short)spec.calibrationIlluminant1 }));
	if (spec.hasColorMatrix2)
		ifd0.push_back(ShortEntry(50779, { (unsigned short)spec.calibrationIlluminant2 }));
	if (spec.hasForwardMatrix1)
		ifd0.push_back(SRationalEntry(50964, spec.forwardMatrix1, 9));

	// RD-D #8.1: caller-supplied extra IFD0 tags (embedded-profile
	// injection), appended verbatim -- the layout pass below handles the
	// inline-vs-offset split like any other entry.
	for (const SDngRawTag &t : spec.extraIfd0Tags)
		ifd0.push_back(STiffEntry{ t.tag, t.type, t.count, t.value });

	// ---- optional IFD1: JPEG preview (NewSubfileType=1, old-JPEG pointers)
	std::vector<STiffEntry> ifd1;
	size_t thumbOffsetIndex = 0;
	if (spec.thumbJpeg != NULL)
	{
		ifd1.push_back(LongEntry(254, 1));                    // reduced-resolution
		ifd1.push_back(LongEntry(256, spec.thumbWidth));
		ifd1.push_back(LongEntry(257, spec.thumbHeight));
		ifd1.push_back(ShortEntry(258, { 8, 8, 8 }));
		ifd1.push_back(ShortEntry(259, { 6 }));               // old-style JPEG
		ifd1.push_back(ShortEntry(262, { 6 }));               // YCbCr
		ifd1.push_back(ShortEntry(277, { 3 }));
		ifd1.push_back(LongEntry(513, 0));                    // JpegIFOffset (patched)
		thumbOffsetIndex = ifd1.size() - 1;
		ifd1.push_back(LongEntry(514, (unsigned)spec.thumbJpeg->size()));
	}

	// ---- layout: header, IFD0, IFD0 overflow, [IFD1, IFD1 overflow,
	// thumb blob], CFA strip. Two passes: place, then emit.
	auto ifdSize = [](const std::vector<STiffEntry> &e)
	{ return 2u + 12u * (unsigned)e.size() + 4u; };

	unsigned pos = 8;
	unsigned ifd0Offset = pos;
	pos += ifdSize(ifd0);
	std::vector<unsigned> off0(ifd0.size(), 0);
	for (size_t i = 0; i < ifd0.size(); i++)
		if (ifd0[i].value.size() > 4)
		{
			if (pos & 1) pos++;
			off0[i] = pos;
			pos += (unsigned)ifd0[i].value.size();
		}
	unsigned ifd1Offset = 0;
	std::vector<unsigned> off1(ifd1.size(), 0);
	unsigned thumbBlobOffset = 0;
	if (!ifd1.empty())
	{
		if (pos & 1) pos++;
		ifd1Offset = pos;
		pos += ifdSize(ifd1);
		for (size_t i = 0; i < ifd1.size(); i++)
			if (ifd1[i].value.size() > 4)
			{
				if (pos & 1) pos++;
				off1[i] = pos;
				pos += (unsigned)ifd1[i].value.size();
			}
		if (pos & 1) pos++;
		thumbBlobOffset = pos;
		pos += (unsigned)spec.thumbJpeg->size();
	}
	if (pos & 1) pos++;
	unsigned stripOffset = pos;

	// Patch the placeholders.
	{
		STiffEntry &e = ifd0[stripOffsetsIndex];
		e.value.clear();
		Put32(e.value, stripOffset);
	}
	if (!ifd1.empty())
	{
		STiffEntry &e = ifd1[thumbOffsetIndex];
		e.value.clear();
		Put32(e.value, thumbBlobOffset);
	}

	std::vector<unsigned char> out;
	out.reserve(stripOffset + stripBytes);
	out.push_back('I'); out.push_back('I');
	Put16(out, 42);
	Put32(out, ifd0Offset);

	auto emitIfd = [&](const std::vector<STiffEntry> &entries,
					   const std::vector<unsigned> &offs, unsigned nextIfd)
	{
		Put16(out, (unsigned short)entries.size());
		for (size_t i = 0; i < entries.size(); i++)
		{
			const STiffEntry &e = entries[i];
			Put16(out, e.tag);
			Put16(out, e.type);
			Put32(out, e.count);
			if (e.value.size() <= 4)
			{
				std::vector<unsigned char> inl = e.value;
				while (inl.size() < 4)
					inl.push_back(0);
				out.insert(out.end(), inl.begin(), inl.end());
			}
			else
			{
				Put32(out, offs[i]);
			}
		}
		Put32(out, nextIfd);
		for (size_t i = 0; i < entries.size(); i++)
			if (entries[i].value.size() > 4)
			{
				while (out.size() < offs[i])
					out.push_back(0);
				out.insert(out.end(), entries[i].value.begin(),
						   entries[i].value.end());
			}
	};

	emitIfd(ifd0, off0, ifd1Offset);
	if (!ifd1.empty())
	{
		while (out.size() < ifd1Offset)
			out.push_back(0);
		emitIfd(ifd1, off1, 0);
		while (out.size() < thumbBlobOffset)
			out.push_back(0);
		out.insert(out.end(), spec.thumbJpeg->begin(), spec.thumbJpeg->end());
	}
	while (out.size() < stripOffset)
		out.push_back(0);
	if (spec.mosaic != nullptr
	    && spec.mosaic->size() == (size_t)spec.width * spec.height)
	{
		for (unsigned short v : *spec.mosaic)
			Put16(out, v);
		return out;
	}
	for (unsigned row = 0; row < spec.height; row++)
	{
		for (unsigned col = 0; col < spec.width; col++)
		{
			// RGGB: even row R G, odd row G B.
			const int c = (row & 1) == 0 ? ((col & 1) == 0 ? 0 : 1)
			                             : ((col & 1) == 0 ? 1 : 2);
			float v = spec.fillValue * spec.fillMul[c];
			if (v < 0.f) v = 0.f;
			if (v > 65535.f) v = 65535.f;
			Put16(out, (unsigned short)(v + 0.5f));
		}
	}
	return out;
}

std::vector<unsigned char> PC_BuildJpegWithIcc(
	unsigned short width, unsigned short height,
	const std::vector<unsigned char> &icc)
{
	if (width == 0 || height == 0)
		return {};

	jpeg_compress_struct cinfo;
	jpeg_error_mgr jerr;
	cinfo.err = jpeg_std_error(&jerr);
	jpeg_create_compress(&cinfo);

	unsigned char *mem = NULL;
	unsigned long memSize = 0;
	jpeg_mem_dest(&cinfo, &mem, &memSize);

	cinfo.image_width = width;
	cinfo.image_height = height;
	cinfo.input_components = 3;
	cinfo.in_color_space = JCS_RGB;
	jpeg_set_defaults(&cinfo);
	jpeg_set_quality(&cinfo, 85, TRUE);
	jpeg_start_compress(&cinfo, TRUE);

	// APP2 ICC_PROFILE segment, single chunk (1 of 1) -- the exact shape
	// CExifReader's ICC extraction reassembles.
	if (!icc.empty())
	{
		std::vector<unsigned char> app2;
		const char hdr[] = "ICC_PROFILE";
		app2.insert(app2.end(), (const unsigned char *)hdr,
					(const unsigned char *)hdr + 12);   // includes the NUL
		app2.push_back(1);   // chunk index
		app2.push_back(1);   // chunk count
		app2.insert(app2.end(), icc.begin(), icc.end());
		jpeg_write_marker(&cinfo, JPEG_APP0 + 2, app2.data(),
						  (unsigned)app2.size());
	}

	std::vector<unsigned char> row((size_t)width * 3, 128);
	JSAMPROW rowPtr = row.data();
	while (cinfo.next_scanline < cinfo.image_height)
		jpeg_write_scanlines(&cinfo, &rowPtr, 1);
	jpeg_finish_compress(&cinfo);
	jpeg_destroy_compress(&cinfo);

	std::vector<unsigned char> out(mem, mem + memSize);
	free(mem);
	return out;
}
