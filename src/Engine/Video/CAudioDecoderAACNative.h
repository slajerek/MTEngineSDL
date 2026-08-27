#ifndef _CAUDIODECODERAACNATIVE_H_
#define _CAUDIODECODERAACNATIVE_H_

#pragma once

// Selects the platform's OS-licensed native AAC decoder. This is the one
// header-visible seam CVideoSourceFFmpeg.h/.cpp depends on -- a cpp-local
// alias (typedef/using declared only inside a .cpp) can never compile on a
// platform whose header the class declaration itself needs to reference
// (CVideoSourceFFmpeg.h declares std::unique_ptr<CAudioDecoderAACNative>
// aacDecoder as a member, so the alias must exist wherever that header is
// parsed, i.e. on every platform that #includes it).
//
// MT_HAVE_NATIVE_AAC is the gate every #ifdef __APPLE__ around aacDecoder/
// aacExtradata/QueueAACPacket in CVideoSourceFFmpeg.h/.cpp re-targets to --
// defined here (and only here) so adding a new platform's native decoder is a
// one-header change.
#if defined(__APPLE__)
	#define MT_HAVE_NATIVE_AAC 1
	class CAudioDecoderAACApple;
	using CAudioDecoderAACNative = CAudioDecoderAACApple;
#elif defined(_WIN32)
	#define MT_HAVE_NATIVE_AAC 1
	class CAudioDecoderAACMF;
	using CAudioDecoderAACNative = CAudioDecoderAACMF;
#endif

#endif
//_CAUDIODECODERAACNATIVE_H_
