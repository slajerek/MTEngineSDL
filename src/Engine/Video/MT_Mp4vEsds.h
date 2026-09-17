#ifndef _MT_MP4VESDS_H_
#define _MT_MP4VESDS_H_

#pragma once

// An MPEG-4 elementary-stream descriptor (`esds`) built from nothing but the
// codec's own configuration bytes.
//
// WHY THIS EXISTS. VideoToolbox will decode MPEG-4 Part 2 -- measured, 30
// frames of real picture -- but it will not do it from a bare format
// description: `CMVideoFormatDescriptionCreate(kCMVideoCodecType_MPEG4Video)`
// with no extensions yields kVTVideoDecoderBadDataErr (-12909). It wants the
// `esds` atom that MP4 carries in its sample description.
//
// AVI has no `esds`. What it has is the MPEG-4 VOS/VOL header sitting as
// trailing bytes after the BITMAPINFOHEADER in the stream's `strf` chunk --
// the same bytes an MP4 muxer would wrap. So the conversion is a wrapper, not
// a translation: this builds the descriptor around them.
//
// MEASURED, NOT DERIVED FROM THE SPEC ALONE. The layout here was read out of a
// working file: ffmpeg's `esds` for a remuxed AVI is 88 bytes, and the one
// this function builds from the same VOL bytes is byte-for-byte 88 bytes and
// VideoToolbox creates a decompression session from it.
//
// PLATFORM-NEUTRAL on purpose. It is pure byte manipulation, so it is
// compiled and tested everywhere even though only the Apple path consumes it.
// Windows does not need it at all -- Media Foundation takes the raw VOL header
// through MF_MT_USER_DATA -- which is precisely why this must not live inside
// an Apple-only file where nothing could test it.

#include "SYS_Defs.h"
#include <vector>
#include <cstddef>

namespace MT_Mp4vEsds
{
	// `vol` is the MPEG-4 Visual configuration (VOS/VOL header). Returns the
	// complete `esds` atom PAYLOAD -- version/flags followed by the ES
	// descriptor -- ready to hand to CMVideoFormatDescriptionCreate under the
	// "esds" key. Empty on invalid input; a caller must treat that as "no
	// configuration", not as an empty-but-valid one.
	std::vector<u8> Build(const u8 *vol, size_t volLen);
}

#endif
//_MT_MP4VESDS_H_
