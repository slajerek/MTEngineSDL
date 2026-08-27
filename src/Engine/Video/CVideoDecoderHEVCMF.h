#ifndef _CVIDEODECODERHEVCMF_H_
#define _CVIDEODECODERHEVCMF_H_

#pragma once

// Native HEVC decode via Windows Media Foundation's HEVC decoder MFT. This is
// the Windows twin of CVideoDecoderHEVCVT (VideoToolbox on Apple) -- the
// bundled FFmpeg carries no HEVC decoder (licensing), so HEVC packets
// demuxed by CVideoSourceFFmpeg are handed to this class instead, which
// decodes them via whichever HEVC decoder MFT Media Foundation resolves
// (hardware first, falling back to the "HEVC Video Extensions" Store add-on's
// software MFT -- see IsHEVCDecodeAvailable() below). Compiles to nothing
// anywhere except a Windows FFmpeg-enabled build. See the .cpp's header
// comment for why this drives the decoder through a Topology/IMFMediaSession
// pipeline rather than a raw IMFTransform push/pull loop.
#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "IVideoPacketDecoder.h"
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class CVideoDecoderHEVCMF : public IVideoPacketDecoder
{
public:
	// Compressed input codec this pipeline decodes (2026-07-18 WMV spec).
	// The Topology/Session machinery below is codec-agnostic -- the session's
	// topology loader auto-inserts whichever decoder MFT matches the push
	// source's subtype -- so the same pipeline serves HEVC (this class's own
	// codec, the default) and the WMV/VC-1 family (CVideoDecoderWMVMF, a
	// thin subclass selecting one of the WMV members below). Input-side
	// differences live entirely in the subtype GUID, the extradata semantics
	// (Annex-B parameter sets for HEVC vs the ASF sequence header for WMV,
	// both delivered via MF_MT_USER_DATA), and WantsAnnexB().
	enum class ECodec
	{
		HEVC,
		WMV1, // Windows Media Video 7
		WMV2, // Windows Media Video 8
		WMV3, // Windows Media Video 9 (VC-1 Simple/Main)
		WVC1, // VC-1 Advanced (AV_CODEC_ID_VC1)
	};

	CVideoDecoderHEVCMF();
	~CVideoDecoderHEVCMF() override;

	// extradata/extradataSize are the ANNEX-B-converted parameter sets from
	// CVideoSourceFFmpeg's hevc_mp4toannexb bitstream filter (bsf->par_out),
	// NOT the container's raw hvcC extradata -- see WantsAnnexB() below and
	// CVideoSourceFFmpeg.cpp's bsf setup. Every packet subsequently handed to
	// DecodePacket() is Annex-B (start-code-prefixed) for the same reason.
	bool Init(const u8 *extradata, int extradataSize, int width, int height,
			  int colorTrc = 2 /* AVCOL_TRC_UNSPECIFIED */) override;
	bool DecodePacket(const AVPacket *pkt, SDecodedVideoFrame &out) override;

	// Abort plumbing (Task 5; contract in IVideoPacketDecoder.h). THIS decoder is
	// the reason the whole chain exists: DecodePacket() blocks on TWO condvar
	// waits of up to 10 SECONDS each (the Session's worker threads consume the
	// packet and hand the frame back asynchronously), so a per-frame cooperative
	// check upstream would be useless against it -- a Close() during a precise
	// seek would sit in join() for ten seconds.
	//
	// SetAbortPredicate(): the source forwards CVideoPlayer's LEVEL-triggered
	// predicate (shouldStop || a newer seek generation) down to us; it is JOINED
	// into both wait predicates and checked once at DecodePacket() entry.
	//
	// Abort(): a PURE WAKE from another thread (the render thread, via
	// CVideoSourceFFmpeg::WakeAbort()) -- notify the condvar so a blocked wait
	// re-evaluates the predicate the caller has already made true. It holds no
	// state of its own: an abort flag would be EDGE-triggered and could be lost
	// (raised between two DecodePacket() calls, then cleared at the next entry,
	// with no second wake ever coming -- a full 10-second sleep, i.e. the freeze
	// this feature removes). The predicate cannot be lost: whenever we look, we
	// see the truth.
	void SetAbortPredicate(std::function<bool()> pred) override { abortPredicate = std::move(pred); }
	void Abort() override;

	// DISCARD-ONLY, same contract as CVideoDecoderHEVCVT::Flush():
	// ProcessMessage(MFT_MESSAGE_COMMAND_FLUSH) drops whatever the MFT is
	// holding internally; it does NOT drain frames out.
	// CVideoSourceFFmpeg::Seek() calls this precisely to throw away
	// now-stale frames (see its "// discard now-stale" comment). EOS drain is
	// a SEPARATE path -- see DecodePacket(nullptr, ...) below.
	void Flush() override;

	const std::string &GetErrorReason() const override { return errorReason; }

	// This decoder's input is Annex-B (start-code-prefixed NAL units with
	// inline parameter sets) -- the MF HEVC decoder MFT's documented input
	// convention -- rather than the length-prefixed hvcC layout
	// CVideoSourceFFmpeg's demuxer hands out by default. See
	// IVideoPacketDecoder::WantsAnnexB()'s doc comment for the full seam.
	bool WantsAnnexB() const override { return true; }

	// Forward-declared here (public) rather than in the private section below
	// purely so the .cpp's anonymous-namespace helper classes (which live
	// outside this class entirely -- see the .cpp's header comment) can name
	// CVideoDecoderHEVCMF::SPipeline via a `using` alias; access control on a
	// forward declaration doesn't expose anything -- the type stays
	// opaque/unusable outside the .cpp regardless.
	struct SPipeline;

protected:
	// Subclass seam (CVideoDecoderWMVMF): same pipeline, different compressed
	// input subtype. HEVC-only behavior keyed off inputCodec below: the P010
	// 10-bit sink fallback (WMV is 8-bit NV12 only) and the Annex-B extradata
	// convention (see Init()'s doc comment).
	explicit CVideoDecoderHEVCMF(ECodec codec);

private:
	ECodec inputCodec = ECodec::HEVC;

	// Owned copy of one decoded frame's planes -- the MFT's IMFMediaBuffer is
	// only valid for the duration of the lock used to read it out, so every
	// plane is copied into buffers we own before the next DecodePacket()/
	// Flush() call, mirroring CVideoDecoderHEVCVT::SPendingFrame exactly
	// (same owned-planes copy-out pattern, same 10-bit LSB-justified
	// conversion -- see EmitOutFrame()/CopyFrameOut() in the .cpp). Kept
	// around (as `outFrame`) until the next call so the SDecodedVideoFrame
	// pointers handed to the caller stay valid that long, per
	// IVideoPacketDecoder's contract.
	//
	// DIVERGENCE from CVideoDecoderHEVCVT: no reorder queue/deque here. MF
	// decoder MFTs emit frames in DISPLAY order already (unlike VT, which
	// emits decode order and needs the pts-sorted reorder window) -- see
	// ProcessOutput's documented ordering guarantee for
	// MFT_OUTPUT_STREAM_INFO without the "provides its own reordering" flag
	// unset from the transform's perspective; the decoder itself performs
	// the reorder internally and only ever hands frames back in presentation
	// order. One `outFrame` slot is therefore enough.
	struct SPendingFrame
	{
		double pts = 0.0;
		int width = 0, height = 0;
		bool is10Bit = false;

		// 8-bit NV12 output.
		std::vector<u8> y;
		std::vector<u8> uv;
		int strideY = 0, strideUV = 0;

		// 10-bit P010 output, de-interleaved into planar U/V and shifted
		// from P010's MSB-justified 16-bit samples to the LSB-justified
		// convention CVideoSourceFFmpeg's AV_PIX_FMT_YUV420P10LE path
		// expects -- identical conversion to CVideoDecoderHEVCVT's 10-bit
		// path (copied verbatim; see that class's OnFrameDecoded()).
		std::vector<u8> u10, v10;
		int strideY10 = 0, strideC10 = 0;
	};

	// All COM objects (custom push-mode IMFMediaSource, the Topology, the
	// IMFMediaSession, the Sample Grabber Sink and its callback, plus the
	// mutex/condvar handoff state -- see the .cpp's header comment for the
	// full design) live behind this PIMPL (SPipeline, forward-declared above)
	// so this header never needs to pull in Media Foundation/COM headers,
	// matching the forward-declare-only discipline the raw-IMFTransform
	// version of this class used to keep via IMFTransform/IMFSample forward
	// declarations.
	//
	// SHARED-OWNED (not a bare owning pointer): every COM helper that MF may
	// invoke on one of its own worker threads -- the session event callback,
	// the Sample Grabber callback, and the push source/stream -- co-owns this
	// SPipeline via its own shared_ptr (see the .cpp). TeardownPipeline() drops
	// THIS decoder-side reference, but the struct itself is destroyed only once
	// the last in-flight worker callback has also released its reference, so an
	// Invoke()/OnProcessSample()/RequestSample() already dispatched can never
	// dereference a freed SPipeline. This is the fence that fixes the observed
	// MFCORE access-violation (raw `session`/`pipeline` pointers dangling in an
	// async callback that raced TeardownPipeline()); see the .cpp for the full
	// lifetime argument.
	std::shared_ptr<SPipeline> pipeline;

	// The player's LEVEL-triggered abort predicate (Task 5), handed down by
	// CVideoSourceFFmpeg::SetAbortPredicate(). Installed before the decode thread
	// exists and never mutated afterwards, so it is read without synchronization
	// (it reads two atomics and takes no lock -- CVideoPlayer::ShouldAbortDecode()).
	// Empty for every caller that installs none (CVideoFrameExtractor, the test
	// suites), and IsAborted() is then permanently false.
	std::function<bool()> abortPredicate;
	bool IsAborted() const { return abortPredicate && abortPredicate(); }

	// Serializes access to the `pipeline` shared_ptr MEMBER against the
	// teardown/rebuild that reassigns it. Abort() runs on the render thread
	// while Flush() -> BuildPipeline() -> TeardownPipeline() runs on the decode
	// thread; without this, Abort() would read/copy the shared_ptr concurrently
	// with TeardownPipeline() reassigning it -- a data race on the smart pointer
	// itself (the pointed-to SPipeline can no longer be freed early -- worker
	// callbacks co-own it, see above -- but the MEMBER handoff still needs this
	// lock). LOCK ORDER: pipelineLifetimeMutex OUTER, SPipeline::mtx INNER (both
	// Abort() and TeardownPipeline() take them in that order; nothing ever takes
	// this one while holding SPipeline::mtx).
	//
	// NOTHING SLOW MAY RUN UNDER IT: TeardownPipeline() swaps the pointer out
	// under this mutex and does the actual IMFMediaSession::Shutdown() (which
	// blocks until MF's workers wind down) on the local copy, OUTSIDE it --
	// otherwise a concurrent Abort() (render thread, called while the player holds
	// cmdMutex) would block on the shutdown of a pipeline the decode thread is
	// rebuilding on every seek, stalling the render thread on a key-repeat burst.
	std::mutex pipelineLifetimeMutex;

	bool BuildPipeline();     // creates+starts source/topology/session/sink against savedExtradata/width/height; sets errorReason and returns false on failure
	void TeardownPipeline();  // stops/shuts down/releases everything in `pipeline`; safe to call when pipeline is already null

	// Copies one decoded frame's raw NV12/P010 bytes (handed to us directly
	// by the Sample Grabber Sink's OnProcessSample callback -- see the .cpp)
	// into outFrame's owned planes. `pitch` is the tightly-packed stride
	// MFCalculateImageSize/the Sample Grabber's declared media type implies
	// for `width` (see BuildPipeline()'s sink media type setup).
	bool CopyFrameOut(const u8 *data, size_t dataSize, double pts);
	bool EmitOutFrame(SDecodedVideoFrame &out);

	std::string errorReason;
	int width = 0, height = 0;
	bool use10Bit = false;    // true once the sink's negotiated output type is P010 (Main10/HLG sources) instead of NV12
	// The clip's AVCOL_TRC from Init(), kept like savedExtradata so a Flush()
	// rebuild negotiates the SAME depth rather than silently dropping to 8-bit
	// mid-clip. PQ (16) / HLG (18) request P010 first (S-5 Phase 5).
	int colorTrc = 2;         // AVCOL_TRC_UNSPECIFIED
	std::vector<u8> savedExtradata;  // Annex-B parameter sets from Init(), kept so Flush() can rebuild the pipeline identically (see the .cpp's Flush() doc comment on why a full rebuild replaces MFT_MESSAGE_COMMAND_FLUSH here)

	// A PRIVATE Media Foundation serial work queue owned by THIS decoder
	// instance -- allocated once in Init() (idempotent across re-Init) and
	// released in the destructor. The session event callback's
	// IMFAsyncCallback::GetParameters() returns this id so MF dispatches OUR
	// Invoke() here instead of onto MF's process-wide shared platform queue,
	// cutting cross-instance contention under many-concurrent-decoder load (see
	// the .cpp's Fix-2 notes -- whether it also relieves the session->Shutdown()
	// stall is an empirical question the Windows VM measures). Serial (not plain)
	// keeps our dispatches one-at-a-time, matching the single-BeginGetEvent-in-
	// flight model. 0 == MFASYNC_CALLBACK_QUEUE_UNDEFINED == "not allocated /
	// fall back to MF's default dispatch". Typed `unsigned long` (== DWORD on
	// Windows) so this header needs no <mfapi.h>/<windows.h>.
	unsigned long serialWorkQueue = 0;

	SPendingFrame outFrame;
};

#endif // _WIN32 && MT_ENABLE_FFMPEG

#endif
//_CVIDEODECODERHEVCMF_H_
