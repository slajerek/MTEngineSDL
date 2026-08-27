#pragma once

// RD-A: full RAW decode -- 16-bit linear camera-space pixels plus the
// metadata needed to interpret them (design doc:
// PhotoCruise/specs/superpowers/specs/2026-08-06-rd-a-engine-foundation-design.md).
//
// THIS HEADER IS UNCONDITIONAL -- no MT_ENABLE_LIBRAW anywhere in it (#5.0).
// That define is engine-PRIVATE in all three build systems, so a guarded
// header would be an EMPTY FILE for PhotoCruise: a silently missing
// declaration, the worst failure shape available. Only CRawDecoder.cpp is
// guarded (with an #else stub), the CImageDataRAW.cpp / CCmsEngineFactory
// pattern. Callers branch on IsAvailable(), never on the define.

#include "SYS_Defs.h"
#include <string>

struct SRawDecodeResult
{
	// Demosaiced, 16-bit, LINEAR, camera-space RGB. Interleaved 3 channels,
	// width*height*3 u16. No alpha: a sensor has none.
	//
	// OWNERSHIP IS OPAQUE. `linear` may point into a block we malloc'd or into
	// LibRaw's libraw_processed_image_t, a struct hack (`unsigned char
	// data[1]`, libraw_types.h:185; malloc'd whole at mem_image.cpp:284-285)
	// whose payload CANNOT be freed independently -- dcraw_clear_mem is a plain
	// ::free of the whole pointer (mem_image.cpp:306-310). FreeResult()
	// branches on `owner`; callers must never free `linear`. See #7.
	u16  *linear     = nullptr;
	void *owner      = nullptr;
	int   width      = 0;
	int   height     = 0;

	// Camera RGB -> CIE XYZ (D65), row-major. Derived per #6, and CAPTURED
	// BETWEEN unpack() AND dcraw_process() -- see #6.2, this is not optional.
	float camToXYZ[3][3] = {};
	bool  hasMatrix      = false;

	// White-balance and scaling provenance. Capture points in #5.3.
	float asShotWB[4]    = {};                     // color.cam_mul, pre-process
	float relativeWB[4]  = { 1.f, 1.f, 1.f, 1.f }; // color.pre_mul, post-process
	float globalScale    = 1.f;                    // the 65535/(max-black) factor
	// Applied multiplier for channel c was relativeWB[c] * globalScale.
	// LibRaw's own combined scale_mul is a local in scale_colors and is not
	// recoverable; this pair reconstructs it. Valid only when scale_colors took
	// its normal branch -- see #5.3; globalScale == 0 means "not
	// reconstructable for this file", never a factor to apply.

	// Level provenance, captured BOTH sides of dcraw_process. Read #5.3 before
	// using the "post" values: on the common path they are ZERO by design --
	// raw2image_ex folds the black level in and zeroes C.black/C.cblack, so
	// the PRE values are the sensor's black level and RD-C must use those.
	unsigned blackLevelPre = 0,  blackLevelPost = 0;
	unsigned whiteLevelPre = 0,  whiteLevelPost = 0;
	unsigned cblackPre[4]  = {}, cblackPost[4]  = {};
	unsigned linearMax[4]  = {};   // color.linear_max, per-channel saturation

	// DNG DefaultCrop (#5.6). APPLIED, not merely surfaced: `linear` is already
	// the cropped rectangle and width/height are its dimensions. These fields
	// are PROVENANCE -- what was requested and what LibRaw actually did, which
	// are not always the same (X-Trans/Fuji snap the origin, #5.6).
	bool     hasDefaultCrop     = false;  // the tag was present (sentinel 0xffff)
	bool     defaultCropApplied = false;  // and we successfully cropped to it
	unsigned defaultCropRequested[4] = {}; // x,y,w,h, visible-area coords, pre-snap

	float baselineExposure    = 0.f;   // color.dng_levels.baseline_exposure
	bool  hasBaselineExposure = false; // sentinel is -999.f, not 0

	// ---- DNG calibration provenance (#5.7). RD-D's input, and RD-C must NOT
	// re-apply any of it: on the DNG path LibRaw has ALREADY folded calibration
	// and analogBalance into rgb_cam, hence into camToXYZ. See #5.7.
	//
	// Absence is carried by dngFields (LIBRAW_DNGFM_* bits), NEVER by a zero
	// matrix. Non-DNG inputs leave all of it zeroed with dngFields == 0.
	// NOTE the mask is RECONSTRUCTED by CRawDecoder: LibRaw 0.22.1 never
	// propagates the per-IFD parsedfields into imgdata.color (verified
	// against identify.cpp:1472-1560 -- values merge, mask does not), so the
	// public mask is dead. Reconstruction is sound because the merge is
	// gated on the per-IFD mask and an all-zero value is illegal as a real
	// value for every field here.
	unsigned dngFields[2]     = {};   // FORWARDMATRIX|ILLUMINANT|COLORMATRIX|CALIBRATION bits
	unsigned dngIlluminant[2] = {};   // LIBRAW_WBI_*; LIBRAW_WBI_None == absent
	float dngColorMatrix[2][3][3]   = {};  // ColorMatrix1/2:   XYZ -> camera
	float dngForwardMatrix[2][3][3] = {};  // ForwardMatrix1/2: camera -> XYZ(D50)
	float dngCalibration[2][3][3]   = {};  // CameraCalibration1/2
	// NOTE: AnalogBalance has NO absence signal -- LibRaw initialises it to
	// identity, the same value the DNG spec assigns an absent tag, so absent
	// and explicit-identity are indistinguishable at the source. Consumers
	// just apply it; identity is a no-op either way.
	float dngAnalogBalance[3]       = { 1.f, 1.f, 1.f };
	float dngAsShotNeutral[3]       = {};  // NOT cam_mul; needed by NeutralToXY
	bool  hasAsShotNeutral          = false;
	char  uniqueCameraModel[64]     = {};  // color.UniqueCameraModel -- DCP match key
	// NOTE: CameraCalibrationSignature / ProfileCalibrationSignature are NOT
	// here because LibRaw 0.22.1 does not parse them at all. RD-D reads those
	// two ASCII tags from the DNG's IFD itself (#5.7 trap 2).

	// ---- Per-stage wall time (#8), filled unconditionally. msProcess is one
	// opaque number: demosaic is not separable from scale_colors /
	// convert_to_rgb from outside dcraw_process (pre/post_interpolate_cb
	// exist if that split is ever needed).
	float msOpen = 0.f, msUnpack = 0.f, msProcess = 0.f, msCopy = 0.f;

	int  colors          = 3;    // idata.colors; only 3 supported (#5.1)
	char cameraMake[64]  = {};
	char cameraModel[64] = {};

	// RD-E (the RD-B #4.2 collapse): the file's XMP packet, straight from
	// LibRaw's already-parsed idata.xmpdata -- so the develop lane's decode
	// is the ONE LibRaw open (ReadEmbeddedXmp would open the same file a
	// second time). Empty when the file carries none.
	std::string xmpPacket;
};

// Scratch budget (design #7), MEASURED 2026-08-07 on the maintainer's
// arm64 macOS machine via fresh-process ru_maxrss deltas around one
// Decode() (probe in the task-13 commit):
//   24.3 Mpx NEF  q3 full: 380 MB = 16.4 B/output-px
//   24.3 Mpx NEF  q0 full: 373 MB = 16.1 B/output-px
//   22.4 Mpx CR2  q3 full: 315 MB = 16.5 B/output-px
//   24.3 Mpx NEF  q3 half: 130 MB (raw stays full-sensor; image+output quarter)
// This matches the #7 model exactly: raw_image 2 B/px + imgdata.image
// 8 B/px + our output 6 B/px = 16 B/px (the copy_mem_image design already
// dropped dcraw_make_mem_image's extra 6 B/px). A future 100 Mpx body makes
// EstimateScratchBytes visibly ~1.7 GB rather than silently OOMing.
// NOT the same thing as SOptions::maxRawMemoryMB, which bounds only the
// raw-INPUT allocation inside LibRaw.
static const unsigned RAW_DECODE_SCRATCH_BYTES_PER_PIXEL = 17;      // full decode
static const unsigned RAW_DECODE_SCRATCH_BYTES_PER_PIXEL_HALF = 6;  // halfSize, per FULL-sensor px

class CRawDecoder
{
public:
	struct SOptions
	{
		// params.user_qual. 3 selects DIFFERENT ALGORITHMS per sensor family:
		// AHD on Bayer, but dcraw_process.cpp:179-184 routes XTRANS to
		// xtrans_interpolate(quality > 2 ? 3 : 1). Benchmarks must say which.
		int  demosaicQuality = 3;
		bool halfSize        = false;   // saves ~3x, not 4x -- see #7
		// #5.3 -- ALSO affects WHICH colour matrix camToXYZ holds: with
		// use_camera_matrix=1 and a non-DNG raw, use_camera_wb=0 rejects the
		// file's embedded matrix in favour of adobe_coeff (trap 3).
		bool applyCameraWB   = true;
		// DNG DefaultCrop via params.cropbox (#5.6). DEFAULT TRUE: RD-C's
		// golden comparison requires ACR's framing. False exists for the
		// dimension tests and for diagnosing a suspect crop, not for callers.
		bool applyDefaultCrop = true;
		// LibRaw's raw-INPUT bound, not a scratch ceiling (#7). 0 means "leave
		// LibRaw's default of 2048 MB alone" -- it is NOT written through as 0,
		// which would throw LIBRAW_EXCEPTION_TOOBIG on everything.
		int  maxRawMemoryMB  = 0;
	};

	// Synchronous. Returns false with `out` zeroed on failure; `outError` gets
	// a short reason for the log and the badge.
	//
	// Threading: LibRaw state is per-instance, so separate calls on separate
	// threads are safe. One instance is NOT safe to share. Stack: LibRaw's
	// decode overflows the 512 KB default stack of macOS secondary threads
	// (verified SIGBUS under two concurrent decodes), so the body runs on an
	// internal 8 MB-stack thread -- callers may invoke from ANY thread
	// without stack ceremony; the spawn cost is noise against the decode.
	//
	// Path encoding: takes UTF-8, converts to wchar_t on Windows for LibRaw's
	// wide open_file. That overload is guarded by
	// `#if defined(_WIN32) || defined(WIN32)` (libraw/libraw.h:198-206) -- NOT
	// by LIBRAW_WIN32_UNICODEPATHS, which gates the datastream implementation.
	// Guarding on the wrong macro is an easy mistake. LoadRAWPreview uses the
	// narrow overload today (CImageDataRAW.cpp:15) and fails on non-ASCII
	// names; new API must not re-import that.
	static bool Decode(const char *utf8FileName, const SOptions &options,
					   SRawDecodeResult *out, std::string *outError);

	// Test seam: decode a synthetic 16-bit Bayer mosaic via LibRaw's
	// open_bayer, sharing everything after open with Decode() -- so the
	// generated-fixture tests (linearity, missing-matrix, flat-field) exercise
	// the production configure/capture/process path. Sets user_mul={1,1,1,1}:
	// open_bayer never sets cam_mul, and without that every synthetic fixture
	// would take LibRaw's content-derived auto-WB path (#5.3 trap 1).
	// `data` is width*height u16 little-endian RGGB photosites.
	static bool DecodeBayer(const unsigned char *data, unsigned dataLen,
							u16 width, u16 height, const SOptions &options,
							SRawDecodeResult *out, std::string *outError);

	static void FreeResult(SRawDecodeResult *result);

	// Compile-time answer, delivered at runtime, because MT_ENABLE_LIBRAW is
	// not visible to PhotoCruise (#5.0). Callers and tests branch on this;
	// false is "unavailable in this build", never an error.
	static bool IsAvailable();

	// Scratch estimate for a full-sensor width x height decode -- RD-E's
	// single-slot policy number (roadmap #2.8).
	static size_t EstimateScratchBytes(int fullWidth, int fullHeight,
									   bool halfSize);

	// Build capabilities for the #4.4 startup log. LIBRAW_CAPS_ZLIB here is
	// the ONLY runtime signal zlib support has (no open-time warning exists
	// for it, unlike LIBRAW_WARN_NO_JPEGLIB). All false when the build has no
	// LibRaw at all.
	// RD-E: run `fn(arg)` on a thread with the guaranteed 8 MB stack and
	// join. EVERY LibRaw entry point needs this off the main thread (LibRaw
	// overflows the 512 KB macOS secondary-thread default -- the SIGBUS the
	// internal Decode thread exists for). Exported so other LibRaw callers
	// (ReadEmbeddedXmp) share the guard instead of rediscovering the crash.
	static bool RunWithBigStack(void (*fn)(void *), void *arg);

	static void GetBuildCapabilities(bool *hasLibRaw, bool *hasJpeg,
									 bool *hasZlib);
};
