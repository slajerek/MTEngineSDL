#include "AacAscParser.h"

// ============================================================================
// Minimal AudioSpecificConfig (ISO/IEC 14496-3) bit parser
// ============================================================================
// Hoisted out of CAudioDecoderAACApple.cpp (see that file's history) so both
// native AAC decoders (Apple AudioToolbox, Windows Media Foundation) can share
// one implementation instead of duplicating hand-parsed bit-layout code that
// is easy to get subtly wrong (see the AOT5/AOT29 "direct" HE-AAC signaling
// commentary below -- a real user-reported defect).
//
// Empirically (on Apple, at least), AudioFormatGetProperty(
// kAudioFormatProperty_FormatList) and AudioConverterSetProperty(
// kAudioConverterDecompressionMagicCookie) both reject even a textbook
// AAC-LC AudioSpecificConfig with kAudioCodecUnsupportedFormatError ('!dat')
// -- verified against known-good cookies, not something specific to any one
// fixture's bytes. Actual decode does NOT require the magic cookie though
// (verified against this project's own h264_aac.mp4 first packet):
// AudioConverterNew() with an explicit input AudioStreamBasicDescription
// (rate/channels/framesPerPacket) is sufficient. So: hand-parse the ASC
// ourselves for those three fields (well-defined, fixed bit layout --
// ISO/IEC 14496-3 1.6.2.1) instead of depending on the property call.
namespace
{
	class BitReader
	{
	public:
		BitReader(const u8 *d, int size) : data(d), bitCount((size_t)size * 8) {}
		// Returns 0 if the read would run past the end (never throws/UB).
		u32 Read(int n)
		{
			u32 v = 0;
			for (int i = 0; i < n; i++)
			{
				v <<= 1;
				if (pos < bitCount)
				{
					size_t byteIdx = pos / 8;
					int bitIdx = 7 - (int)(pos % 8);
					v |= (data[byteIdx] >> bitIdx) & 0x1;
				}
				pos++;
			}
			return v;
		}
		size_t BitsLeft() const { return pos < bitCount ? bitCount - pos : 0; }

	private:
		const u8 *data;
		size_t bitCount;
		size_t pos = 0;
	};

	// Table 1.16 (ISO/IEC 14496-3): samplingFrequencyIndex -> Hz. Index 15
	// means "explicit 24-bit frequency follows" (handled inline below); 13/14
	// are reserved (mapped to 0 = "unknown", caller treats as parse failure).
	int SamplingFrequencyForIndex(u32 idx, BitReader &br)
	{
		static const int kRates[13] = {96000, 88200, 64000, 48000, 44100, 32000,
									   24000, 22050, 16000, 12000, 11025, 8000, 7350};
		if (idx < 13)
			return kRates[idx];
		if (idx == 15)
			return (int)br.Read(24);
		return 0; // 13, 14 reserved
	}

	// Table 1.19 (ISO/IEC 14496-3): channelConfiguration -> channel count.
	// 0 (PCE-defined, no fixed count here) is intentionally unsupported --
	// none of this project's fixtures use it.
	int ChannelCountForConfig(u32 cfg)
	{
		static const int kChannels[8] = {0, 1, 2, 3, 4, 5, 6, 8};
		return (cfg < 8) ? kChannels[cfg] : 0;
	}
}

// Parses just enough of the ASC to learn channel count, the core AAC sample
// rate, and (if explicitly signaled -- the "backward compatible" HE-AAC form
// every fixture in this project uses) SBR presence and its extension sample
// rate. Does not attempt audioObjectType==31 (extended object type) or
// PCE-based channel configs -- neither appears in any fixture this project
// ships.
SAacAscInfo AacParseAudioSpecificConfig(const u8 *asc, int ascSize)
{
	SAacAscInfo out;
	BitReader br(asc, ascSize);

	u32 audioObjectType = br.Read(5);
	if (audioObjectType == 31)
		return out; // extended object type -- not handled

	u32 sfi = br.Read(4);
	int coreRate = SamplingFrequencyForIndex(sfi, br);
	u32 chanCfg = br.Read(4);
	int channels = ChannelCountForConfig(chanCfg);
	if (coreRate <= 0 || channels <= 0)
		return out;

	out.coreSampleRate = coreRate;
	out.channels = channels;

	// Directly-signaled HE-AAC ("explicit hierarchical signaling", ISO/IEC
	// 14496-3 1.6.2.1): when the TOP-LEVEL audioObjectType is 5 (SBR) or 29
	// (SBR+PS), it is NOT the codec's own type -- it's a wrapper meaning
	// "the extension config follows immediately, then the real (core)
	// audioObjectType". Layout: extensionSamplingFrequencyIndex (4 bits;
	// this parser doesn't need the 24-bit escape for index 15 here since
	// SamplingFrequencyForIndex() already consumes it via the shared
	// BitReader when needed) followed by the core audioObjectType (5
	// bits, e.g. 2 = LC), which is what GASpecificConfig below actually
	// describes. This is a DIFFERENT stream shape from the (far more
	// common, and the only one every fixture this project ships prior to
	// this fix used) "backward compatible" form below, where a plain
	// AAC-LC audioObjectType is declared up front and the SBR extension
	// is signaled via a trailing 0x2b7 sync marker instead.
	bool sbrPresentDirect = false;
	int sbrSampleRateDirect = 0;
	if (audioObjectType == 5 || audioObjectType == 29)
	{
		sbrPresentDirect = true;
		u32 extSfi = br.Read(4);
		sbrSampleRateDirect = SamplingFrequencyForIndex(extSfi, br);
		audioObjectType = br.Read(5); // core codec's real audioObjectType (e.g. 2 = LC)
	}

	// GASpecificConfig (object types 2/3/4/6/7, i.e. every AAC family type
	// this project's fixtures use): frameLengthFlag(1), dependsOnCoreCoder
	// (1, +14 bits of coreCoderDelay if set), extensionFlag(1).
	br.Read(1); // frameLengthFlag
	if (br.Read(1) != 0)
		br.Read(14); // coreCoderDelay
	br.Read(1); // extensionFlag

	if (sbrPresentDirect)
	{
		out.sbrPresent = true;
		out.sbrSampleRate = sbrSampleRateDirect;
	}
	// Explicit backward-compatible SBR signaling: an 11-bit sync marker
	// (0x2b7) followed by a 5-bit extensionAudioObjectType. Every AAC
	// fixture this project ships that carries this extension uses it to
	// declare SBR presence explicitly (present or -- just as validly --
	// explicitly absent), so a stream simply not having 16 more bits left
	// is the normal "AAC-LC with no explicit extension" case, not an
	// error. Only checked when the direct form above wasn't already used
	// -- a directly-signaled stream doesn't also carry this trailer.
	else if (br.BitsLeft() >= 16)
	{
		u32 sync = br.Read(11);
		if (sync == 0x2b7)
		{
			u32 extAOT = br.Read(5);
			if (extAOT == 5) // SBR
			{
				out.sbrPresent = (br.Read(1) != 0);
				if (out.sbrPresent && br.BitsLeft() >= 4)
				{
					u32 extSfi = br.Read(4);
					out.sbrSampleRate = SamplingFrequencyForIndex(extSfi, br);
				}
			}
		}
	}

	out.ok = true;
	return out;
}
