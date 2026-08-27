#pragma once
#include "SYS_Defs.h"
#include <functional>
#include <string>
#include <vector>
#include <ctime>

enum class EVideoPixelFormat { Unknown = 0, YUV420P, YUVA420P, NV12, YUV420P10, YUV422P, YUV422P10, RGBA };

struct SVideoInfo
{
	int width = 0, height = 0;          // coded (pre-rotation) dimensions
	double duration = 0.0;              // seconds
	double fps = 0.0;
	std::string videoCodecName;         // "h264", "hevc", "prores", ...
	std::string audioCodecName;         // "" when no audio
	time_t creationTime = 0;            // 0 = unknown
	int rotationDegrees = 0;            // 0/90/180/270 from the display matrix, normalized
	                                     // to match ffprobe's reported `rotation` value
	                                     // (av_display_rotation_get(), unnegated). NOTE: the
	                                     // pixel transform that actually corrects the frame
	                                     // for display is the *counter*-clockwise rotation by
	                                     // this many degrees (equivalently, clockwise by
	                                     // (360-rotationDegrees)%360) -- verified empirically
	                                     // against tests/fixtures/video/h264_rot90.mp4; see
	                                     // CVideoSourceFFmpeg::FillInfo() and
	                                     // CVideoPlayer::RotateRGBA().
	// The ISO-BMFF `tkhd` track_id of the stream av_find_best_stream() chose --
	// i.e. of the track this source actually decodes and displays. Exposed so a
	// container WRITER can CHECK that the track it is about to patch is the one
	// the engine displays, rather than trusting two selection rules to agree
	// forever (PhotoCruise fu-e5 #4.2). 0 when unknown or not applicable --
	// only the mov/mp4 demuxer assigns AVStream::id from a track id.
	int videoTrackId = 0;
	bool hasAlpha = false;
	bool hasAudio = false;
	int audioChannels = 0;
	u32 audioSampleRate = 0;
	int colorSpace = 0;                 // NORMALIZED matrix coefficients, VPX_CS_* convention:
	                                     // 0 unknown, 1 BT.601, 2 BT.709, 5 BT.2020 (ncl). FFmpeg
	                                     // sources normalize AVColorSpace through
	                                     // VideoColor_NormalizeMatrix() below -- the raw AVCOL_SPC_*
	                                     // values must NEVER be stored here (their numbering
	                                     // disagrees: AVCOL_SPC_BT709 == 1 but VPX_CS_BT_709 == 2,
	                                     // an inversion that shipped for months; CM-E fixed it).
	bool fullRange = false;
	int colorPrimaries = 2;             // raw AVCOL_PRI_* value; 2 = unspecified (vpx/WebM
	                                     // sources never set these two -- they stay "unspecified")
	int colorTrc = 2;                   // raw AVCOL_TRC_* value; 2 = unspecified.
	                                     // 16 = SMPTE 2084 (PQ), 18 = ARIB STD-B67 (HLG) -- the
	                                     // HDR pair PhotoCruise's CM-E LUT gate keys on.
};

// CM-E: normalize an FFmpeg AVColorSpace value into the engine's VPX_CS_*
// convention above (the one the YUV shader and the CPU converters branch on).
// UNSPECIFIED and unmapped values resolve by the industry heuristic:
// HD (height >= 720 or width >= 1280) -> BT.709 (2), else BT.601 (1).
// Deliberate mappings the obvious table gets wrong:
//   - AVCOL_SPC_RGB (0) -> 601: RGB-order sources are converted to YUV420P by
//     CVideoSourceFFmpeg's sws fallback with swscale's DEFAULT (601) encode
//     matrix, so 601 is what those pixels actually are.
//   - SMPTE240M (7) -> 709: its coefficients (Kr 0.212 / Kb 0.087) are the
//     709 family, not 601's (0.299 / 0.114).
// Input is the FFMPEG convention ONLY -- never feed an already-normalized
// (VPX-convention) value through this, the conventions cross-contaminate
// (VPX 1 == BT.601 would read as AVCOL BT709). Defined in CVideoPlayer.cpp.
int VideoColor_NormalizeMatrix(int avcolSpc, int width, int height);

struct SDecodedVideoFrame
{
	const u8 *plane[4] = {nullptr, nullptr, nullptr, nullptr};
	int stride[4] = {0, 0, 0, 0};
	int width = 0, height = 0;
	EVideoPixelFormat pixelFormat = EVideoPixelFormat::Unknown;
	double pts = 0.0;                   // CLIP-RELATIVE seconds; see IVideoSource::Seek() below
	// Planes are valid until the next ReadVideoFrame/Seek/Close on the same source.
};

struct SDecodedAudio
{
	std::vector<s16> pcm;               // interleaved
	int channels = 0;
	u32 sampleRate = 0;
	double pts = 0.0;                   // CLIP-RELATIVE seconds; same contract as SDecodedVideoFrame::pts
};

// --- Clip-relative time contract ---
// Every time value that crosses this interface -- Seek()'s argument,
// SDecodedVideoFrame::pts, SDecodedAudio::pts -- is CLIP-RELATIVE: 0.0 is
// this clip's first frame, regardless of what the underlying container's own
// clock reads (MPEG-TS/AVCHD in particular routinely start their internal
// clock at a nonzero offset -- ffprobe's `start_time`, ~1.4s in this
// project's h264_ac3.mts fixture).
//
// Implementations that sit on top of a container with such an offset (see
// CVideoSourceFFmpeg) own exactly one internal "startTime" translation
// point, applied in both directions:
//   - Seek(seconds): clamp the CLIP-RELATIVE `seconds` to [0, Info().duration]
//     FIRST, then add startTime to form the container-absolute seek target.
//     Clamping AFTER adding the offset would compare an absolute value
//     against a clip-relative duration and silently defeat the clamp for
//     exactly the containers that need it most -- this was the original bug
//     shape this contract fixes.
//   - Every pts handed back afterward (ReadVideoFrame/ReadAudio) is the
//     container-absolute decoded pts MINUS startTime, restoring the
//     clip-relative domain.
// Containers with no such offset (WebM/MKV/MP4 in the common case -- see
// CVideoSourceWebMVpx) already start at 0, so startTime is implicitly 0 and
// no translation is needed; they still satisfy this contract trivially.
class IVideoSource
{
public:
	// Stable machine-readable error token (2026-07-18 WMV spec). When Open()
	// fails because this BUILD carries no decoder for the file's codec (e.g.
	// WMV/VC-1 in a commercial/store build on a platform with no OS decoder),
	// the errorReason string STARTS WITH this token, followed by the
	// human-readable display text. Tests and app UI branch on the token
	// (substring match), never on the display copy -- the copy may change or
	// localize, the token never does.
	static constexpr const char *kVideoErrorTokenEditionUnsupported = "[edition-unsupported]";

	virtual ~IVideoSource() = default;
	virtual bool Open(const char *filePath) = 0;
	virtual const SVideoInfo &Info() const = 0;
	// false = end of stream or error; check GetErrorReason() to distinguish ("" = EOS)
	virtual bool ReadVideoFrame(SDecodedVideoFrame &out) = 0;
	virtual bool ReadAudio(SDecodedAudio &out) = 0;   // false = no audio pending

	// AUDIO PUMP-AHEAD (2026-07-18). Demux FORWARD for audio only: decode and
	// queue the next few audio packets (drained by ReadAudio() as usual) while
	// PARKING any compressed video packets encountered, in order, for the next
	// ReadVideoFrame() to consume first -- no video packet is lost or decoded
	// early, and demux order is preserved exactly. Bounded: implementations cap
	// the parked backlog (bytes + count) and give up past it. Returns true if
	// at least one audio buffer was queued; false on EOS, cap, abort, no audio
	// stream, or an implementation that cannot pump (this default).
	//
	// Why: real-world containers (ASF/WMV especially) interleave audio LAZILY
	// -- packets for playback time T sit next to video packets for T+lag, lag
	// routinely exceeding the decoded video ring's depth. Without pumping, a
	// full video ring blocks all demuxing, the mixer drains its backlog, and
	// the consumption-driven A/V clock starves (see CVideoPlayer's
	// audio-starvation free-run, the last-resort fallback for the same wedge).
	// Pumping compressed packets costs kilobytes where buffering decoded video
	// frames would cost megabytes -- the same trick every general-purpose
	// player uses. DECODE THREAD ONLY (same thread as ReadVideoFrame()).
	virtual bool PumpAudioAhead() { return false; }
	// seconds is CLIP-RELATIVE (0 == clip start). See the clip-relative time
	// contract above for the clamp-then-offset rule implementations must follow.
	virtual bool Seek(double seconds) = 0;

	// --- Two-phase seek (Task 6, spec #2.3 "scrubbing must feel instant") -----
	//
	// The FAST half. Seek() is PRECISE: it lands on the keyframe at/before
	// `seconds` and then decodes forward, frame by frame, until it reaches the
	// target -- on long-GOP content (the h264_longgop.mp4 fixture carries ONE
	// keyframe, at t=0) that walk is the entire clip. Under a held arrow key that
	// cost is paid on every step and the scrub crawls.
	//
	// SeekFast() drops the forward walk: land on the keyframe and stop at the
	// first frame that decodes. The picture is INEXACT (up to a GOP short of the
	// target) but it costs ~2 decodes, so a burst stays responsive. CVideoPlayer
	// uses it for every request that arrives inside the burst window and then --
	// once the user goes quiet -- issues ONE precise Seek() to the final target,
	// which is what makes the landing exact (mpv/VLC do exactly this).
	//
	// The DEFAULT IS THE PRECISE SEEK, so a source with no cheap variant
	// (CVideoSourceWebMVpx) degrades transparently: it is slower under a burst,
	// never wrong. Same contract as Seek() in every other respect -- clip-relative
	// seconds, clamped by the implementation, abortable, and an aborted call
	// returns false with an EMPTY GetErrorReason().
	//
	// `outLandedPts` (Coarse-seek task): when non-null, the implementation reports
	// the CLIP-RELATIVE pts of the frame the source actually landed on -- the
	// keyframe it will decode forward from, which on long-GOP content is SECONDS
	// before `seconds`. CVideoPlayer's Coarse mode re-bases the playback clock
	// onto this (not onto the requested target) so video+audio resume in sync from
	// the frame on screen. The DEFAULT impl is the precise fallback (Seek()), which
	// lands exactly at the target, so it reports `seconds`. A source that cannot
	// determine the landed pts (an aborted/failed call, or one that reached EOS
	// before decoding a frame) must LEAVE *outLandedPts UNTOUCHED -- CVideoPlayer
	// seeds it negative and falls back to the requested target on that signal.
	virtual bool SeekFast(double seconds, double *outLandedPts = nullptr)
	{
		if (outLandedPts)
			*outLandedPts = seconds;   // precise fallback lands at the target
		return Seek(seconds);
	}

	// FORWARD keyframe seek (Coarse-seek task): land on the first keyframe AT OR
	// AFTER `seconds` and report its pts. CVideoPlayer's Coarse mode calls this
	// only when a backward keyframe landing made no forward progress (the step is
	// smaller than the current GOP). Returns false -- leaving *outLandedPts
	// untouched -- when there is no later keyframe (single-keyframe/long-GOP file)
	// or the source cannot do a forward-biased seek; CVideoPlayer then falls back
	// to a Precise seek so forward progress is still guaranteed. The DEFAULT is
	// "no forward capability" (returns false), so a source that does not implement
	// it (CVideoSourceWebMVpx) simply always takes the precise fallback -- slower
	// on the pathological case, never wrong.
	virtual bool SeekFastForward(double seconds, double *outLandedPts)
	{
		(void)seconds; (void)outLandedPts;
		return false;
	}

	virtual const std::string &GetErrorReason() const = 0;
	virtual void Close() = 0;

	// --- Abort plumbing (spec #2.3 "Blocking work must be interruptible") ---
	//
	// Seek() and the decode-to-target walk it performs can take hundreds of ms
	// inside the demuxer/decoder. Two things must be able to cut that short:
	// the owner shutting the source down (CVideoPlayer::Close() joins the decode
	// thread) and a NEWER seek landing in the mailbox (a burst of key-repeat
	// steps must PREEMPT the running seek, not serialize behind it).
	//
	// SetAbortPredicate() hands the source a cheap, lock-free predicate it must
	// poll: once per frame iteration inside any scan/decode-to-target loop, and
	// (for sources with blocking I/O) from whatever interrupt hook the
	// underlying library offers -- CVideoSourceFFmpeg installs it as the
	// AVFormatContext's AVIOInterruptCB. It is CALLED FROM THE DECODE THREAD
	// only, and it is GENERATION-SCOPED: true only while the operation currently
	// being serviced is already condemned (shutdown, or superseded by a newer
	// request), false at every other moment -- so there is no "re-arm" step a
	// caller can forget.
	//
	// Abort is NOT an error and NOT end-of-stream. An aborted Seek()/read
	// returns false with an EMPTY GetErrorReason(), and the CALL SITE (which
	// triggered the abort and therefore knows) classifies it by re-checking the
	// predicate -- see CVideoPlayer::ShouldAbortDecode()'s call sites. Surfacing
	// it as an error would latch the player into EVideoPlayerState::Error and
	// wedge it; surfacing it as EOS would fake a "Finished" clip.
	//
	// WakeAbort() is the other half, and it is what makes the abort PROMPT: a
	// per-iteration predicate check does nothing when a single decode call is
	// blocked INSIDE the decoder (the Windows Media Foundation HEVC pipeline
	// waits on a condition variable for up to 10 SECONDS per step). Every submit
	// that invalidates in-flight work must FIRST publish the state the predicate
	// reads (the raised shutdown flag / the bumped generation) and THEN call
	// WakeAbort(), which wakes any wait so it RE-EVALUATES the predicate. State
	// first, wake second, waiter re-evaluates: a wake can never be lost between
	// a check and a sleep.
	//
	// Both default to no-ops -- a source with no blocking work (or no way to
	// interrupt it) simply inherits them and behaves exactly as before.
	virtual void SetAbortPredicate(std::function<bool()> pred) { (void)pred; }
	virtual void WakeAbort() {}
};
