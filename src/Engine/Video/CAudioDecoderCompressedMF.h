#ifndef _CAUDIODECODERCOMPRESSEDMF_H_
#define _CAUDIODECODERCOMPRESSEDMF_H_

#pragma once

// MP3, MPEG-1 Layer II and AC-3 through Media Foundation, for the AVI lane.
// The Windows twin of CAudioDecoderCompressedApple; its public surface is
// verbatim-identical, which is what makes CAudioDecoderCompressedNative.h's
// alias seam work with no #ifdef at any call site.
//
// **NEVER COMPILED.** Written on macOS from the shape of CAudioDecoderAACMF,
// which IS Windows-verified. Two things differ from that class and both are
// deliberate:
//
//   * the MFT is found by MFTEnumEx on the input subtype rather than by a
//     fixed CLSID. There is no single documented CLSID for "the MP3 decoder"
//     the way CLSID_CMSAACDecMFT names the AAC one, and enumerating is also
//     what makes the answer true on an N edition rather than assumed.
//   * frames are split before they reach the MFT, by MT_AudioFrames, because
//     AVI packs several MP3 frames into one chunk and an MFT takes one
//     compressed sample at a time. That splitting is platform-neutral and IS
//     tested on macOS.
//
// See the Windows handover notes for what to check first.
#if defined(_WIN32)

#include "SYS_Defs.h"
#include "MT_MediaPacket.h"
#include <string>
#include <vector>

struct IMFTransform;   // real MF headers stay in the .cpp

class CAudioDecoderCompressedMF
{
public:
	CAudioDecoderCompressedMF();
	~CAudioDecoderCompressedMF();

	bool Init(EMediaCodecId codec, u32 sampleRate, int channels);

	bool DecodeChunk(const u8 *data, int size, std::vector<s16> &outPCM,
					 int &outChannels, u32 &outSampleRate);

	int GetOutputChannels() const   { return outputChannels; }
	u32 GetOutputSampleRate() const { return outputSampleRate; }
	const std::string &GetErrorReason() const { return errorReason; }

private:
	void Teardown();
	bool SetOutputTypePCM();
	bool DecodeOneFrame(const u8 *data, int size, std::vector<s16> &outPCM);

	IMFTransform *decoderMFT = nullptr;
	std::string errorReason;
	EMediaCodecId codecId = EMediaCodecId::Unknown;
	int outputChannels = 0;
	u32 outputSampleRate = 0;
	std::vector<u8> carry;   // a frame split across two AVI chunks
};

#endif // _WIN32
#endif
//_CAUDIODECODERCOMPRESSEDMF_H_
