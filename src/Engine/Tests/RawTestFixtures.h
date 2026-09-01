#pragma once

#include <string>
#include <initializer_list>
#include <vector>

// Shared fixture helpers for the RD-A RAW tests (CTestRawPreview,
// CTestRawDecode). Three tiers per the RD-A design #10.1:
//   - generated at test time (synthetic Bayer, synthetic DNG) -- never skip
//   - git-ignored real files under PC_RAW_FIXTURE_DIR -- skip, REPORTED
// Real RAW/DNG files are never committed.

// Directory named by the PC_RAW_FIXTURE_DIR environment variable, or "" when
// unset. Tests that need real files skip-report on "".
std::string PC_RawFixtureDir();

// First regular file in `dir` whose lowercased extension matches one of
// `exts` (each given with the leading dot, e.g. ".cr2"), or "". Not
// recursive. Deterministic (lexicographic) so repeated runs pick the same
// fixture.
std::string PC_FindFixture(const std::string &dir,
						   std::initializer_list<const char *> exts);

// As PC_FindFixture but inside the subdirectory `sub` of `dir` (e.g. the
// "portrait" and "codec" conventions from the RD-A plan).
std::string PC_FindFixtureIn(const std::string &dir, const char *sub,
							 std::initializer_list<const char *> exts);

// Compression tag (TIFF 259) of a DNG's MAIN raw image, walking SubIFDs to the
// full-resolution IFD, or -1 if it cannot be read. Names it too, when known.
//
// Exists so a decode failure says WHICH compression it choked on. LibRaw
// returns a bare -2 (LIBRAW_FILE_UNSUPPORTED) for every unsupported variant,
// and that is indistinguishable from a corrupt file -- so a fixture authored
// with the wrong Adobe DNG Converter setting looks exactly like a decoder bug.
// It cost real time once: an Adobe DNG Converter "Lossy" export under DNG 1.7
// compatibility is JPEG XL (52546), which this LibRaw cannot decode, and the
// test simply said "unpack failed: -2".
int PC_DngMainCompression(const std::string &path);
const char *PC_DngCompressionName(int compression);

// Copy `src` into the temp directory and XOR-flip `count` bytes starting at
// `offset` (clamped to the file size). Returns the temp path, or "" on
// failure. Caller removes the file.
std::string PC_MakeCorruptedCopy(const std::string &src, size_t offset,
								 size_t count);

// Write `size` bytes of the repeating byte `fill` to a temp file with the
// given extension (".cr2" etc.). Returns the temp path, or "". Caller
// removes the file.
std::string PC_MakeFilledTempFile(const char *extension, size_t size,
								  unsigned char fill);

// Write a whole byte vector to a temp file with the given extension.
// Returns the temp path, or "". Caller removes the file.
std::string PC_WriteTempFile(const char *extension,
							 const std::vector<unsigned char> &bytes);

// 16-bit little-endian RGGB mosaic for CRawDecoder::DecodeBayer: every
// photosite is `value` scaled by its CFA colour's multiplier (defaults
// 1,1,1 = a flat neutral field). Values must stay non-zero: an all-black
// fixture takes adjust_bl's other branch and reads as a wrong black level
// (RD-A design #5.8).
std::vector<unsigned char> PC_MakeBayerRGGB(unsigned short width,
											unsigned short height,
											unsigned short value,
											float rMul = 1.f,
											float gMul = 1.f,
											float bMul = 1.f);

// A pre-packed TIFF IFD entry (little-endian value bytes). Used by
// SSyntheticDngSpec::extraIfd0Tags.
struct SDngRawTag
{
	unsigned short tag = 0;
	unsigned short type = 0;      // TIFF type code
	unsigned count = 0;
	std::vector<unsigned char> value;
};

// ---- Synthetic DNG builder (RD-A design #10 case 4a, #12.8) ----
//
// A minimal little-endian TIFF/DNG: one IFD0 carrying an uncompressed 16-bit
// RGGB CFA strip plus the DNG colour tags. Built to survive LibRaw's
// identify() so the D65-vs-E matrix invariant, the DefaultCrop contract and
// the calibration hand-off run on EVERY machine with no fixture file and no
// skip path. Deterministic bytes (no timestamps) -- RD-C #9.6 reuses this
// builder for its chart fixture.
struct SSyntheticDngSpec
{
	// CFA dimensions. LibRaw's identify() has an early reject for
	// width/height < 22 ("damaged images", identify.cpp:1104) -- the design's
	// "~16x16" fixture is BELOW LibRaw's floor, found empirically. 32x32 is
	// the working minimum here (even, CFA-aligned, > 22).
	unsigned short width = 32, height = 32;
	unsigned short fillValue = 8000;          // every photosite's value
	// Per-CFA-colour multipliers on fillValue (R, G, B). RD-C's falsifier
	// needs a NEUTRAL OBJECT, and with a non-trivial AsShotNeutral a neutral
	// object records photosites PROPORTIONAL TO THE NEUTRAL, not uniform --
	// set these to the asShotNeutral values to photograph a gray card.
	float fillMul[3] = { 1.f, 1.f, 1.f };

	// Full custom mosaic (RD-C #9.6's ramp+patch chart): width*height RGGB
	// photosite values. When set (and sized right) it REPLACES the
	// fillValue/fillMul fill entirely. The pointer must stay valid for the
	// PC_BuildSyntheticDng call.
	const std::vector<unsigned short> *mosaic = nullptr;

	// XYZ -> camera, row-major, REQUIRED. Defaults are a plausible
	// daylight-referenced camera matrix (Canon-5D-class numbers).
	float colorMatrix1[9] = { 0.6446f, -0.0366f, -0.0468f,
							  -0.4358f, 1.2071f, 0.2564f,
							  -0.0587f, 0.1316f, 0.5452f };
	int calibrationIlluminant1 = 21;          // EXIF LightSource: 21 = D65
	float asShotNeutral[3] = { 0.6f, 1.0f, 0.7f };
	const char *uniqueCameraModel = "MTEngine Synthetic";

	// Optional tags; the 'has' flags gate emission.
	bool hasColorMatrix2 = false;
	float colorMatrix2[9] = {};
	int calibrationIlluminant2 = 17;          // StdA
	bool hasCalibration1 = false;
	float cameraCalibration1[9] = {};
	bool hasCalibration2 = false;
	float cameraCalibration2[9] = {};
	bool hasAnalogBalance = false;
	float analogBalance[3] = { 1.f, 1.f, 1.f };
	bool hasForwardMatrix1 = false;
	float forwardMatrix1[9] = {};
	bool hasDefaultCrop = false;
	unsigned short defaultCropOrigin[2] = {};
	unsigned short defaultCropSize[2] = {};
	bool hasBaselineExposure = false;
	float baselineExposure = 0.f;

	// XMP packet (TIFF tag 700, BYTE array) emitted when non-empty -- the
	// same tag LibRaw parses into imgdata.idata.xmpdata for DNG/TIFF RAWs;
	// exercises CImageData::ReadEmbeddedXmp without a real fixture.
	std::string xmpPacket;

	// TIFF Orientation (tag 274) emitted when != 0; 6 = 90 degrees CW. The
	// no-rotation contract (user_flip = 0, roadmap #2.12) is tested with
	// this: LibRaw parses it into tiff_flip, and only a regressed pin would
	// let it reach the output as swapped dimensions.
	unsigned short orientation = 0;

	// Optional embedded JPEG preview: emitted as a second IFD
	// (NewSubfileType=1, Compression=6, JpegIFOffset/Length) with the blob
	// appended -- the layout LoadRAWPreview's unpack_thumb path consumes.
	// The pointer must stay valid for the PC_BuildSyntheticDng call.
	const std::vector<unsigned char> *thumbJpeg = nullptr;
	unsigned short thumbWidth = 0, thumbHeight = 0;

	// Arbitrary extra IFD0 tags, appended verbatim (RD-D #8.1: the DCP
	// fixture writer injects embedded-profile tags through this). `value`
	// is the little-endian packed value bytes; the builder handles the
	// inline-vs-offset split. See MT_DcpFixtureWriter for packers.
	std::vector<SDngRawTag> extraIfd0Tags;
};

// Encode a flat mid-grey width x height JPEG in memory with an APP2
// ICC_PROFILE segment carrying `icc` (via jpeg_write_marker). Empty vector
// on failure. For the CImageDataRAW APP2 hook test (RD-A design #10.1 end).
std::vector<unsigned char> PC_BuildJpegWithIcc(
	unsigned short width, unsigned short height,
	const std::vector<unsigned char> &icc);

// Returns the DNG bytes, or an empty vector on an internal error.
std::vector<unsigned char> PC_BuildSyntheticDng(const SSyntheticDngSpec &spec);
