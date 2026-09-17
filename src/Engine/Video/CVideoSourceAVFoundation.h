#pragma once

// An IVideoSource that demuxes AND decodes through AVFoundation.
//
// WHY THIS EXISTS. Every commercial build in this programme ships only through
// app stores, and LGPL-2.1 #6 cannot be satisfied inside a signed, receipt-
// checked bundle whose EULA forbids reverse engineering -- so FFmpeg cannot be
// distributed at all, as decoder OR demuxer. The OS can: Apple licenses the
// codecs it ships, so a store build plays H.264/HEVC/ProRes/AAC through
// AVFoundation with no licence question of ours.
//
// WHY THE OS DECODES HERE, rather than us feeding packets to VideoToolbox.
// The engine already has a packet-decoder seam (IVideoPacketDecoder,
// CVideoDecoderHEVCVT) and it is kept -- it is what MKV/AVI/MPEG-PS will need,
// because no OS API demuxes those. But for the containers AVFoundation DOES
// demux, letting it decode too is strictly less code and strictly more
// formats: it covers H.264, HEVC, ProRes, MJPEG and MPEG-2 without a wrapper
// class each, and its output arrives already in presentation order, which
// deletes the reorder window and the kVTInvalidSessionErr recovery that owning
// a decompression session requires.
//
// WHAT IT DOES NOT COVER, and why that is not this class's problem: MKV, AVI
// and MPEG-PS. AVFoundation will not open them, so they are routed elsewhere
// (see CVideoPlayer::CreateAndOpenSource). Dropping them was proposed and
// rejected -- they are registered system handlers -- so they keep the FFmpeg
// path in a private build and get permissive demuxers of their own later.

#include "IVideoSource.h"

// Guarded on the CAPABILITY as well as the platform: an app that resolves
// MT_CAP_VIDEO_PLAYBACK=0 (c64d) must not be made to link AVFoundation for a
// source it never constructs.
#if defined(__APPLE__) && (!defined(MT_CAP_VIDEO_PLAYBACK) || MT_CAP_VIDEO_PLAYBACK)

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

class CVideoSourceAVFoundation : public IVideoSource
{
public:
	CVideoSourceAVFoundation();
	~CVideoSourceAVFoundation() override;

	// Cheap container probe used by the routing chain BEFORE constructing a
	// source: does AVFoundation claim to handle this path's extension at all?
	// Extension-based on purpose -- opening the file to find out is what made
	// the old FFmpeg probe stall for the whole download on an iCloud-evicted
	// file, and CFolderScanner calls the routing path per file while scanning.
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

	// Open the container and read Info() only -- no decoding, no audio output.
	// CFolderScanner reads creationTime for every video in a browsed folder and
	// CVideoFrameExtractor wants dimensions before it commits to a decode; both
	// paid for a full decoder open before this existed.
	void SetOpenVideoOnly(bool value) { openVideoOnly = value; }

private:
	struct SImpl;
	SImpl *impl = nullptr;

	// Guards the AVAssetReader pointer ONLY, and exists because WakeAbort()
	// is called from the render/command thread while the decode thread may be
	// inside copyNextSampleBuffer or tearing the reader down. Without it the
	// two race over the same pointer -- observed as a SIGSEGV in
	// CVideoPlayer::StopDecodeThread during a VideoPlayback suite
	// case 13, which cancels a seek mid-flight and then closes.
	//
	// It is NEVER held across a blocking read: the read path copies the
	// pointer out under the lock (ARC retains the local) and releases before
	// calling into AVFoundation, so WakeAbort can always run promptly -- which
	// is the whole point of WakeAbort existing.
	mutable std::mutex readerMutex;

	// WakeAbort() sets this and does NOTHING ELSE -- see the long note on
	// WakeAbort in the .mm. Cancelling an AVAssetReader while a read is in
	// flight crashes inside CoreMedia; this flag lets the read paths abort
	// BETWEEN reads instead, where no cancel can race one.
	std::atomic<bool> cancelRequested{false};

	SVideoInfo info;
	std::string errorReason;
	std::function<bool()> abortPredicate;
	bool openVideoOnly = false;
	std::string filePath;

	// Decoded output, owned here: IVideoSource's contract is that planes stay
	// valid until the next ReadVideoFrame/Seek/Close, and a CVPixelBuffer is
	// not retained past the copy (CVideoDecoderHEVCVT does the same -- there is
	// no zero-copy path through SDecodedVideoFrame's borrowed-plane shape).
	std::vector<u8> planeY, planeUV, planeU10, planeV10;
	int strideY = 0, strideUV = 0, strideC10 = 0;
	int frameWidth = 0, frameHeight = 0;
	bool frameIs10Bit = false;
	// The pts of the frame most recently handed out. Seek() presets it to the
	// target so a caller that seeks and asks for Info-level position before
	// reading gets the requested time rather than a stale one.
	double lastVideoPts = 0.0;

	std::vector<s16> audioPcm;

	bool IsAborted() const
	{
		return cancelRequested.load(std::memory_order_acquire) ||
			   (abortPredicate && abortPredicate());
	}
	bool BuildReader(double startSeconds);
	void TeardownReader();
	bool FillInfoFromAsset();
};

#endif // __APPLE__ && MT_CAP_VIDEO_PLAYBACK
