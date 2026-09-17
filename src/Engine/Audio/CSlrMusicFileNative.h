#ifndef _CSLRMUSICFILENATIVE_H_
#define _CSLRMUSICFILENATIVE_H_

// One name for "the OS's own audio decoder", per platform.
//
// Same seam CAudioDecoderAACNative.h established for the packet decoders, and
// deliberately the same shape so there is one idiom rather than two: this
// header defines MT_HAVE_NATIVE_MUSIC and aliases the platform twin, so a call
// site carries no #ifdef of its own.
//
// It covers AAC and ALAC -- the two formats that live in MP4/M4A and that the
// engine cannot decode itself without either an LGPL library or an MP4 demuxer
// plus a magic-cookie/esds builder (tracker 4.3). The OS holds those codec
// licences, which is what makes this store-safe.
//
// MT_HAVE_NATIVE_MUSIC IS NOT DEFINED ON LINUX, and that is a real gap rather
// than an oversight: there is no OS media API to route to, so .m4a is not
// offered there. Consumers must test for it, exactly as they do for
// MT_HAVE_NATIVE_AAC.

#if defined(__APPLE__) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)
	#include "CSlrMusicFileAVFAudio.h"
	#define MT_HAVE_NATIVE_MUSIC 1
	using CSlrMusicFileNative = CSlrMusicFileAVFAudio;
#elif defined(_WIN32) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)
	#include "CSlrMusicFileMFAudio.h"
	#define MT_HAVE_NATIVE_MUSIC 1
	using CSlrMusicFileNative = CSlrMusicFileMFAudio;
#endif

#endif
