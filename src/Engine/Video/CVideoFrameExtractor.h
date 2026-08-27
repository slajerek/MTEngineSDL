#ifndef _CVIDEOFRAMEEXTRACTOR_H_
#define _CVIDEOFRAMEEXTRACTOR_H_

#pragma once

// Entire header is a no-op unless the platform build links the bundled
// FFmpeg dylibs -- mirrors CVideoSourceFFmpeg.h's own guard so non-macOS
// builds can still #include this header and compile translation units that
// reference it conditionally.
#if MT_ENABLE_FFMPEG

#include "IVideoSource.h"
#include <string>
#include <functional>

class CImageData;

// Static poster-frame + info-probe API consumed by the decode pool (worker
// threads) and by scan/thumbnail code. Both entry points are pure functions
// with no shared mutable state -- each call opens (and fully owns/closes)
// its own private CVideoSourceFFmpeg, so any number of calls may run
// concurrently on different threads against different files (or even the
// same file) safely.
class CVideoFrameExtractor
{
public:
	// Decodes one display-oriented RGBA frame at timeSec (caller-relative,
	// 0 == clip start; clamped to [0, duration]).
	//
	// maxDim: 0 = native display size; otherwise the larger of the two
	// display dimensions is scaled down to maxDim (fit-within, aspect
	// preserved, never upscaled).
	//
	// Internally: CVideoSourceFFmpeg::Seek() is documented (Task 5) to land
	// on the frame AFTER the requested target, so this seeks to a margin
	// BEFORE timeSec and decodes forward until a frame's pts reaches the
	// target (falling back to the first decoded frame on failure). Both
	// timeSec and every pts CVideoSourceFFmpeg reports are already
	// clip-relative (IVideoSource.h's clip-relative time contract -- Task 1
	// of the video playback plan), including containers with a nonzero
	// start_time (e.g. MPEG-TS/AVCHD): no anchor arithmetic is needed here,
	// a request at 0.5s of an MTS clip lands 0.5s into the clip's content
	// because CVideoSourceFFmpeg itself already normalizes away the
	// container's absolute clock.
	//
	// Returns nullptr and fills outError on failure (unopenable/corrupt
	// file, no decodable video frames, unsupported pixel format, ...).
	// Caller owns the returned CImageData (delete when done).
	//
	// outFramePts (optional): when non-null, receives the clip-relative pts
	// (IVideoSource.h contract -- 0 == clip start) of the frame actually
	// returned (the seek-and-decode-forward match, or the frame0 fallback
	// when the match wasn't found/converted).
	//
	// acceptFirstFrameAfterSeek: when true, the decode-forward loop returns
	// the FIRST decodable frame at the seek landing (the keyframe at/before
	// timeSec) instead of walking the GOP to the exact target pts. The GOP
	// walk is the dominant poster cost (hundreds of ms of 4K decode per
	// poster during a fly-through preload), and for a glimpse/thumbnail the
	// nearest keyframe is as good as the exact frame; outFramePts still
	// reports the pts actually returned. Default false = historical
	// exact-frame behavior, byte-identical.
	static CImageData *DecodeFrameRGBA(const char *path, double timeSec, int maxDim,
										std::string &outError, double *outFramePts = nullptr,
										bool acceptFirstFrameAfterSeek = false,
										bool hdrToLinearFloat = false);

	// Single-open variant: opens `path` once, probes SVideoInfo, then calls
	// pickTimeSec(info) exactly once to choose the target time (e.g. a
	// poster-time rule that needs the duration) -- replaces the
	// ReadVideoInfo() + DecodeFrameRGBA() double open. `outInfo` (optional)
	// is filled as soon as the container opens (so callers get
	// duration/codec even when frame decode fails), refreshed after the
	// first decoded frame (fullRange/colorSpace become known then), and left
	// untouched when the container itself fails to open.
	static CImageData *DecodeFrameRGBA(const char *path,
										const std::function<double(const SVideoInfo &)> &pickTimeSec,
										int maxDim, std::string &outError,
										SVideoInfo *outInfo = nullptr,
										double *outFramePts = nullptr,
										bool acceptFirstFrameAfterSeek = false,
										// PERMISSION, not a command: when true AND the
										// opened source reports colorTrc 16 (PQ) or 18
										// (HLG), the returned CImageData is
										// IMG_TYPE_RGBA_16F, LINEAR sRGB primaries,
										// 1.0 = SDR reference white (203 nit, BT.2408) --
										// the app applies the surface's primaries and
										// encoding. In every other case, including an SDR
										// clip with this set, the return is IMG_TYPE_RGBA
										// exactly as today.
										bool hdrToLinearFloat = false);

	// Header-only probe: opens the container, reads stream/codec metadata
	// (SVideoInfo), and closes -- no frame is decoded. Fast enough to call
	// from scan threads on large directories.
	static bool ReadVideoInfo(const char *path, SVideoInfo &outInfo);
};

#endif // MT_ENABLE_FFMPEG

#endif
//_CVIDEOFRAMEEXTRACTOR_H_
