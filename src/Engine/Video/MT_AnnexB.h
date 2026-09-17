#ifndef _MT_ANNEXB_H_
#define _MT_ANNEXB_H_

#pragma once

// Length-prefixed NAL units -> Annex-B start codes, for H.264 and HEVC.
//
// WHY THIS EXISTS. MP4 and Matroska both carry H.264/HEVC as length-prefixed
// NAL units described by an `avcC`/`hvcC` configuration record. Some decoders
// take exactly that (VideoToolbox does), and some want Annex-B -- a flat byte
// stream with 00 00 00 01 start codes and the parameter sets inline. Media
// Foundation is in the second group, which IVideoPacketDecoder::WantsAnnexB()
// already announces.
//
// Until now the conversion was FFmpeg's `hevc_mp4toannexb` bitstream filter,
// installed by CVideoSourceFFmpeg. A store build has no FFmpeg, so a lane that
// demuxes for itself (the Matroska lane) has to do this itself or lose every
// Annex-B decoder -- which on Windows is the only decoder there is.
//
// PLATFORM-NEUTRAL AND SEPARATELY TESTABLE ON PURPOSE. The Windows path that
// needs it cannot be compiled on the machine this was written on, so the
// conversion is pure byte manipulation with no OS dependency, and
// CTestAnnexBConvert exercises it against the real configuration record of a
// real fixture on macOS. A converter that is only exercised by the platform
// that cannot be built here would be untested code shipped on faith.

#include "SYS_Defs.h"
#include <vector>
#include <cstddef>

namespace MT_AnnexB
{
	// Parse an avcC (isHevc=false) or hvcC (isHevc=true) configuration record.
	//
	// `outParameterSets` receives every SPS/PPS/VPS it contains, each preceded
	// by a 4-byte start code -- the extradata an Annex-B decoder expects.
	// `outNalLengthSize` receives the 1/2/4-byte length prefix width that
	// ConvertPacket() then needs.
	//
	// Returns false on a record that is malformed or truncated. It does NOT
	// return false merely because a record contains no parameter sets: some
	// muxers legitimately put them in-band, and refusing those would turn a
	// playable file into an error.
	bool ParseConfigRecord(const u8 *cfg, size_t cfgLen, bool isHevc,
						   std::vector<u8> &outParameterSets, int &outNalLengthSize);

	// One length-prefixed sample -> Annex-B. Appends to `out` (cleared first).
	// Returns false if the sample is truncated relative to its own prefixes,
	// which means the packet is corrupt rather than merely unusual.
	bool ConvertPacket(const u8 *data, size_t size, int nalLengthSize,
					   std::vector<u8> &out);
}

#endif
//_MT_ANNEXB_H_
