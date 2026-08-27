#include "CAudioDecoderAACApple.h"

#ifdef __APPLE__

#include "AacAscParser.h"
#include "DBG_Log.h"
#include <cstring>

// ============================================================================
// AudioConverterFillComplexBuffer input callback
// ============================================================================
namespace
{
	// Private sentinel returned by InputDataProc once the single access unit
	// handed to this DecodePacket() call has already been consumed. Any
	// distinct non-noErr OSStatus works here -- it never crosses outside this
	// translation unit. AudioConverterFillComplexBuffer's documented contract:
	// when the input proc returns a non-noErr status, it stops asking for more
	// input and returns immediately with whatever output it already produced
	// (ioOutputDataPackets reflects the true count) -- that return status is
	// then propagated back to us as AudioConverterFillComplexBuffer's own
	// result, which we treat as "expected end of this packet's input", not a
	// real error, as long as it's this exact value.
	constexpr OSStatus kInputExhausted = 'acEx';

	struct SInputProcState
	{
		const u8 *data = nullptr;
		UInt32 size = 0;
		bool consumed = false;
		AudioStreamPacketDescription desc = {};
	};

	OSStatus InputDataProc(AudioConverterRef /*inAudioConverter*/,
						   UInt32 *ioNumberDataPackets,
						   AudioBufferList *ioData,
						   AudioStreamPacketDescription **outDataPacketDescription,
						   void *inUserData)
	{
		SInputProcState *st = reinterpret_cast<SInputProcState *>(inUserData);
		if (st->consumed || st->size == 0)
		{
			*ioNumberDataPackets = 0;
			ioData->mNumberBuffers = 0;
			return kInputExhausted;
		}

		st->desc.mStartOffset = 0;
		st->desc.mVariableFramesInPacket = 0;
		st->desc.mDataByteSize = st->size;

		ioData->mNumberBuffers = 1;
		ioData->mBuffers[0].mNumberChannels = 1; // opaque compressed blob -- not per-channel
		ioData->mBuffers[0].mData = const_cast<u8 *>(st->data);
		ioData->mBuffers[0].mDataByteSize = st->size;

		if (outDataPacketDescription)
			*outDataPacketDescription = &st->desc;
		*ioNumberDataPackets = 1;
		st->consumed = true;
		return noErr;
	}

}

// ============================================================================
// Construction / teardown
// ============================================================================
CAudioDecoderAACApple::CAudioDecoderAACApple()
{
}

CAudioDecoderAACApple::~CAudioDecoderAACApple()
{
	Teardown();
}

void CAudioDecoderAACApple::Teardown()
{
	if (converter)
	{
		AudioConverterDispose(converter);
		converter = nullptr;
	}
	outputChannels = 0;
	outputSampleRate = 0;
	inputChannels = 0;
}

// ============================================================================
// Init -- AudioSpecificConfig -> AudioConverter (input MPEG4AAC, output LPCM)
// ============================================================================
bool CAudioDecoderAACApple::Init(const u8 *asc, int ascSize)
{
	Teardown();
	errorReason.clear();

	if (!asc || ascSize <= 0)
	{
		errorReason = "no AudioSpecificConfig (extradata) supplied";
		return false;
	}

	SAacAscInfo parsed = AacParseAudioSpecificConfig(asc, ascSize);
	if (!parsed.ok)
	{
		errorReason = "failed to parse AudioSpecificConfig (unsupported audioObjectType/channelConfiguration, or malformed extradata)";
		return false;
	}

	// Declare the SBR-extended rate to AudioConverterNew() whenever the ASC
	// explicitly signals SBR presence (every HE-AAC fixture this project
	// ships uses the explicit "backward compatible" signaling form).
	// mFramesPerPacket is always 1024 regardless -- that's the AAC frame's
	// encoded samples-per-packet count; SBR's 2x sample doubling happens
	// inside the converter's decode, not in this declared value (verified
	// empirically: AudioConverterNew rejects mFramesPerPacket=2048 outright
	// with kAudioConverterErr_UnspecifiedError, but accepts 1024 combined
	// with the SBR-extended mSampleRate just fine).
	AudioStreamBasicDescription inASBD;
	memset(&inASBD, 0, sizeof(inASBD));
	inASBD.mFormatID = kAudioFormatMPEG4AAC;
	inASBD.mChannelsPerFrame = (UInt32)parsed.channels;
	inASBD.mFramesPerPacket = 1024;
	inASBD.mSampleRate = (parsed.sbrPresent && parsed.sbrSampleRate > 0) ? parsed.sbrSampleRate : parsed.coreSampleRate;

	inputChannels = parsed.channels;

	// Output: interleaved 16-bit signed PCM, native endian (no big-endian flag
	// set -- both Intel and Apple Silicon Macs are little-endian). Rate/
	// channels seeded from the input ASBD as AudioConverterNew()'s starting
	// point; the ACTUAL negotiated output (which for HE-AAC's SBR doubling may
	// differ) is read back below via kAudioConverterCurrentOutputStreamDescription
	// and is what populates outputChannels/outputSampleRate -- never hardcoded.
	AudioStreamBasicDescription outASBD;
	memset(&outASBD, 0, sizeof(outASBD));
	outASBD.mFormatID = kAudioFormatLinearPCM;
	outASBD.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	outASBD.mBitsPerChannel = 16;
	outASBD.mChannelsPerFrame = inASBD.mChannelsPerFrame;
	outASBD.mSampleRate = inASBD.mSampleRate;
	outASBD.mFramesPerPacket = 1;
	outASBD.mBytesPerFrame = outASBD.mChannelsPerFrame * (outASBD.mBitsPerChannel / 8);
	outASBD.mBytesPerPacket = outASBD.mBytesPerFrame;

	OSStatus status = AudioConverterNew(&inASBD, &outASBD, &converter);
	if (status != noErr || !converter)
	{
		errorReason = "AudioConverterNew failed";
		converter = nullptr;
		return false;
	}

	// Also try to set the magic cookie as a converter property, best-effort:
	// on some OS versions this lets the converter pick up on details our
	// hand-parse doesn't attempt (e.g. Parametric Stereo). Empirically, on
	// the OS version this was developed/tested against, this call rejects
	// even a textbook-correct AAC-LC cookie with kAudioCodecUnsupportedFormatError
	// ('!dat') -- verified with a known-good reference cookie, not specific to
	// this project's fixtures -- while decode via the explicit ASBD above
	// (rate/channels/framesPerPacket, from AacParseAudioSpecificConfig) works
	// fine regardless. So a failure here is silently non-fatal by design, not
	// logged as a warning.
	AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie,
							   (UInt32)ascSize, asc);

	// Learn the ACTUAL negotiated output format -- must never assume it
	// matches our outASBD guess (HE-AAC's SBR extension can double the
	// sample rate inside the converter).
	AudioStreamBasicDescription actualOut;
	memset(&actualOut, 0, sizeof(actualOut));
	UInt32 actualOutSize = sizeof(actualOut);
	status = AudioConverterGetProperty(converter, kAudioConverterCurrentOutputStreamDescription,
										&actualOutSize, &actualOut);
	if (status == noErr && actualOut.mChannelsPerFrame > 0 && actualOut.mSampleRate > 0.0)
	{
		outputChannels = (int)actualOut.mChannelsPerFrame;
		outputSampleRate = (u32)actualOut.mSampleRate;
	}
	else
	{
		outputChannels = (int)outASBD.mChannelsPerFrame;
		outputSampleRate = (u32)outASBD.mSampleRate;
	}

	return true;
}

// ============================================================================
// DecodePacket
// ============================================================================
bool CAudioDecoderAACApple::DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
										  int &outChannels, u32 &outSampleRate)
{
	outPCM.clear();
	outChannels = outputChannels;
	outSampleRate = outputSampleRate;

	if (!converter)
	{
		errorReason = "converter not initialized";
		return false;
	}
	if (!data || size <= 0)
	{
		errorReason = "empty packet";
		return false;
	}

	// TS-carried AAC is ADTS-framed (each packet: 7-byte header, or 9 with the
	// optional CRC, syncword 0xFFF); mp4/mkv hand us raw access units with the
	// AudioSpecificConfig supplied out-of-band instead (Init()'s asc). The
	// AudioConverter's input format is raw kAudioFormatMPEG4AAC, so any ADTS
	// header must be stripped per-packet before handing the payload over.
	const u8 *payload = data;
	int payloadSize = size;
	if (size >= 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0)
	{
		bool protectionAbsent = (data[1] & 0x01) != 0;
		int headerSize = protectionAbsent ? 7 : 9;
		// >= (not >): a header-only, zero-payload ADTS frame must still be
		// stripped down to an empty payload rather than falling through and
		// handing the raw header bytes to the converter as fake AAC data --
		// state.size==0 below is handled cleanly (treated the same as
		// priming, not an error).
		if (size >= headerSize)
		{
			payload = data + headerSize;
			payloadSize = size - headerSize;
		}
	}

	SInputProcState state;
	state.data = payload;
	state.size = (UInt32)payloadSize;

	// Sized generously: a single AAC frame is 1024 samples/channel (2048 for
	// HE-AAC's SBR-doubled output), well under this per-call ceiling.
	const UInt32 maxFrames = 4096;
	int chansForBuffer = outputChannels > 0 ? outputChannels : 2;
	std::vector<s16> buffer((size_t)maxFrames * (size_t)chansForBuffer);

	AudioBufferList outBufferList;
	outBufferList.mNumberBuffers = 1;
	outBufferList.mBuffers[0].mNumberChannels = (UInt32)chansForBuffer;
	outBufferList.mBuffers[0].mDataByteSize = (UInt32)(buffer.size() * sizeof(s16));
	outBufferList.mBuffers[0].mData = buffer.data();

	UInt32 ioOutputDataPackets = maxFrames;
	OSStatus status = AudioConverterFillComplexBuffer(converter, InputDataProc, &state,
													   &ioOutputDataPackets, &outBufferList, nullptr);

	if (ioOutputDataPackets == 0)
	{
		// Either priming (encoder delay -- normal, not an error) or an actual
		// failure; only the latter should be reported as such.
		if (status != noErr && status != kInputExhausted)
		{
			errorReason = "AudioConverterFillComplexBuffer failed";
			return false;
		}
		return true; // priming: empty outPCM, not an error (per class contract)
	}

	// Got output -- true regardless of `status`, which is expected to be
	// kInputExhausted here (we only ever hand the converter one packet's
	// worth of input per call).
	size_t totalSamples = (size_t)ioOutputDataPackets * (size_t)outBufferList.mBuffers[0].mNumberChannels;
	outPCM.assign(buffer.begin(), buffer.begin() + totalSamples);
	outChannels = (int)outBufferList.mBuffers[0].mNumberChannels;
	outSampleRate = outputSampleRate;
	return true;
}

#endif // __APPLE__
