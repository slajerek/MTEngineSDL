#ifndef _MFCOMTHREADGUARD_H_
#define _MFCOMTHREADGUARD_H_

#pragma once

// Shared thread-affine COM + process-wide Media Foundation lifetime helpers.
// Every native Media Foundation decoder in this engine (CAudioDecoderAACMF --
// Task 5, CVideoDecoderHEVCMF -- Task 6, and any future MF decoder) needs the
// identical two-liner at the top of every entry point that touches COM/MF:
// EnsureComInitializedForThisThread() once per decode-thread call site,
// EnsureMFStarted() once per process. Extracted here (from
// CAudioDecoderAACMF.cpp, where it first appeared) so Task 6 shares the
// mechanism instead of duplicating it -- header-only (inline functions), so
// this stays a zero-link-cost dependency. Compiles to nothing anywhere
// except a Windows FFmpeg-enabled build.
#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include <mfapi.h>
#include <mfidl.h>

namespace MFComThreadGuard
{
	// CVideoPlayer's decode worker thread (CVideoPlayer.cpp, DecodeThreadFunc)
	// is never CoInitialized by anything else in this engine. Every entry
	// point that touches COM/MF constructs one of these (via the thread_local
	// in EnsureComInitializedForThisThread(), below) on first use per thread;
	// CoInitializeEx is therefore called exactly once per thread, and the
	// balancing CoUninitialize runs from this guard's destructor at
	// thread-exit (thread_local storage duration).
	struct SComThreadGuard
	{
		bool ownsInit = false;
		SComThreadGuard()
		{
			HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			// RPC_E_CHANGED_MODE: this thread was already CoInitialized (by
			// someone else, possibly in a different apartment mode) --
			// treat as success-without-ownership: never CoUninitialize a
			// mode this guard didn't establish.
			ownsInit = SUCCEEDED(hr);
		}
		~SComThreadGuard()
		{
			if (ownsInit)
				CoUninitialize();
		}
	};

	inline void EnsureComInitializedForThisThread()
	{
		thread_local SComThreadGuard guard;
		(void)guard;
	}

	// MFStartup is process-wide (not thread-affine) -- a function-local
	// static's initialization is itself thread-safe/once per the standard,
	// so this needs no additional locking. Immortal on purpose: MFShutdown
	// is deliberately never called (process-lifetime, same as the underlying
	// MF decoder DLLs staying loaded for the process's life regardless).
	inline void EnsureMFStarted()
	{
		static HRESULT mfStartupResult = MFStartup(MF_VERSION, MFSTARTUP_LITE);
		(void)mfStartupResult; // best-effort -- a real failure surfaces downstream via CoCreateInstance/MFTEnumEx/SetInputType/SetOutputType
	}
}

#endif // _WIN32 && MT_ENABLE_FFMPEG

#endif
//_MFCOMTHREADGUARD_H_
