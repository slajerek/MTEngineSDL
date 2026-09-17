#ifndef _MT_MediaPacket_h_
#define _MT_MediaPacket_h_

#include "SYS_Defs.h"
#include <cstdint>

// ENGINE-OWNED MEDIA TYPES, so the OS-native decoders stop depending on FFmpeg.
//
// The native decoders -- VideoToolbox HEVC, Media Foundation HEVC/WMV/WMA --
// are the DESTINATION of the FFmpeg removal: they are the licence-clean path a
// store build must take. They were nonetheless gated on MT_ENABLE_FFMPEG and
// four of them included <libavcodec/avcodec.h>, because DecodePacket took a
// const AVPacket * and read pkt->data/size/pts.
//
// So the hardware decoders were switched off by the switch that removes the
// thing they replace, and no MT_ENABLE_FFMPEG=0 configuration could be built at
// all. These two types are what breaks that circle. Measured before the change:
// the entire coupling was 11 lines across four files.

// One compressed access unit. Whoever demuxed it -- libavformat today,
// AVFoundation or Media Foundation later -- fills this in.
struct SVideoPacket
{
	// Absent timestamp. Chosen to equal FFmpeg's AV_NOPTS_VALUE so the
	// translation at the demuxer boundary is a copy, not a comparison.
	static const i64 kNoPts = INT64_MIN;

	const u8 *data;      // borrowed; valid only for the duration of the call
	int       size;
	i64       ptsUs;     // microseconds, or kNoPts
	bool      keyframe;

	SVideoPacket() : data(NULL), size(0), ptsUs(kNoPts), keyframe(false) {}
	SVideoPacket(const u8 *d, int s, i64 pts, bool key)
		: data(d), size(s), ptsUs(pts), keyframe(key) {}

	double PtsSeconds() const
	{
		return (ptsUs != kNoPts) ? (double)ptsUs / 1000000.0 : 0.0;
	}
};

// The codecs a native decoder has to tell apart. Deliberately NOT a mirror of
// AVCodecID: it lists only what the engine's own decoders switch on, so adding
// a demuxer means writing one translation function, not adopting an enumeration
// that belongs to somebody else's library.
enum class EMediaCodecId : int
{
	Unknown = 0,
	H264,
	HEVC,
	// AVI's usual payloads. MPEG4Part2 is DivX/XviD/FMP4/DX50 -- the FourCCs
	// differ, the bitstream does not, and VideoToolbox and Media Foundation
	// both decode it. NOT to be confused with H264, which is MPEG-4 Part 10.
	MPEG4Part2,
	MJPEG,
	WMV1,
	WMV2,
	WMV3,
	VC1,
	WMAV1,
	WMAV2,
	WMAPRO,
	AAC,
	// AVI/RIFF audio, identified by WAVEFORMATEX wFormatTag rather than by a
	// FourCC. PCM needs no decoder at all and is here so a demuxer can say
	// what it found in one vocabulary.
	MP3,
	MP2,
	AC3,
	PCM_S16LE,
};

#endif
//_MT_MediaPacket_h_
