#include "CAudioDecoderCompressedApple.h"

#ifdef __APPLE__

#include "MT_AudioFrames.h"
#include "DBG_Log.h"
#include <cstring>

bool CAudioDecoderCompressedApple::Init(EMediaCodecId codec, u32 sampleRate, int channels)
{
	converter.Teardown();
	carry.clear();
	errorReason.clear();
	codecId = codec;

	UInt32 formatID = 0;
	UInt32 framesPerPacket = 0;
	switch (codec)
	{
		case EMediaCodecId::MP3: formatID = kAudioFormatMPEGLayer3; framesPerPacket = 1152; break;
		case EMediaCodecId::MP2: formatID = kAudioFormatMPEGLayer2; framesPerPacket = 1152; break;
		case EMediaCodecId::AC3: formatID = kAudioFormatAC3;        framesPerPacket = 1536; break;
		default:
			errorReason = "CAudioDecoderCompressedApple: unsupported codec";
			return false;
	}
	if (sampleRate == 0 || channels <= 0)
	{
		errorReason = "CAudioDecoderCompressedApple: container gave no rate or channel count";
		return false;
	}

	AudioStreamBasicDescription inASBD;
	memset(&inASBD, 0, sizeof(inASBD));
	inASBD.mFormatID = formatID;
	inASBD.mSampleRate = (Float64)sampleRate;
	inASBD.mChannelsPerFrame = (UInt32)channels;
	inASBD.mFramesPerPacket = framesPerPacket;

	// No magic cookie: none of these formats has out-of-band configuration.
	// Everything the decoder needs is in each frame's own header.
	if (!converter.Init(inASBD, NULL, 0))
	{
		errorReason = converter.GetErrorReason();
		return false;
	}
	return true;
}

bool CAudioDecoderCompressedApple::DecodeChunk(const u8 *data, int size, std::vector<s16> &outPCM,
											   int &outChannels, u32 &outSampleRate)
{
	outPCM.clear();
	outChannels = converter.GetOutputChannels();
	outSampleRate = converter.GetOutputSampleRate();

	if (!converter.IsOpen())
	{
		errorReason = "converter not initialized";
		return false;
	}
	if (data == NULL || size <= 0)
		return true;   // an empty chunk is not an error; AVI has plenty

	// Join the previous chunk's unfinished tail. The common case is an empty
	// carry and no copy at all.
	const u8 *buf;
	size_t avail;
	std::vector<u8> joined;
	if (carry.empty())
	{
		buf = data;
		avail = (size_t)size;
	}
	else
	{
		joined.reserve(carry.size() + (size_t)size);
		joined.insert(joined.end(), carry.begin(), carry.end());
		joined.insert(joined.end(), data, data + size);
		buf = joined.data();
		avail = joined.size();
	}

	const bool isAc3 = (codecId == EMediaCodecId::AC3);
	size_t pos = 0;
	for (;;)
	{
		// Resynchronise. A chunk that begins mid-frame is not a corrupt file,
		// it is an ordinary AVI; skipping to the next header is what a player
		// does rather than refusing.
		const size_t skip = isAc3 ? MT_AudioFrames::FindAc3Sync(buf + pos, avail - pos)
								  : MT_AudioFrames::FindMpegSync(buf + pos, avail - pos);
		pos += skip;
		if (pos >= avail)
			break;

		const size_t frameSize = isAc3
			? MT_AudioFrames::Ac3FrameSize(buf + pos, avail - pos)
			: MT_AudioFrames::MpegFrameSize(buf + pos, avail - pos);
		if (frameSize == 0)
			break;   // header present but the frame does not fit yet: carry it

		if (!converter.DecodePacket(buf + pos, (int)frameSize, outPCM))
		{
			errorReason = converter.GetErrorReason();
			return false;
		}
		pos += frameSize;
	}

	// Whatever is left is the start of a frame the next chunk completes. The
	// cap is a safety valve: without it, a stream of pure junk would grow this
	// buffer without bound while never producing a frame.
	carry.assign(buf + pos, buf + avail);
	if (carry.size() > 64 * 1024)
		carry.clear();

	outChannels = converter.GetOutputChannels();
	outSampleRate = converter.GetOutputSampleRate();
	return true;
}

#endif // __APPLE__
