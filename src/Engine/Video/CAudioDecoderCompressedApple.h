#ifndef _CAUDIODECODERCOMPRESSEDAPPLE_H_
#define _CAUDIODECODERCOMPRESSEDAPPLE_H_

#pragma once

// MP3, MPEG-1 Layer II and AC-3, decoded by AudioToolbox -- the audio AVI and
// MPEG-PS actually carry.
//
// WHY IT IS NOT CAudioDecoderAACApple. That class is AAC: it parses an
// AudioSpecificConfig, decides the SBR rate from it, and strips ADTS headers.
// None of those exist here. What the two share -- the AudioConverter, the
// LPCM output negotiation, the per-packet fill -- is CAudioConverterApple,
// which both own. Measured before writing either: AudioConverterNew succeeds
// for MPEGLayer3, MPEGLayer2, AC3 and EnhancedAC3 on this OS.
//
// IT WALKS FRAMES ITSELF, and that is the interesting part. AVI does not
// store one audio frame per chunk: a `01wb` chunk routinely holds several MP3
// frames, and the converter takes one packet at a time. So DecodeChunk()
// splits on real frame headers (MT_AudioFrames) rather than assuming a
// constant size -- which works on CBR and silently desynchronises on VBR.
//
// The Windows twin is CAudioDecoderCompressedMF; CAudioDecoderCompressedNative.h
// picks between them, following the seam CAudioDecoderAACNative.h established.
// LINUX HAS NEITHER, so the AVI lane there refuses rather than plays silence.

#ifdef __APPLE__

#include "SYS_Defs.h"
#include "MT_MediaPacket.h"
#include "CAudioConverterApple.h"
#include <string>
#include <vector>

class CAudioDecoderCompressedApple
{
public:
	CAudioDecoderCompressedApple() {}
	~CAudioDecoderCompressedApple() {}

	// `codec` must be MP3, MP2 or AC3. `sampleRate` and `channels` come from
	// the container (AVI's WAVEFORMATEX); they seed the converter, and the
	// ACTUAL negotiated output is read back and is what the accessors report.
	bool Init(EMediaCodecId codec, u32 sampleRate, int channels);

	// A whole container chunk in -- one frame or several -- interleaved s16
	// PCM out. Returns true with an empty result while priming or when the
	// chunk holds no complete frame; false only on a genuine decode failure.
	bool DecodeChunk(const u8 *data, int size, std::vector<s16> &outPCM,
					 int &outChannels, u32 &outSampleRate);

	int GetOutputChannels() const   { return converter.GetOutputChannels(); }
	u32 GetOutputSampleRate() const { return converter.GetOutputSampleRate(); }
	const std::string &GetErrorReason() const
	{
		return errorReason.empty() ? converter.GetErrorReason() : errorReason;
	}

private:
	CAudioConverterApple converter;
	std::string errorReason;
	EMediaCodecId codecId = EMediaCodecId::Unknown;

	// A frame split across two chunks is normal in AVI, so the tail of one
	// chunk is carried into the next rather than dropped. Dropping it costs a
	// frame every few kilobytes -- audible as a tick, and easy to mistake for
	// a decoder bug.
	std::vector<u8> carry;
};

#endif // __APPLE__
#endif
//_CAUDIODECODERCOMPRESSEDAPPLE_H_
