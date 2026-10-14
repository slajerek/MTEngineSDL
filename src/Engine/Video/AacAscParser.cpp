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
// CORRECTION (2026-10-13). This header used to say AudioToolbox rejects even a
// textbook ASC as a magic cookie and as a FormatList specifier, so the decoder
// should be described by hand. The rejection was real but the conclusion was
// wrong: AudioToolbox wants an ES_Descriptor around the ASC, not the bare ASC
// (AacBuildEsdsCookie below), and FormatList takes an AudioFormatInfo
// specifier. With the bare ASC rejected, the hand-built "AAC-LC, 1024
// frames/packet" description silently decoded ~1 HE-AAC packet in 70. The
// parse is still needed: Media Foundation's input type wants the CORE
// rate/channels, and the Apple decoder falls back to it.
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
	out.coreSamplingFrequencyIndex = (int)sfi;
	out.channelConfiguration = (int)chanCfg;

	// Directly-signaled HE-AAC ("explicit hierarchical signaling", ISO/IEC
	// 14496-3 1.6.2.1): when the TOP-LEVEL audioObjectType is 5 (SBR) or 29
	// (SBR+PS), it is NOT the codec's own type -- it's a wrapper meaning
	// "the extension config follows immediately, then the real (core)
	// audioObjectType". Layout: extensionSamplingFrequencyIndex (4 bits;
	// this parser doesn't need the 24-bit escape for index 15 here since
	// SamplingFrequencyForIndex() already consumes it via the shared
	// BitReader when needed) followed by the core audioObjectType (5
	// bits, e.g. 2 = LC), which is what GASpecificConfig below actually
	// describes. This is a DIFFERENT stream shape from the "backward
	// compatible" form below, where a plain AAC-LC audioObjectType is
	// declared up front and the SBR extension is signaled via a trailing
	// 0x2b7 sync marker instead. Real encoders emit both (a 2026-10-13
	// user-supplied web download used this one).
	bool sbrPresentDirect = false;
	int sbrSampleRateDirect = 0;
	u32 extSfiDirect = 0;
	if (audioObjectType == 5 || audioObjectType == 29)
	{
		sbrPresentDirect = true;
		out.directSignaled = true;
		out.psPresent = (audioObjectType == 29);
		extSfiDirect = br.Read(4);
		sbrSampleRateDirect = SamplingFrequencyForIndex(extSfiDirect, br);
		audioObjectType = br.Read(5); // core codec's real audioObjectType (e.g. 2 = LC)
	}
	out.coreAudioObjectType = (int)audioObjectType;

	// GASpecificConfig (object types 2/3/4/6/7, i.e. every AAC family type
	// this project's fixtures use): frameLengthFlag(1), dependsOnCoreCoder
	// (1, +14 bits of coreCoderDelay if set), extensionFlag(1).
	out.frameLengthFlag = (br.Read(1) != 0);
	out.dependsOnCoreCoder = (br.Read(1) != 0);
	if (out.dependsOnCoreCoder)
		out.coreCoderDelay = (int)br.Read(14);
	out.extensionFlag = (br.Read(1) != 0);

	if (sbrPresentDirect)
	{
		out.sbrPresent = true;
		out.sbrSampleRate = sbrSampleRateDirect;
		out.extensionSamplingFrequencyIndex = (int)extSfiDirect;
	}
	// Explicit backward-compatible SBR signaling: an 11-bit sync marker
	// (0x2b7) followed by a 5-bit extensionAudioObjectType. Every AAC
	// fixture this project ships that carries this extension uses it to
	// declare SBR presence explicitly (present or -- just as validly --
	// explicitly absent), so a stream simply not having 16 more bits left
	// is the normal "AAC-LC with no explicit extension" case, not an
	// error. Only checked when the direct form above wasn't already used
	// -- a directly-signaled stream doesn't also carry this trailer.
	// HE-AAC v2 in this form adds a second trailer after the SBR one: an
	// 11-bit 0x548 sync marker and psPresentFlag(1) (ISO/IEC 14496-3 1.6.5).
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
					out.extensionSamplingFrequencyIndex = (int)extSfi;
					out.sbrSampleRate = SamplingFrequencyForIndex(extSfi, br);
					if (br.BitsLeft() >= 12 && br.Read(11) == 0x548)
						out.psPresent = (br.Read(1) != 0);
				}
			}
		}
	}

	out.ok = true;
	return out;
}

// ============================================================================
// AacBuildEsdsCookie
// ============================================================================
std::vector<u8> AacBuildEsdsCookie(const u8 *asc, int ascSize)
{
	std::vector<u8> out;
	// Single-byte descriptor lengths keep this simple; an ASC is a handful of
	// bytes, so the ES_Descriptor's own length (3 + 2+13 + 2+asc + 3) must
	// stay under 128.
	if (!asc || ascSize <= 0 || ascSize > 100)
		return out;

	const int dsiLen = ascSize;
	const int dcdLen = 13 + 2 + dsiLen; // DecoderConfigDescriptor body + DSI
	const int esLen = 3 + 2 + dcdLen + 3; // ES_ID/flags + DCD + SLConfig

	out.reserve((size_t)esLen + 2);
	out.push_back(0x03);              // ES_DescrTag
	out.push_back((u8)esLen);
	out.push_back(0x00);              // ES_ID
	out.push_back(0x00);
	out.push_back(0x00);              // flags: no dependsOn/URL/OCR
	out.push_back(0x04);              // DecoderConfigDescrTag
	out.push_back((u8)dcdLen);
	out.push_back(0x40);              // objectTypeIndication: MPEG-4 Audio
	out.push_back(0x15);              // streamType 5 (audio) << 2 | upStream 0 | reserved 1
	for (int i = 0; i < 11; i++)
		out.push_back(0x00);          // bufferSizeDB(3) maxBitrate(4) avgBitrate(4): unknown
	out.push_back(0x05);              // DecSpecificInfoTag
	out.push_back((u8)dsiLen);
	out.insert(out.end(), asc, asc + ascSize);
	out.push_back(0x06);              // SLConfigDescrTag
	out.push_back(0x01);
	out.push_back(0x02);              // predefined: MP4
	return out;
}

// ============================================================================
// AacMakeBackwardCompatibleAsc
// ============================================================================
namespace
{
	class BitWriter
	{
	public:
		void Write(u32 v, int n)
		{
			for (int i = n - 1; i >= 0; i--)
			{
				if (bitPos == 0)
					bytes.push_back(0);
				if ((v >> i) & 1)
					bytes.back() |= (u8)(0x80 >> bitPos);
				bitPos = (bitPos + 1) & 7;
			}
		}
		std::vector<u8> bytes;

	private:
		int bitPos = 0;
	};

	// Index 15 is the escape: the explicit 24-bit rate follows.
	void WriteSamplingFrequency(BitWriter &bw, int index, int rate)
	{
		bw.Write((u32)index, 4);
		if (index == 15)
			bw.Write((u32)rate, 24);
	}
}

std::vector<u8> AacMakeBackwardCompatibleAsc(const u8 *asc, int ascSize)
{
	SAacAscInfo info = AacParseAudioSpecificConfig(asc, ascSize);
	if (!info.ok)
		return std::vector<u8>();
	if (!info.directSignaled)
		return std::vector<u8>(asc, asc + ascSize);
	if (info.coreAudioObjectType != 2)
		return std::vector<u8>();

	BitWriter bw;
	bw.Write(2, 5);                                   // audioObjectType: AAC-LC
	WriteSamplingFrequency(bw, info.coreSamplingFrequencyIndex, info.coreSampleRate);
	bw.Write((u32)info.channelConfiguration, 4);
	bw.Write(info.frameLengthFlag ? 1 : 0, 1);        // GASpecificConfig
	bw.Write(info.dependsOnCoreCoder ? 1 : 0, 1);
	if (info.dependsOnCoreCoder)
		bw.Write((u32)info.coreCoderDelay, 14);
	bw.Write(info.extensionFlag ? 1 : 0, 1);
	bw.Write(0x2b7, 11);                              // syncExtensionType
	bw.Write(5, 5);                                   // extensionAudioObjectType: SBR
	bw.Write(1, 1);                                   // sbrPresentFlag
	WriteSamplingFrequency(bw, info.extensionSamplingFrequencyIndex, info.sbrSampleRate);
	if (info.psPresent)
	{
		bw.Write(0x548, 11);                          // syncExtensionType
		bw.Write(1, 1);                               // psPresentFlag
	}
	return bw.bytes;
}
