#ifndef _VIDEOFRAMETRANSFORM_H_
#define _VIDEOFRAMETRANSFORM_H_

#pragma once

#include "SYS_Defs.h"

// Shared RGBA pixel-space transforms used by every video consumer that needs
// to present a decoded frame in *display* orientation rather than the
// decoder's *coded* orientation. Originally a private static on CVideoPlayer
// (Task 6); promoted here (Task 9) so CVideoFrameExtractor's poster-frame
// path can apply the exact same rotation without duplicating the pixel math
// or its carefully-verified sign convention.
namespace VideoFrameTransform
{
	// Rotates a WxH RGBA buffer into dst by rotationDeg (0/90/180/270 -- see
	// SVideoInfo::rotationDegrees for the sign convention; the pixel
	// transform applied for 90/270 is the *counter*-clockwise rotation by
	// rotationDeg, empirically verified against tests/fixtures/video/
	// h264_rot90.mp4 -- see the .cpp for the full rationale). dst must be
	// sized for the (possibly swapped) output dimensions: W*H for 180 (and
	// 0, though callers should just skip the call entirely when
	// rotationDeg==0), H*W for 90/270.
	// bytesPerPixel defaults to 4 (RGBA8); pass 8 for an RGBA16F buffer.
	// Rotating a float buffer at a 4-byte stride shears it into plausible
	// garbage rather than failing -- the S-4 capture-path lesson, again.
	void RotateRGBA(const u8 *src, int W, int H, int rotationDeg, u8 *dst,
					int bytesPerPixel = 4);
}

#endif
//_VIDEOFRAMETRANSFORM_H_
