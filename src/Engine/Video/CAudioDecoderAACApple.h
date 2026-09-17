#ifndef _CAUDIODECODERAACAPPLE_H_
#define _CAUDIODECODERAACAPPLE_H_

#pragma once

// Native AAC decode via Apple AudioToolbox. This is the zero-bundled-AAC-
// decoder path: the bundled FFmpeg carries no AAC decoder (licensing), so AAC
// packets demuxed by CVideoSourceFFmpeg are handed to this class, which
// decodes them (LC and HE/SBR profiles alike) with the OS-provided (and
// OS-licensed) AudioConverter. Compiles to nothing anywhere this isn't Apple.
//
// SBR (HE-AAC) detection: Init() hand-parses the AudioSpecificConfig's fixed
// bit layout itself (channel count, core sample rate, and -- if explicitly
// signaled -- SBR presence and its extension sample rate) via the shared,
// platform-neutral AacAscParser.h/.cpp (also consumed by CAudioDecoderAACMF
// on Windows) rather than leaning on AudioConverter to derive it from the
// magic cookie. See AacAscParser.cpp's header comment for why: empirically,
// on the OS version this was developed against,
// AudioConverterSetProperty(kAudioConverterDecompressionMagicCookie) and
// AudioFormatGetProperty(kAudioFormatProperty_FormatList) both reject even a
// textbook-correct cookie, while decoding via an explicit input
// AudioStreamBasicDescription (rate/channels/framesPerPacket) works fine.
#ifdef __APPLE__

#include "SYS_Defs.h"
#include "CAudioConverterApple.h"
#include <string>
#include <vector>

class CAudioDecoderAACApple
{
public:
	CAudioDecoderAACApple();
	~CAudioDecoderAACApple();

	// asc = AudioSpecificConfig (codecpar->extradata from mp4/mkv). Re-callable
	// on an already-initialized instance (e.g. to reset internal AudioConverter
	// state across a Seek()) -- tears down any existing converter first.
	bool Init(const u8 *asc, int ascSize);

	// One AAC access unit in (raw, or ADTS-framed -- the 7/9-byte ADTS header
	// is detected via its 0xFFF syncword and stripped automatically) ->
	// interleaved s16 PCM out. Returns true (with outPCM possibly empty) while
	// AudioConverter is still priming; only returns false on a genuine decode
	// error.
	bool DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
					  int &outChannels, u32 &outSampleRate);

	const std::string &GetErrorReason() const
	{
		return errorReason.empty() ? converter.GetErrorReason() : errorReason;
	}

private:
	void Teardown();

	// THE CONVERTER IS SHARED CODE NOW (CAudioConverterApple), because the AVI
	// lane needs the identical machinery for MP3/MP2/AC-3. What stays here is
	// what is actually AAC: the AudioSpecificConfig parse, the SBR rate
	// decision, and the per-packet ADTS strip.
	CAudioConverterApple converter;
	std::string errorReason;
	int inputChannels = 0;   // compressed-side channel count, from hand-parsing the ASC
};

#endif // __APPLE__

#endif
//_CAUDIODECODERAACAPPLE_H_
