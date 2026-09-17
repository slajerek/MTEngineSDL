#include "CAudioDecoderAACApple.h"

#ifdef __APPLE__

#include "AacAscParser.h"
#include "DBG_Log.h"
#include <cstring>

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
	converter.Teardown();
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

	// The converter, its LPCM output format, the best-effort magic cookie and
	// the read-back of the ACTUAL negotiated output all live in
	// CAudioConverterApple now -- see its .cpp for why the cookie is
	// best-effort and why the output format is learned rather than assumed.
	if (!converter.Init(inASBD, asc, ascSize))
	{
		errorReason = converter.GetErrorReason();
		return false;
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
	outChannels = converter.GetOutputChannels();
	outSampleRate = converter.GetOutputSampleRate();

	if (!converter.IsOpen())
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
	// THIS IS THE AAC-SPECIFIC HALF, and the reason this class still exists
	// separately from CAudioConverterApple.
	const u8 *payload = data;
	int payloadSize = size;
	if (size >= 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0)
	{
		bool protectionAbsent = (data[1] & 0x01) != 0;
		int headerSize = protectionAbsent ? 7 : 9;
		// >= (not >): a header-only, zero-payload ADTS frame must still be
		// stripped down to an empty payload rather than falling through and
		// handing the raw header bytes to the converter as fake AAC data.
		if (size >= headerSize)
		{
			payload = data + headerSize;
			payloadSize = size - headerSize;
		}
	}

	if (payloadSize <= 0)
		return true;   // header-only frame: priming-equivalent, not an error

	if (!converter.DecodePacket(payload, payloadSize, outPCM))
	{
		errorReason = converter.GetErrorReason();
		return false;
	}
	outChannels = converter.GetOutputChannels();
	outSampleRate = converter.GetOutputSampleRate();
	return true;
}

#endif // __APPLE__
