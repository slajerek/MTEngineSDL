#ifndef _CAUDIOCONVERTERAPPLE_H_
#define _CAUDIOCONVERTERAPPLE_H_

#pragma once

// One compressed audio packet in, interleaved s16 LPCM out, through
// AudioToolbox's AudioConverter.
//
// WHY IT IS ITS OWN CLASS. CAudioDecoderAACApple was this code plus AAC's
// AudioSpecificConfig parsing and ADTS stripping. The AVI lane needs the same
// converter for MP3, MP2 and AC-3 -- formats with no ASC and no ADTS -- and
// the honest way to get it is to extract the shared half rather than write a
// second copy that will drift. What is left in the AAC class is exactly what
// is AAC: the ASC parse, the SBR rate decision, and the ADTS header strip.
//
// COMPOSITION, NOT INHERITANCE, deliberately: CAudioDecoderAACApple is reached
// through the CAudioDecoderAACNative alias from headers that only forward-
// declare it, and giving it a base class buys nothing while adding a way for
// that seam to break.
//
// The output format is LEARNED, never assumed. HE-AAC's SBR doubles the rate
// inside the converter and only the converter knows it; the same accessor
// serves MP3, whose declared and actual rates do agree, at no cost.

#ifdef __APPLE__

#include "SYS_Defs.h"
#include <AudioToolbox/AudioToolbox.h>
#include <string>
#include <vector>

class CAudioConverterApple
{
public:
	CAudioConverterApple() {}
	~CAudioConverterApple() { Teardown(); }

	CAudioConverterApple(const CAudioConverterApple &) = delete;
	CAudioConverterApple &operator=(const CAudioConverterApple &) = delete;

	// `inASBD` fully describes the compressed input. `cookie` is optional
	// out-of-band configuration -- for AAC an ES_Descriptor
	// (AacBuildEsdsCookie), NOT a bare ASC, which AudioConverter rejects. A
	// rejected cookie is logged but not fatal; see the note in the .cpp.
	bool Init(const AudioStreamBasicDescription &inASBD,
			  const u8 *cookie, int cookieSize);
	void Teardown();

	bool IsOpen() const { return converter != nullptr; }

	// One compressed packet -> PCM APPENDED to `outPCM` (appended, not
	// assigned: a caller walking several frames out of one container chunk
	// wants them contiguous). Returns true with nothing appended while the
	// converter is priming, which is normal and not an error; false only on
	// a genuine decode failure.
	bool DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM);

	int GetOutputChannels() const   { return outputChannels; }
	u32 GetOutputSampleRate() const { return outputSampleRate; }
	const std::string &GetErrorReason() const { return errorReason; }

private:
	AudioConverterRef converter = nullptr;
	std::string errorReason;
	int outputChannels = 0;
	u32 outputSampleRate = 0;
};

#endif // __APPLE__
#endif
//_CAUDIOCONVERTERAPPLE_H_
