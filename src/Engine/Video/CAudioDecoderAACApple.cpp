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
// Init -- AudioSpecificConfig -> ES_Descriptor cookie -> AudioConverter (input
// whatever AAC layer AudioToolbox reports for the cookie, output LPCM)
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

	// THE COOKIE IS WHAT TELLS AUDIOTOOLBOX ABOUT SBR/PS, so it must be in the
	// form AudioToolbox accepts: an ES_Descriptor (AacBuildEsdsCookie), not
	// the bare ASC. Until 2026-10-13 the bare ASC was passed, AudioToolbox
	// rejected it ('!dat'), and the converter -- told only "AAC, 1024
	// frames/packet" -- emitted output for ~1 packet in 70 on every HE-AAC
	// stream: audibly scrambled, while a "PCM is non-empty" test stayed green.
	// Measured on a real 44.1 kHz direct-signaled HE-AAC mp4: 3 of 216
	// packets decoded; with this cookie, 216 of 216 at 2048 frames each,
	// correlation 1.0000 against FFmpeg's decode.
	std::vector<u8> cookie = AacBuildEsdsCookie(asc, ascSize);

	// Let AudioToolbox describe the stream from the cookie. FormatList
	// returns one entry per decodable layer, richest first (HE-AAC v2 'aacp'
	// > HE-AAC 'aach' > the AAC-LC core 'aac '), each with ITS rate,
	// channels and frames-per-packet -- e.g. 'aacp' 44100 Hz stereo 2048 for
	// a mono-core 22050 Hz PS stream. This is the documented route (FormatInfo
	// on the cookie, then FormatList with an AudioFormatInfo specifier).
	AudioStreamBasicDescription inASBD;
	memset(&inASBD, 0, sizeof(inASBD));
	bool described = false;
	if (!cookie.empty())
	{
		inASBD.mFormatID = kAudioFormatMPEG4AAC;
		UInt32 size = sizeof(inASBD);
		if (AudioFormatGetProperty(kAudioFormatProperty_FormatInfo, (UInt32)cookie.size(), cookie.data(),
								   &size, &inASBD) == noErr)
		{
			AudioFormatInfo info;
			memset(&info, 0, sizeof(info));
			info.mASBD = inASBD;
			info.mMagicCookie = cookie.data();
			info.mMagicCookieSize = (UInt32)cookie.size();

			AudioFormatListItem items[8];
			UInt32 listSize = sizeof(items);
			if (AudioFormatGetProperty(kAudioFormatProperty_FormatList, sizeof(info), &info,
									   &listSize, items) == noErr &&
				listSize >= sizeof(AudioFormatListItem) &&
				items[0].mASBD.mSampleRate > 0.0 && items[0].mASBD.mChannelsPerFrame > 0)
			{
				inASBD = items[0].mASBD;
				described = true;
			}
		}
	}

	// Fallback: describe it by hand from our own parse. The HE format IDs
	// matter -- 'aac ' at the SBR rate with 1024 frames/packet is exactly the
	// broken configuration above. HE-AAC (v1 and v2) is 2048 frames/packet at
	// the SBR rate; PS decodes the mono core to stereo.
	if (!described)
	{
		memset(&inASBD, 0, sizeof(inASBD));
		if (parsed.sbrPresent && parsed.sbrSampleRate > 0)
		{
			inASBD.mFormatID = parsed.psPresent ? kAudioFormatMPEG4AAC_HE_V2 : kAudioFormatMPEG4AAC_HE;
			inASBD.mChannelsPerFrame = parsed.psPresent ? 2 : (UInt32)parsed.channels;
			inASBD.mFramesPerPacket = 2048;
			inASBD.mSampleRate = parsed.sbrSampleRate;
		}
		else
		{
			inASBD.mFormatID = kAudioFormatMPEG4AAC;
			inASBD.mChannelsPerFrame = (UInt32)parsed.channels;
			inASBD.mFramesPerPacket = 1024;
			inASBD.mSampleRate = parsed.coreSampleRate;
		}
	}

	inputChannels = (int)inASBD.mChannelsPerFrame;

	// The converter, its LPCM output format, the cookie and the read-back of
	// the ACTUAL negotiated output all live in CAudioConverterApple.
	if (!converter.Init(inASBD, cookie.empty() ? nullptr : cookie.data(), (int)cookie.size()))
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
