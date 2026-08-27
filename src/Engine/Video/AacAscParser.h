#ifndef _AACASCPARSER_H_
#define _AACASCPARSER_H_

#pragma once

// Platform-neutral AudioSpecificConfig (ISO/IEC 14496-3) bit parser, shared by
// every native AAC decoder this engine ships (CAudioDecoderAACApple on Apple,
// CAudioDecoderAACMF on Windows -- see CAudioDecoderAACNative.h for the
// selection seam). Pure bit-twiddling, no OS dependencies -- compiled
// unconditionally on every platform, unlike the decoders themselves.
//
// Originally file-static inside CAudioDecoderAACApple.cpp; hoisted out here
// so the Media Foundation decoder (which needs the exact same channel count /
// core sample rate / SBR-extended sample rate fields to build its
// HEAACWAVEINFO input media type) doesn't have to duplicate it, and so it can
// be unit-tested directly instead of through a platform-gated test hook.

#include "SYS_Defs.h"

// Parse-result fields:
//   ok             -- true if the ASC parsed as a supported AAC-family shape
//                      (GASpecificConfig object types 2/3/4/6/7, either
//                      explicit-hierarchical or backward-compatible SBR
//                      signaling). false on any unsupported/malformed input
//                      (e.g. audioObjectType==31, PCE-based channel config).
//   channels       -- core channel count (ISO/IEC 14496-3 Table 1.19).
//   coreSampleRate -- core AAC sample rate, Hz (Table 1.16).
//   sbrPresent     -- true if SBR (HE-AAC) extension is explicitly signaled,
//                      either via the "direct" top-level audioObjectType
//                      5/29 form or the "backward compatible" trailing
//                      0x2b7-sync-marker form.
//   sbrSampleRate  -- valid only when sbrPresent; the SBR-extended
//                      (post-doubling) sample rate, Hz.
struct SAacAscInfo
{
	bool ok = false;
	int channels = 0;
	int coreSampleRate = 0;
	bool sbrPresent = false;
	int sbrSampleRate = 0; // valid only if sbrPresent
};

// Parses just enough of the ASC to learn channel count, the core AAC sample
// rate, and (if explicitly signaled) SBR presence and its extension sample
// rate. Does not attempt audioObjectType==31 (extended object type) or
// PCE-based channel configs -- neither appears in any fixture this project
// ships. See AacAscParser.cpp for the full bit-layout commentary.
SAacAscInfo AacParseAudioSpecificConfig(const u8 *asc, int ascSize);

#endif
//_AACASCPARSER_H_
