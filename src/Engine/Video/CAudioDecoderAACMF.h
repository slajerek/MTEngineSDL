#ifndef _CAUDIODECODERAACMF_H_
#define _CAUDIODECODERAACMF_H_

#pragma once

// Native AAC decode via Windows Media Foundation's built-in AAC decoder MFT
// (CLSID_CMSAACDecMFT, msauddecmft.dll -- present on every Windows 10+
// install; LC/HE/HE-v2/xHE profiles are all handled internally by the MFT,
// same coverage the Apple twin gets from AudioToolbox). This is the
// zero-bundled-AAC-decoder path: the bundled FFmpeg carries no AAC decoder
// (licensing), so AAC packets demuxed by CVideoSourceFFmpeg are handed here
// instead. Public surface is verbatim-identical to CAudioDecoderAACApple.h --
// that identity is what makes the CAudioDecoderAACNative.h alias seam work
// (CVideoSourceFFmpeg references std::unique_ptr<CAudioDecoderAACNative>
// with no #ifdef at any call site). Compiles to nothing anywhere except a
// Windows FFmpeg-enabled build.
#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "SYS_Defs.h"
#include <string>
#include <vector>

struct IMFTransform; // <mftransform.h>, forward-declared to keep MF/COM
                      // headers out of anything that merely #includes this
                      // header (mirrors CVideoSourceFFmpeg.h's FFmpeg
                      // forward-decl convention) -- only the .cpp includes
                      // the real Media Foundation headers.

class CAudioDecoderAACMF
{
public:
	CAudioDecoderAACMF();
	~CAudioDecoderAACMF();

	// asc = AudioSpecificConfig (codecpar->extradata from mp4/mkv). Re-callable
	// on an already-initialized instance (e.g. to reset internal MFT state
	// across a Seek()) -- tears down any existing transform first. Same
	// contract as CAudioDecoderAACApple::Init().
	bool Init(const u8 *asc, int ascSize);

	// One AAC access unit in (raw, or ADTS-framed -- the 7/9-byte ADTS header
	// is detected via its 0xFFF syncword and stripped automatically) ->
	// interleaved s16 PCM out. Returns true (with outPCM possibly empty)
	// while the MFT is still priming/buffering internally; only returns
	// false on a genuine decode error. Same contract as
	// CAudioDecoderAACApple::DecodePacket().
	bool DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
					  int &outChannels, u32 &outSampleRate);

	const std::string &GetErrorReason() const { return errorReason; }

private:
	void Teardown();

	// (Re-)negotiates the MFT's output media type to 16-bit interleaved PCM
	// and refreshes outputChannels/outputSampleRate from whatever the MFT
	// actually reports (never assumed/hardcoded -- HE-AAC's SBR doubling is
	// decided inside the MFT, exactly as it is inside AudioToolbox's
	// AudioConverter on the Apple twin). Called once from Init() and again
	// from DecodePacket() whenever ProcessOutput reports
	// MF_E_TRANSFORM_STREAM_CHANGE (the HE-AAC SBR rate-change path).
	bool SetOutputTypePCM();

	IMFTransform *decoderMFT = nullptr; // raw COM ref, Release()d in Teardown() -- mirrors AudioConverterDispose in the Apple twin
	std::string errorReason;

	// Learned from the MFT's actual negotiated output type right after
	// SetOutputType() succeeds (SetOutputTypePCM()) -- never hardcoded, since
	// HE-AAC's SBR sample-rate doubling is decided inside the MFT, not by us.
	int outputChannels = 0;
	u32 outputSampleRate = 0;
	int inputChannels = 0; // compressed-side channel count, from hand-parsing the ASC (AacAscParser.h)
};

#endif // _WIN32 && MT_ENABLE_FFMPEG

#endif
//_CAUDIODECODERAACMF_H_
