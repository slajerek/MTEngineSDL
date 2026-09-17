#include "MT_AnnexB.h"
#include "DBG_Log.h"
#include <cstring>

namespace
{
	inline void AppendStartCode(std::vector<u8> &out)
	{
		static const u8 kStartCode[4] = { 0x00, 0x00, 0x00, 0x01 };
		out.insert(out.end(), kStartCode, kStartCode + 4);
	}

	inline u32 ReadBE(const u8 *p, int n)
	{
		u32 v = 0;
		for (int i = 0; i < n; i++)
			v = (v << 8) | p[i];
		return v;
	}

	// Copy `count` length-prefixed (always 2-byte, inside a config record) NAL
	// units starting at *pos, each preceded by a start code. Advances *pos.
	bool CopyPrefixedNals(const u8 *cfg, size_t cfgLen, size_t *pos,
						  unsigned count, std::vector<u8> &out)
	{
		for (unsigned i = 0; i < count; i++)
		{
			if (*pos + 2 > cfgLen)
				return false;
			const u32 nalLen = ReadBE(cfg + *pos, 2);
			*pos += 2;
			if (*pos + nalLen > cfgLen)
				return false;
			AppendStartCode(out);
			out.insert(out.end(), cfg + *pos, cfg + *pos + nalLen);
			*pos += nalLen;
		}
		return true;
	}
}

bool MT_AnnexB::ParseConfigRecord(const u8 *cfg, size_t cfgLen, bool isHevc,
								  std::vector<u8> &outParameterSets, int &outNalLengthSize)
{
	outParameterSets.clear();
	outNalLengthSize = 4;

	if (cfg == NULL || cfgLen == 0)
		return false;

	if (!isHevc)
	{
		// avcC: 5 fixed bytes, then SPS array, then PPS array. The length size
		// lives in the low 2 bits of byte 4 (the other 6 are reserved 1s).
		if (cfgLen < 7)
			return false;
		outNalLengthSize = (int)((cfg[4] & 0x03) + 1);
		size_t pos = 5;
		const unsigned numSps = cfg[pos++] & 0x1F;
		if (!CopyPrefixedNals(cfg, cfgLen, &pos, numSps, outParameterSets))
			return false;
		if (pos >= cfgLen)
			return true;   // SPS present, PPS in-band: unusual but not an error
		const unsigned numPps = cfg[pos++];
		if (!CopyPrefixedNals(cfg, cfgLen, &pos, numPps, outParameterSets))
			return false;
		return true;
	}

	// hvcC: 22 fixed bytes, then an array-of-arrays (VPS, SPS, PPS, SEI...).
	// The length size is in the low 2 bits of byte 21.
	if (cfgLen < 23)
		return false;
	outNalLengthSize = (int)((cfg[21] & 0x03) + 1);
	size_t pos = 22;
	const unsigned numArrays = cfg[pos++];
	for (unsigned a = 0; a < numArrays; a++)
	{
		// One byte of array_completeness/reserved/NAL_unit_type, which the
		// Annex-B stream does not carry: the NAL header inside each unit
		// already says what it is.
		if (pos + 3 > cfgLen)
			return false;
		pos += 1;
		const unsigned numNalus = ReadBE(cfg + pos, 2);
		pos += 2;
		if (!CopyPrefixedNals(cfg, cfgLen, &pos, numNalus, outParameterSets))
			return false;
	}
	return true;
}

bool MT_AnnexB::ConvertPacket(const u8 *data, size_t size, int nalLengthSize,
							  std::vector<u8> &out)
{
	out.clear();
	if (data == NULL || size == 0)
		return false;
	if (nalLengthSize != 1 && nalLengthSize != 2 && nalLengthSize != 4)
		return false;

	// Every NAL grows by (4 - nalLengthSize) bytes at most; reserving the
	// worst case keeps this to one allocation on the decode thread.
	out.reserve(size + (size / 8) + 16);

	size_t pos = 0;
	while (pos + (size_t)nalLengthSize <= size)
	{
		const u32 nalLen = ReadBE(data + pos, nalLengthSize);
		pos += (size_t)nalLengthSize;
		if (nalLen == 0)
			continue;               // a zero-length NAL is skippable, not fatal
		if (pos + nalLen > size)
		{
			// Truncated relative to its own prefix: the packet is corrupt, and
			// handing a decoder half a NAL is how a crash gets blamed on the
			// decoder.
			LOGWarning("MT_AnnexB::ConvertPacket: NAL length %u overruns %zu-byte packet", nalLen, size);
			return false;
		}
		AppendStartCode(out);
		out.insert(out.end(), data + pos, data + pos + nalLen);
		pos += nalLen;
	}
	return !out.empty();
}
