#ifndef _CVIDEOPLAYER_H_
#define _CVIDEOPLAYER_H_

#pragma once

#include "SYS_Defs.h"
#include "IVideoSource.h"
#include "Core/Render/ERenderTextureFormat.h"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <cstdio>

// RotateRGBA was promoted (Task 9) to a shared free function so
// CVideoFrameExtractor's poster-frame path can reuse the exact same
// rotation math -- see VideoFrameTransform.h for the sign convention.
#include "VideoFrameTransform.h"

// Render-to-texture RGBA output (Task 10): SetOutputRGBATexture(true) makes
// Update() additionally convert each decoded frame through
// CVideoYUVShader::RenderToTarget() into a CGLRenderTarget, so GetRGBATexture()
// can hand callers (Plan 2's viewer) a single display-oriented RGBA8 texture
// instead of 3-4 raw YUV(A) planes + rotation bookkeeping.
#include "CVideoYUVShader.h"
#include "CGLRenderTarget.h"

// OpenGL
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/gl3w.h>
#endif

class CSlrMutex;
class CVideoAudioChannel;

enum class EVideoPlayerState
{
	Idle,
	Playing,
	Paused,
	Finished,
	Error
};

// Result of a NON-BLOCKING relative seek request (Task 3). Computed ATOMICALLY
// with the accumulate-and-submit inside RequestSeekRelative(), which is the only
// place where "where would this step have landed, before clamping?" is knowable
// without racing the decode thread. The stored target is clamped to
// [0, duration] either way -- the outcome is purely a report, so a caller
// (the photo app's video controller) can turn a step off the end of the clip into
// "advance to the next clip" without re-deriving it from a time read that the
// decode thread may already have re-based underneath it.
enum class ESeekRequestOutcome
{
	InRange,      // the accumulated target is inside [0, duration]
	PastEnd,      // it went past duration (target stored clamped to duration)
	BeforeStart   // it went below 0 (target stored clamped to 0)
};

// How a submitted seek is serviced by the decode thread (Coarse-seek task,
// 2026-07-16). Replaces the old `bool forcePrecise` on the request API: the
// three modes are the three points on the "how exact vs how instant" curve.
//
//  * TwoPhase -- the historical default. A request inside the burst window
//    lands FAST (keyframe-only SeekFast()) and, once the user goes quiet, a
//    single PRECISE refine walks the source to the exact target. Great for the
//    seek-bar/arrow paths UNTIL you notice the refine costs a full GOP decode
//    on release (0.5-1s freeze-frame on long-GOP content) -- which is exactly
//    why the app paths moved to Coarse.
//  * Precise -- always exact, never fast, never arms a refine. The frozen
//    contract of the blocking wrappers (Seek()/Stop()/SeekPausedAndDecodeFrame)
//    and of frame-step: land on the requested frame, whatever it costs.
//  * Coarse -- keyframe-FINAL. Every landing is a fast keyframe seek and it is
//    the last word: NO refine is ever armed, and the playback clock is re-based
//    onto the LANDED KEYFRAME's pts (not the requested target), so video AND
//    audio resume immediately and in sync from the frame already on screen.
//    This is mpv's default relative-seek behaviour (hr-seek=no). A held scrub
//    stays silent (the audio gate is held across the burst and dropped on a
//    short quiet deadline, not per step); on release playback runs instantly.
//    Forward progress on files whose GOP is larger than the step is guaranteed
//    by a forward-keyframe retry, falling back to one Precise seek only when the
//    file has no later keyframe at all (single-keyframe fixtures).
enum class ESeekMode
{
	TwoPhase,   // burst-fast + quiet precise refine (historical default)
	Precise,    // always exact (frame-step, blocking wrappers)
	Coarse      // keyframe-final, clock re-based to the landed pts
};

class CVideoPlayer
{
public:
	CVideoPlayer();
	~CVideoPlayer();

	// --- Public API ---

	// Open a .webm file, parse tracks, prepare decoders
	bool Open(const char *filePath);

	// Test-only injection seam (Plan-2 Task 2): when set, the next Open()
	// call uses this pre-constructed IVideoSource instead of allocating its
	// own CVideoSourceFFmpeg/CVideoSourceWebMVpx -- lets tests inject a
	// source with a fake IVideoPacketDecoder (see CFailingPacketDecoder in
	// CTestVideoDecode.cpp) to deterministically exercise decode-failure
	// propagation. Ownership transfers to the player exactly as if it had
	// constructed the source itself: Close()/the destructor delete it via the
	// normal FreeResources() path -- including a still-pending injection that
	// was never consumed because Open() was never called (leak guard in
	// FreeResources()). One-shot -- consumed (and cleared) by the very next
	// Open() call. Not for production use.
	void SetTestVideoSource(IVideoSource *src) { testVideoSource = src; }

	// Route + open the demux/decode source for filePath WITHOUT touching any
	// player state -- safe on a worker thread (pure FFmpeg/vpx: no GL, no
	// audio channel, no app-config reads). nullptr + outErrorReason on
	// failure. Pair with SetPreopenedSource() on the render thread.
	static IVideoSource *CreateAndOpenSource(const char *filePath, std::string &outErrorReason);

	// Hand a source already opened via CreateAndOpenSource() to the next
	// Open(filePath) call, which then skips both the container probe and
	// source->Open() -- i.e. Open() performs no file I/O and is safe on the
	// GL/render thread even for slow/remote files. One-shot; ownership
	// transfers to the player (deleted via the normal Close()/FreeResources()
	// path, including when Open() fails or the player is destroyed with the
	// seam never consumed).
	void SetPreopenedSource(IVideoSource *src);

	// Playback control.
	//
	// Task 2 (spec #2.3 "Persistent, command-driven decode thread"): the decode
	// thread is now alive from Open() to Close() and every state transition is a
	// COMMAND handed to it -- nothing here joins the thread any more. Play() and
	// Pause() are fire-and-forget flag flips (+ a condition-variable notify that
	// wakes an idle decode thread); Stop() and Seek() are BLOCKING wrappers: they
	// submit a command, then wait on a per-generation completion signal, so their
	// observable behavior is exactly what it was when they ran the seek inline on
	// the caller thread. (Task 3 adds the non-blocking request API on top of the
	// same mailbox.)
	void Play();
	void Pause();

	// Blocking. Rewinds to 0, drops queued audio and lands the player in Idle
	// with GetCurrentTime() == 0 and the source's read position back at the
	// start -- serviced by the decode thread as a "seek to 0 + stop" command.
	//
	// NOTE (Task 2 behavior change): Stop() no longer clears the ring buffer (the
	// rewind is a generation-stamped seek; stale frames are superseded, not
	// wiped), and the last displayed frame lives in a reader-owned snapshot. So
	// GetCurrentFrameRGBA() keeps returning the LAST DISPLAYED FRAME after Stop()
	// -- it used to return nullptr. Callers that want a blank surface must clear
	// it themselves.
	void Stop();

	// Blocking. Submits a precise seek to the decode thread and returns once the
	// thread has serviced it: the source has been seeked, stale ring-buffer
	// frames have been superseded (they are dropped by seek generation, NOT by
	// rewinding the ring), the audio channel has been reset and the playback
	// clock has been re-based on `timeSeconds`. A player that was Playing keeps
	// playing (the decode thread simply refills from the new position); Paused
	// stays Paused; any other prior state (Idle/Finished) lands on Idle.
	void Seek(double timeSeconds);

	// --- Non-blocking seek request API (Task 3) -----------------------------
	//
	// The reason the seek mailbox exists. RequestSeek() submits the seek to the
	// decode thread and RETURNS -- in a few microseconds, whatever the source is
	// doing (a long-GOP precise seek can take tens of ms; the blocking Seek()
	// waits for all of it, which is a dropped frame on the render thread and a UI
	// that stutters under a held arrow key).
	//
	// Latest-wins: a request submitted while an earlier one is still outstanding
	// simply replaces it -- the decode thread only ever services the newest, so a
	// burst of held-key steps costs ONE seek, not one per key repeat.
	//
	// Optimistic UI: GetPendingSeekTarget() exposes the outstanding target the
	// instant it is submitted, so a seek bar / time label can jump there
	// immediately, and Update() FREEZES the playback clock until the decode
	// thread completes the seek (otherwise the free-running audio clock would
	// drag GetCurrentTime() away from the last displayed pre-seek frame).
	//
	// Not-Playing players are served too: the decode thread publishes exactly one
	// frame for the seek and the next Update() displays it (see Update()).
	//
	// Safe no-ops when the player has no source or is in EVideoPlayerState::Error.
	//
	// `mode` (Coarse-seek task): TwoPhase preserves the historical burst-fast +
	// quiet-precise-refine behaviour (default, so any un-migrated caller is
	// unchanged); Precise never goes fast and never refines; Coarse is
	// keyframe-final with the clock re-based onto the landed keyframe. See
	// ESeekMode.
	void RequestSeek(double seconds, ESeekMode mode = ESeekMode::TwoPhase);

	// Non-blocking seek BY `deltaSeconds`, accumulated against the OUTSTANDING
	// PENDING TARGET when there is one, else against the current playback clock --
	// computed atomically with the submit, under cmdMutex, INSIDE the player.
	//
	// This is the whole point. A caller cannot do this itself: RequestSeek(
	// GetCurrentTime() + delta) reads the last DISPLAYED frame's time, which does
	// not move until the decode thread has serviced the previous request -- so a
	// held-key burst of K steps would keep restarting from the same place instead
	// of summing to start + K*delta.
	//
	// The stored target is always clamped to [0, duration]; the return value
	// reports whether the UNCLAMPED accumulation left the clip, so the caller can
	// cross a clip boundary (the photo app: step into the next/previous photo)
	// without re-deriving it from a racy time read. Returns InRange for a player
	// with no source / in Error (nothing was submitted).
	//
	// `mode`: see RequestSeek()/ESeekMode. The arrow-scrub app path submits
	// Coarse; frame-step submits Precise.
	ESeekRequestOutcome RequestSeekRelative(double deltaSeconds, ESeekMode mode = ESeekMode::TwoPhase);

	// The outstanding seek target, or < 0 when no seek is in flight. Lock-free.
	//
	// Goes back to -1 the moment the decode thread has FULLY serviced the request
	// (at which point GetCurrentTime() has been re-based onto the target), so a
	// caller that wants "where is this player headed?" reads
	//   t = GetPendingSeekTarget(); if (t < 0) t = GetCurrentTime();
	//
	// Also goes back to -1 if Close() races an outstanding request (the decode
	// thread's shutdown path clears it): a dead player never reports a target.
	double GetPendingSeekTarget() const;

	void Close();

	// Call from main/render thread each frame. Returns true if a new video frame
	// was displayed (uploaded to GPU / snapshotted for GetCurrentFrameRGBA()).
	//
	// Task 3 added two things to it:
	//
	//  * CLOCK FREEZE. While a seek is outstanding (seekGeneration !=
	//    completedSeekGeneration) the A/V-sync recompute is SKIPPED and
	//    currentTime is held at the last displayed frame's time. Otherwise the
	//    audio/wall clock would keep running through the pending window and
	//    GetCurrentTime() would sprint away from the picture, which is still the
	//    pre-seek frame. The decode thread re-bases the whole clock triplet from
	//    the target as it completes the seek, so the clock resumes THERE.
	//
	//  * NOT-PLAYING DELIVERY. Update() used to return false immediately unless
	//    Playing, so a frame published for an async seek issued while Paused/Idle/
	//    Finished (scrubbing, arrow-key stepping) would never be displayed. It now
	//    runs the claim-scan on that path too and displays at most ONE
	//    current-generation frame -- gated on "the newest seek generation has not
	//    been displayed yet" (see lastDisplayedGeneration), so merely pausing
	//    mid-playback does NOT let Update() keep draining buffered future frames.
	//    Neither the EOS/Finished transition nor the clock recompute runs there.
	//    If the claim comes up empty for a generation that is no longer pending
	//    (a Stop() -- which publishes no frame -- or a genuine decode failure/EOS
	//    at the seek target), lastDisplayedGeneration is advanced anyway so the
	//    gate closes instead of re-running the scan on every future Update().
	//
	// The Playing-path EOS -> Finished transition is gated on the seek NOT being
	// outstanding: endOfStream can be a stale latch from before a seek was
	// requested, and firing onFinished() while that seek is still in flight would
	// be a spurious completion. This only DEFERS the transition -- the decode
	// thread always clears endOfStream as part of servicing a seek, and a genuine
	// EOS reached after the seek lands re-sets it and is caught on a later tick.
	bool Update(float deltaTime);

	// Plan-2 Task 3b: paused-seek single-frame delivery. Update() only ever
	// delivers a frame while Playing (it returns false immediately
	// otherwise), so a Paused/Idle player could never show a seeked or
	// stepped-to frame -- this is what makes frame-step and paused scrubbing
	// (Tasks 10/12) possible. `seconds` is CLIP-RELATIVE (same contract as
	// Seek()/IVideoSource::Seek()).
	//
	// BLOCKING, like Seek(): it submits a seek command and waits for the decode
	// thread to service it (Task 2 -- the seek+decode no longer runs inline on
	// the caller thread; only the DISPLAY step does).
	//
	// CALLING IT WHILE PLAYING IS SUPPORTED and keeps the pre-Task-2 contract: the
	// player is parked in Paused for the duration of the seek (the service routine
	// only publishes its single frame while the player is not Playing -- the old
	// body achieved the same by stopping the decode thread), the frame is
	// displayed, the clock is re-based onto that frame's pts, and Playing is then
	// restored so playback continues from the seeked position. On failure the
	// pre-call state (Playing/Paused/Idle/Finished) and clock are restored.
	//
	// While the player is not Playing the service routine decodes exactly ONE
	// frame and publishes it;
	// this call then claims that frame and runs the same display step Update()'s
	// Playing path runs (CPU: last-displayed snapshot; RGBA mode: render-to-
	// texture; legacy GPU mode: UploadYUVToGPU) so GetCurrentFrameRGBA()/
	// GetRGBATexture()/the plane textures reflect it immediately -- no need
	// to pump Update() afterward. On success, GetCurrentTime() == the
	// delivered frame's pts. Returns false on decode failure/EOS (e.g.
	// seeking at/past duration) or when the player is in
	// EVideoPlayerState::Error -- in the Error case this is a pure guard
	// (returns immediately, nothing touched). In the decode-failure/EOS
	// case the attempted seek still runs first (queued audio is dropped, stale
	// ring frames are superseded by seek generation and the source's read
	// position moves, same as Seek() always does), but GetState()/currentTime
	// are left exactly as they were, so the player is not wedged -- a fresh
	// Seek()+Play() afterward works.
	// Does not disturb the audio channel beyond draining/discarding queued
	// audio the same way Seek() already does -- paused stepping never plays
	// audio.
	bool SeekPausedAndDecodeFrame(double seconds);

	// GPU texture handles (GL_R8 luminance textures for YUV shader)
	// void*, not GLuint: these are backend handles now (a GL name cast through
	// uintptr_t, or an id<MTLTexture>), and GLuint is 32-bit -- it would
	// silently truncate half of a Metal texture pointer.
	void *GetYTexture() const { return texY; }
	void *GetUTexture() const { return texU; }
	void *GetVTexture() const { return texV; }
	void *GetATexture() const { return texA; }

	// Render-to-texture RGBA output (Task 10). Call before Open(); requires a
	// current GL context (leave false for headless tests -- see
	// enableGPUUpload above for the same convention). When enabled, Update()
	// converts the newest ring-buffer frame through
	// CVideoYUVShader::RenderToTarget() into an internally-owned
	// CGLRenderTarget sized to GetDisplayWidth()/GetDisplayHeight().
	//
	// EXCLUSIVE with the legacy 3-plane upload: while enabled, Update() skips
	// CreateGPUTextures()/UploadYUVToGPU() entirely, so GetYTexture()/
	// GetUTexture()/GetVTexture()/GetATexture() stay 0 -- consume
	// GetRGBATexture() instead. (Running both would double the per-frame
	// upload cost for no consumer; cutscene callers that sample the raw
	// plane textures simply never enable this mode.)
	void SetOutputRGBATexture(bool enabled) { outputRGBATexture = enabled; }

	// Display-oriented RGBA8 texture handle produced by the render-to-texture
	// path (0 until SetOutputRGBATexture(true) was called and at least one
	// frame has gone through Update()). Rotation is already baked in -- the
	// texture's dimensions are GetDisplayWidth() x GetDisplayHeight(), not the
	// coded GetVideoWidth() x GetVideoHeight().
	// void*, matching the CSlrImage handle convention: GLuint is 32-bit and
	// would silently truncate an id<MTLTexture>.
	void *GetRGBATexture() const;

	// The RESIDENT format of the texture GetRGBATexture() returns: RGBA16F for
	// an HDR clip with the playback gate open, RGBA8 otherwise. The consumer
	// needs it so the CSlrImage adapter wrapping that texture is not assumed
	// 8-bit (S-5 Phase 5).
	ERenderTextureFormat GetRGBATextureFormat() const;

	// CM-E: install/replace the display colour LUT sampled by the YUV shader
	// after the matrix. rgba16 = edge^3 RGBA16 texels (alpha ignored), b-major
	// (r fastest). nullptr or edge < 2 clears the LUT (raw pass-through --
	// pre-CM-E behaviour). Render thread only (GL); no-op in CPU mode
	// (!outputRGBATexture). The player owns the GL texture (deleted in
	// FreeResources()). If a frame is currently displayed, re-runs its display
	// step so the change repaints a paused clip without user input.
	void SetColorLut3D(const u16 *rgba16, int edge);

	// S-5 Phase 5: HDR playback policy, pushed down from the app.
	//
	// The ENGINE cannot decide this. Whether HDR is on is
	// PC_HdrSessionGateOpen(), which lives in the photo app
	// (src/Decode/PC_ResidentFormat.cpp) and is latched once per session from
	// the app's own setting and the display snapshot; the engine has no
	// business reaching into that. So the app resolves it and hands the answer
	// down, exactly as it already does for the CM-E colour LUT above.
	//
	// `gateOpen` false (the default) keeps today's behaviour in every
	// particular: an RGBA8 target and, for HDR clips, the tone-mapped arm.
	void SetHdrPlaybackGate(bool gateOpen);
	bool IsHdrPlaybackGateOpen() const { return hdrGateOpen; }

	// Video metadata
	bool HasAlpha() const { return hasAlpha; }
	int GetColorSpace() const { return vpxColorSpace; }
	int GetColorRange() const { return vpxColorRange; }
	// CM-E: raw AVCOL_PRI_* / AVCOL_TRC_* (2 = unspecified). Trc 16 (PQ) and
	// 18 (HLG) are the HDR pair the photo app's LUT gate keys on. Container-
	// level values are available right after Open(); frame-level tags refine
	// them per decoded frame.
	int GetColorPrimaries() const { return colorPrimaries; }
	int GetColorTrc() const { return colorTrc; }
	int GetVideoWidth() const { return videoWidth; }
	int GetVideoHeight() const { return videoHeight; }

	// Post-rotation (display-space) dimensions. Coded dims are swapped when
	// the source's display matrix carries a 90/270 rotation (see
	// SVideoInfo::rotationDegrees). GetCurrentFrameRGBA() returns pixels in
	// this orientation/size, not GetVideoWidth()/GetVideoHeight()'s coded size.
	int GetDisplayWidth() const { return (rotationDegrees == 90 || rotationDegrees == 270) ? videoHeight : videoWidth; }
	int GetDisplayHeight() const { return (rotationDegrees == 90 || rotationDegrees == 270) ? videoWidth : videoHeight; }

	double GetDuration() const { return duration; }

	// Locked snapshot of the playback clock (Task 2): currentTime is now a
	// read-modify-write shared between the render thread (Update()'s A/V-sync
	// advance) and the decode thread (a serviced seek re-bases it), so every
	// access goes through `mutex`. Never call this while holding `mutex`.
	double GetCurrentTime() const;

	// pts of the frame currently handed to the display snapshot (the last frame
	// DisplayFrame() published), or -1.0 before the first frame is shown. Unlike
	// GetCurrentTime() -- which is the free-running A/V-sync clock -- this is the
	// timestamp of the actual picture on screen. Reader-thread owned (written only
	// by DisplayFrame(), which runs on the same thread as every reader of this).
	double GetLastDisplayedPts() const { return displaySnapshotValid ? displaySnapshot.pts : -1.0; }

	double GetFps() const { return fps; }

	// State
	bool IsPlaying() const { return state == EVideoPlayerState::Playing; }
	bool IsFinished() const { return state == EVideoPlayerState::Finished; }
	EVideoPlayerState GetState() const { return state; }

	// Non-empty when Open() failed or the decode thread ended in
	// EVideoPlayerState::Error (Plan-2 Task 2) -- lets a UI-facing caller
	// (Task 10's controller) show a reason instead of just a bare failure.
	// Empty otherwise (including the ordinary end-of-stream/Finished path).
	// BY VALUE, under the leaf mutex (final review): the decode thread writes
	// this string, and the player is re-usable after an error (Seek() lands
	// Idle, Play() is allowed again), so a SECOND error can rewrite the buffer
	// while a render-thread reader still holds a reference into the first --
	// a data race and a potential use-after-free. Callers all copy anyway.
	std::string GetErrorReason() const;

	// Audio channel for SND_AddChannel
	CVideoAudioChannel *GetAudioChannel() const { return audioChannel; }

	// CPU fallback: returns RGBA pixel buffer (width * height * 4 bytes), or nullptr
	u8 *GetCurrentFrameRGBA();

	// Decimated CPU conversion for per-pixel STATS consumers (histogram tap --
	// 2026-07-19 choppy-4K-playback fix): converts only every step-th pixel of
	// the displayed frame, where step = max(1, ceil(sqrt(w*h / maxPixels))),
	// so outW*outH <= maxPixels and the conversion cost scales with the BUDGET,
	// not the frame area (~30x cheaper than GetCurrentFrameRGBA() on 4K at the
	// histogram's 256K budget). Pixels are byte-identical to the full
	// conversion at (i*step, j*step).
	//
	// Orientation contract: CODED orientation -- rotationDegrees is NOT
	// applied (per-pixel stats are orientation-invariant; skipping the rotate
	// keeps this tap allocation- and copy-minimal). outW/outH are the sampled
	// CODED dims. Same thread contract as GetCurrentFrameRGBA() (reader/render
	// thread only; converts the display snapshot). Returns nullptr when no
	// frame has been displayed yet.
	u8 *GetCurrentFrameRGBASampled(int maxPixels, int &outW, int &outH);

	// Set to false to skip GPU texture creation/upload (e.g., for headless tests)
	// When false, frames are still decoded and available via GetCurrentFrameRGBA()
	bool enableGPUUpload = true;

	// Test hooks (Task 6) -- the two-phase seek is otherwise INVISIBLE from the
	// outside: both modes re-base the clock onto the same target, so a test that
	// only reads GetCurrentTime() cannot tell a fast landing from a precise one
	// and would be vacuous. These count what the decode thread actually asked the
	// source for: one increment per seek SERVICE (source->SeekFast() vs
	// source->Seek()), including the quiet-refine's precise pass and including a
	// service that is subsequently aborted. Mirrors
	// CVideoSourceFFmpeg::GetDebugFramesDecodedAfterSeek()'s convention.
	int GetDebugFastSeeksServiced() const { return debugFastSeeksServiced.load(std::memory_order_relaxed); }
	int GetDebugPreciseSeeksServiced() const { return debugPreciseSeeksServiced.load(std::memory_order_relaxed); }

	// Callback invoked when playback finishes cleanly (called from main thread
	// in Update() once the decode thread hit ordinary end-of-stream and the
	// ring buffer drained). NEVER fires in EVideoPlayerState::Error -- a
	// decode failure is not "finished"; consumers poll GetState()/
	// GetErrorReason() to observe errors (Plan-2 Task 2).
	std::function<void()> onFinished;

private:
	// --- Decoded frame ring buffer ---
	static constexpr int VIDEO_BUFFER_FRAMES = 4;

	struct DecodedFrame
	{
		u8 *yPlane = nullptr;
		u8 *uPlane = nullptr;
		u8 *vPlane = nullptr;
		u8 *aPlane = nullptr;
		int yStride = 0;   // byte strides (packed on copy; not necessarily == width)
		int uStride = 0;
		int vStride = 0;
		int aStride = 0;
		int width = 0;
		int height = 0;
		int allocWidth = 0;  // Allocated plane dimensions (may be >= decoded dimensions)
		int allocHeight = 0;
		double pts = 0.0;
		// Slot ownership state machine (spec #2.3 rule 3). Transitions are
		// CAS-owned: writer EMPTY->WRITING->READY; reader READY->CONSUMING->EMPTY;
		// writer may reclaim a STALE-generation READY tail slot via READY->WRITING.
		static constexpr uint32_t SLOT_EMPTY = 0, SLOT_WRITING = 1,
		                          SLOT_READY = 2, SLOT_CONSUMING = 3;
		std::atomic<uint32_t> stateWord{SLOT_EMPTY};
		// Mailbox generation this frame was decoded under; written while the
		// slot is WRITING (single-owner), read after a successful claim.
		uint64_t generation = 0;
		// ABSOLUTE (pre-modulo) ring index this slot's contents were published
		// under -- the slot's identity, and the ONLY way a reader can tell which
		// frame it just claimed (Task 2 finding: ring-index ABA).
		//
		// A reader loads readIdx = frameReadIdx and only THEN CASes the physical
		// slot readIdx % N. In between, the writer's tail reclamation can free that
		// exact slot (stale tail) and -- with a full window, where writeIdx % N ==
		// readIdx % N -- immediately refill it as readIdx + N. The preempted
		// reader's CAS then succeeds on a slot that is no longer the frame it asked
		// for, and the generation stamp cannot disambiguate (both are current-gen).
		// So every claim re-verifies absIdx and hands the slot back on mismatch --
		// see TryClaimSlotForRead(). Written while WRITING (single-owner), read
		// after the acquire-CAS that wins the claim.
		uint32_t absIdx = 0;
		bool hasAlpha = false;

		// Pixel layout as emitted by IVideoSource::ReadVideoFrame (Task 5 can
		// emit YUV420P/YUVA420P/NV12/YUV420P10/YUV422P/YUV422P10) -- the CPU
		// RGBA fallback path (ConvertYUV420ToRGBA) branches on these to
		// interpret the raw plane bytes correctly.
		EVideoPixelFormat pixelFormat = EVideoPixelFormat::YUV420P;
		int bytesPerSample = 1;        // 1 (8-bit) or 2 (10-bit-in-16, little-endian)
		bool chromaFullHeight = false; // true for 4:2:2 (chroma NOT vertically subsampled)
		bool chromaInterleaved = false;// true for NV12 (uPlane holds interleaved U,V pairs)

		// Byte capacities backing yPlane/uPlane/vPlane/aPlane -- grown on demand
		// in StoreDecodedFrame() so any Task-5 pixel format/bit-depth fits,
		// independent of the 8-bit-4:2:0 sizing AllocateFrameBuffers() used to
		// assume.
		int yCapacity = 0;
		int uCapacity = 0;
		int vCapacity = 0;
		int aCapacity = 0;
	};

	DecodedFrame frameBuffer[VIDEO_BUFFER_FRAMES];
	std::atomic<uint32_t> frameWriteIdx{0};
	std::atomic<uint32_t> frameReadIdx{0};

	// Seek-mailbox generation (spec #2.3). Bumped (under cmdMutex) by EVERY seek
	// submit -- Seek(), SeekPausedAndDecodeFrame() and Stop() all go through
	// SubmitSeekCommandLocked(). Frames stamped with an older value are stale
	// and are consume-dropped by the reader / reclaimed by the writer. Read
	// lock-free by the reader (Update()) and by the writer's tail reclamation:
	// it is the "newest generation anybody asked for" watermark.
	std::atomic<uint64_t> seekGeneration{0};

	// The generation the decode thread is CURRENTLY decoding under -- owned by
	// the decode thread (written in ServiceSeekOnDecodeThread(), read only in
	// StoreDecodedFrame(), both decode-thread-only), so it needs no atomicity.
	//
	// Frames MUST be stamped from this, not from seekGeneration: seekGeneration
	// jumps to the new value the instant a seek is SUBMITTED (on the caller
	// thread), while the decode thread may still be finishing a pre-seek frame.
	// Stamping such a frame with the new generation would disguise a pre-seek
	// picture as a post-seek one and it would be displayed at the seek target.
	uint64_t decodeGeneration = 0;

	// The generation the decode thread is CURRENTLY SERVICING a seek command for
	// (Task 5). Written by ServiceSeekOnDecodeThread() before it touches the
	// source; read (lock-free, from the decode thread and from FFmpeg's I/O
	// interrupt callback) by ShouldAbortDecode().
	//
	// Distinct from decodeGeneration -- which is what frames are STAMPED with and
	// is decode-thread-private -- because the abort predicate is read from
	// FFmpeg's interrupt hook, i.e. potentially from inside a library call, and
	// must therefore be an atomic. They happen to carry the same value; the two
	// exist for different readers.
	//
	// seekGeneration != servicingSeekGeneration is exactly "the work in flight has
	// already been superseded": seekGeneration jumps ahead the instant a newer
	// request is SUBMITTED, and this catches up only when the decode thread starts
	// servicing that newer request. Outside any such window the two are equal and
	// the predicate is FALSE -- the abort is generation-scoped and self-clearing,
	// so there is no re-arm step anybody can forget.
	std::atomic<uint64_t> servicingSeekGeneration{0};

	// --- Decode-thread command & seek mailbox (spec #2.3) ---
	// cmdMutex guards every field in this block (plus the seekGeneration bump).
	// cmdCv wakes the decode thread; serviceCv wakes the blocking wrappers
	// waiting for a submitted command to be serviced.
	//
	// LOCK ORDER: cmdMutex OUTER, the clock mutex (`mutex` below) INNER. The clock
	// mutex is the LEAF -- cmdMutex must never be taken while `mutex` is held.
	//
	// Every existing `mutex` critical section (Update()'s clock recompute,
	// ServiceSeekOnDecodeThread()'s re-base, Seek()/Stop()'s degraded no-thread
	// paths) takes and releases `mutex` on its own, holding no cmdMutex-covered
	// state across it, so this order is consistent everywhere. Task 3's
	// RequestSeekRelative() is the first (and only) place that takes both, in that
	// order: it MUST read the clock and submit the accumulated target as one
	// atomic step, or a concurrent seek could re-base the clock between the read
	// and the submit and the step would accumulate off the wrong base.
	std::mutex cmdMutex;
	std::condition_variable cmdCv;
	std::condition_variable serviceCv;

	// --- Two-phase seek (Task 6, spec #2.3) ---------------------------------
	// The burst window, and the quiet window that closes it. A request landing
	// within this of the PREVIOUS one is part of a burst (held arrow key,
	// seek-bar scrub) and is serviced in FAST mode (IVideoSource::SeekFast(): the
	// keyframe landing, no walk to the target -- see there for why that is
	// hundreds of ms cheaper on long-GOP content). The same window then measures
	// the quiet: once the mailbox has stayed empty for it after a fast landing,
	// the decode thread issues ONE precise seek to the same target, which is what
	// makes the final picture exact.
	static constexpr double kSeekBurstWindowSec = 0.150;

	// Latest-wins seek mailbox: valid when seekTargetPending >= 0.
	double   seekTargetPending = -1.0;
	// Burst classification, decided AT SUBMIT (under cmdMutex) and carried through
	// the mailbox -- NOT re-derived on the decode thread. The question the
	// heuristic answers is "how fast is the USER stepping?", which is a property of
	// the inter-REQUEST spacing; measuring it at consume time instead would answer
	// "how far behind is the decode thread?" and would mis-classify a genuine burst
	// as quiet exactly when the thread is slow -- i.e. on precisely the long-GOP
	// content the fast mode exists for.
	bool     seekIsBurst = false;
	// The mode the mailbox's CURRENT content was submitted with (Coarse-seek
	// task). Carried through the mailbox instead of being re-derived on the
	// decode thread -- the burst classification (seekIsBurst) answers "how fast
	// is the user stepping?", but WHICH servicing strategy to use is the
	// caller's explicit choice and must survive the hand-off. TwoPhase for any
	// legacy submit; the decode thread reads it at consume time.
	ESeekMode seekModePending = ESeekMode::TwoPhase;
	uint64_t seekPendingGeneration = 0;       // generation of the mailbox's CURRENT content
	double   lastSeekRequestWallTime = 0.0;   // wall time of the previous submit (burst detection)
	uint64_t seekServicedGeneration = 0;      // last generation fully serviced

	// The armed quiet-refine (Task 6). Set by the decode thread after a FAST
	// landing, cleared by any newer submit (SubmitSeekCommandLocked), by the stop
	// command and by the decode thread itself once it has fired or invalidated it.
	// All three fields are cmdMutex-guarded.
	//
	// refineGeneration is the generation of the FAST LANDING, and the refine is
	// serviced UNDER IT: it does NOT bump seekGeneration. Bumping would turn the
	// refine into a request in its own right, and the generation rules would then
	// work against it -- the frames the fast landing already published would go
	// stale mid-scrub (rule 1), Update()'s clock would re-freeze and
	// GetPendingSeekTarget() would report an outstanding seek, all for something
	// the user never asked for. Re-running the service under the SAME generation
	// keeps every stamp current and makes the refine invisible to every reader,
	// which is what it should be: a correction of a landing already published, not
	// a new request. It stays fully abortable -- a newer submit bumps
	// seekGeneration, the predicate goes true, and the refine is cut short like any
	// other seek (and the check below never fires it in the first place).
	bool     refinePending = false;
	double   refineTarget = -1.0;
	uint64_t refineGeneration = 0;
	double   refineDeadlineWallTime = 0.0;    // fire once the wall clock passes this

	// GATE-DROP-ONLY variant of the armed deadline (Coarse-seek task). A COARSE
	// landing classified as a burst leaves the audio gate RAISED (like TwoPhase's
	// fast landing) so a held scrub stays silent -- but Coarse has no precise
	// refine to eventually drop it. So it arms the SAME deadline machinery with
	// this flag set: when the deadline fires with nothing newer pending, the
	// command loop ONLY drops the audio gate (no re-seek, no frame). A NON-burst
	// coarse landing drops the gate at completion and never arms this. Guarded by
	// refinePending (only read while that is true), cmdMutex-covered like the rest
	// of the refine block; every disarm path (stale gen / EOS / newer submit)
	// drops the gate too, so it can never strand the gate raised mid-clip.
	bool     refineGateDropOnly = false;

	// Bumped by the decode thread each time a REFINE published a frame for a
	// not-Playing player, and mirrored by the reader in lastDisplayedRefinePublish.
	// It is the ONE thing about the refine a reader must see: Update()'s
	// not-Playing delivery gate closes on "the newest seek generation has already
	// been displayed", and the refine deliberately does not bump that generation --
	// so without this counter the corrected frame would never be claimed and a
	// PAUSED scrub would end on the fast landing's keyframe picture (a whole GOP
	// off) instead of the frame the user actually stopped on. Lock-free: written
	// after the frame is in the ring, read at the top of Update().
	std::atomic<uint64_t> refinePublishCounter{0};
	bool     stopCommandPending = false;      // Stop(): seek-to-0 + land Idle
	// The generation the pending stop was submitted WITH. A Seek() submitted
	// between Stop()'s submit and the decode thread's consume overwrites the
	// mailbox (latest-wins) with a different target and a NEWER generation; without
	// this binding the thread would still see stopCommandPending == true and land
	// Idle at the SEEK's target, satisfying Stop()'s waiter at the wrong position.
	// The thread only honours the stop when stopCommandGeneration == the generation
	// it is actually servicing; a superseded stop just releases its waiter.
	uint64_t stopCommandGeneration = 0;
	bool     stopCommandDone = false;
	// Set by the decode thread as it leaves DecodeThreadFunc (Close()/shutdown).
	// A wrapper released by THIS rather than by a real service must not touch the
	// ring buffer afterwards -- FreeResources() is about to delete[] the planes.
	bool     decodeThreadExited = false;

	// Lock-free mirrors for render-thread reads (written under cmdMutex):
	std::atomic<double>   pendingSeekTargetAtomic{-1.0};   // Task 3: GetPendingSeekTarget()
	std::atomic<uint64_t> completedSeekGeneration{0};      // Task 3: clock-freeze check

	// Test hooks -- see GetDebugFastSeeksServiced()/GetDebugPreciseSeeksServiced().
	// Written by the decode thread, read by the test thread: atomic, relaxed.
	std::atomic<int> debugFastSeeksServiced{0};
	std::atomic<int> debugPreciseSeeksServiced{0};

	// --- GPU textures ---
	void *texY = NULL;
	void *texU = NULL;
	void *texV = NULL;
	void *texA = NULL;
	bool gpuTexturesCreated = false;

	// --- RGBA render-to-texture output (Task 10) ---
	bool outputRGBATexture = false;
	// Obtained from the backend factory, not new'd directly -- that is what makes
	// the RGBA display path work on Metal as well as OpenGL.
	CVideoYUVConverter *rgbaShader = nullptr;
	CRenderTarget *rgbaTarget = nullptr;

	// Plane textures feeding rgbaShader->RenderToTarget() -- separate from
	// texY/texU/texV/texA above because those are always 8-bit GL_R8 (the
	// legacy Render()-path upload, CreateGPUTextures()/UploadYUVToGPU(), never
	// branched on pixelFormat). These do: rgbaTexU doubles as the NV12
	// interleaved GL_RG8 plane, and all three go GL_R16 for YUV420P10/YUV422P10.
	// CM-E: display colour LUT (GL_TEXTURE_3D, RGBA16, lutEdge^3), owned by
	// the player, deleted ONLY in FreeResources() -- see SetColorLut3D().
	// Backend handle, not GLuint: a GL name cast through uintptr_t under
	// OpenGL, an id<MTLTexture> under Metal.
	void *lutTexture = NULL;
	int lutEdge = 0;

	// void*, not GLuint: these come from CRenderBackend::CreatePlaneTexture and
	// hold an id<MTLTexture> under Metal, which a 32-bit GLuint would truncate.
	void *rgbaTexY = NULL;
	void *rgbaTexU = NULL;
	void *rgbaTexV = NULL;
	void *rgbaTexA = NULL;
	int rgbaTexWidth = 0;
	int rgbaTexHeight = 0;
	EYUVShaderMode rgbaTexMode = EYUVShaderMode::YUV420_3Plane;
	bool rgbaTexHasAlpha = false;

	// --- Video metadata ---
	int videoWidth = 0;
	int videoHeight = 0;
	double duration = 0.0;
	double fps = 30.0;
	double currentTime = 0.0;
	bool hasAlpha = false;

	// Display-matrix rotation (0/90/180/270), mirrored from SVideoInfo at
	// Open() time. See SVideoInfo::rotationDegrees for the sign convention.
	int rotationDegrees = 0;

	// Color space info (VPX_CS_* and VPX_CR_* enums stored as int), mirrored from
	// the active IVideoSource's SVideoInfo each time a frame is decoded.
	// vpxColorSpace holds the NORMALIZED matrix value (see SVideoInfo::colorSpace).
	// All four mirrors are atomics: the decode thread refines them per-frame
	// (StoreDecodedFrame) while the render thread reads them for the
	// shader/CPU converters and the HDR gate -- plain ints were a formal
	// data race (programme review 2026-08-11). Each value is independently
	// meaningful and a one-frame-late read is harmless, so no ordering is
	// needed beyond atomicity (default ops used; per-frame cost is nil).
	std::atomic<int> vpxColorSpace { 0 };
	std::atomic<int> vpxColorRange { 0 };
	// CM-E: raw AVCOL_PRI_* / AVCOL_TRC_* mirrored beside them (2 = unspecified).
	std::atomic<int> colorPrimaries { 2 };
	std::atomic<int> colorTrc { 2 };

	// S-5 Phase 5: see SetHdrPlaybackGate(). Render-thread read, app-thread
	// write, so atomic -- the value is a single bool and never a lock.
	std::atomic<bool> hdrGateOpen { false };

	// --- Video source (demux + decode) ---
	IVideoSource *source = nullptr;
	bool hasAudio = false;

	// Test-only injection seam -- see SetTestVideoSource() above. Consumed
	// (and cleared) by the next Open() call.
	IVideoSource *testVideoSource = nullptr;

	// Async-open seam -- see SetPreopenedSource() above. Consumed (and
	// cleared) by the next Open() call.
	IVideoSource *preopenedSource = nullptr;

	// Non-empty on Open() failure or a decode-thread-detected source error
	// (EVideoPlayerState::Error) -- see GetErrorReason() above.
	// Written ONLY through SetErrorReason (leaf mutex); read only through
	// GetErrorReason, which copies under the same lock -- see the accessor.
	std::string errorReason;
	void SetErrorReason(const std::string &reason);

	// --- Audio ---
	CVideoAudioChannel *audioChannel = nullptr;

	// --- Playback state ---
	std::atomic<EVideoPlayerState> state{EVideoPlayerState::Idle};
	double playbackStartWallTime = 0.0;
	double playbackStartOffset = 0.0;

	// AUDIO-STARVATION FREE-RUN state (2026-07-18; see Update()'s clock block).
	// The A/V clock is slaved to audio-ring consumption; when consumption
	// stalls (audio track ended mid-clip, or a lazily-interleaved container --
	// real-world ASF/WMV -- lags audio behind the video ring's depth), the
	// frozen clock deadlocked playback: no displayable frame -> full ring ->
	// blocked decode thread -> the unfreezing audio can never be demuxed.
	// These track when consumption last advanced so Update() can fall back to
	// wall-rate advancement past the stall threshold. Render-thread-only,
	// mutated exclusively inside Update()'s mutex-held clock block (the
	// fresh-epoch reset lives on the audioPos<=0.001 path, which a seek's
	// channel Reset() funnels through).
	double audioClockLastPos = -1.0;         // GetPlaybackPosition() at its last observed advance
	double audioClockLastAdvanceWall = 0.0;  // wall time of that last advance
	double audioClockFreeRunWall = -1.0;     // wall anchor while free-running; -1 = not free-running
	static constexpr double kAudioClockStallFreeRunSec = 0.25; // consumption silence before free-run kicks in

	// AUDIO PUMP-AHEAD low-water mark (2026-07-18; DecodeThreadFunc's
	// ring-full branch): when the mixer's buffered PCM drops below this while
	// the video ring is full, the decode thread asks the source to demux
	// forward for audio only (IVideoSource::PumpAudioAhead()) instead of
	// sleeping -- the lazily-interleaved-container fix that PREVENTS the
	// starvation the free-run above merely survives. Half the audio ring's
	// 2s capacity would be wasteful; 0.5s comfortably covers scheduler jitter
	// plus one pump round-trip, and well-interleaved files never dip under it.
	static constexpr double kAudioPumpLowWaterSec = 0.5;

	// FIRST-FRAME CLOCK ANCHOR (cold-open start fix).
	//
	// Play() bases the clock at the wall-clock instant it is called, with
	// offset = currentTime (0 on a freshly opened clip). But the FIRST frame is
	// not displayable until the decoder's reorder window has emitted it -- for a
	// B-frame HEVC stream that is ~9 packet-decodes (tens of ms on 1080p), plus
	// the async-open latency ahead of Play(). By the time a frame reaches the
	// ring the free-running clock has already advanced past the opening frames'
	// pts, so Update()'s catch-up scan consume-drops them and the FIRST picture
	// shown is frame 2/3 -- a visible jump at the start of playback.
	//
	// Armed by Open() on every freshly opened clip; the first frame displayed
	// while Playing re-bases the whole clock triplet onto THAT frame's pts and
	// disarms this. Cleared by any explicit reposition (Seek/RequestSeek/
	// SeekPausedAndDecodeFrame/Stop) so their own service-time clock re-base owns
	// the start instead -- this must only govern the cold-open-from-0 start.
	//
	// Render-thread only (Open()/Update()/the seek entry points all run there);
	// atomic purely for defensive publication, never contended.
	std::atomic<bool> anchorClockToFirstFrame{false};

	// --- Decode thread ---
	std::thread decodeThread;
	std::atomic<bool> shouldStop{false};
	std::atomic<bool> threadRunning{false};
	std::atomic<bool> endOfStream{false};
	CSlrMutex *mutex = nullptr;

	// --- CPU fallback RGBA buffer ---
	u8 *rgbaBuffer = nullptr;    // unrotated, coded-dimension RGBA (ConvertYUV420ToRGBA output)
	int rgbaBufferSize = 0;
	u8 *rotatedBuffer = nullptr; // display-oriented RGBA (rotationDegrees != 0 only)
	int rotatedBufferSize = 0;
	u8 *rgbaSampledBuffer = nullptr; // GetCurrentFrameRGBASampled() output (coded orientation, decimated)
	int rgbaSampledBufferSize = 0;

	// Reader-owned copy of the last displayed frame, handed over by DisplayFrame()
	// while it still holds the slot's CONSUMING claim (Task 2 finding a).
	//
	// GetCurrentFrameRGBA() used to convert frameBuffer[(frameReadIdx - 1) % N]
	// with NO claim at all. That slot is EMPTY and inside the writer's window, so
	// with a PERSISTENT decode thread StoreDecodedFrame() can memcpy new planes
	// into it while the conversion is walking them -- a torn frame. It cannot be
	// fixed by re-claiming the slot (EMPTY may already be owned by the writer),
	// and retaining the claim would break StoreDecodedFrame()'s tail reclamation
	// (a full window needs exactly the slot the reader would be sitting on).
	//
	// So DisplayFrame() SWAPS the displayed frame's plane pointers + capacities
	// into this snapshot (O(1), no memcpy: the slot gets the snapshot's previous
	// buffers back, which EnsurePlaneCapacity() regrows on demand) and copies its
	// metadata. The snapshot is touched only by the reader thread, so
	// GetCurrentFrameRGBA() can convert it without any synchronization.
	DecodedFrame displaySnapshot;
	bool displaySnapshotValid = false;

	// Seek generation of the last frame DisplayFrame() actually put on screen.
	// Reader-thread-owned (DisplayFrame() runs only on the caller/render thread),
	// so it needs no atomicity.
	//
	// This is what gates Update()'s not-Playing delivery path (Task 3): it fires
	// only while seekGeneration != lastDisplayedGeneration, i.e. "the newest seek
	// anybody asked for has not been shown yet". Without the gate, Update() on a
	// merely PAUSED player would happily keep claiming and displaying the
	// buffered future frames the decode thread left in the ring -- the picture
	// would creep forward for a few frames after every Pause().
	//
	// NOT strictly "the last generation actually DISPLAYED" any more: a
	// generation that is fully serviced but publishes no frame (a Stop() -- see
	// ServiceSeekOnDecodeThread's decodeOneFrameWhenNotPlaying=false -- or a
	// decode failure/EOS at the seek target) also advances this, from inside
	// Update(), so the gate closes instead of re-running an always-empty
	// claim-scan on every subsequent Update() forever.
	uint64_t lastDisplayedGeneration = 0;

	// Reader-side mirror of refinePublishCounter (reader-thread-owned, like
	// lastDisplayedGeneration above): the value that was already displayed/
	// accounted for. They differ exactly while a refine has published a corrected
	// frame the reader has not picked up yet -- which is the second way the
	// not-Playing delivery gate can open.
	uint64_t lastDisplayedRefinePublish = 0;

	// --- Private methods ---
	void AllocateFrameBuffers();

	// Alive from Open() to Close() (Task 2). StopDecodeThread() is the ONLY
	// join and runs only from Close()/FreeResources(): it raises shouldStop
	// under cmdMutex, notifies cmdCv and joins.
	void StartDecodeThread();
	void StopDecodeThread();
	void DecodeThreadFunc();

	// Seek mailbox (spec #2.3). SubmitSeekCommandLocked() must be called with
	// cmdMutex held: it clamps `target` to [0, duration], writes the mailbox,
	// classifies the request as part of a burst or not (Task 6), DISARMS any
	// armed quiet-refine (the target it would refine to is superseded),
	// bumps seekGeneration, mirrors pendingSeekTargetAtomic, notifies cmdCv and
	// returns the new generation. WaitForSeekServiced() blocks until the decode
	// thread has fully serviced that generation (or the thread is shutting down)
	// -- this is what makes Seek()/Stop()/SeekPausedAndDecodeFrame() observably
	// blocking while the actual work runs off the caller thread. It returns TRUE
	// only when the generation was really serviced; FALSE means the decode thread
	// exited underneath us (Close() raced the wrapper), and the caller must then
	// touch NOTHING that FreeResources() is about to free -- in particular no ring
	// slot and no plane pointer.
	uint64_t SubmitSeekCommandLocked(double target, ESeekMode mode);
	bool WaitForSeekServiced(uint64_t generation);

	// THE ABORT PREDICATE (Task 5, spec #2.3 "Blocking work must be
	// interruptible"). Installed on the source at Open() and polled by it from
	// its scan/decode loops and from FFmpeg's AVIOInterruptCB. Lock-free by
	// contract: it reads two atomics and NOTHING else -- in particular it must
	// never take cmdMutex, because it is evaluated from inside source->Seek()
	// while a submit on the render thread is holding cmdMutex and calling
	// source->WakeAbort().
	//
	// True when the work in flight is condemned: the player is shutting down, or
	// a NEWER seek generation has been submitted than the one being serviced.
	bool ShouldAbortDecode() const;

	// Decode-thread side of a seek command: seeks the source, drops queued audio,
	// re-bases the playback clock and (when not Playing, and unless this is a
	// Stop) decodes exactly one frame so the paused/idle caller has something to
	// display. Stale ring frames are NOT cleared -- they are superseded by
	// generation (rule 2) and reclaimed by StoreDecodedFrame()'s tail path.
	//
	// `mode` + `isBurst` pick the servicing strategy (Coarse-seek task, replacing
	// the old `bool fastMode`):
	//   * TwoPhase + isBurst    -> SeekFast(); a successful fast service ARMS the
	//                              quiet-refine.
	//   * TwoPhase + !isBurst   -> Seek() (precise); never refines.
	//   * Precise               -> Seek() (precise); never refines.
	//   * Coarse                -> SeekFast() ALWAYS (even a lone press); NEVER
	//                              refines; re-bases the clock onto the LANDED
	//                              keyframe pts, holds the audio gate across a
	//                              burst (gate-drop-only deadline), and retries
	//                              FORWARD (then falls back to one Precise seek)
	//                              when a forward step made no progress.
	// The refine's own precise pass is submitted back through here as Precise.
	//
	// Returns FALSE when the service was ABORTED (Task 5) -- shutdown, or a newer
	// request superseded this one. An aborted service publishes NOTHING: no clock
	// re-base, no completion for `generation`. The caller simply loops back to the
	// mailbox and services the newest target. Blocking waiters keyed to the
	// aborted generation are released by the newer generation's completion --
	// WaitForSeekServiced() waits for seekServicedGeneration >= generation, not
	// == -- or, on the shutdown path, by decodeThreadExited.
	// `outPublishedNotPlayingFrame` (optional) reports the service's OWN
	// not-Playing decision: true iff it actually decoded and stored a frame on the
	// decodeOneFrameWhenNotPlaying path (i.e. state was not Playing when it
	// checked, AND a frame really was read). The refine-publish counter is bumped
	// off THIS, not off a state re-read after the call returns (M2): a Pause()
	// landing in the microseconds between the service's internal check and the
	// re-read would otherwise bump the counter with no frame in the ring.
	bool ServiceSeekOnDecodeThread(double target, uint64_t generation, ESeekMode mode,
								   bool isBurst, bool decodeOneFrameWhenNotPlaying,
								   bool *outPublishedNotPlayingFrame = nullptr);

	// Outcome of a reader-side slot claim (see DecodedFrame::absIdx).
	enum class ESlotClaim
	{
		Claimed,    // slot is CONSUMING, and it really IS the frame at `absIdx`
		NotReady,   // not published yet, or the writer/another reader owns it
		Recycled    // ABA: the slot was reclaimed + refilled under us; rescan
	};

	// READY->CONSUMING CAS on frameBuffer[absIdx % N] followed by the absolute-
	// index re-verification that makes the claim unambiguous. On Recycled the
	// claim is handed back (CONSUMING->READY) before returning, so the caller
	// must simply re-read frameReadIdx and start its scan over.
	ESlotClaim TryClaimSlotForRead(uint32_t absIdx);

	// Claims (READY->CONSUMING) the NEWEST current-generation frame in the ring,
	// consume-dropping the stale-generation prefix and any older current-gen
	// frames on the way. Returns its absolute index, or -1 when there is none.
	// The caller owns the returned slot and must hand it to DisplayFrame().
	int64_t ClaimNewestCurrentGenerationFrame();

	bool DemuxAndDecodeNextPacket();
	// s16 -> float conversion + PushSamples for everything the source has
	// decoded and queued; shared by DemuxAndDecodeNextPacket() and the
	// ring-full audio pump-ahead (2026-07-18). DECODE THREAD ONLY.
	void DrainSourceAudioToChannel();
	void StoreDecodedFrame(const SDecodedVideoFrame &frame);

	// Plan-2 Task 3b: the "show this already-stored ring-buffer frame" half
	// of Update()'s Playing path, factored out so SeekPausedAndDecodeFrame()
	// can reuse it verbatim instead of duplicating the CPU/legacy-GPU/RGBA
	// display branching. Handles GPU texture (re)creation on a dimension
	// change, uploads/renders per enableGPUUpload/outputRGBATexture,
	// marks the frame consumed, and advances
	// frameReadIdx past it -- exactly what Update() did inline before this
	// task. `frameIdx` is the absolute (pre-modulo) ring-buffer index of the
	// frame to display; precondition: the slot at that index must already
	// be CONSUMING (claimed by the caller via a READY->CONSUMING CAS).
	// DisplayFrame() hands the frame's planes over to displaySnapshot (see
	// above) and releases the claim (CONSUMING->EMPTY) before returning.
	void DisplayFrame(uint32_t frameIdx);

	void CreateGPUTextures(int width, int height);
	void UploadYUVToGPU(DecodedFrame &frame);
	void DestroyGPUTextures();

	// Task 10: uploads `frame`'s planes (whichever layout StoreDecodedFrame
	// tagged it with -- YUV420P/YUVA420P/NV12/YUV420P10/YUV422P/YUV422P10)
	// into rgbaTexY/U/V/A, (re)creating them when size/mode/alpha changes,
	// then renders through rgbaShader into rgbaTarget (sized to display dims).
	void RenderFrameToRGBATexture(DecodedFrame &frame);
	void DestroyRGBATextures();

	void ConvertYUV420ToRGBA(const DecodedFrame &frame, u8 *outRGBA);
	// Shared decimating core: converts the outW x outH grid of pixels at
	// (i*stepX, j*stepY). ConvertYUV420ToRGBA() is the step=1 case; the
	// sampled histogram tap passes the budget-derived step. Single
	// implementation so every pixel-format branch (10-bit, NV12, 4:2:2,
	// alpha, color matrix/range) stays in exactly one place.
	void ConvertYUV420ToRGBASampled(const DecodedFrame &frame, u8 *outRGBA,
	                                int stepX, int stepY, int outW, int outH);

	void FreeResources();
	void ClearRingBuffer();

	double GetWallTime() const;
};

#endif
//_CVIDEOPLAYER_H_
