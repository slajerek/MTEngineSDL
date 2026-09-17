#pragma once

// The Windows twin of CVideoSourceAVFoundation: an IVideoSource that demuxes
// AND decodes through Media Foundation, so a Microsoft Store build ships no
// FFmpeg. Read that header first -- the reasoning is identical and is not
// repeated here.
//
// NEVER COMPILED. Written on macOS, where no part of this file can be built or
// run. Every design choice mirrors the AVFoundation source deliberately, so the
// two can be compared line for line; where Media Foundation forces a different
// shape it is called out in the .cpp. See the Windows handover notes for the
// checks this owes before it can be believed.

#include "IVideoSource.h"

#if defined(_WIN32) && (!defined(MT_CAP_VIDEO_PLAYBACK) || MT_CAP_VIDEO_PLAYBACK)

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

class CVideoSourceMediaFoundation : public IVideoSource
{
public:
	CVideoSourceMediaFoundation();
	~CVideoSourceMediaFoundation() override;

	// Extension probe, matching CVideoSourceAVFoundation::CanProbablyOpen.
	// Media Foundation demuxes MORE than AVFoundation does -- it reads MPEG-TS
	// and ASF, which AVFoundation will not touch -- so this list is
	// deliberately LONGER than the macOS one. That asymmetry is real: a Windows
	// store build keeps .mts/.m2ts/.ts and .wmv/.asf, and a macOS store build
	// does not.
	static bool CanProbablyOpen(const char *filePath);

	bool Open(const char *filePath) override;
	const SVideoInfo &Info() const override { return info; }
	bool ReadVideoFrame(SDecodedVideoFrame &out) override;
	bool ReadAudio(SDecodedAudio &out) override;
	bool Seek(double seconds) override;
	const std::string &GetErrorReason() const override { return errorReason; }
	void Close() override;

	void SetAbortPredicate(std::function<bool()> pred) override;
	void WakeAbort() override;

	void SetOpenVideoOnly(bool value) { openVideoOnly = value; }

private:
	struct SImpl;
	SImpl *impl = nullptr;

	SVideoInfo info;
	std::string errorReason;
	std::function<bool()> abortPredicate;
	bool openVideoOnly = false;

	// Same cooperative-abort design as the AVFoundation source, and for the
	// same measured reason: tearing a reader down from another thread while a
	// read is in flight crashes inside the OS media stack. WakeAbort raises
	// this; the read paths honour it BETWEEN reads.
	std::atomic<bool> cancelRequested{false};
	mutable std::mutex readerMutex;

	std::vector<u8> planeY, planeUV;
	int strideY = 0, strideUV = 0;
	int frameWidth = 0, frameHeight = 0;
	double lastVideoPts = 0.0;

	std::vector<s16> audioPcm;

	bool IsAborted() const
	{
		return cancelRequested.load(std::memory_order_acquire) ||
			   (abortPredicate && abortPredicate());
	}
	bool ConfigureStreams();
	bool FillInfoFromReader();
};

#endif // _WIN32 && MT_CAP_VIDEO_PLAYBACK
