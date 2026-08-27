#ifndef _CAUDIODECODERWMANATIVE_H_
#define _CAUDIODECODERWMANATIVE_H_

#pragma once

// Selects the platform's OS-licensed native WMA decoder (2026-07-18 WMV
// spec) -- the WMA-family twin of CAudioDecoderAACNative.h; read that
// header's comment for why this seam must be a real header (not a cpp-local
// alias): CVideoSourceFFmpeg.h declares std::unique_ptr<CAudioDecoderWMANative>
// as a member, so the alias must exist wherever that header is parsed.
//
// MT_HAVE_NATIVE_WMA is the gate every wmaDecoder/wmaInit/QueueWMAPacket
// site in CVideoSourceFFmpeg.h/.cpp re-targets to -- defined here (and only
// here). UNLIKE AAC, only Windows has a native WMA decoder (Media
// Foundation's in-box WMAudio Decoder MFT): Apple's AudioToolbox has no WMA
// support at all, so there is no __APPLE__ arm and macOS/Linux full builds
// decode WMA through the bundled FFmpeg instead (commercial builds degrade
// to video-only -- see OpenAudioDecoder()).
#if defined(_WIN32)
	#define MT_HAVE_NATIVE_WMA 1
	class CAudioDecoderWMAMF;
	using CAudioDecoderWMANative = CAudioDecoderWMAMF;
#endif

#endif
//_CAUDIODECODERWMANATIVE_H_
