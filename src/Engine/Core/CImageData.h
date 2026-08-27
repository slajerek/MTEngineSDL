#ifndef IMG_CIMAGEDATA_H_
#define IMG_CIMAGEDATA_H_

#include "SYS_Defs.h"
#include <string>
#include "SYS_Main.h"
#include "SYS_Funct.h"
// For EExifColorSpaceHint / EExifColorHintSource on the preview hint below.
// Safe here: CExifReader.h pulls only <cstdint>/<string>/<vector>, nothing
// platform, and nothing in the exif TU includes this header back.
#include "CExifReader.h"

#ifdef USE_BUFFER_OFFSETS
#include "CBufferOffsets.h"
#endif

#include "png.h"

class CByteBuffer;

using namespace std;

typedef enum
{
	IMG_TYPE_UNKNOWN = 0,
	IMG_TYPE_GRAYSCALE,
	IMG_TYPE_GRAYSCALE_16BIT,
	IMG_TYPE_GRAYSCALE_32BIT,
	IMG_TYPE_RGB,
	IMG_TYPE_RGBA,		// =5
	IMG_TYPE_CIELAB,
	IMG_TYPE_GPU_COMPRESSED,	// =7  KTX2/UASTC-transcoded GPU block data, NO resultData
	// =8  Four channels of 16 bits, interleaved RGBA, 8 bytes per pixel.
	// The colour counterpart of IMG_TYPE_GRAYSCALE_16BIT, added so a 16-bit
	// PNG keeps its precision instead of being stripped to 8 at the decoder.
	//
	// It does NOT reach the GPU as 16-bit: CSlrImage converts to RGBA8 at
	// upload, because the texture path (and the display) are 8-bit. The point
	// is everything BEFORE that -- a colour transform run on 16-bit source
	// data does not band the way one run on pre-truncated 8-bit data does.
	IMG_TYPE_RGBA_16BIT,
	// =9  Four channels of IEEE half-float, interleaved RGBA, 8 bytes per
	// pixel -- the SAME storage footprint as IMG_TYPE_RGBA_16BIT, and
	// deliberately so, but carrying RANGE rather than precision.
	//
	// unorm16 has 65 536 uniform steps in 0..1; half has ~1 024 between 0.5
	// and 1.0. Half is therefore WORSE than unorm16 inside 0..1, and this
	// type does not exist to be more precise. It exists because a half can
	// hold values ABOVE 1.0 -- a RAW's specular highlights, an HDR HEIC's
	// gain-mapped sky, a PQ clip's above-white -- which unorm16 has nowhere
	// to put. Float earns its place through range, not precision, which is
	// why IMG_TYPE_RGBA_16BIT stays exactly as it is for 16-bit PNG/TIFF.
	//
	// Unlike IMG_TYPE_RGBA_16BIT this one CAN reach the GPU unchanged, as
	// MTLPixelFormatRGBA16Float / GL_RGBA16F, when the backend supports it
	// and the surface can carry the range. CSlrImage is the single funnel
	// that decides (S-5).
	IMG_TYPE_RGBA_16F
} imageTypes;

// ---------------------------------------------------------------------------
// Half-float and extended-sRGB helpers (S-5)
//
// Shared and inline because three lanes need exactly these and nothing else:
// the develop chain, the video poster extractor and the HDR HEIC arm all write
// half and all encode for the surface. Before S-5 the tree had no float<->half
// conversion at all except a file-local static in CKTX2Loader.cpp, which is the
// wrong direction and not exported.
// ---------------------------------------------------------------------------

// IEEE 754 binary16 <-> binary32.
//
// THE "NOT IN A HOT LOOP" ASSUMPTION WAS WRONG, and measurement is what
// corrected it: the video poster's float lane calls these THREE TIMES PER
// PIXEL -- 1.77M calls for a 1024x576 poster -- and after the transfer tables
// removed the powf cost, the software conversion below was 19 ms of a 25 ms
// poster, i.e. the single largest remaining item on that path.
//
// So there are now two implementations. The software one is kept, unchanged
// and reachable, because it is the reference the hardware path is verified
// against (CTestVideoTransfer compares them across the ENTIRE 16-bit space);
// the original comment already recorded that this code was written to match
// __fp16 exactly, ties-to-even included, so using the hardware instruction is
// behaviour-preserving by that same verification rather than by assumption.
#if defined(__aarch64__) || defined(__ARM_NEON)
	#define MT_HAS_HW_HALF 1
#else
	#define MT_HAS_HW_HALF 0
#endif

inline u16 FloatToHalfSoftware(float f)
{
	union { float f; u32 u; } in; in.f = f;
	const u32 sign = (in.u >> 16) & 0x8000u;
	int exp  = (int)((in.u >> 23) & 0xFFu) - 127 + 15;
	u32 mant = in.u & 0x007FFFFFu;

	if (((in.u >> 23) & 0xFFu) == 0xFFu)             // Inf / NaN
		return (u16)(sign | 0x7C00u | (mant ? 0x0200u : 0u));
	if (exp >= 0x1F)                                  // overflows half -> Inf
		return (u16)(sign | 0x7C00u);
	// Rounding is round-to-nearest, TIES TO EVEN -- the IEEE 754 default, and
	// what the hardware conversion does. Ties-away (`(mant >> (shift-1)) & 1`)
	// is the obvious-looking version and is wrong on exactly-halfway values:
	// verified differentially against __fp16 across the whole 16-bit space plus
	// ~180k sampled floats, it disagreed on 10 of them. One ULP of half is not
	// visible, but a conversion that disagrees with the hardware is a trap for
	// anyone who ever compares the two.
	if (exp <= 0)                                     // subnormal or zero
	{
		if (exp < -10)
			return (u16)sign;
		mant |= 0x00800000u;
		const int shift = 14 - exp;
		const u32 halfMant = mant >> shift;
		const u32 rem = mant & ((1u << shift) - 1u);
		const u32 halfway = 1u << (shift - 1);
		const u32 round = (rem > halfway || (rem == halfway && (halfMant & 1u))) ? 1u : 0u;
		return (u16)(sign | (halfMant + round));
	}
	const u32 halfMant = mant >> 13;
	const u32 rem = mant & 0x1FFFu;
	const u32 round = (rem > 0x1000u || (rem == 0x1000u && (halfMant & 1u))) ? 1u : 0u;
	u32 bits = (u32)(sign | ((u32)exp << 10) | halfMant);
	return (u16)(bits + round);
}

inline float HalfToFloatSoftware(u16 h)
{
	const u32 sign = (u32)(h & 0x8000u) << 16;
	const u32 exp  = (h >> 10) & 0x1Fu;
	const u32 mant = h & 0x03FFu;
	union { float f; u32 u; } out;

	if (exp == 0)
	{
		if (mant == 0) { out.u = sign; return out.f; }   // +-0
		// subnormal half -> normal float: renormalise
		int e = -1;
		u32 m = mant;
		do { m <<= 1; e++; } while ((m & 0x0400u) == 0);
		m &= 0x03FFu;
		out.u = sign | ((u32)(127 - 15 - e) << 23) | (m << 13);
		return out.f;
	}
	if (exp == 0x1F)                                     // Inf / NaN
	{
		out.u = sign | 0x7F800000u | (mant << 13);
		return out.f;
	}
	out.u = sign | ((exp - 15 + 127) << 23) | (mant << 13);
	return out.f;
}

// The conversions everything actually calls. On arm64 these compile to a
// single fcvt instruction; elsewhere they are the software reference above.
inline u16 FloatToHalf(float f)
{
#if MT_HAS_HW_HALF
	__fp16 h = (__fp16)f;
	u16 out;
	memcpy(&out, &h, sizeof(out));
	return out;
#else
	return FloatToHalfSoftware(f);
#endif
}

inline float HalfToFloat(u16 h)
{
#if MT_HAS_HW_HALF
	__fp16 v;
	memcpy(&v, &h, sizeof(v));
	return (float)v;
#else
	return HalfToFloatSoftware(h);
#endif
}

// SrgbExtendedEncode / SrgbExtendedDecode -- the IEC 61966-2-1 curve,
// sign-symmetric and continued above 1.0 -- MOVED to MT_SrgbCurve.h (S-6 A1
// review round 1) so a render backend can reach them without dragging png.h,
// CExifReader.h and this file's global `using namespace std` into a
// translation unit that also includes <windows.h>. Included here so every
// existing caller of this header is untouched and there is still one copy.
#include "MT_SrgbCurve.h"

// Per-mip GPU-compressed block payload for an IMG_TYPE_GPU_COMPRESSED image.
// GOTCHA (design note §8.1): for tiny mips the logical (orig) dimensions
// (e.g. 2x2, 1x1) are smaller than the physical block-padded dimensions
// (multiple of 4). Both are stored — logical drives the upload region/extent,
// physical drives bytesPerRow and total payload size.
struct SCompressedMip
{
	int origWidth;      // logical mip width (texels), may be < block size
	int origHeight;     // logical mip height (texels), may be < block size
	int physWidth;      // block-padded width (multiple of block size)
	int physHeight;     // block-padded height (multiple of block size)
	u8 *blockData;      // transcoded GPU block bytes (BC7/ASTC), owned by CImageData
	u32 blockDataSize;  // size of blockData in bytes
};

typedef enum
{
	//IMG_ORIG = 0,
	IMG_TEMP = 1,
	IMG_RESULT
} imageSources;

//TODO: access pointers
/*	access = new T*[h];   // allocate space for row pointers

	//initialize row pointers
 	for (int i = 0; i < h; i++)
		access[i] = data + (i * w);
*/


class CImageData
{
public:
	//void *origData;
	u8 *tempData;
	u8 *resultData;
	u8 type;

	u8 *mask;

	// Raw ICC profile bytes exactly as found in the file; NULL when the image
	// is untagged. Owned by this CImageData. Deliberately NOT reduced to a
	// colour-space enum at the loader boundary: the whole point of ICC is that
	// arbitrary, custom-calibrated profiles exist, and collapsing them to
	// "sRGB / AdobeRGB / other" would discard exactly the profiles colour
	// management is for. The engine never interprets these bytes; it hands
	// them to the platform CMS as-is.
	//
	// Note IMG_TYPE_GPU_COMPRESSED images can carry a profile but have no
	// resultData to transform -- consumers must treat that as "skip", not as
	// an error.
	u8 *iccProfile = NULL;
	u32 iccProfileSize = 0;

	// Deep-copies the bytes after checking they are structurally a profile;
	// malformed input leaves the image untagged rather than passing a broken
	// profile to a CMM, where it is a crash rather than a colour bug. Frees
	// any profile already held.
	void SetIccProfile(const u8 *bytes, u32 size);
	void DeallocIccProfile();

	// CM-C1: what the RAW's EMBEDDED PREVIEW says about its own colour space.
	//
	// Set only by LoadRAWPreview, and only when the preview carries no ICC
	// profile of its own -- a profile is authoritative and still wins. A camera
	// preview usually carries neither, which is the gap the container-level
	// signals exist to fill; this covers the formats (RAF above all) whose
	// preview does bring its own EXIF.
	//
	// Lifecycle mirrors iccProfile EXACTLY: reset in Load() before the loader
	// dispatch, copied by the copy constructor, and copied-or-CLEARED by the
	// pixel-copy path. The specialised loaders return before this class's own
	// dealloc, so without the unconditional reset a .NEF displayed after a .DNG
	// would inherit the DNG's colour space and be transformed with it.
	EExifColorSpaceHint  previewColorHint       = EExifColorSpaceHint::Unknown;
	EExifColorHintSource previewColorHintSource = EExifColorHintSource::None;

public:
	CImageData();
	CImageData(const char *fileName);
	CImageData(CByteBuffer *byteBuffer);
	CImageData(int width, int height);
	CImageData(int width, int height, u8 type);
	CImageData(int width, int height, u8 type, bool allocTemp, bool allocResult);
	CImageData(int width, int height, u8 type, u8 *data);
	CImageData(CImageData *src);
	virtual ~CImageData();

#ifdef USE_BUFFER_OFFSETS
	CBufferOffsets *bufferOffsets;
#endif

	//int originalWidth, originalHeight;
	int width, height;

	// TRUE when Load() only succeeded after repairing the file in memory --
	// today, a JPEG whose EOI marker is missing (see Load()). The pixels are
	// as good as the file allows, which is usually all of them, but the file
	// itself is damaged and the host should say so rather than pretend
	// otherwise. Reset by Load() before every dispatch, like the colour hints.
	bool decodeWasRepaired = false;

	// IMG_TYPE_RGBA_16F only: are the half pixels LINEAR, or already in the
	// surface's encoding? (S-5 Phase 2 contract.)
	//
	// It has to travel WITH the pixels because the two are only meaningful
	// together, and because the resident funnel -- engine code, the caller that
	// must pass `inputIsLinear` to the tone-map -- has no other way to know.
	// The engine's own float producers emit LINEAR and the host encodes
	// afterwards, so `false` is the correct default and a host that never
	// encodes still gets a right answer.
	bool floatIsSurfaceEncoded = false;

	// IMG_TYPE_RGBA_16F only: the largest LINEAR channel value anywhere in the
	// image, 1.0 meaning SDR reference white. 0 = not measured.
	//
	// HDR10 mastering metadata describes the content's actual peak, and a
	// constant would describe someone else's content. Tracked INSIDE the
	// per-pixel loop each producer already runs -- never by a second O(pixels)
	// pass, which would be exactly the bulk work MT_ASSERT_NOT_RENDER_THREAD
	// exists to keep off the render thread.
	float contentMaxComponent = 0.0f;

	// GPU-compressed payload (KTX2/UASTC path). Empty/0 for the normal RGBA path.
	bool isCompressed;                          // true <=> type == IMG_TYPE_GPU_COMPRESSED
	u8 compressedGpuFormat;                     // EImageGpuFormat value of the transcoded blocks
	int compressedMipCount;                     // number of valid entries in compressedMips
	SCompressedMip *compressedMips;             // owned array, compressedMipCount entries, mip 0 = largest
	void DeallocCompressed();                   // frees compressedMips + all blockData (leak-free on reload)

	void AllocImage(/*bool allocOrig,*/ bool allocTemp, bool allocResult);
	void AllocTempImage();	// additional alloc if necessary
	void AllocResultImage();
	void DeallocTemp();
	void DeallocResult();
	void DeallocImage();

	u8 getImageType();
	void setImageType(u8 type);
	void setResultImage(u8 *data, u8 type);

	// grayscale
	u8 GetPixelResultByte(int x, int y);
	void SetPixelResultByte(int x, int y, u8 val);
	u8 GetPixelResultByteSafe(int x, int y);
	void SetPixelResultByteSafe(int x, int y, u8 val);
	u8 GetPixelResultByteBorder(int x, int y);
	u8 GetPixelTemporaryByte(int x, int y);
	void SetPixelTemporaryByte(int x, int y, u8 val);
	u8 *getGrayscaleResultData();
	u8 *getGrayscaleTemporaryData();
	void setGrayscaleResultData(u8 *data);
	// rgb
//	void GetPixel(int x, int y, u8 *r, u8 *g, u8 *b);
	void GetPixelResultRGB(int x, int y, u8 *r, u8 *g, u8 *b);
	void SetPixelResultRGB(int x, int y, u8 r, u8 g, u8 b);
	void GetPixelTemporaryRGB(int x, int y, u8 *r, u8 *g, u8 *b);
	void SetPixelTemporaryRGB(int x, int y, u8 r, u8 g, u8 b);
	u8 *getRGBResultData();
	void setRGBResultData(u8 *data);
	// rgba
	void GetPixel(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a);
	void GetPixelFloat(int x, int y, float *r, float *g, float *b, float *a);
	
	void GetPixelResultRGBA(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a);
	void SetPixel(int x, int y, u8 r, u8 g, u8 b, u8 a);
	void SetPixelResultRGBA(int x, int y, u8 r, u8 g, u8 b, u8 a);
	void GetPixelTemporaryRGBA(int x, int y, u8 *r, u8 *g, u8 *b, u8 *a);
	void SetPixelTemporaryRGBA(int x, int y, u8 r, u8 g, u8 b, u8 a);
	u8 *getRGBAResultData();

	// The pixel pointer for a GPU UPLOAD, which may legitimately be RGBA8 or
	// RGBA16F -- the resident funnel decided which, and the backend was told.
	//
	// Deliberately NOT a relaxation of getRGBAResultData(): that one's
	// strictness is what has been catching type mistakes all over the engine,
	// and widening it would throw that away everywhere to serve one caller.
	// Returns NULL rather than fatal-exiting for anything else, so an upload
	// path can refuse instead of taking the process down.
	u8 *getResultDataForUpload();
	void setRGBAResultData(u8 *data);
	// cielab
	void GetPixelResultCIELAB(int x, int y, int *l, int *a, int *b);
	void SetPixelResultCIELAB(int x, int y, int l, int a, int b);
	void GetPixelTemporaryCIELAB(int x, int y, int *l, int *a, int *b);
	void SetPixelTemporaryCIELAB(int x, int y, int l, int a, int b);
	int *getCIELABResultData();
	void setCIELABResultData(int *data);
	// IMG_TYPE_GRAYSCALE_16BIT -- one 16-bit channel
	short unsigned int GetPixelResultGrayscale16Bit(int x, int y);
	void SetPixelResultGrayscale16Bit(int x, int y, short unsigned int val);
	short unsigned int GetPixelTemporaryGrayscale16Bit(int x, int y);
	void SetPixelTemporaryGrayscale16Bit(int x, int y, short unsigned int val);
	short unsigned int *getGrayscale16BitResultData();
	void setGrayscale16BitResultData(short unsigned int *data);
	// IMG_TYPE_GRAYSCALE_32BIT -- one 32-bit channel
	long unsigned int GetPixelResultGrayscale32Bit(int x, int y);
	void SetPixelResultGrayscale32Bit(int x, int y, long unsigned int val);
	long unsigned int GetPixelTemporaryGrayscale32Bit(int x, int y);
	void SetPixelTemporaryGrayscale32Bit(int x, int y, long unsigned int val);
	long unsigned int *getGrayscale32BitResultData();
	void setGrayscale32BitResultData(long unsigned int *data);

	void copyTemporaryToResult();
	void copyResultToTemporary()	;

	void ConvertToByte();
	void ConvertToByte(u8 componentNum);
	void ConvertToGrayscale();
	void ConvertToGrayscale(u8 componentNum);
	// Distinct colours -> class indices, as IMG_TYPE_GRAYSCALE_16BIT.
	void ConvertToGrayscale16BitCount();
	void ConvertToGrayscale16Bit();
	void ConvertToRGBA();
	void ConvertToRGB();

	uint8 *GetResultDataAsRGBA();
	
	void DrawImage(CImageData *drawImage, int x, int y, int width, int height, float alpha);
	
	int GetDataLength();
	void Save(const char *fileName);
	void SaveScaled(const char *fileName, short int min, short int max);
	bool Load(const char *fileName, bool dealloc);
	// Retry a failed JPEG with the file repaired in memory (missing EOI).
	bool LoadRepairedJpeg(const char *fileName);
	bool LoadKTX2(const char *fileName);  // KTX2/UASTC decode: compressed blocks or RGBA32 fallback
	// PNG through libpng, not stb_image. Measured 1.21x faster with NEON on a
	// 4000x3000 RGBA file, byte-identical output -- but the reasons that
	// matter are not speed: it returns the embedded ICC profile in the SAME
	// pass (stb discards all metadata, so the caller had to re-read the file
	// header just for the profile), and it is the reference implementation,
	// continuously fuzzed, facing what is by definition untrusted input.
	// Falls back to the stb chain in Load() when it refuses a file.
	bool LoadPNG(const char *fileName);

	// 16-bit colour access. GetPixelResultRGBA answers in 8 bits for any type
	// (scaling a 16-bit image down); these two are for callers that want the
	// precision, and for the one place that must give it up -- the GPU upload.
	bool GetPixelResultRGBA16Bit(int x, int y, unsigned short *r, unsigned short *g,
	                          unsigned short *b, unsigned short *a);
	// The 16-bit writer. Added with S-5 because there was a getter and no
	// setter, so nothing outside a decoder could build a 16-bit image at all.
	void SetPixelResultRGBA16Bit(int x, int y, unsigned short r, unsigned short g,
	                          unsigned short b, unsigned short a);
	bool ConvertRGBA16BitToRGBA8();

	// ---- IMG_TYPE_RGBA_16F (S-5) ----------------------------------------
	//
	// CONVENIENCE ACCESSORS, NOT THE HOT PATH. Storage is half, so every call
	// here does a software half<->float conversion: right for a test or a
	// one-off probe, wrong for 24 million pixels. The producers (the develop
	// chain, the video poster extractor, the HDR HEIC arm) write half
	// DIRECTLY with FloatToHalf and must never route through these. Two
	// conversion paths exist on purpose; naming which is which is what stops
	// the slow one being reached for later.
	bool GetPixelResultFloat(int x, int y, float *r, float *g, float *b, float *a);
	void SetPixelResultFloat(int x, int y, float r, float g, float b, float a);

	// unorm16 -> half. 65535 lands exactly on 1.0 and 0 on 0.0.
	bool ConvertRGBA16BitToRGBA16F();

	// half -> RGBA8, tone-mapping rather than clipping whatever sits above
	// 1.0. BULK, O(pixels): decode workers only, never the render thread
	// (MT_ASSERT_NOT_RENDER_THREAD guards it).
	//
	// `headroom` is passed IN by the caller -- CImageData never samples the
	// display itself, so tests drive any headroom without one and the policy
	// stays at the call site.
	//
	// WHAT IT MEANS, precisely: the INPUT value that maps to output white.
	//   headroom == 1.0  -> the identity on 0..1, so the SDR body comes out
	//                       exactly where it does today; anything above 1.0
	//                       clips, which is also what today does.
	//   headroom  > 1.0  -> 0..headroom is compressed into 0..1, so values
	//                       above white stay SEPARABLE from each other instead
	//                       of all flattening to 255. That separation is the
	//                       whole reason to tone-map rather than clamp: a
	//                       photographer judging a frame needs to see that one
	//                       highlight is hotter than another. The body darkens
	//                       slightly in exchange, which is the trade.
	//
	// Note this is NOT "the display can show more, so make it brighter": an
	// 8-bit output has no headroom by definition -- 255 is its maximum
	// whatever the display does. The parameter buys highlight DETAIL, not
	// brightness.
	//
	// `inputIsLinear` says whether the half pixels are linear or already
	// surface-encoded (the S-5 Phase 2 contract puts resident float in the
	// surface's encoding), because the tone-map has to happen in LINEAR: when
	// false the converter decodes with SrgbExtendedDecode first, maps, then
	// re-encodes.
	//
	// It undoes the surface's TRANSFER only, not its PRIMARIES: a
	// surface-encoded buffer bound for a Display P3 layer also carries P3
	// primaries, which this re-encodes as sRGB. Unreachable in the app today
	// -- an open gate implies a backend that takes float, so this converter
	// never runs on such a buffer -- and called out rather than silently
	// assumed, because the parameter's name promises more than it delivers.
	// A host that DOES reach it wants a primaries argument here.
	bool ConvertRGBA16FToRGBA8(float headroom, bool inputIsLinear);
	// Writes IMG_TYPE_GRAYSCALE_16BIT / IMG_TYPE_RGBA_16BIT as a real 16-bit PNG.
	// Save() routes to it automatically; exposed for callers that want the
	// success/failure answer.
	bool SavePNG16(const char *fileName);
	bool LoadTIFF(const char *fileName);
	bool LoadWebP(const char *fileName);
	bool LoadHEIF(const char *fileName);
	// ---- HEIF availability, as DATA for the host ------------------------
	//
	// "Can this machine decode HEIF at all?" -- answerable WITHOUT opening a
	// file, and distinct from "did this file decode". On Windows a missing
	// HEVC Video Extensions makes every HEIC fail identically until the user
	// installs it: a system property, not a file property.
	//
	// Ask the system-level question ONCE and keep the answer. A host that only
	// ever learns "this file failed" ends up repeating a system-level message
	// per file -- hundreds of times for a folder of HEICs. If a design needs a
	// "show once" flag to stay tolerable, the design is wrong.
	//
	// Nothing here raises UI or returns a human sentence: decode runs on worker
	// threads, and this engine ships no localized strings (there is no
	// assets/locale here). The host resolves the key through the same
	// translate-hook seam SetImGuiTitleTranslateFunc uses. Rationale in full:
	// CImageDataHEIF.cpp.
	enum EHeifAvailability
	{
		HEIF_AVAILABLE = 0,
		// Windows: the HEVC Video Extensions package is absent. THE USER CAN
		// FIX THIS -- GetHeifCodecInstallUrl() is non-NULL in this state only.
		HEIF_UNAVAILABLE_SYSTEM_CODEC_MISSING,
		// No backend compiled in (Linux without libheif, or HEIF gated off).
		// The user can do nothing about it; it is a property of the build.
		HEIF_UNAVAILABLE_NOT_BUILT
	};
	static EHeifAvailability GetHeifAvailability();
	// Convenience over the above, for callers that only need the yes/no.
	static bool IsHEIFDecodeAvailable();
	// i18n KEY for the current state, or NULL when HEIF is available. Never an
	// English sentence -- see the note above.
	static const char *GetHeifAvailabilityI18nKey();
	// Where the user can get the missing codec, or NULL when there is nothing
	// to install (every state except the Windows one, and every platform
	// except Windows). Data, not a link the engine ever opens itself.
	static const char *GetHeifCodecInstallUrl();
	// ImageIO (Apple) / WIC (Windows) decode of ANY format the platform reads.
	// Named for what it is, not for the format it was first written for.
	bool LoadWithImageIO_Apple(const char *fileName);
	// The HDR arm (S-5, macOS 14+ only, fails closed). True ONLY when the file
	// carries a gain map AND the float decode succeeded -- product is
	// IMG_TYPE_RGBA_16F, LINEAR sRGB primaries, 1.0 = SDR reference white.
	// Any other outcome returns false with the image untouched and the caller
	// falls through to the ordinary 8-bit path.
	bool LoadWithImageIO_AppleHDR(const char *fileName);
	bool LoadWithWIC_Windows(const char *fileName);
	// Last resort: hand the file to the operating system's own decoder after
	// ours has refused it. Returns false on platforms without one (Linux).
	bool LoadWithPlatformDecoder(const char *fileName);
	bool LoadHEIF_libheif(const char *fileName);
	bool LoadAVIF(const char *fileName);
	bool LoadRAWPreview(const char *fileName);

	// RD-B #4.2: extract the XMP packet LibRaw already parses at open_file
	// (TIFF tag 700 for DNG/TIFF-based RAWs; CR3 uuid box; RAF) WITHOUT a
	// decode. Declared UNCONDITIONALLY, following LoadRAWPreview:
	// MT_ENABLE_LIBRAW is a PRIVATE engine define invisible to the app, so a
	// guarded declaration would be uncallable from PhotoCruise. The
	// implementation is guarded with an #else stub returning false.
	// Returns true when a packet was found; outXmp gets the packet text
	// (strnlen-bounded -- xmplen is NOT trustworthy across LibRaw's three
	// fill sites). A LIBRAW_FILE_UNSUPPORTED open with a packet still yields
	// the packet (a plain TIFF, or the missing-libjpeg DNG case).
	static bool ReadEmbeddedXmp(const char *utf8FileName, std::string *outXmp);
	const char *GetLoadError();
	void RawSave(const char *fileName);
	void RawLoad(const char *fileName);

	void LoadFromByteBufferUncompressed(CByteBuffer *byteBuffer);
	void StoreToByteBufferUncompressed(CByteBuffer *byteBuffer);

	void StoreToByteBuffer(CByteBuffer *byteBuffer, int compressionType);
	static CImageData *GetFromByteBuffer(CByteBuffer *byteBuffer);

	// temporary here -> move to image filters
	void EraseContent(u8 r, u8 g, u8 b, u8 a);
	void FlipVertically();
	void Scale(float scaleX, float scaleY);
	void DrawLine(int startX, int startY, int endX, int endY, u8 r, u8 g, u8 b);
	void DrawLine(int startX, int startY, int endX, int endY, u8 r, u8 g, u8 b, u8 a, int thickness);
	void DrawFilledRectangle(int leftX, int topY, int rightX, int bottomY, u8 r, u8 g, u8 b, u8 a);

	void CopyDataFrom(CImageData *src);
	
	bool isInsideCircularMask(int x, int y);

	void debugPrint();

private:
	png_bytep *row_pointers;
};


#endif /*IMG_CIMAGEDATA_H_*/
