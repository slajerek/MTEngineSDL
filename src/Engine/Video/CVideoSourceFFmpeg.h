#ifndef _CVIDEOSOURCEFFMPEG_H_
#define _CVIDEOSOURCEFFMPEG_H_

#pragma once

// Entire header is a no-op unless the platform build links the bundled
// FFmpeg dylibs (macOS today; see platform/MacOS/build-video_codecs.sh).
// Non-macOS builds must still be able to #include this header and compile
// translation units that reference it conditionally.
#if MT_ENABLE_FFMPEG

#include "IVideoSource.h"
#include "IVideoPacketDecoder.h"
#include <cstdint>
#include <deque>
#include <memory>
#include <string>
#include <vector>

// Forward declarations for FFmpeg's C API -- keeps the FFmpeg headers (and
// the licensing surface they imply) out of anything that merely #includes
// this header. Only CVideoSourceFFmpeg.cpp includes the real libav* headers.
extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVPacketSideData;
}
struct SwsContext;
struct SwrContext;
struct AVBSFContext;

#include "CAudioDecoderAACNative.h"
#include "CAudioDecoderWMANative.h"

// IVideoPacketDecoder (IVideoPacketDecoder.h) is the hook consumed by Task 7
// (platform-native HEVC decode -- the bundled FFmpeg carries no HEVC decoder
// for licensing reasons). A packet decoder receives raw HEVC access units
// demuxed by CVideoSourceFFmpeg and hands back frames in the same
// SDecodedVideoFrame shape as the libavcodec path.

// FFmpeg (libavformat/libavcodec/libswresample/libswscale) demux + decode
// behind IVideoSource. Covers every container/codec combination the bundled
// FFmpeg ships a decoder for (H.264, ProRes, MJPEG, MPEG-4 part 2, MPEG-2,
// VP8, VP9, ...); HEVC and AAC are intentionally absent from the bundled
// build (licensing) and are refused/skipped here until Tasks 7/8 land.
class CVideoSourceFFmpeg : public IVideoSource
{
public:
	CVideoSourceFFmpeg();
	virtual ~CVideoSourceFFmpeg();

	bool Open(const char *filePath) override;
	const SVideoInfo &Info() const override { return info; }
	bool ReadVideoFrame(SDecodedVideoFrame &out) override;
	bool ReadAudio(SDecodedAudio &out) override;
	// Audio pump-ahead (see IVideoSource.h): demuxes forward queueing audio,
	// parking compressed video packets for the next ReadVideoFrame(). Caps:
	// kMaxParkedPackets / kMaxParkedBytes below.
	bool PumpAudioAhead() override;
	bool Seek(double seconds) override;
	// Fast-seek variant (poster/thumbnail extraction). The interface Seek()
	// decodes forward internally until it reaches `seconds - 0.05`, so the
	// caller's next ReadVideoFrame() returns the frame at/after the target --
	// exact, but for long-GOP content that internal walk decodes (and
	// discards) a whole GOP's worth of frames. With
	// stopAtFirstDecodedFrame=true the internal walk stops at the FIRST
	// successfully decoded frame after the backward keyframe landing; the
	// caller's next ReadVideoFrame() then returns the frame right after the
	// keyframe (~2 decodes total). Same clamp/flush/margin-retry semantics
	// otherwise; false is byte-identical to the interface Seek().
	bool Seek(double seconds, bool stopAtFirstDecodedFrame);
	// The IVideoSource fast-seek capability (Task 6): the burst half of
	// CVideoPlayer's two-phase seek, mapped onto the internal seek primitive --
	// the very same keyframe-only landing the poster/thumbnail path has always
	// used. `outLandedPts` (Coarse-seek task) reports the landed keyframe's
	// clip-relative pts; see IVideoSource::SeekFast() for the contract.
	bool SeekFast(double seconds, double *outLandedPts = nullptr) override;

	// FORWARD keyframe seek (Coarse-seek task). CVideoPlayer's Coarse mode calls
	// this when a BACKWARD keyframe landing made no forward progress (the step is
	// smaller than the current GOP, so AVSEEK_FLAG_BACKWARD re-lands the same
	// keyframe): seek to the FIRST keyframe AT OR AFTER `seconds` (av_seek_frame
	// without AVSEEK_FLAG_BACKWARD) and stop at the first decoded frame, reporting
	// its pts in `outLandedPts`. Returns false (leaving *outLandedPts untouched)
	// when there is no later keyframe -- a single-keyframe/long-GOP file -- or on
	// abort; the caller then falls back to a Precise seek to guarantee progress.
	bool SeekFastForward(double seconds, double *outLandedPts) override;
	const std::string &GetErrorReason() const override { return errorReason; }
	void Close() override;

	// --- Abort plumbing (spec #2.3; see IVideoSource.h for the contract) ------
	//
	// TWO levels, because they cover disjoint kinds of blocking:
	//  * AVIOInterruptCB on the AVFormatContext -- cuts short blocking DEMUX I/O
	//    (av_seek_frame / av_read_frame on a slow file). FFmpeg POLLS it between
	//    I/O operations; it does NOT unblock a read(2) already stuck in the
	//    kernel, and it does NOT cover Open() with today's callers (they install
	//    the predicate afterwards) -- see the Open() comment in the .cpp.
	//  * a cooperative predicate check once per frame iteration in the seek
	//    forward-scan and in ReadVideoFrame()'s packet loop -- interrupts the
	//    CPU DECODE work the interrupt callback cannot see, bounding abort
	//    latency to ONE frame decode instead of a whole scan-to-target.
	//  * the SAME predicate, FORWARDED to the active packet decoder
	//    (IVideoPacketDecoder::SetAbortPredicate()) -- interrupts a decoder that
	//    can block INSIDE a single DecodePacket() call (Media Foundation's HEVC
	//    pipeline: a 10-second condvar wait), which neither of the two above can
	//    see. WakeAbort() then merely POKES that decoder so its wait re-evaluates
	//    the (level-triggered) predicate; it carries no state of its own.
	void SetAbortPredicate(std::function<bool()> pred) override;
	void WakeAbort() override;

	// Set before Open(). When the video stream is HEVC and a native decoder is
	// installed (Task 7), packets go there; without one, Open() fails with
	// kVideoErrorHEVCNeedsNativeDecoder in commercial builds, and falls
	// through to FFmpeg software decode in full builds (2026-07-19
	// codec-superset spec). An injected decoder's Init() failure always
	// propagates as an error (no fallthrough) so injected-failure tests stay
	// deterministic; only the auto-installed decoder falls back.
	void SetHEVCPacketDecoder(IVideoPacketDecoder *decoder) { nativeDecoder = decoder; }

	// Test/future-proofing hook: when true, Open() skips ALL native decoders
	// (video and audio -- the HEVC/WMV video auto-install and the native
	// AAC/WMA audio blocks), regardless of SetHEVCPacketDecoder(). Defaults
	// to false. Commercial builds then refuse HEVC with the "no native
	// decoder available on this platform" message; full builds fall through
	// to FFmpeg software decode (2026-07-19 codec-superset spec) -- so this
	// seam tests the refusal path AND the software fallbacks independently
	// of what the platform actually provides.
	void SetDisableAutoNativeDecoders(bool disable) { disableAutoNativeDecoders = disable; }

	// Set before Open(). Video-only consumers (poster/thumbnail extraction --
	// CVideoFrameExtractor) never call ReadAudio(), yet a normal Open() still
	// opens the audio decoder and every ReadVideoFrame() decodes + queues the
	// interleaved audio, all of it thrown away (visible in the field as
	// AudioCodecInitialize attempts and PushPendingAudio cap-overflow
	// warnings during poster preloads). When true, Open() skips the audio
	// decoder entirely and audio packets are discarded at demux; ReadAudio()
	// then never yields data. Info().hasAudio stays TRUTHFUL -- it reports
	// stream presence from container metadata, not decoder state.
	void SetOpenVideoOnly(bool videoOnly) { openVideoOnly = videoOnly; }

	// Platform-conditional: Windows gets an actionable, store-facing message
	// (the "HEVC Video Extensions" free add-on resolves most cases; the rest
	// are pre-HEVC-hardware GPUs where no software MFT is registered either).
	// Every other platform keeps the generic message -- macOS never actually
	// hits this string (VideoToolbox is always available, see
	// IVideoPacketDecoder::IsHEVCDecodeAvailable()'s Apple definition), and
	// Linux has no native decoder story yet.
#ifdef _WIN32
	static constexpr const char *kVideoErrorHEVCNeedsNativeDecoder =
		"This HEVC video needs a newer GPU, or install the Microsoft HEVC Video Extensions.";
#else
	static constexpr const char *kVideoErrorHEVCNeedsNativeDecoder =
		"HEVC requires the platform video decoder";
#endif

	// WMV/VC-1 refusal (2026-07-18 WMV spec): surfaced when the file's video
	// is the WMV family and neither a native decoder (Windows MF) nor an
	// FFmpeg software decoder (full builds only) is available -- i.e.
	// commercial builds on macOS/Linux, or a Windows N edition without the
	// Media Feature Pack. Starts with IVideoSource's stable
	// kVideoErrorTokenEditionUnsupported token (tests/UI match the token,
	// not this copy).
	static constexpr const char *kVideoErrorWMVEditionUnsupported =
		"[edition-unsupported] WMV playback is not available in this edition of the app.";

	// Quick probe (opens/closes its own AVFormatContext) used by CVideoPlayer's
	// routing: true only for a Matroska/WebM container whose video stream is
	// VP9 AND carries an "alpha_mode" tag. The legacy nestegg/vpx/opus path
	// (CVideoSourceWebMVpx) is the only one that decodes the alpha plane
	// today, so those specific files keep routing there; every other file
	// (including ordinary non-alpha VP9 webm) comes through this class.
	static bool ProbeIsAlphaVP9WebM(const char *filePath);

	// Test hook (Task 1 of the video playback plan): number of frames decoded
	// via ReadVideoFrame() since the most recent Seek() call (reset to 0 at
	// the top of Seek(), incremented once per frame actually emitted --
	// EmitFrame() success or a native-decoder frame). Lets tests confirm a
	// seek used real GOP-based seeking (a handful of frames) rather than
	// degenerating into a full linear decode from the start of the stream.
	int GetDebugFramesDecodedAfterSeek() const { return debugFramesDecodedAfterSeek; }

private:
	// True while the operation currently in flight is condemned (shutdown, or a
	// newer seek request superseded it). Cheap and lock-free by contract -- it is
	// polled once per frame iteration and from FFmpeg's I/O interrupt hook.
	// Always false when no predicate was installed (CVideoFrameExtractor and the
	// test suites never install one), so every such caller behaves exactly as it
	// did before this seam existed.
	bool IsAborted() const { return abortPredicate && abortPredicate(); }

	// AVIOInterruptCB trampoline: FFmpeg polls this from inside its blocking I/O
	// (av_seek_frame/av_read_frame). Returning nonzero makes the call in progress
	// bail out with AVERROR_EXIT.
	static int StaticInterruptCb(void *opaque);

	bool OpenAudioDecoder();
	void FillInfo();
	bool EmitFrame(SDecodedVideoFrame &out);
	// Converts a raw decoded pts (already scaled to seconds, in the
	// container's own absolute clock) to the clip-relative value this class
	// emits, per IVideoSource.h's contract. Lazily establishes startTime from
	// the first call when Open() couldn't determine it from fmtCtx->start_time
	// (see startTime/startTimeKnown below). Shared by the video (EmitFrame,
	// the nativeDecoder path) and audio (DecodeAudioFrame, QueueAACPacket) pts
	// computations so both streams share one clip-relative epoch.
	double NormalizePtsSeconds(double absSeconds);
	// Shared seek workhorse (Coarse-seek task). `stopAtFirstDecodedFrame`: stop at
	// the keyframe landing instead of walking to the target (fast/poster path).
	// `forwardOnly`: seek to the first keyframe AT OR AFTER the target
	// (av_seek_frame WITHOUT AVSEEK_FLAG_BACKWARD, no margin loop) instead of the
	// backward keyframe landing -- used only by SeekFastForward(). `outLandedPts`
	// (optional): set to the clip-relative pts of the frame the source stopped on,
	// left untouched on abort/EOF-before-a-frame. The public Seek() overloads and
	// SeekFast()/SeekFastForward() all funnel through here.
	bool SeekCore(double seconds, bool stopAtFirstDecodedFrame, bool forwardOnly,
				  double *outLandedPts);
	// Flush codec/bitstream/audio state after a raw av_seek_frame() lands, so the
	// forward scan starts clean. Shared by SeekCore()'s backward margin loop and
	// its forward path (Coarse-seek task).
	void FlushDecodersAfterSeek();
	void QueueAudioPacket(AVPacket *pkt);
	void DecodeAudioFrame(AVFrame *f);
	void FreeResources();
	// Bounded push shared by the FFmpeg (DecodeAudioFrame) and native AAC
	// (QueueAACPacket) paths -- see kMaxPendingAudioFrames below.
	void PushPendingAudio(SDecodedAudio &&audio);
#ifdef MT_HAVE_NATIVE_AAC
	void QueueAACPacket(AVPacket *pkt);
#endif
#ifdef MT_HAVE_NATIVE_WMA
	void QueueWMAPacket(AVPacket *pkt);
#endif

	SVideoInfo info;
	std::string errorReason;

	// Installed by SetAbortPredicate() (CVideoPlayer::Open(), before the decode
	// thread exists) and never mutated afterwards, so the decode thread and
	// FFmpeg's I/O callback can both read it without synchronization.
	std::function<bool()> abortPredicate;

	// --- Demuxer ---
	AVFormatContext *fmtCtx = nullptr;
	int videoStream = -1;
	int audioStream = -1;
	double videoTimeBase = 0.0;
	double audioTimeBase = 0.0;

	// --- Clip-relative time contract (IVideoSource.h) ---
	// Offset (seconds, container's own absolute clock) subtracted from every
	// raw decoded pts to produce the clip-relative pts this class emits, and
	// added back to a caller's clip-relative Seek() target to form the
	// absolute av_seek_frame() target. Preferred source: fmtCtx->start_time
	// (sample-accurate for MPEG-TS/AVCHD, populated by Open()); when the
	// demuxer doesn't know it (AV_NOPTS_VALUE), falls back to the first
	// decoded frame's own raw pts, established lazily by the first EmitFrame()
	// call (same anchor CVideoFrameExtractor used to compute independently
	// before this contract existed -- now computed once, here, instead).
	double startTime = 0.0;
	bool startTimeKnown = false;

	// Test hook state -- see GetDebugFramesDecodedAfterSeek() above.
	int debugFramesDecodedAfterSeek = 0;

	// --- Decoders ---
	AVCodecContext *vctx = nullptr;
	AVCodecContext *actx = nullptr;
	IVideoPacketDecoder *nativeDecoder = nullptr;
	// Owns the decoder CVideoSourceFFmpeg::Open() auto-installs for HEVC on
	// Apple (Task 7) when the caller didn't inject one via
	// SetHEVCPacketDecoder(). Externally-injected decoders are never owned
	// here and survive Close()/re-Open() cycles on the same instance; this
	// one is torn down and (re)created fresh by every Open() that needs it.
	std::unique_ptr<IVideoPacketDecoder> ownedNativeDecoder;
	bool disableAutoNativeDecoders = false;
	bool openVideoOnly = false; // SetOpenVideoOnly(): skip audio decode/queue entirely

	// Annex-B bitstream-filter plumbing (Task 6): only allocated when
	// nativeDecoder->WantsAnnexB() is true (Media Foundation on Windows -- see
	// IVideoPacketDecoder::WantsAnnexB()'s doc comment). VideoToolbox never
	// sets WantsAnnexB(), so hevcBsf stays nullptr on Apple and every call
	// site below that checks it executes the exact pre-Task-6 code path.
	// hevcBsf owns the filter graph (av_bsf_free in FreeResources());
	// bsfPacket is a dedicated AVPacket for the filter's output -- never the
	// reusable `packet` above, which the filter's av_bsf_send_packet() call
	// MOVES the ref out of.
	AVBSFContext *hevcBsf = nullptr;
	AVPacket *bsfPacket = nullptr;

	// --- Reusable decode scratch (avoids per-call alloc/free) ---
	AVFrame *frame = nullptr;
	AVFrame *audioFrame = nullptr;
	AVPacket *packet = nullptr;

	// --- Audio pump-ahead parking (2026-07-18; see PumpAudioAhead()) ---
	// Compressed VIDEO packets demuxed while pumping for audio, in demux
	// order; ReadVideoFrame()'s packet acquisition consumes these before
	// touching av_read_frame() again, so the video stream sees the exact
	// same packet sequence with or without pumping. Owned AVPackets
	// (av_packet_alloc + move_ref); freed by ClearParkedPackets() on
	// seek-flush and teardown. Bounds: a pump gives up when either cap is
	// hit (pathological interleave falls back to CVideoPlayer's
	// audio-starvation free-run). DECODE THREAD ONLY.
	std::deque<AVPacket *> parkedVideoPackets;
	size_t parkedVideoBytes = 0;
	static constexpr size_t kMaxParkedPackets = 4096;
	static constexpr size_t kMaxParkedBytes = 32 * 1024 * 1024;
	// How many audio buffers one PumpAudioAhead() call queues before
	// returning (keeps the decode thread responsive to seek commands
	// between pumps).
	static constexpr int kPumpAudioBuffersPerCall = 4;
	void ClearParkedPackets();

	// --- Pixel format fallback conversion (anything not natively mapped -> YUV420P) ---
	SwsContext *swsCtx = nullptr;
	int swsSrcFormat = -1;
	int swsSrcW = 0;
	int swsSrcH = 0;
	// The srcRange sws was last configured with (-1 = never set). Needed so
	// a metadata-tagged full-range source (yuv444p + color_range=JPEG -- the
	// modern tagging since YUVJ deprecation) is actually RANGE-CONVERTED to
	// the limited output the emitted fullRange=false tag promises
	// (programme review round 2, F1).
	int swsSrcRange = -1;
	std::vector<uint8_t> swsBuffer;

	// --- Audio resample/format conversion (source rate/layout -> interleaved S16) ---
	SwrContext *swrCtx = nullptr;
	int swrInFormat = -1;
	int swrInSampleRate = 0;
	int swrInChannels = -1;

	// Decoded ahead of the caller; drained one at a time by ReadAudio. Capped
	// (PushPendingAudio, drop-oldest) so a consumer that reads video frames
	// but never drains audio (e.g. muted playback, or a caller that simply
	// never calls ReadAudio) can't grow this queue unbounded -- 64 frames is
	// generous headroom (each AAC/opus/vorbis frame is ~1024-2048 samples, so
	// 64 frames is multiple seconds of buffered audio) while still bounding
	// worst-case memory.
	static constexpr size_t kMaxPendingAudioFrames = 64;
	std::deque<SDecodedAudio> pendingAudioFrames;
	bool droppingPendingAudio = false; // true while mid drop-burst, so the warning logs once, not per-frame

#ifdef MT_HAVE_NATIVE_AAC
	// Zero-bundled-AAC-decoder path (licensing): mirrors nativeDecoder above but
	// for audio -- AV_CODEC_ID_AAC packets go through the OS-native decoder
	// (CAudioDecoderAACNative -- AudioToolbox on Apple, Media Foundation on
	// Windows; see CAudioDecoderAACNative.h) instead of FFmpeg's (deliberately
	// absent) aac decoder. All other audio codecs stay on the FFmpeg actx
	// path above.
	std::unique_ptr<CAudioDecoderAACNative> aacDecoder;
	// Cached AudioSpecificConfig (codecpar->extradata) so Seek() can fully
	// reinitialize the native decoder (discarding any stale internal decode
	// state) by re-running Init() rather than needing a separate reset API.
	std::vector<u8> aacExtradata;
#endif

#ifdef MT_HAVE_NATIVE_WMA
	// Native WMA path (2026-07-18 WMV spec): mirrors aacDecoder above but for
	// the WMA family (wmav1/wmav2/wmapro) inside ASF -- Windows-only (Media
	// Foundation's in-box WMAudio Decoder MFT; no other platform has a native
	// WMA decoder, see CAudioDecoderWMANative.h). Preferred over the FFmpeg
	// software path on Windows in BOTH build modes so the MF path is
	// exercised by full/dev builds too; on native-init failure full builds
	// fall through to FFmpeg software decode, commercial builds degrade to
	// video-only.
	std::unique_ptr<CAudioDecoderWMANative> wmaDecoder;
	// Everything Seek() needs to fully re-Init() the native decoder
	// (mirrors aacExtradata; WMA additionally needs the WAVEFORMATEX-style
	// stream parameters that AAC's self-contained ASC doesn't). codecId is
	// the plain AVCodecID value, kept as int so this header needs no FFmpeg
	// includes.
	struct SWMAInitParams
	{
		int codecId = 0;
		std::vector<u8> extradata;
		int channels = 0;
		int sampleRate = 0;
		int blockAlign = 0;
		int avgBytesPerSec = 0;
	};
	SWMAInitParams wmaInit;
#endif
};

#endif // MT_ENABLE_FFMPEG

#endif
//_CVIDEOSOURCEFFMPEG_H_
