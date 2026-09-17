#include "MT_Mp4vEsds.h"

namespace
{
	// Descriptor lengths are a base-128 varint. The 4-byte long form is used
	// unconditionally -- it is what every muxer in the wild writes, it keeps
	// the encoding fixed-width so the enclosing lengths need no second pass,
	// and it is what the reference file measured against contains.
	void PutTagLen(std::vector<u8> &o, u8 tag, size_t len)
	{
		o.push_back(tag);
		o.push_back((u8)(0x80 | ((len >> 21) & 0x7F)));
		o.push_back((u8)(0x80 | ((len >> 14) & 0x7F)));
		o.push_back((u8)(0x80 | ((len >>  7) & 0x7F)));
		o.push_back((u8)(len & 0x7F));
	}
}

std::vector<u8> MT_Mp4vEsds::Build(const u8 *vol, size_t volLen)
{
	std::vector<u8> out;
	if (vol == NULL || volLen == 0)
		return out;

	// DecoderConfigDescriptor (tag 0x04).
	std::vector<u8> dcd;
	dcd.push_back(0x20);                        // objectTypeIndication: MPEG-4 Visual
	dcd.push_back(0x11);                        // streamType 4 (visual) << 2 | reserved 1
	dcd.insert(dcd.end(), 3, (u8)0);            // bufferSizeDB -- unknown, and unused by VT
	dcd.insert(dcd.end(), 4, (u8)0);            // maxBitrate
	dcd.insert(dcd.end(), 4, (u8)0);            // avgBitrate
	PutTagLen(dcd, 0x05, volLen);               // DecoderSpecificInfo
	dcd.insert(dcd.end(), vol, vol + volLen);

	// ES_Descriptor body (tag 0x03).
	std::vector<u8> es;
	es.push_back(0x00); es.push_back(0x01);     // ES_ID
	es.push_back(0x00);                         // flags: no dependency, no URL, no OCR
	PutTagLen(es, 0x04, dcd.size());
	es.insert(es.end(), dcd.begin(), dcd.end());
	PutTagLen(es, 0x06, 1);                     // SLConfigDescriptor
	es.push_back(0x02);                         // "predefined: MP4"

	out.insert(out.end(), 4, (u8)0);            // full-box version + flags
	PutTagLen(out, 0x03, es.size());
	out.insert(out.end(), es.begin(), es.end());
	return out;
}
