#include "VideoFrameTransform.h"

#include <cstring>

// ============================================================================
// RotateRGBA -- rotates an RGBA buffer by 0/90/180/270 degrees.
//
// Sign convention: rotationDeg matches SVideoInfo::rotationDegrees (the
// unnegated, normalized av_display_rotation_get() value / ffprobe's
// "rotation" field). The pixel transform that correctly displays the frame
// for rotationDeg==90 is a *counter*-clockwise 90 rotation of the coded
// pixels (and clockwise for rotationDeg==270) -- verified empirically
// against tests/fixtures/video/h264_rot90.mp4 (ffmpeg's own default
// autorotate, when transcoding that fixture, produces pixel-identical
// output to `transpose=cclock` applied to the un-rotated source, and
// substantially different output from `transpose=clock`).
//
// The remap math below mirrors PhotoCruise's PC_TransformOrientation (EXIF
// orientation 6 == rotate-90-CW, 8 == rotate-90-CCW), reimplemented locally
// so the engine has no dependency on PhotoCruise: rotationDeg==90 uses the
// orientation-8 (CCW) formula, rotationDeg==270 uses the orientation-6 (CW)
// formula.
//
// Promoted (Task 9) from CVideoPlayer::RotateRGBA (Task 6) into a shared,
// free function so CVideoFrameExtractor's poster-frame path can reuse it
// verbatim instead of re-deriving/duplicating the rotation math. Behavior is
// unchanged from the original CVideoPlayer implementation.
// ============================================================================
void VideoFrameTransform::RotateRGBA(const u8 *src, int W, int H, int rotationDeg, u8 *dst,
									 int bytesPerPixel)
{
	const size_t bpp = (size_t)(bytesPerPixel > 0 ? bytesPerPixel : 4);
	if (rotationDeg == 180)
	{
		for (int oy = 0; oy < H; oy++)
		{
			for (int ox = 0; ox < W; ox++)
			{
				const u8 *p = src + ((size_t)(H - 1 - oy) * W + (W - 1 - ox)) * bpp;
				u8 *q = dst + ((size_t)oy * W + ox) * bpp;
				memcpy(q, p, bpp);
			}
		}
		return;
	}

	// 90/270: output dimensions are swapped (nW=H, nH=W).
	int nW = H, nH = W;
	for (int oy = 0; oy < nH; oy++)
	{
		for (int ox = 0; ox < nW; ox++)
		{
			int sx, sy;
			if (rotationDeg == 90)
			{
				// orientation-8 (CCW) formula
				sx = W - 1 - oy;
				sy = ox;
			}
			else // 270
			{
				// orientation-6 (CW) formula
				sx = oy;
				sy = H - 1 - ox;
			}
			const u8 *p = src + ((size_t)sy * W + sx) * bpp;
			u8 *q = dst + ((size_t)oy * nW + ox) * bpp;
			memcpy(q, p, bpp);
		}
	}
}
