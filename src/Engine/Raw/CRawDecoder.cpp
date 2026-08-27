#include "CRawDecoder.h"

#include <cstring>

// Only this implementation file is guarded (#5.0, #9): CMakeLists.txt:80-87
// force-disables MT_ENABLE_LIBRAW when the image-codec bundle is absent, so
// the implementation degrades to the stub below rather than failing to
// compile -- the CImageDataRAW.cpp pattern. The header stays unconditional.
#if MT_ENABLE_LIBRAW

#include <libraw/libraw.h>
#include "DBG_Log.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32) || defined(WIN32)
#include "SYS_FileUtf8.h"
#endif

static bool RawFail(SRawDecodeResult *out, std::string *outError,
					const std::string &reason)
{
	if (out)
		*out = SRawDecodeResult();
	if (outError)
		*outError = reason;
	return false;
}

static float RawMsSince(std::chrono::steady_clock::time_point t0)
{
	return std::chrono::duration<float, std::milli>(
		std::chrono::steady_clock::now() - t0).count();
}

// ALL parameters are assigned before open_file(): use_camera_matrix and
// use_camera_wb are consumed inside identify(), which open_file calls --
// setting them later silently changes which matrix you get (#5.2). The one
// documented exception is params.cropbox (#5.6), whose inputs do not exist
// until identify() has run.
static void RawConfigureParams(LibRaw &raw, const CRawDecoder::SOptions &opt)
{
	libraw_output_params_t &p = raw.imgdata.params;

	p.output_bps = 16;               // the premise
	p.gamm[0] = 1.0;                 // linear: the default 0.45/4.5 would bake
	p.gamm[1] = 1.0;                 //   an sRGB-ish curve into data RD-C tone-maps
	p.no_auto_bright = 1;            // per-image gain from histogram stats
	// CRITICAL: adjust_maximum() would otherwise derive the output scale from
	// the picture's own content (lowers C.maximum toward data_maximum);
	// < 0.00001 returns early (utils_libraw.cpp:453-454).
	p.adjust_maximum_thr = 0.0f;
	// CRITICAL and least obvious (#5.3 trap 1): with use_camera_wb=1 and no
	// cam_mul in the file, LibRaw runs its greybox auto-WB scan -- the exact
	// non-reproducibility no_auto_bright/adjust_maximum_thr are pinned
	// against. This bit makes it fall back to daylight multipliers instead.
	raw.imgdata.rawparams.options |= LIBRAW_RAWOPTIONS_CAMERAWB_FALLBACK_TO_DAYLIGHT;
	p.bright = 1.0;                  // multiplies output via imax; pinned
	p.output_color = 0;              // raw camera colour -- the matrix is DATA (#6)
	p.highlight = 1;                 // "unclipped": normalise by the LARGEST
	                                 //   multiplier or R/B clip at the top and
	                                 //   highlight recovery dies (#5.4). Note
	                                 //   whiteLevel therefore does NOT map to 65535.
	// CRITICAL: default -1 keeps the file's EXIF orientation, and the app's
	// tiff:Orientation path would rotate AGAIN -- the double rotation roadmap
	// #2.12 exists to prevent. 0 also keeps the S.flip dimension swap off.
	p.user_flip = 0;
	p.use_fuji_rotate = 0;           // fuji_rotate()/stretch() change dimensions
	p.no_auto_scale = 0;             // scale_colors must run
	p.use_auto_wb = 0;               // necessary but NOT sufficient (trap 1)
	p.use_camera_matrix = 1;         // file's embedded matrix wins over
	                                 //   adobe_coeff; RD-D revisits (#12.4)
	p.user_qual = opt.demosaicQuality;
	p.half_size = opt.halfSize ? 1 : 0;

	if (opt.applyCameraWB)
	{
		p.use_camera_wb = 1;
	}
	else
	{
		// Unity must be EXPLICIT: with use_camera_wb=0 and user_mul[0]==0,
		// pre_mul keeps the D65/daylight multipliers, not 1 (#5.3 trap 2).
		p.use_camera_wb = 0;
		p.user_mul[0] = p.user_mul[1] = p.user_mul[2] = p.user_mul[3] = 1.f;
	}

	// Raw-INPUT bound, not a scratch ceiling (#7). 0 = leave LibRaw's 2048 MB
	// default alone -- writing 0 through would TOOBIG everything.
	if (opt.maxRawMemoryMB > 0)
		raw.imgdata.rawparams.max_raw_memory_mb = (unsigned)opt.maxRawMemoryMB;
}

// Everything after a successful open, shared by Decode() and DecodeBayer()
// so generated fixtures exercise the production path.
static bool RawDecodeOpened(LibRaw &raw, const CRawDecoder::SOptions &opt,
							bool isBayerFixture, SRawDecodeResult *out,
							std::string *outError)
{
	// Fail early, before unpacking a file we are going to reject. Only
	// colors==3 is supported: 4 (CMYG/sRAW) is never collapsed under
	// output_color=0, and 1 (monochrome) has no 3x3 camToXYZ meaning (#5.1).
	if (raw.imgdata.idata.colors != 3)
	{
		char msg[64];
		snprintf(msg, sizeof(msg), "unsupported: %d-channel sensor",
				 raw.imgdata.idata.colors);
		return RawFail(out, outError, msg);
	}

	// ---- PRE captures: parse-time facts, read before LibRaw mutates them.
	// On the common path raw2image_ex folds the black level in and ZEROES
	// C.black/C.cblack, so these pre values are the sensor's levels (#5.3).
	const libraw_colordata_t &col = raw.imgdata.color;
	for (int i = 0; i < 4; i++)
	{
		out->asShotWB[i] = col.cam_mul[i];
		out->cblackPre[i] = col.cblack[i];
	}
	out->blackLevelPre = col.black;
	out->whiteLevelPre = col.maximum;
	out->baselineExposure = col.dng_levels.baseline_exposure;
	out->hasBaselineExposure = (col.dng_levels.baseline_exposure > -998.f);
	if (!out->hasBaselineExposure)
		out->baselineExposure = 0.f;
	if (raw.imgdata.idata.xmpdata != nullptr && raw.imgdata.idata.xmplen > 0)
		out->xmpPacket.assign(raw.imgdata.idata.xmpdata,
		                      (size_t)raw.imgdata.idata.xmplen);
	strncpy(out->cameraMake, raw.imgdata.idata.make,
			sizeof(out->cameraMake) - 1);
	strncpy(out->cameraModel, raw.imgdata.idata.model,
			sizeof(out->cameraModel) - 1);
	out->colors = raw.imgdata.idata.colors;

	// ---- DNG calibration provenance (#5.7): RD-D's input, a plain copy of
	// parse-time facts. Absence travels as the parsedfields bitmask, never a
	// zero matrix. Only the 3x3 / 3-vector prefixes are kept: colors==3 is
	// guaranteed above. Two things RD-D reads from here later, recorded at
	// the source: LibRaw has ALREADY folded CameraCalibration*AnalogBalance
	// into rgb_cam on the DNG path (identify.cpp:1400-1418), so RD-C applies
	// neither; and the calibration-signature tags 0xC6F3/0xC6F4 are not
	// parsed by LibRaw 0.22.1 at all -- RD-D reads those itself.
	// SPEC-vs-SOURCE CORRECTION (found at implementation, probe-verified):
	// imgdata.color.dng_color[i].parsedfields is NEVER populated by LibRaw
	// 0.22.1 -- identify_process_dng_fields copies the VALUES field-by-field
	// (COPYARR, identify.cpp:1472-1560) and drops the per-IFD mask, so the
	// public mask reads 0 for every DNG. The design's "copy the mask" is
	// unimplementable as written. Reconstructed here instead, which is sound
	// because (a) the merge itself is GATED on the per-IFD mask, so a value
	// present in imgdata.color was genuinely parsed, and (b) for each of
	// these fields the all-zero encoding is illegal as a real value (a
	// ColorMatrix must be invertible, a calibration is near-diagonal, a
	// neutral has positive components) -- so zero => flagged ABSENT, and
	// RD-D never consumes a zero matrix as data. That is the opposite of the
	// zero-read-as-identity trap the design warns about.
	for (int ill = 0; ill < 2; ill++)
	{
		const libraw_dng_color_t &dc = col.dng_color[ill];
		unsigned mask = 0;
		bool cmZero = true, fmZero = true, ccZero = true;
		for (int r = 0; r < 3; r++)
			for (int c = 0; c < 3; c++)
			{
				out->dngColorMatrix[ill][r][c] = dc.colormatrix[r][c];
				out->dngForwardMatrix[ill][r][c] = dc.forwardmatrix[r][c];
				out->dngCalibration[ill][r][c] = dc.calibration[r][c];
				if (dc.colormatrix[r][c] != 0.f)   cmZero = false;
				if (dc.forwardmatrix[r][c] != 0.f) fmZero = false;
				if (dc.calibration[r][c] != 0.f)   ccZero = false;
			}
		if (!fmZero) mask |= LIBRAW_DNGFM_FORWARDMATRIX;
		// Fidelity gap of the reconstruction, accepted: EXIF LightSource 0
		// ("Unknown") is a legal CalibrationIlluminant value that reads as
		// absent here. An Unknown illuminant is unusable for temperature
		// interpolation anyway, so RD-D loses nothing actionable.
		if (dc.illuminant != 0 && dc.illuminant != LIBRAW_WBI_None)
			mask |= LIBRAW_DNGFM_ILLUMINANT;
		if (!cmZero) mask |= LIBRAW_DNGFM_COLORMATRIX;
		if (!ccZero) mask |= LIBRAW_DNGFM_CALIBRATION;
		out->dngFields[ill] = mask;
		out->dngIlluminant[ill] = dc.illuminant;
	}
	// AnalogBalance: guarded copy, and the guard is LOAD-BEARING on exactly
	// one path (probe-verified both ways): open_file's init sets
	// dng_levels.analogbalance to {1,1,1,1} (init_close_utils.cpp:191) --
	// the DNG-spec identity for an absent tag, so on that path absent and
	// explicit-identity are indistinguishable and no absence signal can
	// exist for this field -- but open_bayer's initdata() leaves it {0,0,0},
	// and copying zeros would hand RD-D a nonsense balance. All-zero is
	// illegal as a real value, so: zero -> keep the {1,1,1} default.
	// The distinct imgdata.other.analogbalance is a Canon-specific cam_mul
	// echo, one dot away and a different thing; do not touch it.
	if (col.dng_levels.analogbalance[0] != 0.f
		|| col.dng_levels.analogbalance[1] != 0.f
		|| col.dng_levels.analogbalance[2] != 0.f)
	{
		for (int c = 0; c < 3; c++)
			out->dngAnalogBalance[c] = col.dng_levels.analogbalance[c];
	}
	out->hasAsShotNeutral = (col.dng_levels.asshotneutral[0] > 0.f
							 && col.dng_levels.asshotneutral[1] > 0.f
							 && col.dng_levels.asshotneutral[2] > 0.f);
	if (out->hasAsShotNeutral)
		for (int c = 0; c < 3; c++)
			out->dngAsShotNeutral[c] = col.dng_levels.asshotneutral[c];
	strncpy(out->uniqueCameraModel, col.UniqueCameraModel,
			sizeof(out->uniqueCameraModel) - 1);

	// ---- DNG DefaultCrop (#5.6) -- the ONE parameter written after open,
	// because raw_inset_crops and the margins do not exist until identify()
	// has run. LibRaw parses the tag but never applies it; the bridge is
	// ours. Notes that took source-reading to get right:
	//  - absence sentinel is cleft/ctop == 0xffff, NOT zero -- a zero origin
	//    is an ordinary crop;
	//  - raw_inset_crops[0] is in RAW coordinates (identify_process_dng_fields
	//    sets cleft = left_margin + origin), params.cropbox is VISIBLE-AREA:
	//    subtract the margins;
	//  - LibRaw only PROMOTES the DNG tag into raw_inset_crops when
	//    origin+size fits STRICTLY inside the raw frame
	//    (identify.cpp:1439-1441) -- a malformed or exactly-full-frame crop
	//    never reaches this code and decodes uncropped;
	//  - clamp before writing anyway (Sony fills raw_inset_crops directly at
	//    parse time with no such gate): LibRaw throws BAD_CROP on an empty
	//    result AFTER recycle(), a whole failed decode over a checkable
	//    detail;
	//  - never halve for halfSize: cropbox is consumed before IO.shrink, so
	//    the shrink applies to the already-cropped size (halving here would
	//    crop twice);
	//  - width/height are read back from the POST-process state, never from
	//    these numbers: X-Trans snaps the origin /6*6, Fuji rotated layouts
	//    /4*4, so requested and applied can differ by up to 5 px --
	//    defaultCropRequested keeps the pre-snap rectangle so a framing
	//    discrepancy is diagnosable rather than mysterious.
	{
		const libraw_raw_inset_crop_t &ins = raw.imgdata.sizes.raw_inset_crops[0];
		out->hasDefaultCrop = (ins.cleft != 0xffff && ins.ctop != 0xffff
							   && ins.cwidth > 0 && ins.cheight > 0);
		if (opt.applyDefaultCrop && out->hasDefaultCrop)
		{
			const libraw_image_sizes_t &S = raw.imgdata.sizes;
			long cx = (long)ins.cleft - S.left_margin;
			long cy = (long)ins.ctop - S.top_margin;
			long cw = ins.cwidth;
			long ch = ins.cheight;
			if (cx < 0) { cw += cx; cx = 0; }
			if (cy < 0) { ch += cy; cy = 0; }
			if (cw > (long)S.width - cx)  cw = (long)S.width - cx;
			if (ch > (long)S.height - cy) ch = (long)S.height - cy;
			if (cw > 0 && ch > 0)
			{
				raw.imgdata.params.cropbox[0] = (unsigned)cx;
				raw.imgdata.params.cropbox[1] = (unsigned)cy;
				raw.imgdata.params.cropbox[2] = (unsigned)cw;
				raw.imgdata.params.cropbox[3] = (unsigned)ch;
				out->defaultCropRequested[0] = (unsigned)cx;
				out->defaultCropRequested[1] = (unsigned)cy;
				out->defaultCropRequested[2] = (unsigned)cw;
				out->defaultCropRequested[3] = (unsigned)ch;
				out->defaultCropApplied = true;
			}
			// empty after clamping: leave cropbox alone, applied stays false
		}
	}

	if (isBayerFixture)
	{
		// open_bayer never sets cam_mul, so without this every synthetic
		// fixture would take the auto-WB path -- or, with the daylight
		// fallback bit, daylight multipliers derived from a matrix the
		// fixture does not have. Unity is the reproducible choice (#5.3).
		libraw_output_params_t &p = raw.imgdata.params;
		p.user_mul[0] = p.user_mul[1] = p.user_mul[2] = p.user_mul[3] = 1.f;
	}

	// ---- unpack
	auto t0 = std::chrono::steady_clock::now();
	int rc = raw.unpack();
	out->msUnpack = RawMsSince(t0);
	if (rc != LIBRAW_SUCCESS)
	{
		// On the exception paths recycle() has already run: read NOTHING
		// from imgdata after any failure (#5.5).
		if (rc == LIBRAW_TOO_BIG || rc == LIBRAW_UNSUFFICIENT_MEMORY)
			return RawFail(out, outError, "out of memory");
		char msg[48];
		snprintf(msg, sizeof(msg), "unpack failed: %d", rc);
		return RawFail(out, outError, msg);
	}

	// camToXYZ / hasMatrix -- captured HERE, after unpack() and before
	// dcraw_process(), and nowhere else (#6.2). Two traps this placement and
	// source avoid: convert_to_rgb forces raw_color=1 under output_color=0,
	// so a post-process read is false for every file forever; and
	// imgdata.color.cam_xyz is written only by adobe_coeff's table -- the
	// DNG/TIFF path installs its matrix via cmatrix->rgb_cam without touching
	// cam_xyz, so a cam_xyz test is wrong in both directions. raw_color is
	// cleared by every real-matrix path, which makes it the right sentinel.
	// (unpack can also SET it -- load_mfbacks for non-CFA backs -- a genuine
	// "no matrix" signal this read respects.)
	out->hasMatrix =
		(raw.get_internal_data_pointer()->internal_output_params.raw_color == 0);
	if (out->hasMatrix)
	{
		// rgb_cam is camera -> sRGB, already D65-row-normalised by
		// cam_xyz_coeff (rows of cam_rgb sum to 1; sRGB white IS D65 -- #6.1).
		// Lift to XYZ with the standard sRGB->XYZ(D65) matrix, declared here
		// so the derivation is self-contained. Do NOT re-normalise anything:
		// the tempting normalise-cam_xyz-rows-and-invert derivation lands on
		// illuminant E, a permanent colour cast (#6.1) -- CTestRawMatrixD65
		// exists to keep this exact mistake out.
		static const float SRGB_TO_XYZ_D65[3][3] = {
			{ 0.4124564f, 0.3575761f, 0.1804375f },
			{ 0.2126729f, 0.7151522f, 0.0721750f },
			{ 0.0193339f, 0.1191920f, 0.9503041f },
		};
		for (int x = 0; x < 3; x++)
			for (int cc = 0; cc < 3; cc++)
			{
				float v = 0.f;
				for (int r = 0; r < 3; r++)
					v += SRGB_TO_XYZ_D65[x][r] * col.rgb_cam[r][cc];
				out->camToXYZ[x][cc] = v;
			}
	}

	// ---- dcraw_process
	t0 = std::chrono::steady_clock::now();
	rc = raw.dcraw_process();
	out->msProcess = RawMsSince(t0);
	if (rc != LIBRAW_SUCCESS)
	{
		if (rc == LIBRAW_TOO_BIG || rc == LIBRAW_UNSUFFICIENT_MEMORY)
			return RawFail(out, outError, "out of memory");
		char msg[48];
		if (rc == LIBRAW_BAD_CROP)
		{
			// A bug in OUR cropbox conversion -- #5.6 clamps before writing,
			// so this should be unreachable; the requested rectangle is in
			// defaultCropRequested for the log.
			snprintf(msg, sizeof(msg), "default crop rejected");
		}
		else
		{
			snprintf(msg, sizeof(msg), "decode failed: %d", rc);
		}
		return RawFail(out, outError, msg);
	}

	// ---- POST captures
	for (int i = 0; i < 4; i++)
	{
		out->cblackPost[i] = col.cblack[i];
		out->relativeWB[i] = col.pre_mul[i];
		out->linearMax[i] = col.linear_max[i];
	}
	out->blackLevelPost = col.black;
	out->whiteLevelPost = col.maximum;
	// relativeWB[c] * globalScale reconstructs scale_mul[c] -- valid only when
	// scale_colors took its normal branch. maximum==0 or an all-tiny pre_mul
	// means scale_mul was forced to 1.0; publish 0 rather than a wrong factor.
	{
		float maxPre = 0.f;
		for (int i = 0; i < 4; i++)
			if (col.pre_mul[i] > maxPre)
				maxPre = col.pre_mul[i];
		if (col.maximum > 0 && maxPre > 0.00001f)
			out->globalScale = 65535.f / (float)col.maximum;
		else
			out->globalScale = 0.f;
	}

	// ---- output: our own buffer + copy_mem_image, which is both the memory
	// lever (skips dcraw_make_mem_image's ~270 MB duplicate; #7) and the
	// partial-failure detector (dcraw_make_mem_image ignores
	// copy_mem_image's return value and never sets errcode on success; #5.2).
	t0 = std::chrono::steady_clock::now();
	int w = 0, h = 0, c = 0, bps = 0;
	raw.get_mem_image_format(&w, &h, &c, &bps);
	if (w <= 0 || h <= 0 || c != 3 || bps != 16)
	{
		char msg[64];
		snprintf(msg, sizeof(msg), "image build failed: %dx%d c=%d bps=%d",
				 w, h, c, bps);
		return RawFail(out, outError, msg);
	}
	size_t stride = (size_t)w * 3 * sizeof(u16);
	u16 *buffer = (u16 *)malloc(stride * h);
	if (buffer == NULL)
		return RawFail(out, outError, "out of memory");
	rc = raw.copy_mem_image(buffer, (int)stride, 0 /* RGB, not BGR */);
	out->msCopy = RawMsSince(t0);
	if (rc != LIBRAW_SUCCESS)
	{
		free(buffer);
		char msg[48];
		snprintf(msg, sizeof(msg), "image build failed: %d", rc);
		return RawFail(out, outError, msg);
	}

	out->linear = buffer;
	out->owner = NULL;   // ours; FreeResult frees with free()
	out->width = w;
	out->height = h;
	return true;
}

// LibRaw's decode path overflows the 512 KB stack macOS gives secondary
// threads (verified: two concurrent decodes SIGBUS reliably on default
// pthread/std::thread stacks and are clean at 8 MB; the main thread's 8 MB
// masks it for single-threaded callers). Rather than documenting a fragile
// minimum for every future caller -- RD-E's develop lane would be the first
// to forget -- the decode body runs on an internal thread with a guaranteed
// stack. One thread spawn (~tens of microseconds) against a ~1 s decode is
// noise.
static const size_t RAW_DECODE_STACK_BYTES = 8u * 1024u * 1024u;

#if defined(_WIN32) || defined(WIN32)
#include <windows.h>
#include <process.h>
#include <cstdint>
struct SRawThreadTrampoline
{
	void (*fn)(void *);
	void *arg;
};
static unsigned __stdcall RawThreadEntry(void *p)
{
	SRawThreadTrampoline *t = (SRawThreadTrampoline *)p;
	t->fn(t->arg);
	return 0;
}
static bool RawRunWithBigStack(void (*fn)(void *), void *arg)
{
	SRawThreadTrampoline t{ fn, arg };
	uintptr_t h = _beginthreadex(NULL, (unsigned)RAW_DECODE_STACK_BYTES,
								 RawThreadEntry, &t,
								 STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
	if (h == 0)
		return false;
	WaitForSingleObject((HANDLE)h, INFINITE);
	CloseHandle((HANDLE)h);
	return true;
}
#else
#include <pthread.h>
struct SRawThreadTrampoline
{
	void (*fn)(void *);
	void *arg;
};
static void *RawThreadEntry(void *p)
{
	SRawThreadTrampoline *t = (SRawThreadTrampoline *)p;
	t->fn(t->arg);
	return NULL;
}
static bool RawRunWithBigStack(void (*fn)(void *), void *arg)
{
	SRawThreadTrampoline t{ fn, arg };
	pthread_attr_t attr;
	if (pthread_attr_init(&attr) != 0)
		return false;
	pthread_attr_setstacksize(&attr, RAW_DECODE_STACK_BYTES);
	pthread_t th;
	int rc = pthread_create(&th, &attr, RawThreadEntry, &t);
	pthread_attr_destroy(&attr);
	if (rc != 0)
		return false;
	pthread_join(th, NULL);
	return true;
}
#endif

static bool RawDecodeFileBody(const char *utf8FileName,
							  const CRawDecoder::SOptions &options,
							  SRawDecodeResult *out, std::string *outError)
{
	try
	{
		LibRaw raw;
		RawConfigureParams(raw, options);

		auto t0 = std::chrono::steady_clock::now();
#if defined(_WIN32) || defined(WIN32)
		// Wide overload -- guarded by _WIN32/WIN32 in libraw.h:198-206, NOT by
		// LIBRAW_WIN32_UNICODEPATHS. The narrow overload goes through the ANSI
		// code page and fails on non-ASCII names (the LoadRAWPreview defect,
		// CImageDataRAW.cpp:15); this API must not re-import that.
		std::wstring wide = SYS_Utf8ToWide(utf8FileName);
		int rc = raw.open_file(wide.c_str());
#else
		int rc = raw.open_file(utf8FileName);
#endif
		out->msOpen = RawMsSince(t0);
		if (rc != LIBRAW_SUCCESS)
		{
			// Checked BEFORE the generic failure: a lossy-JPEG DNG in a
			// jpeg-less build refuses to open with this warning set --
			// OUR bug, not the file's, and the diagnosis is the guard's
			// whole job (#4.4).
			if (raw.imgdata.process_warnings & LIBRAW_WARN_NO_JPEGLIB)
				return RawFail(out, outError, "libjpeg missing in LibRaw build");
			return RawFail(out, outError, "cannot open");
		}
		return RawDecodeOpened(raw, options, false, out, outError);
	}
	catch (...)
	{
		// Backstop only: LibRaw maps its exceptions to return codes before
		// they reach us (#5.5); this catches our own failures.
		return RawFail(out, outError, "internal error during raw decode");
	}
}

static bool RawDecodeBayerBody(const unsigned char *data, unsigned dataLen,
							   u16 width, u16 height,
							   const CRawDecoder::SOptions &options,
							   SRawDecodeResult *out, std::string *outError)
{
	try
	{
		LibRaw raw;
		RawConfigureParams(raw, options);

		auto t0 = std::chrono::steady_clock::now();
		int rc = raw.open_bayer(data, dataLen, width, height, 0, 0, 0, 0,
								0, LIBRAW_OPENBAYER_RGGB, 0, 0, 0);
		out->msOpen = RawMsSince(t0);
		if (rc != LIBRAW_SUCCESS)
			return RawFail(out, outError, "cannot open");
		return RawDecodeOpened(raw, options, true, out, outError);
	}
	catch (...)
	{
		return RawFail(out, outError, "internal error during raw decode");
	}
}

// Opt-in per-decode diagnostics (#8), the PC_SCRUB_DIAG_FILE mould: when
// PC_RAW_DIAG_FILE names a file, append one CSV line per successful decode.
// msProcess is one opaque number -- demosaic is not separable from
// scale_colors/convert_to_rgb from outside dcraw_process.
static void RawMaybeDumpDiag(const char *source,
							 const CRawDecoder::SOptions &opt,
							 const SRawDecodeResult &r)
{
	const char *path = getenv("PC_RAW_DIAG_FILE");
	if (path == NULL || path[0] == 0)
		return;
	FILE *f = fopen(path, "a");
	if (f == NULL)
		return;
	fprintf(f, "%s,%d,%d,%d,%d,%.2f,%.2f,%.2f,%.2f\n",
			source, r.width, r.height, opt.demosaicQuality,
			opt.halfSize ? 1 : 0, r.msOpen, r.msUnpack, r.msProcess, r.msCopy);
	fclose(f);
}

// The public entries validate on the caller's thread, then run the LibRaw
// body on the big-stack thread. If thread creation itself fails the body
// runs inline -- correct on a main thread, and strictly no worse than not
// having the guard at all.
struct SRawDecodeJob
{
	const char *utf8FileName = NULL;
	const unsigned char *bayerData = NULL;
	unsigned bayerLen = 0;
	u16 bayerW = 0, bayerH = 0;
	const CRawDecoder::SOptions *options = NULL;
	SRawDecodeResult *out = NULL;
	std::string *outError = NULL;
	bool result = false;
};

static void RawDecodeJobRun(void *p)
{
	SRawDecodeJob *j = (SRawDecodeJob *)p;
	if (j->utf8FileName != NULL)
		j->result = RawDecodeFileBody(j->utf8FileName, *j->options, j->out,
									  j->outError);
	else
		j->result = RawDecodeBayerBody(j->bayerData, j->bayerLen, j->bayerW,
									   j->bayerH, *j->options, j->out,
									   j->outError);
}

bool CRawDecoder::Decode(const char *utf8FileName, const SOptions &options,
						 SRawDecodeResult *out, std::string *outError)
{
	if (out == NULL)
		return false;
	*out = SRawDecodeResult();
	if (utf8FileName == NULL || utf8FileName[0] == 0)
		return RawFail(out, outError, "cannot open");

	SRawDecodeJob job;
	job.utf8FileName = utf8FileName;
	job.options = &options;
	job.out = out;
	job.outError = outError;
	bool ok;
	if (!RawRunWithBigStack(RawDecodeJobRun, &job))
		ok = RawDecodeFileBody(utf8FileName, options, out, outError);
	else
		ok = job.result;
	if (ok)
		RawMaybeDumpDiag(utf8FileName, options, *out);
	return ok;
}

bool CRawDecoder::DecodeBayer(const unsigned char *data, unsigned dataLen,
							  u16 width, u16 height, const SOptions &options,
							  SRawDecodeResult *out, std::string *outError)
{
	if (out == NULL)
		return false;
	*out = SRawDecodeResult();
	if (data == NULL || dataLen == 0 || width == 0 || height == 0)
		return RawFail(out, outError, "cannot open");

	SRawDecodeJob job;
	job.bayerData = data;
	job.bayerLen = dataLen;
	job.bayerW = width;
	job.bayerH = height;
	job.options = &options;
	job.out = out;
	job.outError = outError;
	bool ok;
	if (!RawRunWithBigStack(RawDecodeJobRun, &job))
		ok = RawDecodeBayerBody(data, dataLen, width, height, options, out,
								outError);
	else
		ok = job.result;
	if (ok)
		RawMaybeDumpDiag("<bayer>", options, *out);
	return ok;
}

void CRawDecoder::FreeResult(SRawDecodeResult *result)
{
	if (result == NULL)
		return;
	if (result->linear != NULL)
	{
		if (result->owner != NULL)
		{
			// The whole libraw_processed_image_t is one malloc'd block;
			// `linear` points into its payload and must not be freed itself.
			LibRaw::dcraw_clear_mem((libraw_processed_image_t *)result->owner);
		}
		else
		{
			free(result->linear);
		}
	}
	*result = SRawDecodeResult();
}

bool CRawDecoder::RunWithBigStack(void (*fn)(void *), void *arg)
{
	return RawRunWithBigStack(fn, arg);
}

bool CRawDecoder::IsAvailable()
{
	return true;
}

size_t CRawDecoder::EstimateScratchBytes(int fullWidth, int fullHeight,
										 bool halfSize)
{
	if (fullWidth <= 0 || fullHeight <= 0)
		return 0;
	size_t px = (size_t)fullWidth * (size_t)fullHeight;
	return px * (halfSize ? RAW_DECODE_SCRATCH_BYTES_PER_PIXEL_HALF
						  : RAW_DECODE_SCRATCH_BYTES_PER_PIXEL);
}

void CRawDecoder::GetBuildCapabilities(bool *hasLibRaw, bool *hasJpeg,
									   bool *hasZlib)
{
	unsigned caps = (unsigned)LibRaw::capabilities();
	if (hasLibRaw) *hasLibRaw = true;
	if (hasJpeg)   *hasJpeg = (caps & LIBRAW_CAPS_JPEG) != 0;
	if (hasZlib)   *hasZlib = (caps & LIBRAW_CAPS_ZLIB) != 0;
}

#else // !MT_ENABLE_LIBRAW -------------------------------------------------

bool CRawDecoder::Decode(const char *, const SOptions &,
						 SRawDecodeResult *out, std::string *outError)
{
	if (out)
		*out = SRawDecodeResult();
	if (outError)
		*outError = "raw decode not available in this build";
	return false;
}

bool CRawDecoder::DecodeBayer(const unsigned char *, unsigned, u16, u16,
							  const SOptions &, SRawDecodeResult *out,
							  std::string *outError)
{
	if (out)
		*out = SRawDecodeResult();
	if (outError)
		*outError = "raw decode not available in this build";
	return false;
}

void CRawDecoder::FreeResult(SRawDecodeResult *result)
{
	if (result != NULL)
		*result = SRawDecodeResult();
}

bool CRawDecoder::IsAvailable()
{
	return false;
}

size_t CRawDecoder::EstimateScratchBytes(int fullWidth, int fullHeight,
										 bool halfSize)
{
	if (fullWidth <= 0 || fullHeight <= 0)
		return 0;
	size_t px = (size_t)fullWidth * (size_t)fullHeight;
	return px * (halfSize ? RAW_DECODE_SCRATCH_BYTES_PER_PIXEL_HALF
						  : RAW_DECODE_SCRATCH_BYTES_PER_PIXEL);
}

void CRawDecoder::GetBuildCapabilities(bool *hasLibRaw, bool *hasJpeg,
									   bool *hasZlib)
{
	if (hasLibRaw) *hasLibRaw = false;
	if (hasJpeg)   *hasJpeg = false;
	if (hasZlib)   *hasZlib = false;
}

#endif // MT_ENABLE_LIBRAW
