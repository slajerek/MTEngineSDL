#include "CAudioConverterApple.h"

#ifdef __APPLE__

#include "DBG_Log.h"
#include <cstring>

namespace
{
	// Private sentinel returned by InputDataProc once the single packet handed
	// to this DecodePacket() call has been consumed. Any distinct non-noErr
	// OSStatus works -- it never leaves this translation unit.
	// AudioConverterFillComplexBuffer's contract: a non-noErr input-proc
	// status stops it asking for more and returns whatever output it already
	// produced, propagating that status as its own result. So this exact value
	// coming back means "end of this packet's input", not a failure.
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

void CAudioConverterApple::Teardown()
{
	if (converter)
	{
		AudioConverterDispose(converter);
		converter = nullptr;
	}
	outputChannels = 0;
	outputSampleRate = 0;
}

bool CAudioConverterApple::Init(const AudioStreamBasicDescription &inASBD,
								const u8 *cookie, int cookieSize)
{
	Teardown();
	errorReason.clear();

	// Interleaved 16-bit signed PCM, native endian (no big-endian flag: both
	// Intel and Apple Silicon are little-endian). Rate and channels seed the
	// negotiation; the ACTUAL negotiated output is read back below and is what
	// the accessors report -- never hardcoded.
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

	// NON-FATAL, BUT NOT SILENT. For AAC the cookie is what carries SBR/PS
	// (CAudioDecoderAACApple builds it as an ES_Descriptor via
	// AacBuildEsdsCookie). It used to be passed as a bare ASC, which
	// AudioToolbox rejects with kAudioCodecUnsupportedFormatError ('!dat');
	// that rejection was mistaken for "the cookie never works" and silenced,
	// and every HE-AAC stream decoded ~1 packet in 70 (2026-10-13). A
	// rejection now is a real fault worth a log line; decode is still
	// attempted from the ASBD alone.
	if (cookie != NULL && cookieSize > 0)
	{
		OSStatus cookieStatus = AudioConverterSetProperty(converter, kAudioConverterDecompressionMagicCookie,
														   (UInt32)cookieSize, cookie);
		if (cookieStatus != noErr)
			LOGWarning("CAudioConverterApple::Init: magic cookie rejected (OSStatus %d) -- decoding from the ASBD alone",
					   (int)cookieStatus);
	}

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

bool CAudioConverterApple::DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM)
{
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

	SInputProcState state;
	state.data = data;
	state.size = (UInt32)size;

	// Sized generously: an AAC frame is 1024 samples/channel (2048 with SBR),
	// an MP3 frame 1152, an AC-3 frame 1536 -- all well under this ceiling.
	const UInt32 maxFrames = 4096;
	const int chansForBuffer = outputChannels > 0 ? outputChannels : 2;
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
		// Priming (encoder delay -- normal) or a real failure; only the second
		// is reported as one.
		if (status != noErr && status != kInputExhausted)
		{
			errorReason = "AudioConverterFillComplexBuffer failed";
			return false;
		}
		return true;
	}

	const size_t totalSamples = (size_t)ioOutputDataPackets *
								(size_t)outBufferList.mBuffers[0].mNumberChannels;
	outPCM.insert(outPCM.end(), buffer.begin(), buffer.begin() + totalSamples);
	return true;
}

#endif // __APPLE__
