#include "CVideoFrameExtractor.h"

#if MT_ENABLE_FFMPEG

#include "CVideoSourceFFmpeg.h"
#include "VideoFrameTransform.h"
#include "CVideoTransferFunctions.h"
#include "CImageData.h"
#include "DBG_Log.h"

extern "C" {
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace
{
	// Maps the closed EVideoPixelFormat enum (IVideoSource.h) back onto the
	// AVPixelFormat sws_getContext needs. CVideoSourceFFmpeg::EmitFrame()
	// collapses every source format it sees into one of these (falling back
	// to YUV420P via its own internal SwsContext for anything exotic), so
	// this switch is exhaustive against everything ReadVideoFrame() can ever
	// hand back.
	AVPixelFormat MapPixelFormat(EVideoPixelFormat fmt, bool fullRange)
	{
		switch (fmt)
		{
		case EVideoPixelFormat::YUV420P:   return fullRange ? AV_PIX_FMT_YUVJ420P : AV_PIX_FMT_YUV420P;
		case EVideoPixelFormat::YUV422P:   return fullRange ? AV_PIX_FMT_YUVJ422P : AV_PIX_FMT_YUV422P;
		case EVideoPixelFormat::YUV420P10: return AV_PIX_FMT_YUV420P10LE;
		case EVideoPixelFormat::YUV422P10: return AV_PIX_FMT_YUV422P10LE;
		case EVideoPixelFormat::NV12:      return AV_PIX_FMT_NV12;
		case EVideoPixelFormat::YUVA420P:  return AV_PIX_FMT_YUVA420P;
		case EVideoPixelFormat::RGBA:      return AV_PIX_FMT_RGBA;
		default:                           return AV_PIX_FMT_NONE;
		}
	}

	// Converts one already-decoded SDecodedVideoFrame to a display-oriented
	// (rotation-corrected, optionally maxDim-downscaled) CImageData via a
	// single SwsContext + sws_scale call (handling 4:2:0/4:2:2, 8/10-bit,
	// NV12, and alpha uniformly -- CPU cost is a non-issue for a one-shot
	// poster/thumbnail decode). Returns nullptr + outError on failure.
	//
	// NOTE: reads f.plane[]/f.stride[] -- per SDecodedVideoFrame's contract
	// these are only valid until the next ReadVideoFrame/Seek/Close on the
	// same source, so callers must invoke this before advancing the source
	// any further.
	// R'G'B' unorm16 (PQ or HLG encoded, BT.2020 primaries) -> linear sRGB
	// half-float, 1.0 = SDR reference white.
	//
	// THE MATHS MOVED to CVideoTransferFunctions.h (S-5 Phase 5, Task 1) so the
	// live playback shader uses the same constants this poster lane does. The
	// local PqEotf/HlgInverseOetf/OOTF/kBt2020ToSrgb copies that used to sit
	// here were DELETED rather than left beside the header -- two copies of a
	// transfer function is exactly how a poster and its own playing frames
	// drift apart, which is the defect Phase 5 exists to remove.
	static void HdrRgb64ToLinearHalf(const u16 *src, int w, int h, int srcStridePixels,
									 u16 *dst, int colorTrc)
	{
		// TABLE-DRIVEN (see CVideoTransferFunctions.h). The analytic chain costs
		// two powf per CHANNEL for PQ -- 3.5M powf on a 1024x576 poster, which
		// measured 56 ms in Release against 6.6 ms for the same-size SDR
		// poster. This path is on the browse engine, and since the t=0 still
		// also takes it, every HDR clip entry paid it twice.
		//
		// The code values are handed to the table UNNORMALISED: they are
		// already the exact 16-bit index, so there is no divide, no multiply
		// and no rounding between the pixel and its answer.
		const u16 halfOne = FloatToHalf(1.0f);
		for (int y = 0; y < h; y++)
		{
			const u16 *srow = src + (size_t)y * srcStridePixels * 4;
			u16 *drow = dst + (size_t)y * w * 4;
			for (int x = 0; x < w; x++)
			{
				const u16 *code = srow + x * 4;
				float out[3];
				VideoTransfer::HdrCodeToLinearSrgb(code, colorTrc, out);

				for (int c = 0; c < 3; c++)
					drow[x * 4 + c] = FloatToHalf(out[c]);
				drow[x * 4 + 3] = halfOne;
			}
		}
	}

	CImageData *ConvertFrameToRGBA(const SDecodedVideoFrame &f, const SVideoInfo &info,
									int maxDim, std::string &outError,
									bool hdrToLinearFloat)
	{
		if (f.width <= 0 || f.height <= 0 || f.plane[0] == nullptr)
		{
			outError = "decoded frame has invalid dimensions or no data";
			return nullptr;
		}

		AVPixelFormat srcFmt = MapPixelFormat(f.pixelFormat, info.fullRange);
		if (srcFmt == AV_PIX_FMT_NONE)
		{
			outError = "unsupported decoded pixel format";
			return nullptr;
		}

		const int codedW = f.width;
		const int codedH = f.height;

		// Fit-within downscale is computed in *display* space (post-rotation)
		// per the maxDim contract (0 = native; else larger display dim scales
		// to maxDim, aspect preserved, never upscaled), then mapped back onto
		// the pre-rotation coded-space dimensions sws_scale actually produces
		// -- rotation is applied to the sws output afterward.
		const bool swapDims = (info.rotationDegrees == 90 || info.rotationDegrees == 270);
		const int dispW = swapDims ? codedH : codedW;
		const int dispH = swapDims ? codedW : codedH;

		int outDispW = dispW, outDispH = dispH;
		if (maxDim > 0)
		{
			const int larger = std::max(dispW, dispH);
			if (larger > maxDim)
			{
				const double scale = (double)maxDim / (double)larger;
				outDispW = std::max(1, (int)std::lround(dispW * scale));
				outDispH = std::max(1, (int)std::lround(dispH * scale));
			}
		}

		const int scaledCodedW = swapDims ? outDispH : outDispW;
		const int scaledCodedH = swapDims ? outDispW : outDispH;

		// The float path takes the SAME sws conversion with a wider
		// destination: RGBA64LE keeps the source's 10 bits instead of
		// truncating to 8, and the pixels come back still PQ/HLG-encoded and
		// still BT.2020 -- sws applies the YUV matrix, never a transfer
		// function. The EOTF and the primaries change are ours, below.
		//
		// `hdrToLinearFloat` is PERMISSION, not a command: an SDR clip takes
		// the ordinary 8-bit path however the flag is set, because there is no
		// above-white in it to preserve.
		const bool wantFloat = hdrToLinearFloat &&
							   (info.colorTrc == 16 || info.colorTrc == 18);
		const AVPixelFormat dstFmt = wantFloat ? AV_PIX_FMT_RGBA64LE : AV_PIX_FMT_RGBA;
		const int dstBytesPerPixel = wantFloat ? 8 : 4;

		SwsContext *sws = sws_getContext(codedW, codedH, srcFmt,
										  scaledCodedW, scaledCodedH, dstFmt,
										  SWS_BILINEAR, nullptr, nullptr, nullptr);
		if (!sws)
		{
			outError = "sws_getContext failed";
			return nullptr;
		}

		// CM-E: select the YUV->RGB matrix from the source's NORMALIZED colour
		// space (info.colorSpace is already the engine's VPX_CS_* convention --
		// do NOT re-normalize it here) instead of swscale's default (BT.601
		// regardless of content). Without this, a BT.709 clip's poster/still is
		// matrix-shifted against its own live frames. The return value is
		// deliberately ignored: sws_setColorspaceDetails returns -1 for
		// conversions it cannot retune (e.g. an RGBA source frame), where no
		// YUV matrix is in play anyway.
		const int swsSel = (info.colorSpace == 5) ? SWS_CS_BT2020
						 : (info.colorSpace == 2) ? SWS_CS_ITU709
												  : SWS_CS_ITU601;
		sws_setColorspaceDetails(sws,
								 sws_getCoefficients(swsSel), info.fullRange ? 1 : 0,
								 sws_getCoefficients(SWS_CS_DEFAULT), /*dstRange*/ 1,
								 /*brightness*/ 0, /*contrast*/ 1 << 16, /*saturation*/ 1 << 16);

		std::vector<u8> scaledRGBA((size_t)scaledCodedW * (size_t)scaledCodedH * dstBytesPerPixel);
		const uint8_t *srcPlanes[4] = { f.plane[0], f.plane[1], f.plane[2], f.plane[3] };
		int srcStrides[4] = { f.stride[0], f.stride[1], f.stride[2], f.stride[3] };
		uint8_t *dstPlanes[4] = { scaledRGBA.data(), nullptr, nullptr, nullptr };
		int dstStrides[4] = { scaledCodedW * dstBytesPerPixel, 0, 0, 0 };

		sws_scale(sws, srcPlanes, srcStrides, 0, codedH, dstPlanes, dstStrides);
		sws_freeContext(sws);

		const int finalW = outDispW;
		const int finalH = outDispH;

		// The float lane converts BEFORE rotating, so the rotation moves the
		// finished 8-byte pixels rather than half-converted ones.
		std::vector<u8> converted;
		const u8 *rotateSrc = scaledRGBA.data();
		if (wantFloat)
		{
			converted.resize((size_t)scaledCodedW * (size_t)scaledCodedH * 8);
			HdrRgb64ToLinearHalf((const u16 *)scaledRGBA.data(), scaledCodedW, scaledCodedH,
								 scaledCodedW, (u16 *)converted.data(), info.colorTrc);
			rotateSrc = converted.data();
		}

		u8 *finalPixels = nullptr;
		const size_t finalBytes = (size_t)finalW * finalH * dstBytesPerPixel;
		// THE ARRAY FORM MUST MATCH THE ALLOCATION. DeallocResult frees an
		// IMG_TYPE_RGBA_16F buffer as `delete [] (unsigned short int*)` and an
		// IMG_TYPE_RGBA one as `delete [] (u8*)`, so allocating float pixels as
		// `new u8[]` is a mismatched delete -- benign today only because
		// trivially-destructible arrays carry no cookie, and precisely the
		// invariant DeallocResult documents as "heap corruption rather than a
		// leak".
		auto allocFinal = [&]() -> u8 * {
			return wantFloat
				? (u8 *)new unsigned short int[(size_t)finalW * finalH * 4]
				: new u8[finalBytes];
		};
		if (info.rotationDegrees == 0)
		{
			// No rotation needed -- hand the buffer straight to CImageData
			// (setRGBAResultData transfers ownership).
			finalPixels = allocFinal();
			memcpy(finalPixels, rotateSrc, finalBytes);
		}
		else
		{
			finalPixels = allocFinal();
			// dstBytesPerPixel, not 4: the rotation copies whole pixels, and
			// an 8-byte pixel moved 4 bytes at a time shears the image.
			VideoFrameTransform::RotateRGBA(rotateSrc, scaledCodedW, scaledCodedH,
											 info.rotationDegrees, finalPixels,
											 dstBytesPerPixel);
		}

		CImageData *img = new CImageData(finalW, finalH,
										 wantFloat ? IMG_TYPE_RGBA_16F : IMG_TYPE_RGBA);
		img->setRGBAResultData(finalPixels);
		if (wantFloat)
		{
			// The engine's product is LINEAR (the Phase 2 contract); the app
			// applies the surface's primaries and encoding afterwards.
			img->floatIsSurfaceEncoded = false;
			float peak = 0.0f;
			const u16 *px = (const u16 *)finalPixels;
			const size_t n = (size_t)finalW * finalH * 4;
			for (size_t i = 0; i < n; i += 4)
				for (int c = 0; c < 3; c++)
				{
					const float v = HalfToFloat(px[i + c]);
					if (v > peak) peak = v;
				}
			img->contentMaxComponent = peak;
		}
		return img;
	}
} // namespace

// ============================================================================
// DecodeFrameRGBA
// ============================================================================
CImageData *CVideoFrameExtractor::DecodeFrameRGBA(const char *path,
												   const std::function<double(const SVideoInfo &)> &pickTimeSec,
												   int maxDim, std::string &outError,
												   SVideoInfo *outInfo, double *outFramePts,
												   bool acceptFirstFrameAfterSeek,
												   bool hdrToLinearFloat)
{
	outError.clear();

	CVideoSourceFFmpeg src;
	// Posters never need audio: skip the audio decoder so the decode-forward
	// below doesn't decode + queue interleaved audio it always throws away
	// (and doesn't trip PushPendingAudio's cap-overflow warning).
	src.SetOpenVideoOnly(true);
	if (!src.Open(path))
	{
		outError = src.GetErrorReason().empty() ? "failed to open video" : src.GetErrorReason();
		return nullptr;
	}

	if (outInfo)
		*outInfo = src.Info();

	SDecodedVideoFrame frame0;
	if (!src.ReadVideoFrame(frame0))
	{
		outError = src.GetErrorReason().empty() ? "no decodable video frames" : src.GetErrorReason();
		src.Close();
		return nullptr;
	}

	// Captured AFTER the first ReadVideoFrame(), not right after Open():
	// since CM-E, FillInfo() seeds colorSpace/fullRange from the container's
	// codecpar at Open(), but frame-level tags (EmitFrame()) refine them per
	// decoded frame and pix-fmt-derived full-range (e.g. MJPEG's YUVJ420P)
	// is only known once a frame decodes -- an info snapshot taken before
	// any frame is decoded could still mis-tag such sources as limited-range
	// in MapPixelFormat() below. This
	// copy is stable for the rest of the call: Seek()/ReadVideoFrame() may
	// update src's live SVideoInfo further (colorSpace changes frame to
	// frame in principle) but fullRange/dimensions/rotation are fixed for
	// the whole file, which is all this function relies on afterward.
	const SVideoInfo info = src.Info();
	if (outInfo)
		*outInfo = info;
	const double timeSec = pickTimeSec(info);

	std::string convErr;
	CImageData *fallbackImg = ConvertFrameToRGBA(frame0, info, maxDim, convErr, hdrToLinearFloat);
	if (!fallbackImg)
	{
		outError = convErr.empty() ? "pixel conversion failed" : convErr;
		src.Close();
		return nullptr;
	}

	// CVideoSourceFFmpeg's pts (and Seek()'s argument) are clip-relative
	// (IVideoSource.h contract -- 0 == clip start), exactly like timeSec here,
	// so no anchor arithmetic is needed: `rel` is directly both the Seek()
	// target domain and the domain f.pts below is compared against.
	double rel = timeSec;
	if (rel < 0.0)
		rel = 0.0;
	if (info.duration > 0.0 && rel > info.duration)
		rel = info.duration;

	// Margin big enough to comfortably cover Seek()'s "lands after the
	// target" behavior plus a typical short-GOP keyframe interval; the
	// forward-decode loop below is what actually pins down the target frame,
	// so this only needs to be "safely early", not precise.
	//
	// NOT an adaptive/retry margin (investigated for Plan-2 Task 3, kept
	// fixed -- YAGNI): CVideoSourceFFmpeg::Seek() already does BACKWARD
	// keyframe seeking plus its own bounded margin-escalation retry on
	// EOF-before-target (see Seek()'s marginCandidates loop, Task 1), and the
	// while() loop below decodes forward from wherever Seek() lands until it
	// reaches `rel`, independent of how close the seek landed. Empirically
	// (h264_longgop.mp4 fixture, PhotoCruise CTestVideoExtractor -- a 4s clip
	// with its only keyframe at t=0): every seekTarget in [0, duration]
	// lands on that same sole keyframe regardless of kMarginSeconds's value,
	// so widening the margin here can never change where Seek() lands or how
	// this function's result is chosen -- there is no reachable case where a
	// bigger margin fixes an otherwise-wrong answer.
	//
	// One genuine overshoot WAS found during this investigation (not fixable
	// here): requesting rel <= kMarginSeconds on a short-GOP MPEG-TS fixture
	// (h264_ac3.mts, 1s clip) makes seekTarget clamp to 0.0 for every margin
	// value, and CVideoSourceFFmpeg::Seek(0.0)'s own BACKWARD av_seek_frame
	// on that file lands past the true first keyframe onto the *second* one
	// (a byte-position-estimate overshoot, same root cause the marginCandidates
	// comment in Seek() documents for a clip's tail, reproduced here at a
	// clip's head instead) -- and because Seek()'s internal target is
	// `seconds - 0.05` (trivially satisfied by any frame when seconds==0), its
	// own EOF-retry never fires, so Seek() "succeeds" already overshot. Since
	// seekTarget is floor-clamped at 0.0 independent of kMarginSeconds, no
	// value of this margin changes that outcome -- a fix (if warranted) would
	// have to live inside Seek() itself, not here. Left as a known finding.
	constexpr double kMarginSeconds = 0.5;
	const double seekTarget = std::max(0.0, rel - kMarginSeconds);

	CImageData *result = nullptr;
	double resultPts = 0.0;
	// rel ~ 0 (near clip start) means frame0 is already the answer -- skip
	// the redundant seek/decode-forward round trip entirely. Fast-poster
	// mode uses the fast Seek variant too: the interface Seek() performs its
	// own internal GOP walk to `seekTarget - 0.05` (that is where the poster
	// cost actually lives), so accepting the first frame in the loop below
	// without also stopping that internal walk would save nothing.
	if (rel > 1e-6 && src.Seek(seekTarget, acceptFirstFrameAfterSeek))
	{
		SDecodedVideoFrame f;
		while (src.ReadVideoFrame(f))
		{
			// Fast-poster mode: the first decodable frame at the seek
			// landing (the keyframe) is the answer -- skip the GOP walk to
			// the exact target pts (see the header contract).
			if (acceptFirstFrameAfterSeek || f.pts >= rel - 0.05)
			{
				std::string ignoredErr; // any failure here just means "keep the fallback"
				result = ConvertFrameToRGBA(f, info, maxDim, ignoredErr, hdrToLinearFloat);
				resultPts = f.pts;
				break;
			}
		}
		// Loop exhausted (EOF) without reaching the target, or the matching
		// frame failed to convert: outError stays empty, `result` stays
		// null, and the caller falls through to the frame0 fallback below --
		// this is the documented "or first frame on failure" behavior.
	}

	src.Close();

	if (result)
	{
		delete fallbackImg;
		if (outFramePts)
			*outFramePts = resultPts;
		return result;
	}
	if (outFramePts)
		*outFramePts = frame0.pts;
	return fallbackImg;
}

CImageData *CVideoFrameExtractor::DecodeFrameRGBA(const char *path, double timeSec, int maxDim,
												  std::string &outError, double *outFramePts,
												  bool acceptFirstFrameAfterSeek,
												  bool hdrToLinearFloat)
{
	return DecodeFrameRGBA(path,
						   [timeSec](const SVideoInfo &) { return timeSec; },
						   maxDim, outError, nullptr, outFramePts,
						   acceptFirstFrameAfterSeek, hdrToLinearFloat);
}

// ============================================================================
// ReadVideoInfo
// ============================================================================
bool CVideoFrameExtractor::ReadVideoInfo(const char *path, SVideoInfo &outInfo)
{
	CVideoSourceFFmpeg src;
	// Header-only probe: no frame (let alone audio) is ever decoded, so
	// don't pay the audio-decoder open either. info.hasAudio stays truthful
	// (stream presence from container metadata).
	src.SetOpenVideoOnly(true);
	if (!src.Open(path))
	{
		src.Close();
		return false;
	}

	outInfo = src.Info();
	src.Close();
	return true;
}

#endif // MT_ENABLE_FFMPEG
