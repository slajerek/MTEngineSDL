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
#include <vector>

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
//   psPresent      -- true if Parametric Stereo (HE-AAC v2) is signaled,
//                      either by the direct top-level audioObjectType 29 or
//                      by the backward-compatible 0x548 sync marker after the
//                      SBR extension. PS decodes a mono core to STEREO, so
//                      `channels` (the core count) is then 1 while the decoded
//                      output is 2.
//   directSignaled -- true if the ASC used the direct ("explicit
//                      hierarchical") form, top-level audioObjectType 5/29.
//                      Media Foundation rejects that form; see
//                      AacMakeBackwardCompatibleAsc().
struct SAacAscInfo
{
	bool ok = false;
	int channels = 0;
	int coreSampleRate = 0;
	bool sbrPresent = false;
	int sbrSampleRate = 0; // valid only if sbrPresent
	bool psPresent = false;
	bool directSignaled = false;

	// Raw fields kept so the ASC can be re-emitted in the other signaling
	// form (AacMakeBackwardCompatibleAsc). Not meaningful unless ok.
	int coreAudioObjectType = 0;
	int coreSamplingFrequencyIndex = 0;
	int channelConfiguration = 0;
	int extensionSamplingFrequencyIndex = 0; // valid only if sbrPresent
	bool frameLengthFlag = false;
	bool dependsOnCoreCoder = false;
	int coreCoderDelay = 0;
	bool extensionFlag = false;
};

// Parses just enough of the ASC to learn channel count, the core AAC sample
// rate, and (if explicitly signaled) SBR presence and its extension sample
// rate. Does not attempt audioObjectType==31 (extended object type) or
// PCE-based channel configs -- neither appears in any fixture this project
// ships. See AacAscParser.cpp for the full bit-layout commentary.
SAacAscInfo AacParseAudioSpecificConfig(const u8 *asc, int ascSize);

// The AAC magic cookie AudioToolbox actually accepts: an MPEG-4 ES_Descriptor
// (tag 0x03) wrapping a DecoderConfigDescriptor (0x04, objectType 0x40 =
// MPEG-4 Audio, streamType 0x05 = audio) whose DecoderSpecificInfo (0x05) is
// the ASC -- i.e. the payload of an mp4 `esds` atom WITHOUT its 4 bytes of
// version/flags, which is the shape AudioFile reports as an AAC cookie.
// A bare ASC is rejected with kAudioCodecUnsupportedFormatError ('!dat');
// that is why the cookie looked "unusable" for so long (2026-10-13: measured
// on a real direct-signaled HE-AAC mp4, see CAudioDecoderAACApple.cpp).
// Platform-neutral so it is unit-tested everywhere. Empty on invalid input.
std::vector<u8> AacBuildEsdsCookie(const u8 *asc, int ascSize);

// Media Foundation's AAC decoder accepts ONLY the backward-compatible form:
// "audioObjectType ... must be 2 ... extensionAudioObjectType must be 5 for
// SBR or 29 for PS" (Microsoft Learn, "AAC Decoder", MF_MT_USER_DATA). This
// re-emits a directly-signaled (AOT 5/29) ASC in that form -- AOT 2 core,
// GASpecificConfig, then the 0x2b7 SBR trailer and, for PS, the 0x548 PS
// trailer. Any other ASC (already backward-compatible, plain LC) is returned
// unchanged. Empty if the ASC does not parse, or if the direct form wraps a
// core other than AAC-LC (no backward-compatible equivalent MF would accept).
std::vector<u8> AacMakeBackwardCompatibleAsc(const u8 *asc, int ascSize);

#endif
//_AACASCPARSER_H_
