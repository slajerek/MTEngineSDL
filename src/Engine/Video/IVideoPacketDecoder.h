#pragma once
#include "IVideoSource.h"
#include <functional>
struct AVPacket;
class IVideoPacketDecoder
{
public:
	virtual ~IVideoPacketDecoder() = default;
	// `colorTrc` is the clip's AVCOL_TRC (16 = PQ / SMPTE2084, 18 = HLG /
	// ARIB STD-B67, 2 = unspecified). It exists so a decoder can choose its
	// OUTPUT PIXEL FORMAT from the transfer function -- S-5 Phase 5: HDR clips
	// must be decoded at 10 bits, because PQ allocates its code space assuming
	// 10 bits or more and quantising it to 8 bands visibly in the shadows no
	// matter how correct the shader downstream is.
	//
	// DEFAULTED to unspecified so a caller that genuinely has no colour
	// information (or a codec family that is 8-bit by nature, like the WMV
	// path) reads as SDR and behaves exactly as before. Note the default does
	// NOT make this a source-compatible change for IMPLEMENTERS: an override
	// declaring the old 4-argument form stops overriding, leaves the class
	// abstract and fails to compile. That is deliberate -- a decoder silently
	// ignoring the trc would be a quiet wrong picture, which is worse than a
	// build error.
	virtual bool Init(const u8 *extradata, int extradataSize, int width, int height,
					  int colorTrc = 2 /* AVCOL_TRC_UNSPECIFIED */) = 0;
	// Returns true when a decoded frame is available in `out` (may buffer internally
	// for reorder). Frame planes valid until next DecodePacket/Flush.
	//
	// Returns false otherwise -- check GetErrorReason() to distinguish the two
	// very different reasons: empty means "no frame yet, keep feeding packets"
	// (buffering, e.g. the B-frame reorder window isn't full yet); non-empty
	// means a genuine decode failure the caller (CVideoSourceFFmpeg::ReadVideoFrame)
	// must propagate as an error rather than silently skip toward a false
	// "clean EOS".
	virtual bool DecodePacket(const AVPacket *pkt, SDecodedVideoFrame &out) = 0;

	// --- Abort plumbing (spec #2.3 "Blocking work must be interruptible") -----
	//
	// The SAME predicate the owning IVideoSource polls (see
	// IVideoSource::SetAbortPredicate()), forwarded down to the decoder by the
	// source (CVideoSourceFFmpeg::SetAbortPredicate()). It is LEVEL-triggered by
	// contract: true for exactly as long as the operation currently in flight is
	// condemned (shutdown, or superseded by a newer seek generation), false at
	// every other moment. There is no flag to raise, clear or re-arm here --
	// generation scoping falls out of the predicate itself.
	//
	// A decoder that can block for a long time inside a single DecodePacket()
	// call (CVideoDecoderHEVCMF: up to 10 seconds on each of its two condvar
	// waits) MUST override this and JOIN the predicate into every wait predicate
	// (and check it once at DecodePacket() entry), so a wait wakes and gives up
	// the moment the predicate reads true. Called from the DECODE thread only.
	// Default no-op: decoders whose DecodePacket() is bounded
	// (CVideoDecoderHEVCVT) need nothing.
	virtual void SetAbortPredicate(std::function<bool()> pred) { (void)pred; }

	// PURE WAKE -- no state of its own. Called from ANOTHER thread than the one
	// inside DecodePacket(), via IVideoSource::WakeAbort(), AFTER the caller has
	// published the state the predicate above reads (raised shutdown flag /
	// bumped generation). All it must do is notify whatever the decoder can be
	// blocked on, so the wait RE-EVALUATES the (already-true) predicate.
	//
	// Why no flag: an edge-triggered abort flag can be LOST. The wake is fired
	// once per submit; if it lands while the decode thread is between two
	// DecodePacket() calls, a flag raised by it would be cleared at the next
	// DecodePacket() entry (there is no "second wake" coming) and that call would
	// then sleep out its full 10-second wait -- exactly the freeze this plumbing
	// exists to remove. A level-triggered predicate cannot be lost: whenever the
	// decoder next looks, it sees the truth.
	//
	// An aborted DecodePacket() returns false with an EMPTY GetErrorReason()
	// (it is neither a decode failure nor EOS); CVideoSourceFFmpeg's seek/read
	// loops -- which triggered the abort -- re-check the abort predicate after
	// ANY false return and classify it there. Default no-op.
	virtual void Abort() {}

	virtual void Flush() = 0;
	virtual const std::string &GetErrorReason() const = 0;
	// True when this decoder needs its input packets pre-converted from the
	// container's length-prefixed NAL layout (hvcC/avcC-style) to Annex-B
	// (start-code-prefixed) before DecodePacket() -- CVideoSourceFFmpeg installs
	// an hevc_mp4toannexb bitstream filter in front of any decoder that returns
	// true here (see CVideoSourceFFmpeg.cpp's hevcBsf plumbing) and this
	// decoder's Init() then receives the bsf's Annex-B-converted parameter sets
	// instead of the raw hvcC extradata. VideoToolbox (CVideoDecoderHEVCVT)
	// consumes hvcC directly and never overrides this -- default false keeps
	// that path bsf-free and byte-identical to before this seam existed.
	virtual bool WantsAnnexB() const { return false; }
	static bool IsHEVCDecodeAvailable();   // implemented per platform; false stub elsewhere
	// WMV family (WMV1/WMV2/WMV3/WVC1): true only on Windows when the in-box
	// WMVideo Decoder MFT resolves (absent on N editions without the Media
	// Feature Pack). No other platform has a native WMV story -- Apple/Linux
	// get the false stub (CVideoSourceFFmpeg.cpp); Windows implements it in
	// CVideoDecoderWMVMF.cpp.
	static bool IsWMVDecodeAvailable();
};
