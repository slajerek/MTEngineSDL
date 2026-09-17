#include "IVideoPacketDecoder.h"

// Fallback definitions for IVideoPacketDecoder's two static availability
// probes, on the platforms that have no native decoder to answer them.
//
// WHY THIS FILE EXISTS AT ALL, and why it must never be FFmpeg-gated.
//
// These two stubs used to live at the end of CVideoSourceFFmpeg.cpp, whose
// comment explained the placement as "kept alongside the interface's only
// other FFmpeg-enabled TU so the symbol always exists once FFmpeg is linked
// in". That reasoning was sound only while FFmpeg was unconditionally linked.
// The whole point of the media-codecs programme is that it stops being: at
// MT_ENABLE_FFMPEG=0 the entire body of CVideoSourceFFmpeg.cpp compiles to
// nothing, and with it went IsWMVDecodeAvailable's only definition on macOS
// (VideoToolbox has no WMV/VC-1 support, so there is no Apple-side
// implementation file to host it) and BOTH definitions on Linux.
//
// Apps call IsWMVDecodeAvailable() from their test suites, so the
// symptom was a link failure in the one configuration the programme exists to
// produce -- and it went unnoticed because nobody had built that configuration.
// A capability's fallback must not be hosted by a translation unit the same
// capability switch can erase.
//
// The real implementations, which these defer to per platform:
//   Apple   -- CVideoDecoderHEVCVT.cpp  (HEVC: always true, VideoToolbox)
//   Windows -- CVideoDecoderHEVCMF.cpp  (HEVC: true iff an HEVC MFT resolves)
//              CVideoDecoderWMVMF.cpp   (WMV:  the in-box WMVideo Decoder MFT)

#if !defined(__APPLE__) && !defined(_WIN32)
// Builds with no native HEVC decoder story at all (Linux today) have no
// IVideoPacketDecoder implementation registered anywhere -- this is the
// "false stub elsewhere" IVideoPacketDecoder.h's comment refers to.
bool IVideoPacketDecoder::IsHEVCDecodeAvailable()
{
	return false;
}
#endif

#if !defined(_WIN32)
// Native WMV decode exists only on Windows. Apple AND Linux take this false
// stub: VideoToolbox has no WMV/VC-1 support at all, so unlike HEVC there is
// no Apple-side implementation file to host it.
bool IVideoPacketDecoder::IsWMVDecodeAvailable()
{
	return false;
}
#endif

// The one place in the engine allowed to answer this by asking FFmpeg, and the
// guard is the point: at MT_ENABLE_FFMPEG=0 the answer is a plain false, with
// no libavcodec header anywhere near a caller.
#if MT_ENABLE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
}
bool IVideoPacketDecoder::IsHEVCSoftwareDecodeAvailable()
{
	return avcodec_find_decoder(AV_CODEC_ID_HEVC) != nullptr;
}
#else
bool IVideoPacketDecoder::IsHEVCSoftwareDecodeAvailable()
{
	return false;
}
#endif
