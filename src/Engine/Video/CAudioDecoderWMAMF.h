#ifndef _CAUDIODECODERWMAMF_H_
#define _CAUDIODECODERWMAMF_H_

#pragma once

// Native WMA decode via Windows Media Foundation's in-box WMAudio Decoder
// (wmadmod.dll -- a dual DMO/MFT, resolved by input subtype through
// MFTEnumEx with CLSID_CWMADecMediaObject as the direct-instantiation
// fallback; present on every non-N Windows 10+ install). This is the
// WMA-family sibling of CAudioDecoderAACMF (2026-07-18 WMV spec): the
// bundled FFmpeg carries no WMA decoders in commercial builds, so WMA
// packets demuxed from ASF by CVideoSourceFFmpeg are handed here instead
// (and on full builds too, so the path is exercised dev-side).
//
// Interface DIVERGENCE from the AAC twin, by necessity: AAC's
// AudioSpecificConfig is self-contained, but a WMA bitstream is
// undecodable without its WAVEFORMATEX-style stream parameters (channel
// count, sample rate, nBlockAlign, nAvgBytesPerSec) PLUS the codec-private
// extradata (the WAVEFORMATEX cbSize tail: 4 bytes for v1, 10 for v2, 18
// for Pro) -- so Init() takes all of them, exactly as FFmpeg's
// AVCodecParameters carries them out of the ASF demuxer. Compiles to
// nothing anywhere except a Windows FFmpeg-enabled build.
#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "SYS_Defs.h"
#include <string>
#include <vector>

struct IMFTransform; // forward-declared; only the .cpp includes MF headers

class CAudioDecoderWMAMF
{
public:
	CAudioDecoderWMAMF();
	~CAudioDecoderWMAMF();

	// avCodecId is the plain AVCodecID value (AV_CODEC_ID_WMAV1/WMAV2/
	// WMAPRO), kept as int so this header needs no FFmpeg includes.
	// extradata is codecpar->extradata verbatim (may be null/empty only for
	// streams that genuinely carry none -- the MFT then decides whether it
	// can cope). Re-callable on an already-initialized instance (e.g. to
	// reset internal MFT state across a Seek()) -- tears down any existing
	// transform first. Same reset-by-re-Init contract as the AAC twin.
	bool Init(int avCodecId, const u8 *extradata, int extradataSize,
			  int channels, int sampleRate, int blockAlign, int avgBytesPerSec);

	// One ASF audio payload in -> interleaved s16 PCM out. Returns true
	// (with outPCM possibly empty) while the MFT is still priming/buffering
	// internally; only returns false on a genuine decode error. Same
	// contract as CAudioDecoderAACMF::DecodePacket().
	bool DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
					  int &outChannels, u32 &outSampleRate);

	const std::string &GetErrorReason() const { return errorReason; }

private:
	void Teardown();

	// (Re-)negotiates the MFT's output media type to 16-bit interleaved PCM
	// and refreshes outputChannels/outputSampleRate from whatever the MFT
	// actually reports -- verbatim mechanism from the AAC twin's
	// SetOutputTypePCM() (WMA has no SBR-style mid-stream rate change, but
	// MF_E_TRANSFORM_STREAM_CHANGE is still handled identically for the
	// first-output-type-resolution case some MFT implementations exhibit).
	bool SetOutputTypePCM();

	IMFTransform *decoderMFT = nullptr; // raw COM ref, Release()d in Teardown()
	std::string errorReason;

	// Learned from the MFT's actual negotiated output type right after
	// SetOutputType() succeeds -- never hardcoded (the decoder may fold
	// multichannel WMA Pro down to stereo, or not; whatever it reports is
	// the truth downstream resampling consumes).
	int outputChannels = 0;
	u32 outputSampleRate = 0;
};

#endif // _WIN32 && MT_ENABLE_FFMPEG

#endif
//_CAUDIODECODERWMAMF_H_
