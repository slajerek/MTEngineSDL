#ifndef _CAUDIODECODERCOMPRESSEDNATIVE_H_
#define _CAUDIODECODERCOMPRESSEDNATIVE_H_

#pragma once

// Selects the platform's OS-licensed MP3 / MP2 / AC-3 decoder, for the AVI
// lane. The same one-header seam CAudioDecoderAACNative.h established, and
// for the same reason: a cpp-local alias cannot serve a class declaration
// that names the type as a member.
//
// MT_HAVE_NATIVE_COMPRESSED_AUDIO is the gate. LINUX DEFINES NEITHER, and
// that is a real limitation rather than an oversight: there is no OS media
// API there, so an AVI's audio cannot be decoded without vendoring a decoder,
// and the lane says so instead of playing silence.

#if defined(__APPLE__)
	#define MT_HAVE_NATIVE_COMPRESSED_AUDIO 1
	class CAudioDecoderCompressedApple;
	using CAudioDecoderCompressedNative = CAudioDecoderCompressedApple;
#elif defined(_WIN32)
	#define MT_HAVE_NATIVE_COMPRESSED_AUDIO 1
	class CAudioDecoderCompressedMF;
	using CAudioDecoderCompressedNative = CAudioDecoderCompressedMF;
#endif

#endif
//_CAUDIODECODERCOMPRESSEDNATIVE_H_
