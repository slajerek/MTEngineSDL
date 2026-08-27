#include "CVideoPlayer.h"
#include "CVideoTransferFunctions.h"
#include "MT_SrgbCurve.h"   // SrgbExtendedEncode -- one definition, shared with the poster lane
#include "Core/Render/VID_Main.h"
#include "Core/Render/CRenderBackend.h"
#include "CVideoAudioChannel.h"
#include "CVideoSourceWebMVpx.h"
#if MT_ENABLE_FFMPEG
#include "CVideoSourceFFmpeg.h"
#endif
#include "SYS_Threading.h"
#include "DBG_Log.h"
#include "SND_SoundEngine.h"
#include "SND_Main.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>
#include <vector>

// ============================================================================
// VideoColor_NormalizeMatrix (CM-E) -- see the contract in IVideoSource.h.
// FFmpeg AVColorSpace input ONLY; output is the engine's VPX_CS_* convention.
// ============================================================================
int VideoColor_NormalizeMatrix(int avcolSpc, int width, int height)
{
	switch (avcolSpc)
	{
	case 1:             // AVCOL_SPC_BT709
	case 7:             // AVCOL_SPC_SMPTE240M -- coefficients are the 709 family
		return 2;       // VPX_CS_BT_709
	case 0:             // AVCOL_SPC_RGB -- sws fallback encodes these as 601
	case 5:             // AVCOL_SPC_BT470BG (601-625)
	case 6:             // AVCOL_SPC_SMPTE170M (601-525)
		return 1;       // VPX_CS_BT_601
	case 9:             // AVCOL_SPC_BT2020_NCL
	case 10:            // AVCOL_SPC_BT2020_CL (approximated by ncl)
		return 5;       // VPX_CS_BT_2020
	default:            // AVCOL_SPC_UNSPECIFIED(2) and anything unmapped:
		// the industry resolution heuristic -- HD content is 709, SD is 601.
		return (height >= 720 || width >= 1280) ? 2 : 1;
	}
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CVideoPlayer::CVideoPlayer()
{
	mutex = new CSlrMutex("CVideoPlayer");
	// DecodedFrame members have default initializers (nullptr, 0, false) — no memset needed
}

CVideoPlayer::~CVideoPlayer()
{
	Close();

	if (mutex)
	{
		delete mutex;
		mutex = nullptr;
	}
}

// ============================================================================
// CreateAndOpenSource
// ============================================================================


IVideoSource *CVideoPlayer::CreateAndOpenSource(const char *filePath, std::string &outErrorReason)
{
	IVideoSource *src;
	// Same routing Open() has always done: the legacy nestegg/vpx source
	// only for alpha-channel VP9 webm (the only path decoding the alpha
	// plane), FFmpeg for everything else, legacy-only without FFmpeg.
#if MT_ENABLE_FFMPEG
	if (CVideoSourceFFmpeg::ProbeIsAlphaVP9WebM(filePath))
		src = new CVideoSourceWebMVpx();
	else
		src = new CVideoSourceFFmpeg();
#else
	src = new CVideoSourceWebMVpx();
#endif
	if (!src->Open(filePath))
	{
		// Copy the reason unconditionally (even if empty) -- behavior-
		// preserving vs the pre-refactor inline block, which never invented
		// a fallback reason. Downstream consumers supply their own
		// user-facing fallback for empty reasons.
		outErrorReason = src->GetErrorReason();
		delete src;
		return nullptr;
	}
	return src;
}

void CVideoPlayer::SetPreopenedSource(IVideoSource *src)
{
	preopenedSource = src;
}

// ============================================================================
// Open
// ============================================================================
bool CVideoPlayer::Open(const char *filePath)
{
	LOGD("CVideoPlayer::Open: %s", filePath);

	// Test-only seam (Plan-2 Task 2): consume the injected source BEFORE
	// Close() below -- Close()/FreeResources() treat a still-pending injected
	// source as abandoned and delete it (leak guard), so grabbing it first is
	// what keeps it alive across the Close(). One-shot -- cleared here so a
	// later Open() without a fresh injection falls back to normal construction.
	IVideoSource *injectedSource = testVideoSource;
	testVideoSource = nullptr;

	// Async-open seam (Task 6): same reasoning -- consume BEFORE Close() so a
	// still-pending preopened source survives the Close() below instead of
	// being treated as abandoned by FreeResources()'s leak guard.
	IVideoSource *preopened = preopenedSource;
	preopenedSource = nullptr;

	// Close any previously opened file
	Close();
	errorReason.clear();

	bool sourceAlreadyOpen = false;
	if (injectedSource)
	{
		source = injectedSource;           // test seam: Open() still called below
	}
	else if (preopened)
	{
		source = preopened;                // async seam: already opened off-thread
		sourceAlreadyOpen = true;
	}
	else
	{
		std::string routeErr;
		source = CreateAndOpenSource(filePath, routeErr);
		if (!source)
		{
			errorReason = routeErr;
			LOGError("CVideoPlayer::Open: failed to open '%s': %s", filePath, errorReason.c_str());
			state = EVideoPlayerState::Error;
			return false;
		}
		sourceAlreadyOpen = true;
	}

	if (!sourceAlreadyOpen && !source->Open(filePath))
	{
		// Copy the reason BEFORE deleting the source (Plan-2 Task 2) so a
		// UI-facing caller (Task 10's controller) can show it via
		// GetErrorReason() after this call returns.
		SetErrorReason(source->GetErrorReason());
		LOGError("CVideoPlayer::Open: failed to open '%s': %s", filePath, errorReason.c_str());
		delete source;
		source = nullptr;
		state = EVideoPlayerState::Error;
		return false;
	}

	// ABORT PLUMBING (Task 5, spec #2.3). Installed here -- before
	// StartDecodeThread() below, so the source is never polled by a decode thread
	// that does not yet have a predicate, and never mutated afterwards. The
	// predicate reads two atomics and nothing else (see ShouldAbortDecode()); the
	// capture is `this`, and the source is destroyed by FreeResources() strictly
	// after the decode thread has been joined, so it can never outlive the player.
	// The source forwards it on to its packet decoder, if it has one.
	//
	// This is AFTER the open on every path (constructed, injected, preopened), so
	// the OPEN itself is not covered by it -- deliberately: the source cannot be
	// opened any earlier than the player exists on the constructed path, the
	// preopened source is opened on a worker thread that no player owns yet, and
	// this player's shutdown/generation state is not even reset to its
	// fresh-clip values until below (shouldStop is still TRUE here, left raised by
	// the Close() above -- a predicate installed before the open would abort it
	// instantly). Interruptible OPEN is a separate problem, solved upstream by
	// PhotoCruise's async open + dataless-file detection.
	source->SetAbortPredicate([this] { return ShouldAbortDecode(); });

	// Copy source metadata into our existing fields (public API unchanged)
	const SVideoInfo &info = source->Info();
	videoWidth = info.width;
	videoHeight = info.height;
	duration = info.duration;
	fps = (info.fps > 0.0) ? info.fps : 30.0;
	hasAlpha = info.hasAlpha;
	hasAudio = info.hasAudio;
	vpxColorSpace = info.colorSpace;
	vpxColorRange = info.fullRange ? 1 : 0;
	colorPrimaries = info.colorPrimaries;   // CM-E: raw AVCOL_PRI_* (2 = unspecified)
	colorTrc = info.colorTrc;               // CM-E: raw AVCOL_TRC_* (16 PQ / 18 HLG = HDR)
	rotationDegrees = info.rotationDegrees;

	if (hasAudio)
	{
		// Create audio channel for the sound engine
		audioChannel = new CVideoAudioChannel();
	}

	// Allocate frame ring buffer planes
	AllocateFrameBuffers();

	// Index-rewinding ring reset is legal ONLY here and on the Close()/
	// FreeResources() path (Task 2 rule 2): no decode thread and no reader can
	// be looking at the ring at either point. Seek()/Stop() must never do it.
	ClearRingBuffer();

	state = EVideoPlayerState::Idle;
	currentTime = 0.0;

	// Reset the command mailbox for the new clip (Close() above joined any
	// previous decode thread, so nobody else can be looking at these).
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		seekTargetPending = -1.0;
		seekIsBurst = false;
		lastSeekRequestWallTime = 0.0;   // no previous request on a fresh clip -- the
										 // first seek must not read as a burst member
		seekServicedGeneration = 0;
		stopCommandPending = false;
		stopCommandDone = false;
		refinePending = false;
		refineTarget = -1.0;
		refineGeneration = 0;
		refinePublishCounter.store(0, std::memory_order_relaxed);
		debugFastSeeksServiced.store(0, std::memory_order_relaxed);
		debugPreciseSeeksServiced.store(0, std::memory_order_relaxed);
		seekGeneration.store(0, std::memory_order_relaxed);
		decodeGeneration = 0;
		// Must be reset in lockstep with seekGeneration, or the abort predicate
		// would read "0 != <previous clip's last generation>" and condemn every
		// decode this fresh clip attempts.
		servicingSeekGeneration.store(0, std::memory_order_relaxed);
		pendingSeekTargetAtomic.store(-1.0, std::memory_order_relaxed);
		completedSeekGeneration.store(0, std::memory_order_relaxed);
		endOfStream.store(false, std::memory_order_relaxed);
	}
	// Reader-side counterpart of the generation reset (Task 3): a fresh clip has
	// displayed nothing yet, and seekGeneration is back at 0, so the not-Playing
	// delivery gate must start out "nothing outstanding" rather than inheriting
	// the previous clip's generation. Same for the refine mirror (Task 6).
	lastDisplayedGeneration = 0;
	lastDisplayedRefinePublish = 0;

	// Arm the first-frame clock anchor for this fresh clip (see the field's
	// header comment): the first frame shown while Playing re-bases the clock
	// onto its pts, so a cold autoplay start lands on the clip's opening frame
	// instead of wherever the free-running clock has raced during the decoder's
	// reorder-window fill. Any explicit reposition before then clears it.
	anchorClockToFirstFrame.store(true, std::memory_order_release);

	// Task 2: the decode thread now lives for as long as the clip does. It
	// parks on cmdCv until it is told to play or handed a seek command -- so
	// Seek()/Stop() never have to join (and restart) it any more.
	StartDecodeThread();

	LOGD("CVideoPlayer::Open: successfully opened '%s'", filePath);
	return true;
}

// ============================================================================
// AllocateFrameBuffers
// ============================================================================
void CVideoPlayer::AllocateFrameBuffers()
{
	// Use ceiling division for UV planes to handle odd dimensions correctly
	int uvWidth = (videoWidth + 1) / 2;
	int uvHeight = (videoHeight + 1) / 2;
	int ySize = videoWidth * videoHeight;
	int uvSize = uvWidth * uvHeight;

	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		frameBuffer[i].yPlane = new u8[ySize];
		frameBuffer[i].uPlane = new u8[uvSize];
		frameBuffer[i].vPlane = new u8[uvSize];
		frameBuffer[i].yCapacity = ySize;
		frameBuffer[i].uCapacity = uvSize;
		frameBuffer[i].vCapacity = uvSize;
		frameBuffer[i].yStride = videoWidth;
		frameBuffer[i].uStride = uvWidth;
		frameBuffer[i].vStride = uvWidth;
		frameBuffer[i].width = videoWidth;
		frameBuffer[i].height = videoHeight;
		frameBuffer[i].allocWidth = videoWidth;
		frameBuffer[i].allocHeight = videoHeight;
		frameBuffer[i].stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_relaxed);
		frameBuffer[i].generation = 0;
		frameBuffer[i].absIdx = 0;
		frameBuffer[i].hasAlpha = false;
		frameBuffer[i].aPlane = nullptr;
		frameBuffer[i].aStride = 0;
		frameBuffer[i].aCapacity = 0;
		frameBuffer[i].pixelFormat = EVideoPixelFormat::YUV420P;
		frameBuffer[i].bytesPerSample = 1;
		frameBuffer[i].chromaFullHeight = false;
		frameBuffer[i].chromaInterleaved = false;

		if (hasAlpha)
		{
			frameBuffer[i].aPlane = new u8[ySize];
			frameBuffer[i].aStride = videoWidth;
			frameBuffer[i].aCapacity = ySize;
		}
	}
}

// ============================================================================
// Play / Pause / Stop / Close
// ============================================================================
void CVideoPlayer::Play()
{
	LOGD("CVideoPlayer::Play");

	if (state == EVideoPlayerState::Error)
		return;

	if (!source)
	{
		LOGError("CVideoPlayer::Play: no file opened");
		return;
	}

	// Record wall-clock start time for A/V sync (clock triplet: under `mutex`).
	mutex->Lock();
	playbackStartWallTime = GetWallTime();
	playbackStartOffset = currentTime;
	mutex->Unlock();

	// Fire-and-forget command: flip the (atomic) state and wake the decode
	// thread. The store happens UNDER cmdMutex so it cannot slip in between the
	// decode thread evaluating its wait predicate and going to sleep (a lost
	// wakeup would park the thread with the player nominally Playing).
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		state = EVideoPlayerState::Playing;
	}
	cmdCv.notify_all();
}

void CVideoPlayer::Pause()
{
	LOGD("CVideoPlayer::Pause");

	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		if (state == EVideoPlayerState::Playing)
		{
			state = EVideoPlayerState::Paused;
		}
	}
	// Notify so the decode thread re-evaluates its predicate promptly (it will
	// simply park: nothing pending, not Playing).
	cmdCv.notify_all();
}

void CVideoPlayer::Stop()
{
	LOGD("CVideoPlayer::Stop");

	// Never-opened (or failed-Open) player: no decode thread to command. Land
	// the same Idle/currentTime==0 result the old inline body produced.
	if (!source || !threadRunning.load())
	{
		mutex->Lock();
		currentTime = 0.0;
		playbackStartOffset = 0.0;
		playbackStartWallTime = GetWallTime();
		mutex->Unlock();
		state = EVideoPlayerState::Idle;
		return;
	}

	// Stop is a decode-thread command: "seek to 0, then land Idle". The decode
	// thread owns the source, so rewinding it here (as the old body did, after
	// joining the thread) is no longer allowed.
	//
	// The stop is BOUND to the generation it is submitted with: a Seek() that
	// lands in the mailbox before the decode thread consumes it wins (latest-wins)
	// and the stop is superseded rather than silently re-targeted at the seek's
	// position (which would satisfy this waiter at the wrong time and in the wrong
	// state). A superseded stop still releases this waiter -- see DecodeThreadFunc.
	{
		std::unique_lock<std::mutex> lk(cmdMutex);
		stopCommandDone = false;
		stopCommandPending = true;
		stopCommandGeneration = SubmitSeekCommandLocked(0.0, ESeekMode::Precise);
		serviceCv.wait(lk, [this] { return stopCommandDone || !threadRunning.load(); });
	}
}

void CVideoPlayer::Close()
{
	LOGD("CVideoPlayer::Close");
	StopDecodeThread();
	FreeResources();
	state = EVideoPlayerState::Idle;
}

// ============================================================================
// Seek
// ============================================================================
void CVideoPlayer::Seek(double timeSeconds)
{
	LOGD("CVideoPlayer::Seek: %.3f sec", timeSeconds);

	if (!source)
		return;

	// Clamp to valid range
	if (timeSeconds < 0.0)
		timeSeconds = 0.0;
	if (duration > 0.0 && timeSeconds > duration)
		timeSeconds = duration;

	// Never-started decode thread (shouldn't happen for an opened player, but a
	// failed/degraded Open() path must not wedge the caller): nothing to command.
	if (!threadRunning.load())
	{
		mutex->Lock();
		currentTime = timeSeconds;
		playbackStartOffset = timeSeconds;
		playbackStartWallTime = GetWallTime();
		mutex->Unlock();
		return;
	}

	// Task 3b: a Paused seek must land back on Paused, not degrade to Idle --
	// otherwise paused scrubbing (drag the seek bar while stopped) silently
	// resumes playback semantics (Idle reads as "never played") and a
	// second Seek() while still dragging loses the wasPlaying=false branch's
	// only way to tell "was actually paused" from "was never played".
	bool wasPlaying = (state == EVideoPlayerState::Playing);
	bool wasPaused = (state == EVideoPlayerState::Paused);

	// Hand the seek to the decode thread and block until it has serviced it
	// (Task 2). The thread keeps running throughout -- it owns the source, the
	// resampler and the ring buffer; nothing here touches them.
	uint64_t gen;
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		gen = SubmitSeekCommandLocked(timeSeconds, ESeekMode::Precise);
	}
	WaitForSeekServiced(gen);

	// State restoration. A Playing player simply KEEPS playing -- the decode
	// thread never stopped, it just refills from the new position (no restart).
	// Paused stays Paused. Any other pre-seek state (Idle/Finished) lands on
	// Idle -- Seek() on a never-played or finished player is not "resuming"
	// anything (and the Finished latch must not survive the seek).
	if (!wasPlaying && !wasPaused)
	{
		state = EVideoPlayerState::Idle;
	}
}

// ============================================================================
// Non-blocking seek request API (Task 3)
//
// Same mailbox as Seek()/Stop()/SeekPausedAndDecodeFrame() -- these just skip
// the WaitForSeekServiced() half. The decode thread does the source seek, the
// audio reset, the clock re-base and (when not Playing) the single-frame decode;
// Update() picks the frame up and un-freezes the clock.
// ============================================================================
void CVideoPlayer::RequestSeek(double seconds, ESeekMode mode)
{
	if (!source || state.load() == EVideoPlayerState::Error)
		return;

	LOGD("CVideoPlayer::RequestSeek: %.3f sec (mode=%d)", seconds, (int)mode);

	// Degraded player (Open() never got the decode thread up): nobody will ever
	// service the mailbox, so submitting would leave GetPendingSeekTarget()
	// stuck >= 0 forever and Update()'s clock frozen for good. Do what Seek()'s
	// equivalent branch does -- move the clock and be done.
	if (!threadRunning.load())
	{
		if (seconds < 0.0) seconds = 0.0;
		if (duration > 0.0 && seconds > duration) seconds = duration;
		mutex->Lock();
		currentTime = seconds;
		playbackStartOffset = seconds;
		playbackStartWallTime = GetWallTime();
		mutex->Unlock();
		return;
	}

	std::lock_guard<std::mutex> lk(cmdMutex);
	SubmitSeekCommandLocked(seconds, mode);   // clamps, bumps the generation, notifies
}

ESeekRequestOutcome CVideoPlayer::RequestSeekRelative(double deltaSeconds, ESeekMode mode)
{
	if (!source || state.load() == EVideoPlayerState::Error)
		return ESeekRequestOutcome::InRange;

	if (!threadRunning.load())
	{
		// Degraded player: no mailbox to accumulate against. Fall back to the
		// clock, which RequestSeek() then moves directly.
		double raw = GetCurrentTime() + deltaSeconds;
		ESeekRequestOutcome outcome = ESeekRequestOutcome::InRange;
		if (duration > 0.0 && raw > duration) outcome = ESeekRequestOutcome::PastEnd;
		else if (raw < 0.0)                   outcome = ESeekRequestOutcome::BeforeStart;
		RequestSeek(raw, mode);
		return outcome;
	}

	// The base and the submit MUST be one atomic step under cmdMutex, or a burst
	// of held-key steps does not sum: read the base outside the lock and a
	// concurrent submit (or the decode thread completing one) can move the world
	// between the read and our submit, and the step accumulates off the wrong
	// place.
	//
	// LOCK ORDER (see the cmdMutex block in the header): cmdMutex OUTER, the clock
	// `mutex` INNER. GetCurrentTime() below takes `mutex` while we hold cmdMutex --
	// that is the declared order. Nothing anywhere takes cmdMutex while holding
	// `mutex` (the decode thread's re-base and Update()'s recompute both take
	// `mutex` alone), so there is no cycle.
	std::lock_guard<std::mutex> lk(cmdMutex);

	// Base = the OUTSTANDING target if there is one, else the displayed-frame
	// clock. Note this reads pendingSeekTargetAtomic, NOT the seekTargetPending
	// mailbox slot: the mailbox is cleared the instant the decode thread CONSUMES
	// the command, but the seek is not done until it has been SERVICED (the clock
	// is re-based at the very end). A step landing in that window would otherwise
	// accumulate off the stale pre-seek clock and the burst would lose a step.
	// pendingSeekTargetAtomic stays >= 0 across that whole window -- and it is
	// exactly what GetPendingSeekTarget() reports, so the API is self-consistent.
	double pending = pendingSeekTargetAtomic.load(std::memory_order_acquire);
	double base = (pending >= 0.0) ? pending : GetCurrentTime();

	double raw = base + deltaSeconds;

	ESeekRequestOutcome outcome = ESeekRequestOutcome::InRange;
	if (duration > 0.0 && raw > duration)
		outcome = ESeekRequestOutcome::PastEnd;
	else if (raw < 0.0)
		outcome = ESeekRequestOutcome::BeforeStart;

	LOGD("CVideoPlayer::RequestSeekRelative: %+.3f sec from base %.3f -> %.3f", deltaSeconds, base, raw);

	SubmitSeekCommandLocked(raw, mode);   // stores the CLAMPED value
	return outcome;
}

double CVideoPlayer::GetPendingSeekTarget() const
{
	return pendingSeekTargetAtomic.load(std::memory_order_acquire);
}

// ============================================================================
// SeekPausedAndDecodeFrame (Plan-2 Task 3b)
// ============================================================================
bool CVideoPlayer::SeekPausedAndDecodeFrame(double seconds)
{
	// Mirror Play()'s Error-state guard (Task 2): never touch state or the
	// ring buffer once the player has entered EVideoPlayerState::Error.
	if (state == EVideoPlayerState::Error)
		return false;

	if (!source)
		return false;

	LOGD("CVideoPlayer::SeekPausedAndDecodeFrame: %.3f sec", seconds);

	// Clamp to valid range (same clamp Seek() applies; IVideoSource::Seek()
	// re-clamps internally too, but this keeps the two entry points
	// symmetric).
	if (seconds < 0.0)
		seconds = 0.0;
	if (duration > 0.0 && seconds > duration)
		seconds = duration;

	if (!threadRunning.load())
		return false;

	// Snapshot the clock so a failed seek (decode failure/EOS) can put it back
	// exactly as it was -- the documented "not wedged" contract. The decode
	// thread re-bases the clock onto the seek target as part of servicing, so
	// this is now an explicit undo rather than a "never touched it".
	mutex->Lock();
	double priorTime = currentTime;
	double priorStartOffset = playbackStartOffset;
	double priorStartWallTime = playbackStartWallTime;
	mutex->Unlock();

	// A PLAYING player is handled explicitly (Task 2 finding: the rewrite lost
	// this branch). ServiceSeekOnDecodeThread() only publishes its single frame
	// while the player is NOT Playing -- called on a Playing player it would seek
	// the source and reset the audio queue but publish nothing, so the claim below
	// would fail and we would restore a clock that no longer matches the decoder's
	// read head. Park the player in Paused for the duration of the seek (the
	// pre-Task-2 body achieved exactly this by stopping the decode thread), then
	// restore Playing at the end -- from the re-based clock, so playback continues
	// from the frame we just displayed.
	bool wasPlaying = false;
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		if (state == EVideoPlayerState::Playing)
		{
			state = EVideoPlayerState::Paused;
			wasPlaying = true;
		}
	}

	// Submit + block (Task 2): the seek, the source rewind, the audio-queue
	// drop and the single-frame decode all happen on the decode thread. While
	// the player is not Playing, servicing publishes exactly one frame for us.
	uint64_t gen;
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		gen = SubmitSeekCommandLocked(seconds, ESeekMode::Precise);
	}

	if (!WaitForSeekServiced(gen))
	{
		// Released by the decode thread's shutdown (Close() raced this call), NOT
		// by a real service. FreeResources() is deleting the ring's planes right
		// now -- claiming a slot or reading a frame here would be a use-after-free.
		// Touch nothing (not even the state, which Close() is setting to Idle) and
		// report failure.
		LOGD("CVideoPlayer::SeekPausedAndDecodeFrame: released by shutdown, abandoning seek to %.3f sec", seconds);
		return false;
	}

	// The display step still runs HERE, on the caller thread (spec
	// Compatibility): GetCurrentFrameRGBA()/GetRGBATexture()/the plane textures
	// must reflect the new frame the moment this call returns, and the GL work
	// has to happen on the thread that owns the context.
	int64_t claimed = ClaimNewestCurrentGenerationFrame();
	if (claimed < 0)
	{
		// Decode failure or EOS (e.g. seeking at/past duration): nothing was
		// published for this generation. Restore the clock and the pre-call state
		// -- a fresh Seek()+Play() afterward still works.
		LOGD("CVideoPlayer::SeekPausedAndDecodeFrame: no frame published for %.3f sec", seconds);
		mutex->Lock();
		currentTime = priorTime;
		playbackStartOffset = priorStartOffset;
		playbackStartWallTime = priorStartWallTime;
		mutex->Unlock();

		if (wasPlaying)
		{
			{
				std::lock_guard<std::mutex> lk(cmdMutex);
				// Only re-arm Playing if nothing else moved the state meanwhile
				// (an Error the decode thread raised must not be overwritten).
				if (state == EVideoPlayerState::Paused)
					state = EVideoPlayerState::Playing;
			}
			cmdCv.notify_all();
		}
		return false;
	}

	// On success the clock reflects the DELIVERED frame's pts (not the request):
	// frame-step callers step by exactly one frame off GetCurrentTime().
	double displayedPts = frameBuffer[(uint32_t)claimed % VIDEO_BUFFER_FRAMES].pts;

	DisplayFrame((uint32_t)claimed);

	mutex->Lock();
	currentTime = displayedPts;
	playbackStartOffset = currentTime;
	playbackStartWallTime = GetWallTime();
	mutex->Unlock();

	// Re-base done: resuming from here makes Update()'s A/V-sync advance start at
	// the displayed frame's pts, exactly like Play() does after a Seek().
	if (wasPlaying)
	{
		{
			std::lock_guard<std::mutex> lk(cmdMutex);
			if (state == EVideoPlayerState::Paused)
				state = EVideoPlayerState::Playing;
		}
		cmdCv.notify_all();
	}

	return true;
}

// ============================================================================
// TryClaimSlotForRead -- the ONE place a reader takes ownership of a slot.
//
// The CAS alone is not enough to identify what was claimed (Task 2 finding:
// ring-index ABA). A reader loads readIdx = frameReadIdx and only then CASes the
// physical slot readIdx % N; in between, the writer's tail reclamation can free
// that very slot (it holds a stale-generation frame) and -- because a FULL window
// means writeIdx % N == readIdx % N -- refill the SAME physical slot as
// readIdx + N, a full plane memcpy's worth of preemption window. The reader's CAS
// then succeeds on a slot that no longer holds the frame it asked for, and the
// generation stamp cannot tell the two apart (the refilled frame is current-gen
// too). Displaying it stores frameReadIdx = readIdx + 1 while the slot backing
// readIdx + N is now EMPTY -- the scan hits that hole and never advances again,
// the writer sees a full window with a non-READY tail and drops every frame from
// then on: the ring is wedged for good.
//
// So the writer stamps each slot with its ABSOLUTE index (under WRITING
// ownership, published by the release-store of READY) and every claim re-verifies
// it. On a mismatch the slot is handed straight back and the caller rescans from
// a freshly loaded frameReadIdx -- which the writer has by then already advanced,
// so the rescan makes progress.
// ============================================================================
CVideoPlayer::ESlotClaim CVideoPlayer::TryClaimSlotForRead(uint32_t absIdx)
{
	DecodedFrame &frame = frameBuffer[absIdx % VIDEO_BUFFER_FRAMES];

	uint32_t expected = DecodedFrame::SLOT_READY;
	if (!frame.stateWord.compare_exchange_strong(expected, DecodedFrame::SLOT_CONSUMING,
												 std::memory_order_acquire))
		return ESlotClaim::NotReady;   // not published yet (or the writer owns it)

	if (frame.absIdx != absIdx)
	{
		// Reclaimed + refilled under us: this is somebody else's frame (a LATER
		// one -- absIdx only ever grows). Hand the claim back untouched.
		LOGD("CVideoPlayer::TryClaimSlotForRead: slot recycled under the claim (wanted %u, holds %u)",
			 absIdx, frame.absIdx);
		frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
		return ESlotClaim::Recycled;
	}

	return ESlotClaim::Claimed;
}

// ============================================================================
// ClaimNewestCurrentGenerationFrame
// Reader-side scan shared by SeekPausedAndDecodeFrame() (and, in spirit, by
// Update()): consume-drop everything stale, keep the newest current-generation
// frame CONSUMING-claimed for the caller.
// ============================================================================
int64_t CVideoPlayer::ClaimNewestCurrentGenerationFrame()
{
	for (;;)
	{
		uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);
		uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
		const uint64_t curGen = seekGeneration.load(std::memory_order_acquire);

		int64_t bestFrameIdx = -1;
		bool rescan = false;

		while (readIdx != writeIdx)
		{
			ESlotClaim claim = TryClaimSlotForRead(readIdx);
			if (claim == ESlotClaim::NotReady)
				break;

			if (claim == ESlotClaim::Recycled)
			{
				// Only possible while we hold NO candidate: a held candidate sits
				// exactly on the tail (frameReadIdx == bestFrameIdx) and is
				// CONSUMING, so the writer's reclamation CAS on it must fail. Bail
				// out and rescan from the writer-advanced frameReadIdx.
				if (bestFrameIdx >= 0)
					break;
				rescan = true;
				break;
			}

			DecodedFrame &frame = frameBuffer[readIdx % VIDEO_BUFFER_FRAMES];

			if (frame.generation < curGen)
			{
				// STALE (older than the newest seek anybody asked for): consume-drop.
				// Stale frames are always a strict PREFIX of the ring (generations
				// are stamped in non-decreasing order), so this cannot follow a
				// candidate -- but never risk rewinding frameReadIdx if it ever did:
				// hand the claim back and stop.
				if (bestFrameIdx >= 0)
				{
					frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
					break;
				}
				frame.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
				frameReadIdx.store(readIdx + 1, std::memory_order_release);
				readIdx++;
				continue;
			}

			if (frame.generation > curGen)
			{
				// NEWER than our snapshot of seekGeneration: a seek was submitted
				// (and already serviced!) after we read curGen. This frame belongs
				// to that seek -- it must NOT be consume-dropped. Leave it READY for
				// the next scan, which will read the newer generation.
				frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
				break;
			}

			if (bestFrameIdx >= 0)
			{
				// Superseded by a newer current-generation frame: consume-drop it.
				DecodedFrame &prev = frameBuffer[(uint32_t)bestFrameIdx % VIDEO_BUFFER_FRAMES];
				prev.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
				frameReadIdx.store((uint32_t)bestFrameIdx + 1, std::memory_order_release);
			}
			bestFrameIdx = (int64_t)readIdx;   // stays CONSUMING (we hold the claim)
			readIdx++;
		}

		if (rescan)
			continue;

		return bestFrameIdx;
	}
}

// ============================================================================
// Update (main thread)
// ============================================================================
bool CVideoPlayer::Update(float deltaTime)
{
	// A seek is OUTSTANDING while the newest generation anybody asked for has not
	// been completed by the decode thread. Two things hang off this (Task 3):
	// the clock freeze below, and the not-Playing delivery path right here.
	//
	// ONE load of seekGeneration, reused by both (Task 4). Reading it twice let a
	// submit landing between the two reads compute `seekPending` against
	// generation N (false: N was completed) while the display gate below saw
	// N+1 -- and the `claimed < 0` branch would then take the !seekPending path
	// and close the gate on a generation whose frame is still on its way, so a
	// paused seeked frame would never be displayed. Unreachable today (every
	// submit path runs on this same render thread), but it is a landmine.
	const uint64_t curGen = seekGeneration.load(std::memory_order_acquire);
	const bool seekPending =
		curGen != completedSeekGeneration.load(std::memory_order_acquire);

	if (state != EVideoPlayerState::Playing)
	{
		// --- Not-Playing delivery (Task 3) ---------------------------------
		// Before Task 3 this was a bare `return false`, so a frame the decode
		// thread published for an async seek issued while Paused/Idle/Finished
		// (scrubbing, arrow-key stepping -- the whole reason RequestSeek()
		// exists) was decoded and then never shown.
		//
		// Gate: only while the newest seek generation has not been DISPLAYED
		// yet. `seekPending` alone is not enough -- it goes false as soon as the
		// decode thread completes, and the frame is claimed on a LATER Update();
		// and it is too much -- without the lastDisplayedGeneration half, a
		// merely Paused player would keep claiming the buffered future frames
		// still sitting in the ring and the picture would creep forward after
		// every Pause().
		if (state == EVideoPlayerState::Error || !source)
			return false;

		const uint64_t newestGen = curGen;   // same load as `seekPending` -- see above

		// The SECOND way this gate opens (Task 6): a quiet-refine has published a
		// corrected frame for the generation we already displayed. The refine does
		// not bump the seek generation (deliberately -- see the refine* fields in
		// the header), so `newestGen == lastDisplayedGeneration` still holds and the
		// test above would slam the door on the very frame the refine exists to
		// deliver: the paused scrub would keep showing the fast landing's keyframe.
		// Read BEFORE the claim, mirrored only once a frame has been displayed (or
		// proven unclaimable), so a refine landing mid-Update() is picked up by the
		// next one instead of being lost.
		const uint64_t refinePublish = refinePublishCounter.load(std::memory_order_acquire);
		if (!seekPending && newestGen == lastDisplayedGeneration &&
			refinePublish == lastDisplayedRefinePublish)
			return false;

		// Exactly ONE current-generation frame, no EOS/Finished transition and no
		// clock recompute: the decode thread already re-based the clock onto the
		// seek target, and there is no A/V sync to run while nothing is playing.
		int64_t claimed = ClaimNewestCurrentGenerationFrame();
		if (claimed < 0)
		{
			// Nothing to display for `newestGen`. If the seek is still pending,
			// leave the gate open -- the decode thread may still publish a frame
			// for it and a later Update() must retry. But if it is NOT pending
			// (completedSeekGeneration already caught up), no frame is EVER
			// coming for this generation: either it is a Stop() (whose
			// ServiceSeekOnDecodeThread call passes decodeOneFrameWhenNotPlaying=
			// false -- see DecodeThreadFunc -- so nothing is ever stored for a
			// stop-generation) or a genuine decode failure/EOS at the seek
			// target. Close the gate ourselves so the claim-scan above does not
			// re-run on every subsequent Update() forever (Finding 4) -- there is
			// nothing left in the ring for `newestGen` to ever claim.
			if (!seekPending)
			{
				lastDisplayedGeneration = newestGen;
				lastDisplayedRefinePublish = refinePublish;
			}
			return false;
		}

		DisplayFrame((uint32_t)claimed);   // sets lastDisplayedGeneration
		lastDisplayedRefinePublish = refinePublish;
		return true;
	}

	// Check if decode thread signaled EOF and ring buffer is fully drained. Gated
	// on !seekPending (Finding 1 of the Task 3 review): a seek in flight can leave
	// endOfStream latched true from BEFORE the seek was requested (the decode
	// thread hit EOS, then was asked to seek before Update() ever saw the ring
	// drain) -- transitioning to Finished on that stale latch would fire onFinished()
	// out from under an outstanding seek. ServiceSeekOnDecodeThread() always clears
	// endOfStream before it re-bases the clock and publishes completion, so by the
	// time seekPending goes false again endOfStream correctly reflects the POST-seek
	// position -- nothing is permanently suppressed, only deferred until the seek
	// that was in flight has actually landed.
	if (!seekPending && endOfStream.load(std::memory_order_acquire))
	{
		uint32_t ri = frameReadIdx.load(std::memory_order_acquire);
		uint32_t wi = frameWriteIdx.load(std::memory_order_acquire);
		if (ri == wi)
		{
			// All buffered frames consumed — transition to Finished
			LOGD("CVideoPlayer::Update: buffer drained after EOF, finishing");
			state = EVideoPlayerState::Finished;
			if (onFinished) onFinished();
			return false;
		}
	}

	// FIRST-FRAME CLOCK ANCHOR (cold-open start fix -- see the field's header).
	// Until the clip's opening frame has actually been shown, do NOT run the
	// free-running-clock catch-up scan below: that clock was based at Play(),
	// which fired before the decoder's reorder window had emitted anything, so it
	// has already raced past the opening frames' pts and the scan would consume-
	// drop them (the visible start-of-playback jump). Instead show the OLDEST
	// buffered current-generation frame -- the clip's first frame -- and re-base
	// the whole clock triplet onto its pts so playback proceeds in real time from
	// there. Disarmed on the first display here, or earlier by any explicit seek.
	//
	// This is scoped to the cold open: a fresh clip has no stale frames (seek
	// generation 0) and the writer never reclaims the tail without a stale frame
	// to evict, so the oldest current frame is simply the ring tail (frameReadIdx)
	// and a plain claim of it is race-free. A NotReady/Recycled result just means
	// "not published yet" -- return and retry next Update with the clock still
	// parked, so no time is lost.
	if (anchorClockToFirstFrame.load(std::memory_order_acquire))
	{
		uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);
		uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
		if (readIdx == writeIdx)
			return false;   // nothing buffered yet -- wait, clock stays at the start
		if (TryClaimSlotForRead(readIdx) != ESlotClaim::Claimed)
			return false;   // writer still publishing the tail -- retry next Update
		double firstPts = frameBuffer[readIdx % VIDEO_BUFFER_FRAMES].pts;
		DisplayFrame(readIdx);   // precondition met: slot is CONSUMING (claimed above)
		mutex->Lock();
		currentTime = firstPts;
		playbackStartOffset = firstPts;
		playbackStartWallTime = GetWallTime();
		mutex->Unlock();
		anchorClockToFirstFrame.store(false, std::memory_order_release);
		return true;
	}

	// Compute current playback time. The clock triplet (currentTime /
	// playbackStartOffset / playbackStartWallTime) is shared with the decode
	// thread now (a serviced seek re-bases it), so every read-modify-write goes
	// through `mutex` -- Task 2. Frame data itself stays lock-free.
	double clockNow;
	{
		mutex->Lock();
		if (seekPending)
		{
			// CLOCK FREEZE (Task 3, spec "Optimistic UI"). A seek is in flight:
			// hold currentTime where the last displayed frame put it. The audio
			// channel has been (or is about to be) Reset() by the decode thread and
			// the wall-clock base still points at the PRE-seek position, so
			// recomputing here would march the clock forward through a window in
			// which the picture cannot move -- the seek bar and the time label would
			// run away from the frame on screen, and then snap back when the seek
			// lands.
			//
			// ServiceSeekOnDecodeThread() re-bases the whole triplet (currentTime /
			// playbackStartOffset / playbackStartWallTime) from the target as it
			// completes, so the very next Update() after the freeze resumes from
			// THERE -- no catch-up jump, no lost time.
			clockNow = currentTime;
		}
		else if (hasAudio && audioChannel)
		{
			double audioPos = audioChannel->GetPlaybackPosition();
			if (audioPos > 0.001)
			{
				const double wallNow = GetWallTime();
				if (audioPos != audioClockLastPos)
				{
					// Audio is actually consuming -- authoritative A/V clock.
					// Monotonic guard: after a starvation free-run (below) the
					// audio clock lags the presented time by however long the
					// starvation lasted; adopting it verbatim would step
					// currentTime BACKWARD and replay frames already shown.
					audioClockLastPos = audioPos;
					audioClockLastAdvanceWall = wallNow;
					const double audioTime = playbackStartOffset + audioPos;
					if (audioTime >= currentTime)
					{
						// Caught up -- back on the pure audio clock.
						audioClockFreeRunWall = -1.0;
						currentTime = audioTime;
					}
					else if (audioClockFreeRunWall >= 0.0)
					{
						// Still behind the free-run point: KEEP wall-driving.
						// Exiting free-run on every micro-burst of late audio
						// re-arms the 0.25s stall threshold between bursts and
						// plays chronically-lazy interleave (real-world ASF) in
						// slow motion -- observed at ~0.7x on the nokia_n90
						// sample before this branch existed.
						currentTime += wallNow - audioClockFreeRunWall;
						audioClockFreeRunWall = wallNow;
					}
					// else: slightly behind but never free-ran -- hold
					// currentTime for a beat (ordinary mixer-quantum jitter).
				}
				else if (wallNow - audioClockLastAdvanceWall > kAudioClockStallFreeRunSec)
				{
					// AUDIO-STARVATION FREE-RUN (2026-07-18 WMV field report).
					// Consumption stopped: the audio track ran out mid-clip, or
					// the container interleaves audio lazily (real-world
					// ASF/WMV routinely lags audio behind video by more than
					// the video ring's depth). Freezing here is a DEADLOCK, not
					// sync: no frame qualifies for display, the full video ring
					// blocks the decode thread, so the audio that would unfreeze
					// the clock can never be demuxed. Advance at wall rate
					// instead -- display keeps consuming the ring, the decode
					// thread keeps demuxing, and late audio (if any is coming)
					// resumes on its own, re-adopted by the monotonic guard
					// above once it catches up. The 0.25s threshold keeps
					// ordinary mixer-quantum jitter on the pure audio clock.
					if (audioClockFreeRunWall < 0.0)
						audioClockFreeRunWall = wallNow;
					currentTime += wallNow - audioClockFreeRunWall;
					audioClockFreeRunWall = wallNow;
				}
				// else: consumption paused for less than the threshold --
				// hold currentTime (normal between-mixer-quanta gap).
			}
			else
			{
				// Audio not yet playing (e.g., not registered with SND mixer yet, or first frame)
				// Fall back to wall clock to avoid blocking video on audio startup
				double wallNow = GetWallTime();
				currentTime = playbackStartOffset + (wallNow - playbackStartWallTime);
				// Fresh audio epoch (clip start, or a seek's channel Reset()
				// dropped GetPlaybackPosition() back to 0): forget stall state.
				audioClockLastPos = -1.0;
				audioClockLastAdvanceWall = wallNow;
				audioClockFreeRunWall = -1.0;
			}
			clockNow = currentTime;
		}
		else
		{
			// Wall-clock fallback
			double wallNow = GetWallTime();
			currentTime = playbackStartOffset + (wallNow - playbackStartWallTime);
			clockNow = currentTime;
		}
		mutex->Unlock();
	}

	// Find the best frame to display: skip frames whose PTS < currentTime (catch up)
	// Use -1 sentinel via int64_t to avoid signed/unsigned issues with uint32_t.
	// The whole scan may have to restart: TryClaimSlotForRead() can report that the
	// writer reclaimed + refilled the slot we were about to claim (ring-index ABA;
	// see that function), in which case our readIdx is simply out of date.
	int64_t bestFrameIdx = -1;
	for (;;)
	{
		uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);
		uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);

		if (readIdx == writeIdx)
		{
			// No frames available
			return false;
		}

		// DELIBERATE RE-LOAD, per scan attempt -- NOT the function-scope `curGen`
		// snapshot taken at the top of Update() (which is pinned on purpose: the
		// clock-freeze and the not-Playing delivery gate must agree on one value).
		// Here the newest generation is exactly what we want: a seek that landed
		// while we were scanning makes every frame we were about to display stale,
		// and re-loading is what lets this attempt drop them. Named apart so the
		// two never get confused for one another.
		const uint64_t scanGen = seekGeneration.load(std::memory_order_acquire);
		bool rescan = false;
		bestFrameIdx = -1;

		while (readIdx != writeIdx)
		{
			ESlotClaim claim = TryClaimSlotForRead(readIdx);
			if (claim == ESlotClaim::NotReady)
				break;   // not published yet (or writer owns it)

			if (claim == ESlotClaim::Recycled)
			{
				// Only reachable while we hold NO candidate (a candidate is
				// CONSUMING *on the tail*, which blocks the writer's reclamation
				// CAS). Restart from the writer-advanced frameReadIdx.
				if (bestFrameIdx >= 0)
					break;
				rescan = true;
				break;
			}

			DecodedFrame &frame = frameBuffer[readIdx % VIDEO_BUFFER_FRAMES];

			// Stale-generation frame (spec #2.3 rule 1): consume-drop without
			// display. Strictly OLDER, not merely "different": a frame stamped with
			// a NEWER generation than our snapshot is a frame a concurrent seek has
			// legitimately published, and dropping it would destroy that seek's
			// result.
			if (frame.generation < scanGen)
			{
				// Stale frames are a strict PREFIX (generations are stamped in
				// non-decreasing order), so this branch can never run while a
				// candidate is held. Guard it anyway: storing readIdx + 1 here with
				// a candidate at bestFrameIdx < readIdx would let DisplayFrame()
				// rewind frameReadIdx afterwards and wedge the ring for good.
				if (bestFrameIdx >= 0)
				{
					frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
					break;
				}
				frame.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
				frameReadIdx.store(readIdx + 1, std::memory_order_release);
				readIdx++;
				continue;
			}

			if (frame.generation > scanGen)
			{
				// Published by a seek newer than our scanGen snapshot: leave it
				// READY (its own pts is meaningless against our stale clock -- the
				// next Update() re-reads both).
				frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
				break;
			}

			if (frame.pts <= clockNow)
			{
				// This frame is at or before current time -- it's a candidate
				if (bestFrameIdx >= 0)
				{
					// Previous candidate superseded: release it consumed-dropped.
					DecodedFrame &prev = frameBuffer[(uint32_t)bestFrameIdx % VIDEO_BUFFER_FRAMES];
					prev.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
					frameReadIdx.store((uint32_t)bestFrameIdx + 1, std::memory_order_release);
				}
				bestFrameIdx = (int64_t)readIdx;   // stays CONSUMING (we hold the claim)
				readIdx++;
			}
			else
			{
				// Future frame: hand the claim back (writer only reclaims stale
				// generations, so restoring READY cannot race a reclaim).
				frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);
				break;
			}
		}

		if (rescan)
			continue;

		break;
	}

	if (bestFrameIdx < 0)
		return false;

	DisplayFrame((uint32_t)bestFrameIdx);   // precondition: slot is CONSUMING (claimed)

	return true;
}

// ============================================================================
// DisplayFrame (Task 3b: factored out of Update() so
// SeekPausedAndDecodeFrame() can reuse the exact same display step)
// Precondition: the slot at frameIdx must already be CONSUMING (claimed by
// the caller via a READY->CONSUMING CAS). Releases the claim
// (CONSUMING->EMPTY) before returning.
// ============================================================================
void CVideoPlayer::DisplayFrame(uint32_t frameIdx)
{
	uint32_t bufIdx = frameIdx % VIDEO_BUFFER_FRAMES;
	DecodedFrame &displayFrame = frameBuffer[bufIdx];

	// Reader-owned: "the newest seek generation we have actually SHOWN". Update()'s
	// not-Playing path fires only while this trails seekGeneration (Task 3), so it
	// has to be recorded on EVERY display -- including the ones that go through
	// SeekPausedAndDecodeFrame(), whose frame must not then be delivered a second
	// time by the next Update().
	lastDisplayedGeneration = displayFrame.generation;

	// Update video dimensions from frame
	if (displayFrame.width != videoWidth || displayFrame.height != videoHeight)
	{
		videoWidth = displayFrame.width;
		videoHeight = displayFrame.height;
		if (enableGPUUpload) DestroyGPUTextures();
	}

	// Create GPU textures and upload (only when GPU upload is enabled).
	// Skipped entirely in RGBA output mode: the two paths are exclusive --
	// an RGBA-texture consumer never reads GetYTexture()/GetUTexture()/...
	// (it samples GetRGBATexture() instead), so running the legacy 3-plane
	// upload as well would just double the per-frame upload cost. Cutscene
	// callers (CGuiViewVideoPlayer) never enable RGBA mode, so their path is
	// untouched.
	if (enableGPUUpload && !outputRGBATexture)
	{
		if (!gpuTexturesCreated)
		{
			CreateGPUTextures(videoWidth, videoHeight);
		}
		UploadYUVToGPU(displayFrame);
	}

	// Render-to-texture RGBA output (Task 10) -- unlike the legacy path above
	// (which stays 8-bit-3-plane-only for existing Render() callers), this
	// one handles whatever pixelFormat StoreDecodedFrame tagged the frame
	// with, including NV12/10-bit.
	if (outputRGBATexture)
	{
		RenderFrameToRGBATexture(displayFrame);
	}

	// Hand the pixels over to the reader-owned snapshot BEFORE releasing the
	// claim (Task 2 finding a): GetCurrentFrameRGBA() used to convert the slot
	// at frameReadIdx - 1 with no claim at all, which the persistent decode
	// thread is free to overwrite mid-conversion. Swapping the plane pointers
	// (and their capacities) is O(1) -- the slot gets the snapshot's previous
	// buffers, which StoreDecodedFrame()'s EnsurePlaneCapacity() regrows if the
	// next frame needs more room. allocWidth/allocHeight stay with the SLOT:
	// they are its Open()-time resolution guard, not a property of the planes.
	std::swap(displaySnapshot.yPlane, displayFrame.yPlane);
	std::swap(displaySnapshot.uPlane, displayFrame.uPlane);
	std::swap(displaySnapshot.vPlane, displayFrame.vPlane);
	std::swap(displaySnapshot.aPlane, displayFrame.aPlane);
	std::swap(displaySnapshot.yCapacity, displayFrame.yCapacity);
	std::swap(displaySnapshot.uCapacity, displayFrame.uCapacity);
	std::swap(displaySnapshot.vCapacity, displayFrame.vCapacity);
	std::swap(displaySnapshot.aCapacity, displayFrame.aCapacity);
	displaySnapshot.yStride = displayFrame.yStride;
	displaySnapshot.uStride = displayFrame.uStride;
	displaySnapshot.vStride = displayFrame.vStride;
	displaySnapshot.aStride = displayFrame.aStride;
	displaySnapshot.width = displayFrame.width;
	displaySnapshot.height = displayFrame.height;
	displaySnapshot.pts = displayFrame.pts;
	displaySnapshot.hasAlpha = displayFrame.hasAlpha;
	displaySnapshot.pixelFormat = displayFrame.pixelFormat;
	displaySnapshot.bytesPerSample = displayFrame.bytesPerSample;
	displaySnapshot.chromaFullHeight = displayFrame.chromaFullHeight;
	displaySnapshot.chromaInterleaved = displayFrame.chromaInterleaved;
	displaySnapshotValid = true;

	// Release the claim (CONSUMING->EMPTY) and advance read index
	displayFrame.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
	frameReadIdx.store(frameIdx + 1, std::memory_order_release);
}

// ============================================================================
// Decode thread
// ============================================================================
void CVideoPlayer::StartDecodeThread()
{
	if (threadRunning.load())
		return;

	LOGD("CVideoPlayer::StartDecodeThread");
	shouldStop = false;
	{
		// Fresh thread: clear the shutdown latch a previous Open()/Close() cycle
		// left behind, or every wrapper would immediately report "released by
		// shutdown".
		std::lock_guard<std::mutex> lk(cmdMutex);
		decodeThreadExited = false;
	}
	threadRunning = true;
	decodeThread = std::thread(&CVideoPlayer::DecodeThreadFunc, this);
}

// The ONLY join (Task 2): called from Close()/the destructor, never from a
// seek/stop/play path any more. shouldStop is raised under cmdMutex so it
// cannot race the decode thread's wait-predicate evaluation.
void CVideoPlayer::StopDecodeThread()
{
	if (!threadRunning.load())
		return;

	LOGD("CVideoPlayer::StopDecodeThread");
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		shouldStop = true;
	}
	cmdCv.notify_all();

	// WAKE (Task 5), and in this ORDER: shouldStop -- the state the abort
	// predicate reads -- is published above, and only THEN is the source woken, so
	// a wait that was about to sleep re-evaluates the predicate and sees the
	// shutdown instead. Without this, join() below would block for as long as the
	// in-flight source->Seek()/DecodePacket() takes: hundreds of ms in FFmpeg, up
	// to TEN SECONDS in the Windows Media Foundation HEVC pipeline -- i.e. the
	// exact freeze this whole feature exists to kill, just relocated into
	// "navigate away from a video".
	//
	// OUTSIDE cmdMutex, deliberately: WakeAbort() reaches into the decoder's own
	// mutex/condvar, and taking that while holding cmdMutex would add a lock edge
	// we do not need here. (SubmitSeekCommandLocked() does call it under cmdMutex
	// -- it has no choice, the bump and the wake must not be split by a racing
	// submit -- which is safe because nothing in the decoder ever takes cmdMutex,
	// so cmdMutex -> decoderMutex is an edge with no back-edge.)
	if (source)
		source->WakeAbort();

	if (decodeThread.joinable())
	{
		decodeThread.join();
	}

	threadRunning = false;
}

// The abort predicate the source polls (installed in Open(); contract in
// IVideoSource.h). Two atomic loads, no locks -- see the header's note on why it
// must never take cmdMutex.
bool CVideoPlayer::ShouldAbortDecode() const
{
	if (shouldStop.load(std::memory_order_acquire))
		return true;

	// "The generation being serviced is no longer the newest one anybody asked
	// for." Also true in the window between a submit and the decode thread
	// picking the command up -- which is exactly right: a Playing refill decode
	// that is in flight when a seek lands should be cut short too, so the seek is
	// serviced sooner rather than a frame later.
	return seekGeneration.load(std::memory_order_acquire) !=
		   servicingSeekGeneration.load(std::memory_order_acquire);
}

// ============================================================================
// Seek mailbox (spec #2.3)
// ============================================================================
uint64_t CVideoPlayer::SubmitSeekCommandLocked(double target, ESeekMode mode)
{
	// Caller holds cmdMutex.
	//
	// Any explicit reposition (this is the single chokepoint for Seek/RequestSeek/
	// RequestSeekRelative/SeekPausedAndDecodeFrame/Stop) disarms the cold-open
	// first-frame anchor: from here on the clock is re-based by the seek service
	// onto the seek target, not onto the clip's opening frame.
	anchorClockToFirstFrame.store(false, std::memory_order_release);

	if (target < 0.0)
		target = 0.0;
	if (duration > 0.0 && target > duration)
		target = duration;

	seekTargetPending = target;
	seekModePending = mode;   // carried through the mailbox to the decode thread

	// A PRECISE seek is never part of a scrub burst: frame-step and every
	// blocking wrapper submit Precise. TwoPhase AND Coarse are scrub steps -- both
	// want the burst classification (TwoPhase to go fast, Coarse to hold the audio
	// gate across the burst rather than blip it per step).
	const bool isPrecise = (mode == ESeekMode::Precise);

	// BURST CLASSIFICATION (Task 6), decided here and carried through the mailbox.
	// A request landing within kSeekBurstWindowSec of the PREVIOUS one is a held
	// key / a scrub; a lone request is not. Precise is never a burst, whatever the
	// spacing.
	//
	// lastSeekRequestWallTime == 0 is "no previous request on this clip" (Open()
	// resets it), which must not read as a burst.
	const double now = GetWallTime();
	seekIsBurst = !isPrecise && lastSeekRequestWallTime > 0.0 &&
				  (now - lastSeekRequestWallTime) < kSeekBurstWindowSec;

	// M5: only a NON-precise submit seeds the burst clock. A precise seek is not a
	// scrub step -- Stop(), the blocking Seek() and frame-step all submit Precise
	// -- so it must neither read as a burst member (the !isPrecise term above
	// already guarantees that) NOR make the NEXT request read as one by leaving a
	// fresh timestamp behind it. Without this guard a RequestSeek() arriving within
	// the window of a Stop() would be mis-classified as a scrub and go fast.
	if (!isPrecise)
		lastSeekRequestWallTime = now;

	// DISARM the quiet-refine: it exists to refine the last fast landing to the
	// target the user stopped on, and the user has just moved. Its target is
	// superseded, and (unlike the mailbox) it is not latest-wins -- if this newer
	// request lands fast, the service re-arms it with the new target; if it lands
	// precise, nothing needs refining. (refineGateDropOnly is only ever read while
	// refinePending is true, but clear it too so a stale value can never leak into
	// a future arm.)
	refinePending = false;
	refineGateDropOnly = false;

	// AUDIO GATE UP, at SUBMIT time (Task 4). The seek is serviced later, on the
	// decode thread; until then the audio ring still holds PRE-seek PCM and the
	// mixer would keep playing it -- audible sound from a position the user has
	// already left. From here on MixIn() drains the ring but contributes silence.
	// Dropped again by ServiceSeekOnDecodeThread(), but only if no NEWER request
	// is pending by then (both mutations happen under cmdMutex, so they are
	// serialized against each other and against the seekTargetPending check).
	if (audioChannel)
		audioChannel->SetSeekPending(true);

	// Latest-wins: the bump immediately invalidates every frame already in the
	// ring (and any frame the decode thread is still stamping from the OLD
	// decodeGeneration), so a full ring can never block the new seek's result.
	uint64_t gen = seekGeneration.fetch_add(1, std::memory_order_acq_rel) + 1;
	pendingSeekTargetAtomic.store(target, std::memory_order_release);

	// The generation the MAILBOX now carries. The decode thread must service the
	// command with exactly this value (loading seekGeneration at consume time
	// would pick up a generation whose target it never read) -- and it is what a
	// pending Stop() is bound to (see Stop()).
	seekPendingGeneration = gen;

	cmdCv.notify_all();

	// WAKE (Task 5), state-first/wake-second: seekGeneration -- the state the
	// abort predicate reads -- was published by the fetch_add above, so any wait
	// woken from here re-evaluates the predicate and sees this newer generation.
	// A wake can therefore never be lost between a check and a sleep, in either
	// interleaving:
	//   * we bump BEFORE the waiter's last predicate check -> it never sleeps;
	//   * we bump AFTER it -> it is already asleep and this wake reaches it.
	// Without this, a burst of key-repeat seeks would SERIALIZE behind whatever
	// precise seek is already running inside the source instead of preempting it.
	//
	// Called with cmdMutex HELD (this function's contract): the bump and the wake
	// must not be split by a racing submit. Lock order is cmdMutex -> the source's
	// internal decoder mutex, and nothing on the decoder side ever takes cmdMutex,
	// so that edge has no back-edge and cannot cycle. The predicate itself takes
	// NO lock at all (ShouldAbortDecode()), so the woken decode thread can
	// re-evaluate it while we still hold cmdMutex.
	if (source)
		source->WakeAbort();

	return gen;
}

// Returns false when the wait ended because the decode thread EXITED rather than
// because `generation` was serviced -- the caller must then not touch the ring
// buffer or any frame plane (Close()/FreeResources() is freeing them).
bool CVideoPlayer::WaitForSeekServiced(uint64_t generation)
{
	std::unique_lock<std::mutex> lk(cmdMutex);
	serviceCv.wait(lk, [this, generation] {
		return seekServicedGeneration >= generation || decodeThreadExited || !threadRunning.load();
	});

	return !decodeThreadExited && threadRunning.load() && seekServicedGeneration >= generation;
}

bool CVideoPlayer::ServiceSeekOnDecodeThread(double target, uint64_t generation, ESeekMode mode,
											 bool isBurst, bool decodeOneFrameWhenNotPlaying,
											 bool *outPublishedNotPlayingFrame)
{
	// The effective fast/precise decision. Coarse is ALWAYS fast (even a lone
	// press is a keyframe seek); TwoPhase is fast only inside a burst; Precise is
	// never fast. (The refine's own pass comes back through here as Precise.)
	const bool coarse = (mode == ESeekMode::Coarse);
	const bool useFast = coarse || (mode == ESeekMode::TwoPhase && isBurst);

	LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: %.3f sec (gen %llu, mode=%d, %s)", target,
		 (unsigned long long)generation, (int)mode, useFast ? "FAST" : "precise");

	if (outPublishedNotPlayingFrame)
		*outPublishedNotPlayingFrame = false;

	// Everything decoded from here on belongs to `generation` (see the
	// decodeGeneration comment in the header: stamping from seekGeneration
	// would let a pre-seek frame masquerade as a post-seek one).
	decodeGeneration = generation;

	// OPEN THE SERVICE WINDOW (Task 5): from here until a newer generation is
	// submitted (or shutdown), ShouldAbortDecode() reads false -- this is the one
	// operation the source is allowed to run to completion. Must be published
	// BEFORE source->Seek() below, or the seek we just started would see
	// seekGeneration != servicingSeekGeneration (the previous value) and abort
	// itself instantly.
	servicingSeekGeneration.store(generation, std::memory_order_release);

	// A seek clears the end-of-stream latch: the thread survives EOS now, and a
	// seek back into the clip must let it decode again.
	endOfStream.store(false, std::memory_order_relaxed);

	// Where the clock is re-based to once the source lands. TwoPhase/Precise land
	// the clock on the requested `target`; Coarse lands it on the KEYFRAME the
	// source actually reached (filled below), so video+audio resume in sync from
	// the frame on screen instead of claiming a target the picture is a GOP short
	// of. Seeded to `target` so any path that does not report a landed pts (WebM
	// default impl, an EOS landing, the precise fallback) re-bases to the target.
	double clockBase = target;

	if (coarse)
	{
		// --- COARSE service (keyframe-final, Coarse-seek task) -----------------
		// PRE-SEEK POSITION for the forward-progress guard. Read the clock under
		// its own mutex (we hold NEITHER cmdMutex NOR `mutex` here -- lock order
		// permits taking `mutex` alone). This is "where the player is now"; a
		// forward step that lands back at/behind it made no progress.
		const double preSeek = GetCurrentTime();
		const double kProgressEps = 1e-4;

		double landedPts = -1.0;   // < 0 == "source reported no landing" sentinel
		debugFastSeeksServiced.fetch_add(1, std::memory_order_relaxed);
		bool ok = source ? source->SeekFast(target, &landedPts) : true;
		if (source && !ok)
		{
			// Same abort/failure classification as the precise path below.
			if (ShouldAbortDecode() && source->GetErrorReason().empty())
			{
				LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: gen %llu ABORTED during coarse seek",
					 (unsigned long long)generation);
				return false;
			}
			LOGWarning("CVideoPlayer::ServiceSeekOnDecodeThread: coarse seek failed for time %.3f", target);
		}
		if (landedPts < 0.0)
			landedPts = target;   // source could not report a landing -> use target

		// FORWARD-PROGRESS GUARD. AVSEEK_FLAG_BACKWARD lands the keyframe at/before
		// the target, so a forward step whose target is still inside the current
		// GOP re-lands the SAME keyframe -- held-forward would visually freeze.
		// When the landing made no forward progress (landed <= preSeek) but the
		// user asked to go forward (target > preSeek), retry FORWARD; if there is
		// no later keyframe at all (single-keyframe/long-GOP file), fall back to
		// ONE precise seek so progress is guaranteed (the old stall cost, but only
		// on pathological files). Backward steps always progress naturally.
		if (ok && target > preSeek + kProgressEps && landedPts <= preSeek + kProgressEps)
		{
			double fwdPts = -1.0;
			debugFastSeeksServiced.fetch_add(1, std::memory_order_relaxed);
			bool fok = source && source->SeekFastForward(target, &fwdPts);
			if (fok && fwdPts > preSeek + kProgressEps)
			{
				landedPts = fwdPts;   // a later keyframe carried us forward
			}
			else
			{
				// No usable later keyframe (or the forward retry was aborted).
				// Distinguish a real abort so we publish nothing for a superseded
				// generation; otherwise guarantee progress with a precise seek.
				if (source && ShouldAbortDecode() && source->GetErrorReason().empty())
				{
					LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: gen %llu ABORTED during coarse forward retry",
						 (unsigned long long)generation);
					return false;
				}
				LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: coarse forward retry found no later keyframe -> precise fallback to %.3f",
					 target);
				debugPreciseSeeksServiced.fetch_add(1, std::memory_order_relaxed);
				if (source && !source->Seek(target))
				{
					if (ShouldAbortDecode() && source->GetErrorReason().empty())
					{
						LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: gen %llu ABORTED during coarse precise fallback",
							 (unsigned long long)generation);
						return false;
					}
					LOGWarning("CVideoPlayer::ServiceSeekOnDecodeThread: coarse precise fallback failed for time %.3f", target);
				}
				landedPts = target;   // precise seek lands exactly at the target
			}
		}

		clockBase = landedPts;
	}
	else
	{
		// --- TwoPhase / Precise service (unchanged behaviour) ------------------
		// THE TWO-PHASE SEEK (Task 6). In a burst, SeekFast(): the keyframe
		// landing, no walk to the target -- inexact, but it costs ~2 decodes
		// instead of a whole GOP, which is what makes a held arrow key keep up. The
		// quiet-refine armed at the bottom of this function is what makes the FINAL
		// landing exact. A source with no fast variant (CVideoSourceWebMVpx)
		// inherits the default, which IS Seek(): slower under a burst, never wrong.
		if (useFast)
			debugFastSeeksServiced.fetch_add(1, std::memory_order_relaxed);
		else
			debugPreciseSeeksServiced.fetch_add(1, std::memory_order_relaxed);

		if (source && !(useFast ? source->SeekFast(target) : source->Seek(target)))
		{
			// ABORT CLASSIFICATION at the call site (spec #2.3 -- IVideoSource::Seek()
			// has no third return state): the source returns false for a genuine
			// failure AND for an abort, so ask the predicate, which is the thing that
			// caused the abort in the first place.
			//
			// ...but the predicate ALONE is not enough to classify: a genuine seek
			// failure that happens to race a newer submit would read "aborted" and have
			// its error swallowed -- benign once (the newer seek re-attempts and the
			// same failure resurfaces), but a permanently-failing source under key
			// repeat could have its error suppressed indefinitely. TIEBREAK on the error
			// reason: an aborted Seek() leaves it EMPTY by contract (IVideoSource.h; the
			// FFmpeg source clears it at every abort-return, and every source that
			// cannot abort at all never takes this branch spuriously), so a non-empty
			// reason means a REAL failure regardless of what the predicate says.
			if (ShouldAbortDecode() && source->GetErrorReason().empty())
			{
				// Superseded (a newer seek is already in the mailbox) or shutting
				// down. Publish NOTHING for this generation -- no clock re-base (it
				// would land the clock on a target we never actually reached), no
				// completion (Update() must keep the clock frozen until the NEWEST
				// seek lands). Return to the mailbox: the command loop immediately
				// services the newest target, and its completion releases any blocking
				// waiter keyed to this generation too (WaitForSeekServiced() waits for
				// seekServicedGeneration >= generation).
				LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: gen %llu ABORTED during source seek (superseded or shutting down)",
					 (unsigned long long)generation);
				return false;
			}
			LOGWarning("CVideoPlayer::ServiceSeekOnDecodeThread: source seek failed for time %.3f", target);
		}
	}

	// Decode-thread-owned audio reset (the resampler/queue belong to the decode
	// side; stale pre-seek audio must never survive the seek).
	if (audioChannel)
	{
		audioChannel->Reset();
	}

	// NO ClearRingBuffer() here (rule 2): rewinding the indices under a live
	// reader is exactly the race Task 1's slot protocol exists to prevent.
	// Stale frames are dropped by generation on the reader side and reclaimed
	// from the tail by StoreDecodedFrame() on the writer side.

	if (source && decodeOneFrameWhenNotPlaying && state.load() != EVideoPlayerState::Playing)
	{
		// Paused/Idle: nobody is pumping Update() to refill, so publish exactly
		// ONE frame -- SeekPausedAndDecodeFrame()'s display step (or the next
		// Update() after a Play()) picks it up. Audio decoded on the way is
		// discarded: paused scrubbing/stepping never plays sound.
		SDecodedAudio discardAudio;
		while (source->ReadAudio(discardAudio)) { /* discard */ }

		SDecodedVideoFrame decodedFrame;
		if (source->ReadVideoFrame(decodedFrame))
		{
			StoreDecodedFrame(decodedFrame);
			// M2: record that THIS service actually published a not-Playing frame,
			// so the caller bumps refinePublishCounter on the real decision rather
			// than re-reading `state` after we return (which a Pause() landing in
			// the interim would corrupt).
			if (outPublishedNotPlayingFrame)
				*outPublishedNotPlayingFrame = true;
			while (source->ReadAudio(discardAudio)) { /* trailing audio, same discard */ }
		}
		else if (ShouldAbortDecode() && source->GetErrorReason().empty())
		{
			// Same classification as the Seek() branch above, INCLUDING its
			// tiebreak: the predicate alone would swallow a genuine decode error
			// that merely happened to race a newer submit. An aborted read leaves
			// the reason EMPTY by contract (IVideoSource.h), so a non-empty one is a
			// real failure no matter what the predicate says -- it falls through to
			// the "no frame" branch below, which publishes no frame but completes
			// the generation, exactly as a decode failure has always done.
			//
			// The single-frame decode was cut short, not failed. Discard the partial
			// result and publish nothing for this generation.
			LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: gen %llu ABORTED during the single-frame decode",
				 (unsigned long long)generation);
			return false;
		}
		else
		{
			LOGD("CVideoPlayer::ServiceSeekOnDecodeThread: no frame at %.3f (decode failure or EOS)", target);
		}
	}

	// Re-base the playback clock (clock triplet: under `mutex`, never with
	// cmdMutex held -- see the lock-order note in the header). Coarse re-bases onto
	// the LANDED KEYFRAME pts (clockBase == landedPts); TwoPhase/Precise onto the
	// requested target (clockBase == target). This is the A/V-sync guard: the clock
	// never claims a position the picture on screen is a GOP short of.
	mutex->Lock();
	currentTime = clockBase;
	playbackStartOffset = clockBase;
	playbackStartWallTime = GetWallTime();
	mutex->Unlock();

	// Publish completion. A NEWER seek may already be pending (latest-wins):
	// in that case leave pendingSeekTargetAtomic pointing at it.
	{
		std::lock_guard<std::mutex> lk(cmdMutex);

		// ARM THE DEADLINE (Task 6 quiet-refine + Coarse gate-drop). Two kinds:
		//
		//  * TwoPhase fast landing -> a QUIET-REFINE. This landing is inexact -- up
		//    to a GOP short of `target`. If the user goes quiet, the command loop
		//    re-runs this service PRECISELY under this SAME generation (see the
		//    refine* fields in the header for why it must not bump).
		//  * Coarse BURST landing -> a GATE-DROP-ONLY deadline. Coarse is
		//    keyframe-final (no re-seek ever), but a burst landing keeps the audio
		//    gate RAISED so a held scrub stays silent; when the user goes quiet the
		//    loop drops the gate (no re-seek). A NON-burst coarse landing arms
		//    nothing and drops the gate at completion below.
		//
		// Neither is armed by a plain precise service (Precise mode, TwoPhase
		// non-burst, or the refine's own pass), so a refine can never loop and a
		// non-scrub seek behaves exactly as before. A submit racing this either
		// landed BEFORE (seekTargetPending >= 0: do not arm) or AFTER (it clears
		// refinePending under this same mutex).
		const bool armTwoPhaseRefine = (mode == ESeekMode::TwoPhase && useFast);
		const bool armCoarseGateDrop = (coarse && isBurst);
		if ((armTwoPhaseRefine || armCoarseGateDrop) && seekTargetPending < 0.0 &&
			seekGeneration.load(std::memory_order_acquire) == generation)
		{
			refinePending = true;
			refineGateDropOnly = armCoarseGateDrop;
			refineTarget = target;
			refineGeneration = generation;
			refineDeadlineWallTime = GetWallTime() + kSeekBurstWindowSec;
		}

		seekServicedGeneration = generation;
		completedSeekGeneration.store(generation, std::memory_order_release);
		if (seekTargetPending < 0.0)
		{
			pendingSeekTargetAtomic.store(-1.0, std::memory_order_release);

			// AUDIO GATE DOWN (Task 4) -- and ONLY here, in the same cmdMutex
			// critical section as the "nothing newer is pending" test. Everything
			// still in the ring at this point is POST-seek: Reset() emptied it
			// above and the only thread that can have refilled it since is this
			// one (PushSamples() is decode-thread work, and we are the decode
			// thread). Dropping the gate unconditionally would instead unmute the
			// ring for a seek that has ALREADY been superseded -- a burst of
			// arrow-key steps would leak the audio of an intermediate position.
			// A submit racing this either lands BEFORE (we see seekTargetPending
			// >= 0 and leave the gate up) or AFTER (it re-raises the gate under
			// this same mutex): the gate is never left down with a seek pending.
			//
			// M1: keep the gate RAISED while a deadline is armed (refinePending,
			// just set above -- a TwoPhase fast landing's quiet-refine OR a Coarse
			// burst's gate-drop deadline). A fast/coarse landing leaves the demuxer
			// read head at the KEYFRAME -- seconds before `target` on long-GOP
			// content -- so dropping the gate here would let the Playing refill push
			// audio decoded FROM THE KEYFRAME FORWARD into the mixer: audible sound
			// from a position the user has already scrubbed past, exactly what this
			// gate exists to prevent. The armed deadline drops the gate when it
			// fires (the TwoPhase refine on completing its precise pass; the Coarse
			// gate-drop deadline directly). A precise service (Precise mode /
			// TwoPhase non-burst / a NON-burst coarse landing) arms nothing and drops
			// the gate here immediately, exactly as before. pendingSeekTargetAtomic
			// is still cleared above regardless -- the deadline is invisible to
			// GetPendingSeekTarget(); only the AUDIO gate waits it out.
			if (audioChannel && !refinePending)
				audioChannel->SetSeekPending(false);
		}
	}
	serviceCv.notify_all();
	return true;
}

// ============================================================================
// DecodeThreadFunc -- the command loop (Task 2)
//
// Alive from Open() to Close(). It parks on cmdCv until there is something to
// do (a pending seek/stop command, Playing, or shutdown), so an idle player
// costs nothing and -- crucially -- a seek is serviced HERE instead of on the
// render thread, which used to join this thread and run the whole seek inline.
// ============================================================================
std::string CVideoPlayer::GetErrorReason() const
{
	if (mutex == nullptr)
		return errorReason;   // pre-Init / post-teardown: single-threaded by construction
	mutex->Lock();
	const std::string copy = errorReason;
	mutex->Unlock();
	return copy;
}

void CVideoPlayer::SetErrorReason(const std::string &reason)
{
	if (mutex == nullptr)
	{
		errorReason = reason;
		return;
	}
	mutex->Lock();
	errorReason = reason;
	mutex->Unlock();
}

void CVideoPlayer::DecodeThreadFunc()
{
	LOGD("CVideoPlayer::DecodeThreadFunc: started");

	while (true)
	{
		double target = -1.0;
		ESeekMode mode = ESeekMode::TwoPhase;
		bool isBurst = false;
		bool isRefine = false;
		uint64_t gen = 0;
		bool doStop = false;
		bool notifyStopWaiter = false;

		{
			std::unique_lock<std::mutex> lk(cmdMutex);

			// The endOfStream/Error terms keep the Playing branch from
			// spinning after it has run out of stream: the thread must stay
			// alive (a later seek clears endOfStream and resumes it) but it
			// must not busy-loop. state is left Playing after EOS on purpose
			// -- Update() is what turns it into Finished once the ring drains.
			auto haveWork = [this] {
				if (shouldStop.load())
					return true;
				if (seekTargetPending >= 0.0)
					return true;
				return state.load() == EVideoPlayerState::Playing
					   && !endOfStream.load(std::memory_order_acquire);
			};

			if (!haveWork())
			{
				if (refinePending)
				{
					// TIMED wait -- ONLY while a quiet-refine is armed (Task 6). The
					// refine has to fire on a clock, not on an event: "the user went
					// quiet" is precisely the absence of one. Waking at the deadline
					// costs a single wakeup per burst, and with nothing armed the
					// thread still parks INDEFINITELY on the untimed wait below --
					// there is no periodic tick and no busy-spin here.
					//
					// A command landing inside the window satisfies haveWork() and
					// wakes us early; SubmitSeekCommandLocked() has already cleared
					// refinePending by then, so the refine simply evaporates.
					const double remaining = refineDeadlineWallTime - GetWallTime();
					if (remaining > 0.0)
					{
						cmdCv.wait_for(lk, std::chrono::duration<double>(remaining), haveWork);
					}
				}
				else
				{
					cmdCv.wait(lk, haveWork);
				}
			}

			if (shouldStop.load())
				break;

			if (seekTargetPending >= 0.0)
			{
				target = seekTargetPending;
				// MODE + BURST (Coarse-seek task): both carried through the mailbox
				// from the submit. seekIsBurst already accounts for Precise (a
				// precise request is never a burst member and never leaves a
				// refine/gate-drop deadline armed behind it). The service picks the
				// fast/precise strategy from (mode, isBurst).
				mode = seekModePending;
				isBurst = seekIsBurst;
				// The generation the MAILBOX carries -- not a fresh load of
				// seekGeneration, which a submit racing this consume may already
				// have bumped past the target we are reading here.
				gen = seekPendingGeneration;

				// A pending Stop() is honoured only if it was submitted WITH this
				// exact generation. If a Seek() overwrote the mailbox afterwards
				// (latest-wins), the stop is superseded: we must not land Idle at
				// the SEEK's target. Release Stop()'s waiter anyway -- it is a
				// blocking call and a superseded command must not hang it.
				doStop = (stopCommandPending && stopCommandGeneration == gen);
				bool stopSuperseded = (stopCommandPending && !doStop);

				seekTargetPending = -1.0;   // consumed; a newer submit re-arms it
				stopCommandPending = false;

				if (stopSuperseded)
				{
					LOGWarning("CVideoPlayer::DecodeThreadFunc: Stop() (gen %llu) superseded by a newer seek (gen %llu)",
							   (unsigned long long)stopCommandGeneration, (unsigned long long)gen);
					stopCommandDone = true;
					notifyStopWaiter = true;
				}
			}
			else if (refinePending)
			{
				// --- THE QUIET-REFINE (Task 6) ----------------------------------
				// The mailbox is empty and the last landing was a FAST one. Either
				// the user has gone quiet long enough (fire: one precise seek to the
				// same target, under the SAME generation -- see the refine* fields in
				// the header) or the landing has since been superseded (drop it).
				if (seekGeneration.load(std::memory_order_acquire) != refineGeneration)
				{
					// DEFENCE IN DEPTH -- unreachable in practice, NOT a live case.
					// Reaching this branch means refinePending is still true, and
					// every submit (SubmitSeekCommandLocked) clears refinePending
					// under cmdMutex; so no submit can have bumped seekGeneration
					// since the arm, which itself required seekGeneration ==
					// generation under that same lock. Kept as a belt-and-braces
					// disarm should that invariant ever change; nothing to correct.
					refinePending = false;
					refineGateDropOnly = false;

					// M1: drop the audio gate the fast/coarse landing raised. Disarming
					// without servicing (no ServiceSeekOnDecodeThread call) would
					// otherwise leave it stuck raised. Safe against a pending seek:
					// this whole else-if runs only when seekTargetPending < 0.0 (the
					// first branch was false), and every submit clears refinePending
					// under cmdMutex -- so reaching here means nothing newer is
					// pending. Mirror the drop site above exactly.
					if (audioChannel)
						audioChannel->SetSeekPending(false);
				}
				else if (endOfStream.load(std::memory_order_acquire) ||
						 state.load() == EVideoPlayerState::Finished ||
						 state.load() == EVideoPlayerState::Error)
				{
					// M3: the fast landing was near EOS. On a Playing player the
					// refill then ran the stream out and Update() has (or is about
					// to) transition to Finished and fire onFinished(). Firing the
					// refine now would clear endOfStream, re-base the clock BACKWARDS
					// onto the target and publish a frame AFTER the clip has already
					// finished -- a host merely SHOWING Finished would see the picture
					// and clock jump back. Disarm instead.
					//
					// M2 (Error): a refill that hit a genuine decode error left the
					// player wedged in Error. Firing the refine would re-seek the
					// source and clear endOfStream on a dead player -- so fold Error
					// into the same disarm-without-service guard above.
					//
					// This cannot strand a legitimate mid-clip refine: endOfStream is
					// only latched when the refill genuinely reached EOS, which a
					// mid-clip target never does, and a PAUSED fast landing never
					// refills at all (so it never latches EOS here either). The
					// deadline branch below still fires for every landing inside the
					// clip.
					LOGD("CVideoPlayer::DecodeThreadFunc: quiet-refine disarmed -- player at/near EOS or Error (gen %llu)",
						 (unsigned long long)refineGeneration);
					refinePending = false;
					refineGateDropOnly = false;

					// M1: drop the audio gate the fast landing raised (same reasoning
					// as the stale-generation branch above -- no newer seek can be
					// pending here, so this never unmutes a live seek).
					if (audioChannel)
						audioChannel->SetSeekPending(false);
				}
				else if (GetWallTime() >= refineDeadlineWallTime)
				{
					if (refineGateDropOnly)
					{
						// COARSE gate-drop deadline (Coarse-seek task): the held scrub
						// has gone quiet. Coarse never re-seeks -- the keyframe landing
						// was final -- so just DROP THE AUDIO GATE the burst kept raised
						// (target stays -1: no service call, no frame). Safe against a
						// pending seek for the same reason as the disarm branches above:
						// this else-if runs only with seekTargetPending < 0.0, and every
						// submit clears refinePending under cmdMutex.
						refinePending = false;
						refineGateDropOnly = false;
						if (audioChannel)
							audioChannel->SetSeekPending(false);
						LOGD("CVideoPlayer::DecodeThreadFunc: coarse gate-drop deadline fired (gen %llu) -- audio gate dropped, no re-seek",
							 (unsigned long long)refineGeneration);
					}
					else
					{
						// TwoPhase QUIET-REFINE: one precise pass to the exact target,
						// under the SAME generation, submitted as Precise.
						target = refineTarget;
						gen = refineGeneration;
						mode = ESeekMode::Precise;   // THE POINT: this pass is the precise one
						isBurst = false;
						isRefine = true;
						refinePending = false;       // one-shot; a precise service never re-arms
						LOGD("CVideoPlayer::DecodeThreadFunc: quiet-refine -> precise seek to %.3f (gen %llu)",
							 target, (unsigned long long)gen);
					}
				}
			}
		}

		if (notifyStopWaiter)
			serviceCv.notify_all();

		if (target >= 0.0)
		{
			// A Stop() rewinds and lands Idle -- it has no interest in a frame.
			bool publishedNotPlayingFrame = false;
			bool serviced = ServiceSeekOnDecodeThread(target, gen, mode, isBurst,
													  /*decodeOneFrameWhenNotPlaying=*/!doStop,
													  &publishedNotPlayingFrame);

			// A refine on a NOT-PLAYING player published a corrected frame (the
			// service's decodeOneFrameWhenNotPlaying pass). Update()'s not-Playing
			// delivery gate is keyed to the seek generation, which the refine
			// deliberately does NOT bump -- so without this the paused reader would
			// keep showing the FAST landing's keyframe picture (up to a whole GOP
			// short of the target: the entire clip, on the single-keyframe long-GOP
			// fixture) and the corrected frame would rot in the ring. Publishing
			// AFTER the service means the frame is already in the ring when the
			// reader sees the counter move.
			//
			// M2: gate the bump on the service's OWN not-Playing decision, NOT on a
			// re-read of `state` here. If the player was Playing when the service
			// checked (so no frame was published for the refine) but Paused by the
			// time this line runs, a `state != Playing` re-read would bump the
			// counter with nothing in the ring -- Update()'s gate would then open
			// and display one stale buffered frame (picture creeps after Pause()).
			if (serviced && isRefine && publishedNotPlayingFrame)
				refinePublishCounter.fetch_add(1, std::memory_order_release);

			if (doStop)
			{
				// An ABORTED stop (Task 5) never reached position 0 -- do not claim
				// it did by landing Idle. Its waiter is released either way: a stop
				// aborted by a newer SEEK is exactly the "superseded" case Stop()
				// already documents (latest-wins), and one aborted by SHUTDOWN is
				// released by decodeThreadExited below. Not releasing it here would
				// hang Close()'s own caller.
				if (serviced)
					state = EVideoPlayerState::Idle;
				{
					std::lock_guard<std::mutex> lk(cmdMutex);
					stopCommandDone = true;
				}
				serviceCv.notify_all();
			}
			continue;
		}

		// Woken with nothing to service and nothing to play: the quiet-refine's
		// TIMED wait (Task 6) is the only wait that can return without haveWork()
		// -- e.g. the refine was disarmed as stale above, or the deadline has not
		// quite arrived yet. Loop back and park/re-wait rather than falling into
		// the refill body below, which must only ever run on a PLAYING player.
		if (state.load() != EVideoPlayerState::Playing ||
			endOfStream.load(std::memory_order_acquire))
			continue;

		// --- Playing: refill the ring (unchanged decode body) ---------------
		// Check ring buffer space.
		//
		// NOTE: this pre-check is why StoreDecodedFrame()'s tail-reclamation branch
		// is UNREACHABLE from the Playing path -- we never even decode when the
		// window is full, we sleep and re-check (the render thread is pumping
		// Update() and will drain it). Reclamation exists purely for the
		// not-Playing single-frame decode inside ServiceSeekOnDecodeThread(), where
		// nobody is draining the ring. Do not assume it covers Playing.
		uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
		uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);

		if (writeIdx - readIdx >= VIDEO_BUFFER_FRAMES)
		{
			// Ring buffer is full. Before sleeping, top up the AUDIO pipeline
			// (2026-07-18): with only VIDEO_BUFFER_FRAMES (~0.13s) of decoded
			// video, a container that interleaves audio lazily (real-world
			// ASF/WMV) starves the mixer while we sit here -- the demuxer is
			// the only path more audio can arrive by, and it was gated on this
			// very ring. PumpAudioAhead() demuxes forward for audio only,
			// parking compressed video packets (kilobytes) for the next
			// ReadVideoFrame(); the low-water check keeps the pump off for
			// well-interleaved files (their backlog never dips) and the
			// pump-then-continue shape keeps this loop responsive to seek
			// commands between pumps. The audio-starvation free-run in
			// Update() remains the last-resort fallback (audio track ends
			// early, pathological interleave past the parking caps).
			if (hasAudio && audioChannel &&
				state.load() == EVideoPlayerState::Playing &&
				audioChannel->GetBufferedSeconds() < kAudioPumpLowWaterSec &&
				source && source->PumpAudioAhead())
			{
				DrainSourceAudioToChannel();
				continue;
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
			continue;
		}

		bool gotPacket = DemuxAndDecodeNextPacket();
		if (!gotPacket)
		{
			// ABORT CLASSIFICATION, FIRST (Task 5). A refill decode cut short by a
			// newly-submitted seek (or by shutdown) returns false with an empty
			// source error reason -- i.e. it looks EXACTLY like end-of-stream. It is
			// not: it is not EOS, so do not latch endOfStream for it. Just loop
			// back: the wait predicate below either breaks out (shutdown) or
			// immediately hands us the pending seek command.
			//
			// DEFENCE IN DEPTH, not a live bug fix: Update() could not have fired
			// onFinished() off a latch set here anyway -- it gates the Finished
			// transition on !seekPending, and seekPending is true for as long as a
			// newer generation is outstanding (the only way the generation term of
			// the predicate can be true); and for the shutdown term, Update() and
			// Close() are both render-thread and cannot interleave. This check keeps
			// the latch semantically honest ("endOfStream means the source really hit
			// EOS") rather than relying on that downstream gate to stay in place.
			// The `GetErrorReason().empty()` half is the same tiebreak the seek
			// sites apply (Task 6): a GENUINE decode error that races a newly-
			// submitted seek would otherwise read as "aborted" here and be silently
			// swallowed -- under key repeat, indefinitely. An aborted read leaves the
			// reason empty by contract, so a non-empty reason means a real failure
			// and falls through to the Error branch below.
			if (ShouldAbortDecode() && (!source || source->GetErrorReason().empty()))
			{
				LOGD("CVideoPlayer::DecodeThreadFunc: refill decode aborted (superseded seek or shutdown) -- not EOS");
				continue;
			}

			// Distinguish a genuine source decode failure (Plan-2 Task 2)
			// from ordinary end-of-stream: source->GetErrorReason() is
			// non-empty only for the former (IVideoSource's contract).
			//
			// Neither case breaks out of the thread any more (Task 2): the
			// thread must survive to service later seeks. Both fall back to the
			// cv wait, whose predicate no longer admits the Playing branch
			// (Error clears Playing; endOfStream gates it) -- so no spinning.
			if (source && !source->GetErrorReason().empty())
			{
				const std::string reason = source->GetErrorReason();
				SetErrorReason(reason);   // under the leaf mutex; readers copy
				LOGError("CVideoPlayer::DecodeThreadFunc: source decode error, entering Error state: %s",
						 reason.c_str());
				state = EVideoPlayerState::Error;
				continue;
			}

			// EOF — signal main thread to drain remaining frames before finishing
			LOGD("CVideoPlayer::DecodeThreadFunc: end of stream, signaling endOfStream");
			endOfStream.store(true, std::memory_order_release);
			continue;
		}
	}

	// Shutting down: release anybody blocked in a wrapper (a command submitted
	// concurrently with Close() would otherwise wait for a service that will
	// never come). threadRunning is only cleared after the join, so the waiters'
	// !threadRunning escape hatch cannot fire on its own -- decodeThreadExited is
	// what tells a released wrapper that it was woken by SHUTDOWN and not by a real
	// service. SeekPausedAndDecodeFrame() must know: unlike Seek()/Stop() (which
	// touch nothing after the wait) it would otherwise go on to claim a ring slot
	// and display planes that FreeResources() is about to delete[].
	{
		std::lock_guard<std::mutex> lk(cmdMutex);
		decodeThreadExited = true;
		seekServicedGeneration = seekGeneration.load(std::memory_order_relaxed);
		stopCommandPending = false;
		stopCommandDone = true;
		seekTargetPending = -1.0;
		refinePending = false;   // Task 6: a refine must never outlive the thread
		// Finding 3 (Task 3 review): the mailbox slot above is thread-local to
		// this loop, but pendingSeekTargetAtomic is the PUBLIC "where is this
		// player headed?" reader (GetPendingSeekTarget()) -- a Close() racing an
		// outstanding RequestSeek() must not leave it reporting a target forever
		// on a now-dead player.
		pendingSeekTargetAtomic.store(-1.0, std::memory_order_release);
	}
	serviceCv.notify_all();

	LOGD("CVideoPlayer::DecodeThreadFunc: exiting");
}

// ============================================================================
// DemuxAndDecodeNextPacket
// ============================================================================
// Drain audio the source decoded while demuxing, pushing it into the mixer
// channel. SDecodedAudio hands out s16 PCM; CVideoAudioChannel::PushSamples
// takes float (it was written against Opus's float decode output) -- convert.
// DECODE THREAD ONLY. Shared by DemuxAndDecodeNextPacket() and the ring-full
// audio pump-ahead in DecodeThreadFunc() (2026-07-18).
void CVideoPlayer::DrainSourceAudioToChannel()
{
	SDecodedAudio decodedAudio;
	while (source->ReadAudio(decodedAudio))
	{
		if (audioChannel && !decodedAudio.pcm.empty() && decodedAudio.channels > 0)
		{
			static thread_local std::vector<float> floatBuf;
			floatBuf.resize(decodedAudio.pcm.size());
			for (size_t i = 0; i < decodedAudio.pcm.size(); i++)
			{
				floatBuf[i] = static_cast<float>(decodedAudio.pcm[i]) / 32767.0f;
			}
			int numFrames = (int)(decodedAudio.pcm.size() / decodedAudio.channels);
			audioChannel->PushSamples(floatBuf.data(), numFrames, decodedAudio.channels,
			                          (int)decodedAudio.sampleRate);
		}
	}
}

bool CVideoPlayer::DemuxAndDecodeNextPacket()
{
	DrainSourceAudioToChannel();

	SDecodedVideoFrame decodedFrame;
	if (!source->ReadVideoFrame(decodedFrame))
	{
		// false = end of stream or error; DecodeThreadFunc distinguishes via
		// source->GetErrorReason() (Plan-2 Task 2: non-empty means a genuine
		// decode failure -> EVideoPlayerState::Error, not the ordinary
		// endOfStream/Finished path).
		// Push any trailing audio decoded during the final scan before giving up.
		DrainSourceAudioToChannel();
		return false;
	}

	StoreDecodedFrame(decodedFrame);

	// Also drain any audio decoded while the source was scanning for this frame
	DrainSourceAudioToChannel();

	return true;
}

// ============================================================================
// StoreDecodedFrame
// ============================================================================
namespace
{
	// Grows a DecodedFrame plane buffer to at least neededBytes, preserving
	// no data (planes are fully overwritten by the caller immediately after).
	void EnsurePlaneCapacity(u8 *&plane, int &capacity, int neededBytes)
	{
		if (capacity < neededBytes)
		{
			delete[] plane;
			plane = new u8[neededBytes];
			capacity = neededBytes;
		}
	}
}

void CVideoPlayer::StoreDecodedFrame(const SDecodedVideoFrame &decoded)
{
	// Check ring buffer space
	uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
	uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);

	if (writeIdx - readIdx >= VIDEO_BUFFER_FRAMES)
	{
		// Full window. Before dropping, try to reclaim the TAIL slot if it holds a
		// STALE-generation frame (spec #2.3 rule 3).
		//
		// Why this is required (it is NOT a deadlock guard -- StoreDecodedFrame
		// never blocks on ring space, it DROPS, and ServiceSeekOnDecodeThread
		// publishes its completion either way, so a missing reclamation cannot hang
		// anybody): a blocking Seek()/SeekPausedAndDecodeFrame() is called FROM the
		// render thread, i.e. the only thread that drains the ring is parked inside
		// the wrapper. The ring is therefore still full of the pre-seek frames when
		// the decode thread wants to publish the seek's own frame. Without
		// reclamation that frame is silently DROPPED -- the seek "succeeds", the
		// wrapper's claim finds nothing for its generation and
		// SeekPausedAndDecodeFrame() returns false / the picture never updates.
		// Silent frame loss, not a hang.
		//
		// A full window means writeIdx - readIdx == capacity, i.e. the slot we want
		// to write (writeIdx % N) IS the tail slot (readIdx % N) -- reclaiming it
		// frees exactly the slot we need.
		uint32_t tail = frameReadIdx.load(std::memory_order_acquire);
		DecodedFrame &tailSlot = frameBuffer[tail % VIDEO_BUFFER_FRAMES];
		uint64_t curGen = seekGeneration.load(std::memory_order_acquire);
		uint32_t expectedTail = DecodedFrame::SLOT_READY;

		// `generation` is only ever written under WRITING ownership, so reading
		// it before the CAS is safe for a READY slot (the reader never writes
		// it), and no re-check is needed once we own the slot. Strictly OLDER
		// only: a frame from a NEWER generation than our snapshot cannot exist here
		// (this thread is the only writer and stamps from decodeGeneration), and
		// `!=` would reclaim it if it ever did.
		if (tailSlot.generation < curGen &&
			tailSlot.stateWord.compare_exchange_strong(expectedTail, DecodedFrame::SLOT_WRITING,
													   std::memory_order_acquire))
		{
			// Won the claim: advance the tail past it and free it. The window
			// invariant (writeIdx - readIdx <= capacity) holds by construction --
			// the reader only ever pushes frameReadIdx forward too.
			//
			// The reader may be sitting on a pre-CAS load of frameReadIdx == tail
			// and be about to CAS this very slot; the absIdx stamp below is what
			// makes it notice that the slot it won is no longer `tail` (see
			// TryClaimSlotForRead).
			frameReadIdx.store(tail + 1, std::memory_order_release);
			tailSlot.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
			readIdx = tail + 1;
		}
		else
		{
			// Genuinely full of current-generation frames (or the reader owns
			// the tail right now): drop, exactly as before.
			LOGWarning("CVideoPlayer::StoreDecodedFrame: ring buffer full, dropping frame at pts=%.3f", decoded.pts);
			return;
		}
	}

	uint32_t bufIdx = writeIdx % VIDEO_BUFFER_FRAMES;
	DecodedFrame &frame = frameBuffer[bufIdx];
	uint32_t expected = DecodedFrame::SLOT_EMPTY;
	if (!frame.stateWord.compare_exchange_strong(expected, DecodedFrame::SLOT_WRITING,
												 std::memory_order_acquire))
	{
		LOGWarning("CVideoPlayer::StoreDecodedFrame: slot %u not EMPTY (state=%u), dropping frame", bufIdx, expected);
		return;
	}

	int imgWidth = decoded.width;
	int imgHeight = decoded.height;

	// Guard against mid-stream resolution changes exceeding allocated buffer
	if (imgWidth > frame.allocWidth || imgHeight > frame.allocHeight)
	{
		LOGWarning("CVideoPlayer::StoreDecodedFrame: resolution changed to %dx%d (allocated %dx%d), dropping frame",
				   imgWidth, imgHeight, frame.allocWidth, frame.allocHeight);
		frame.stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_release);
		return;
	}

	// Task 5's EmitFrame can hand back 8-bit 4:2:0 (YUV420P/YUVA420P/NV12),
	// 8-bit 4:2:2 (YUV422P), or 10-bit-in-16 variants of either (YUV420P10/
	// YUV422P10). Plane byte-width/height depend on all three axes, so plane
	// buffers are grown on demand here rather than assuming the 8-bit 4:2:0
	// sizing AllocateFrameBuffers() used at Open() time.
	const bool is10Bit = (decoded.pixelFormat == EVideoPixelFormat::YUV420P10 ||
						   decoded.pixelFormat == EVideoPixelFormat::YUV422P10);
	const bool is422 = (decoded.pixelFormat == EVideoPixelFormat::YUV422P ||
						decoded.pixelFormat == EVideoPixelFormat::YUV422P10);
	const bool isNV12 = (decoded.pixelFormat == EVideoPixelFormat::NV12);
	const int bytesPerSample = is10Bit ? 2 : 1;
	const int uvWidth = (imgWidth + 1) / 2;
	const int uvHeight = is422 ? imgHeight : (imgHeight + 1) / 2;

	// Copy Y plane (packed: dst stride == imgWidth * bytesPerSample, no padding)
	int yDstStride = imgWidth * bytesPerSample;
	EnsurePlaneCapacity(frame.yPlane, frame.yCapacity, yDstStride * imgHeight);
	{
		int ySrcStride = decoded.stride[0];
		for (int row = 0; row < imgHeight; row++)
		{
			memcpy(frame.yPlane + row * yDstStride,
				   decoded.plane[0] + row * ySrcStride,
				   static_cast<size_t>(yDstStride));
		}
	}

	if (isNV12)
	{
		// Single interleaved U,V plane (8-bit only; FFmpeg's NV12 has no
		// 10-bit variant we map here). uPlane holds the interleaved pairs;
		// vPlane is unused for this format.
		int uvDstStride = uvWidth * 2;
		EnsurePlaneCapacity(frame.uPlane, frame.uCapacity, uvDstStride * uvHeight);
		int uvSrcStride = decoded.stride[1];
		for (int row = 0; row < uvHeight; row++)
		{
			memcpy(frame.uPlane + row * uvDstStride,
				   decoded.plane[1] + row * uvSrcStride,
				   static_cast<size_t>(uvDstStride));
		}
		frame.uStride = uvDstStride;
		frame.vStride = 0;
	}
	else
	{
		int uvDstStride = uvWidth * bytesPerSample;
		EnsurePlaneCapacity(frame.uPlane, frame.uCapacity, uvDstStride * uvHeight);
		EnsurePlaneCapacity(frame.vPlane, frame.vCapacity, uvDstStride * uvHeight);
		int uSrcStride = decoded.stride[1];
		int vSrcStride = decoded.stride[2];
		for (int row = 0; row < uvHeight; row++)
		{
			memcpy(frame.uPlane + row * uvDstStride,
				   decoded.plane[1] + row * uSrcStride,
				   static_cast<size_t>(uvDstStride));
			memcpy(frame.vPlane + row * uvDstStride,
				   decoded.plane[2] + row * vSrcStride,
				   static_cast<size_t>(uvDstStride));
		}
		frame.uStride = uvDstStride;
		frame.vStride = uvDstStride;
	}

	// Copy alpha plane if present
	// VP9 alpha in WebM: alpha_mode flag + plane index 3
	bool frameHasAlpha = false;
	if (hasAlpha && decoded.pixelFormat == EVideoPixelFormat::YUVA420P && decoded.plane[3] != nullptr)
	{
		frameHasAlpha = true;
		int aDstStride = imgWidth;
		EnsurePlaneCapacity(frame.aPlane, frame.aCapacity, aDstStride * imgHeight);
		int aSrcStride = decoded.stride[3];
		for (int row = 0; row < imgHeight; row++)
		{
			memcpy(frame.aPlane + row * aDstStride,
				   decoded.plane[3] + row * aSrcStride,
				   static_cast<size_t>(aDstStride));
		}
		frame.aStride = aDstStride;
	}
	else
	{
		frame.aStride = 0;
	}

	// Mirror the source's color space/range info (only known once the bitstream
	// is decoded) so GetColorSpace()/GetColorRange() and the RGBA fallback path
	// stay accurate.
	if (source)
	{
		const SVideoInfo &info = source->Info();
		vpxColorSpace = info.colorSpace;
		vpxColorRange = info.fullRange ? 1 : 0;
		colorPrimaries = info.colorPrimaries;
		colorTrc = info.colorTrc;
	}

	frame.yStride = yDstStride;
	frame.width = imgWidth;
	frame.height = imgHeight;
	frame.pts = decoded.pts;
	frame.hasAlpha = frameHasAlpha;
	frame.pixelFormat = decoded.pixelFormat;
	frame.bytesPerSample = bytesPerSample;
	frame.chromaFullHeight = is422;
	frame.chromaInterleaved = isNV12;
	// Stamp with the generation this DECODE belongs to, not the newest one
	// anybody has asked for (seekGeneration may already have moved on: a seek
	// submitted while this frame was being decoded must NOT adopt it).
	frame.generation = decodeGeneration;
	// Slot identity: which ABSOLUTE ring index these bytes are. Published by the
	// release-store below, re-verified by every reader claim (TryClaimSlotForRead)
	// -- without it a reader that got preempted between loading frameReadIdx and
	// CASing the slot cannot tell this frame from the one we just reclaimed out of
	// the same physical slot.
	frame.absIdx = writeIdx;
	frame.stateWord.store(DecodedFrame::SLOT_READY, std::memory_order_release);

	frameWriteIdx.store(writeIdx + 1, std::memory_order_release);
}

// ============================================================================
// GPU texture management
// ============================================================================
void CVideoPlayer::CreateGPUTextures(int width, int height)
{
	// Routed through the backend rather than glGenTextures, so the direct
	// YUV-plane draw path (CGuiViewVideoPlayer, and LightHeroes' cutscenes)
	// works on Metal too. This used to be gated on VideoGpuPathAvailable(),
	// i.e. SupportsOpenGLShaders() -- which under Metal meant the planes were
	// never created and cutscenes rendered nothing at all.
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL) return;

	LOGD("CVideoPlayer::CreateGPUTextures: %dx%d", width, height);

	int uvWidth = (width + 1) / 2;
	int uvHeight = (height + 1) / 2;

	// All four planes are single-channel 8-bit (GL_R8 / MTLPixelFormatR8Unorm).
	texY = backend->CreatePlaneTexture(width, height, 1, 1);
	texU = backend->CreatePlaneTexture(uvWidth, uvHeight, 1, 1);
	texV = backend->CreatePlaneTexture(uvWidth, uvHeight, 1, 1);

	// Alpha plane only when the stream actually carries one.
	if (hasAlpha)
		texA = backend->CreatePlaneTexture(width, height, 1, 1);

	gpuTexturesCreated = true;
}

void CVideoPlayer::UploadYUVToGPU(DecodedFrame &frame)
{
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL) return;

	int uvWidth = (frame.width + 1) / 2;
	int uvHeight = (frame.height + 1) / 2;

	backend->UpdatePlaneTexture(texY, frame.yPlane, frame.width, frame.height, frame.yStride);
	backend->UpdatePlaneTexture(texU, frame.uPlane, uvWidth, uvHeight, frame.uStride);
	backend->UpdatePlaneTexture(texV, frame.vPlane, uvWidth, uvHeight, frame.vStride);

	if (frame.hasAlpha && texA != NULL && frame.aPlane)
		backend->UpdatePlaneTexture(texA, frame.aPlane, frame.width, frame.height, frame.aStride);
}

void CVideoPlayer::DestroyGPUTextures()
{
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL) return;

	backend->DeletePlaneTexture(texY); texY = NULL;
	backend->DeletePlaneTexture(texU); texU = NULL;
	backend->DeletePlaneTexture(texV); texV = NULL;
	backend->DeletePlaneTexture(texA); texA = NULL;
	gpuTexturesCreated = false;
}

// ============================================================================
// RGBA render-to-texture output (Task 10)
// ============================================================================
void *CVideoPlayer::GetRGBATexture() const
{
	return rgbaTarget ? rgbaTarget->GetTexture() : NULL;
}

void CVideoPlayer::RenderFrameToRGBATexture(DecodedFrame &frame)
{
	// No GL-capability guard here any more: this whole path now goes through the
	// backend (plane textures, render target, converter), so it works on Metal
	// too. The remaining guards are on the LEGACY GL-only paths below.
	if (frame.width <= 0 || frame.height <= 0)
		return;

	if (!rgbaShader)
	{
		// From the backend, so this path runs on whichever renderer is live.
		rgbaShader = VID_GetRenderBackend()->CreateVideoYUVConverter();
		if (!rgbaShader)
		{
			LOGError("CVideoPlayer: render backend '%s' provides no YUV converter; "
					 "video will decode but not display", VID_GetCurrentRenderBackendName());
			return;
		}
		rgbaShader->Compile();
	}

	const bool is10Bit = (frame.bytesPerSample == 2);
	const bool isNV12 = frame.chromaInterleaved;
	const EYUVShaderMode mode = is10Bit ? EYUVShaderMode::YUV420P10
							   : isNV12 ? EYUVShaderMode::NV12
										: EYUVShaderMode::YUV420_3Plane;

	const int uvWidth = (frame.width + 1) / 2;
	const int uvHeight = frame.chromaFullHeight ? frame.height : (frame.height + 1) / 2;

	// (Re)create the plane textures whenever dimensions, sampling mode, or
	// alpha presence changed -- mirrors CreateGPUTextures()'s "recreate on
	// resolution change" contract, generalized to also cover format changes
	// (a mid-stream pixel-format change is unlikely but StoreDecodedFrame
	// makes no promise it can't happen).
	bool needsRecreate = (rgbaTexWidth != frame.width || rgbaTexHeight != frame.height ||
						  rgbaTexMode != mode || rgbaTexHasAlpha != frame.hasAlpha ||
						  rgbaTexY == NULL);
	if (needsRecreate)
	{
		LOGD("CVideoPlayer::RenderFrameToRGBATexture: (re)creating plane textures %dx%d mode=%s%s",
			 frame.width, frame.height,
			 (mode == EYUVShaderMode::YUV420P10 ? "YUV420P10" : mode == EYUVShaderMode::NV12 ? "NV12" : "YUV420_3Plane"),
			 frame.hasAlpha ? " +alpha" : "");
		DestroyRGBATextures();

		// Through the backend, so the same code allocates GL textures on OpenGL
		// and MTLTextures on Metal. bytesPerChannel 2 is the 10-bit case (the
		// raw 0..1023 code word LSB-aligned in 16 bits); NV12's chroma plane is
		// the only two-channel one.
		CRenderBackend *backend = VID_GetRenderBackend();
		const int bpc = is10Bit ? 2 : 1;

		rgbaTexY = backend->CreatePlaneTexture(frame.width, frame.height, 1, bpc);
		rgbaTexU = isNV12 ? backend->CreatePlaneTexture(uvWidth, uvHeight, 2, 1)
						  : backend->CreatePlaneTexture(uvWidth, uvHeight, 1, bpc);
		if (!isNV12)
			rgbaTexV = backend->CreatePlaneTexture(uvWidth, uvHeight, 1, bpc);
		if (frame.hasAlpha)
			rgbaTexA = backend->CreatePlaneTexture(frame.width, frame.height, 1, 1);

		if (rgbaTexY == NULL || rgbaTexU == NULL)
		{
			LOGError("CVideoPlayer::RenderFrameToRGBATexture: plane texture allocation failed");
			return;
		}

		rgbaTexWidth = frame.width;
		rgbaTexHeight = frame.height;
		rgbaTexMode = mode;
		rgbaTexHasAlpha = frame.hasAlpha;
	}

	// Upload plane data. frame.yStride/uStride/vStride are byte strides
	// (StoreDecodedFrame always packs them tightly: width * bytesPerSample,
	// no row padding), so dividing by bytesPerSample recovers GL's
	// pixel-count GL_UNPACK_ROW_LENGTH. NV12's interleaved plane is 2
	// bytes/pixel (R+G) regardless of bytesPerSample (NV12 is 8-bit only).
	// Strides are BYTE strides; the backend's UpdatePlaneTexture takes them as
	// bytes and converts to whatever its API wants (GL needs a pixel count for
	// GL_UNPACK_ROW_LENGTH, Metal wants bytesPerRow directly).
	CRenderBackend *uploadBackend = VID_GetRenderBackend();

	uploadBackend->UpdatePlaneTexture(rgbaTexY, frame.yPlane, frame.width, frame.height, frame.yStride);

	if (isNV12)
	{
		uploadBackend->UpdatePlaneTexture(rgbaTexU, frame.uPlane, uvWidth, uvHeight, frame.uStride);
	}
	else
	{
		uploadBackend->UpdatePlaneTexture(rgbaTexU, frame.uPlane, uvWidth, uvHeight, frame.uStride);
		uploadBackend->UpdatePlaneTexture(rgbaTexV, frame.vPlane, uvWidth, uvHeight, frame.vStride);
	}

	if (frame.hasAlpha && rgbaTexA != NULL && frame.aPlane)
	{
		uploadBackend->UpdatePlaneTexture(rgbaTexA, frame.aPlane, frame.width, frame.height, frame.aStride);
	}

	// Render target sized to *display* dims (post-rotation) -- so
	// GetRGBATexture() is already display-oriented and Plan 2 never has to
	// think about rotation. See CVideoYUVShader::RenderToTarget's uRotation
	// comment (CVideoYUVShader.cpp) for the UV-space rotation this relies on.
	const int dispW = GetDisplayWidth();
	const int dispH = GetDisplayHeight();
	if (!rgbaTarget)
	{
		rgbaTarget = VID_GetRenderBackend()->CreateRenderTarget();
		if (!rgbaTarget)
		{
			LOGError("CVideoPlayer: render backend '%s' provides no render target; "
					 "video will decode but not display", VID_GetCurrentRenderBackendName());
			return;
		}
	}
	// S-5 Phase 5: WHAT THIS CLIP NEEDS.
	//
	// Float only when the clip actually carries above-white data (PQ/HLG) AND
	// the app's session gate is open AND the backend can render float. Any
	// other combination gets the RGBA8 target and the shader's tone-mapped arm
	// -- which is CORRECT, just not bright, and is what most users see.
	CRenderBackend *renderBackend = VID_GetRenderBackend();
	SVideoHdrOutput hdrOut;
	hdrOut.colorTrc = colorTrc.load(std::memory_order_relaxed);
	const bool wantFloat =
		VideoTransfer::IsHdrTrc(hdrOut.colorTrc) &&
		hdrGateOpen.load(std::memory_order_relaxed) &&
		renderBackend != NULL &&
		renderBackend->SupportsTextureFormat(RENDER_TEXTURE_RGBA16F);
	hdrOut.floatTarget = wantFloat;
	if (renderBackend != NULL)
	{
		hdrOut.surfaceIsLinear = renderBackend->GetSurfaceIsLinearColorSpace();
		hdrOut.surfaceIsP3 =
			(VID_GetMainWindowRenderColorGamut() == VID_DISPLAY_COLOR_GAMUT_DISPLAY_P3);
		// LIVE headroom, sampled here rather than cached -- the poster's 8-bit
		// conversion samples it at the moment of the map for the same reason
		// (macOS grants it lazily and keeps moving it with display
		// brightness), and a cached value here would disagree with the poster.
		hdrOut.toneMapHeadroom = renderBackend->GetDisplayHdrHeadroom();
	}

	// Create() is itself a no-op when the size AND FORMAT already match, so it
	// is called unconditionally rather than behind a second, weaker guard. The
	// guard here used to compare dimensions only, which was correct while
	// there was one possible format and became a latent bug the moment there
	// were two: a target switching RGBA8 -> RGBA16F at the SAME size would
	// never have been re-created, and would have gone on clamping away exactly
	// the above-white values the switch was made for (S-5 Phase 5).
	{
		if (!rgbaTarget->Create(dispW, dispH,
								wantFloat ? RENDER_TEXTURE_RGBA16F : RENDER_TEXTURE_RGBA8))
		{
			// CGLRenderTarget::Create() already LOGError()s the GL failure
			// reason (FBO incompleteness); skip the render rather than call
			// RenderToTarget() on a target whose fbo is 0 -- GetRGBATexture()
			// keeps returning 0 (its documented "no frame yet" contract)
			// instead of silently binding the default framebuffer.
			LOGError("CVideoPlayer::RenderFrameToRGBATexture: render target creation failed at %dx%d, skipping frame", dispW, dispH);
			return;
		}
	}

	rgbaShader->RenderToTarget(mode,
							   (void *)(uintptr_t)rgbaTexY, (void *)(uintptr_t)rgbaTexU,
							   (void *)(uintptr_t)rgbaTexV,
							   frame.hasAlpha, (void *)(uintptr_t)rgbaTexA,
							   vpxColorSpace, (vpxColorRange == 1),
							   rotationDegrees, lutTexture, lutEdge,
							   *rgbaTarget, hdrOut);
}

// ============================================================================
// SetColorLut3D (CM-E)
// ============================================================================
// Install/replace the display colour LUT sampled by the YUV shader after the
// matrix. rgba16 = edge^3 RGBA16 texels (alpha ignored), b-major (r fastest --
// matches glTexImage3D's x-fastest layout and texture(texLut, rgb)'s x=r).
// nullptr or edge < 2 clears the LUT (raw pass-through -- pre-CM-E behaviour).
// Render thread only (GL). The GL texture is owned by the player and deleted
// in FreeResources() beside rgbaTarget/rgbaShader -- deliberately NOT in
// DestroyRGBATextures(), which also fires on RenderFrameToRGBATexture()'s
// needsRecreate path (always taken on the first frame after Open, i.e. right
// AFTER the LUT was installed -- parking the LUT there would destroy it on
// the common path and every later draw would sample a deleted texture name).
// If a frame is currently displayed, re-runs the display step for it so a
// profile change repaints a paused clip without user input (the snapshot is
// reader-owned and RenderFrameToRGBATexture is self-contained on it).
// ============================================================================
// SetHdrPlaybackGate (S-5 Phase 5)
// ============================================================================
// The app's resolved HDR session gate. See the header for why the engine
// cannot answer this itself.
ERenderTextureFormat CVideoPlayer::GetRGBATextureFormat() const
{
	// Ask the target itself rather than re-deriving the decision. Re-computing
	// "would this clip want float?" here could drift from what was actually
	// created -- the backend may have refused float, or the gate may have been
	// resolved before the first frame -- and the consumer needs the truth about
	// the texture it is being handed, not the intent behind it.
	return rgbaTarget ? rgbaTarget->GetFormat() : RENDER_TEXTURE_RGBA8;
}

void CVideoPlayer::SetHdrPlaybackGate(bool gateOpen)
{
	hdrGateOpen.store(gateOpen, std::memory_order_relaxed);
}

void CVideoPlayer::SetColorLut3D(const u16 *rgba16, int edge)
{
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL) return;
	if (!outputRGBATexture)
		return;   // CPU/headless mode: no render target on this path, nothing to install

	if (rgba16 == nullptr || edge < 2)
	{
		backend->DeleteLutTexture3D(lutTexture);
		lutTexture = NULL;
		lutEdge = 0;
	}
	else
	{
		// Reuse the texture when the edge length is unchanged -- this is called
		// on every display-profile change, and a monitor drag can fire it
		// repeatedly.
		const bool reuse = (lutTexture != NULL && lutEdge == edge);
		if (!reuse && lutTexture != NULL)
		{
			backend->DeleteLutTexture3D(lutTexture);
			lutTexture = NULL;
		}
		if (lutTexture == NULL)
			lutTexture = backend->CreateLutTexture3D(edge);
		backend->UpdateLutTexture3D(lutTexture, rgba16, edge);
		lutEdge = edge;
	}

	// Paused repaint: make the change visible without user input (the
	// FPV-parity event is dragging the window to a differently-profiled
	// monitor while a clip sits paused).
	if (displaySnapshotValid)
		RenderFrameToRGBATexture(displaySnapshot);
}

void CVideoPlayer::DestroyRGBATextures()
{
	// Backend-routed, so this frees GL names on OpenGL and releases MTLTextures
	// on Metal. No GL-capability guard: these handles came from the backend, so
	// they must go back to it whichever one is live.
	CRenderBackend *backend = VID_GetRenderBackend();
	if (backend == NULL)
		return;
	if (rgbaTexY) { backend->DeletePlaneTexture(rgbaTexY); rgbaTexY = NULL; }
	if (rgbaTexU) { backend->DeletePlaneTexture(rgbaTexU); rgbaTexU = NULL; }
	if (rgbaTexV) { backend->DeletePlaneTexture(rgbaTexV); rgbaTexV = NULL; }
	if (rgbaTexA) { backend->DeletePlaneTexture(rgbaTexA); rgbaTexA = NULL; }
}

// ============================================================================
// GetCurrentFrameRGBA - CPU fallback
// ============================================================================
u8 *CVideoPlayer::GetCurrentFrameRGBA()
{
	// Convert the reader-owned snapshot DisplayFrame() handed us, NOT the ring
	// slot at frameReadIdx - 1 (Task 2 finding a): that slot is EMPTY and inside
	// the writer's window, so the now-persistent decode thread can memcpy fresh
	// planes into it while this conversion walks them -- a torn frame. The
	// snapshot is only ever touched by this (reader) thread.
	if (!displaySnapshotValid)
		return nullptr;

	DecodedFrame &frame = displaySnapshot;

	if (frame.width <= 0 || frame.height <= 0)
		return nullptr;

	int requiredSize = frame.width * frame.height * 4;
	if (rgbaBuffer == nullptr || rgbaBufferSize < requiredSize)
	{
		delete[] rgbaBuffer;
		rgbaBuffer = new u8[requiredSize];
		rgbaBufferSize = requiredSize;
	}

	ConvertYUV420ToRGBA(frame, rgbaBuffer);

	if (rotationDegrees == 0)
		return rgbaBuffer;

	// Rotate into a second scratch buffer sized for the (possibly swapped)
	// display dimensions; GetCurrentFrameRGBA() always returns display-space
	// pixels, matching GetDisplayWidth()/GetDisplayHeight().
	bool swapDims = (rotationDegrees == 90 || rotationDegrees == 270);
	int dstW = swapDims ? frame.height : frame.width;
	int dstH = swapDims ? frame.width : frame.height;
	int rotatedRequired = dstW * dstH * 4;
	if (rotatedBuffer == nullptr || rotatedBufferSize < rotatedRequired)
	{
		delete[] rotatedBuffer;
		rotatedBuffer = new u8[rotatedRequired];
		rotatedBufferSize = rotatedRequired;
	}

	// RotateRGBA was promoted to VideoFrameTransform.h/.cpp (Task 9) so
	// CVideoFrameExtractor's poster-frame path can share the exact same
	// rotation math -- see that file for the full sign-convention rationale.
	VideoFrameTransform::RotateRGBA(rgbaBuffer, frame.width, frame.height, rotationDegrees, rotatedBuffer);
	return rotatedBuffer;
}

// ============================================================================
// GetCurrentFrameRGBASampled - decimated CPU conversion for stats consumers
// ============================================================================
u8 *CVideoPlayer::GetCurrentFrameRGBASampled(int maxPixels, int &outW, int &outH)
{
	outW = 0;
	outH = 0;

	// Same reader-owned-snapshot rationale as GetCurrentFrameRGBA() above.
	if (!displaySnapshotValid)
		return nullptr;

	DecodedFrame &frame = displaySnapshot;
	if (frame.width <= 0 || frame.height <= 0)
		return nullptr;

	if (maxPixels < 1)
		maxPixels = 1;

	// Square decimation step chosen so the sampled grid fits the budget:
	// ceil(sqrt(area/budget)) guarantees (w/step)*(h/step) <= maxPixels.
	int step = (int)ceil(sqrt((double)frame.width * (double)frame.height / (double)maxPixels));
	if (step < 1)
		step = 1;

	int sw = frame.width / step;
	int sh = frame.height / step;
	if (sw < 1) sw = 1;
	if (sh < 1) sh = 1;

	int requiredSize = sw * sh * 4;
	if (rgbaSampledBuffer == nullptr || rgbaSampledBufferSize < requiredSize)
	{
		delete[] rgbaSampledBuffer;
		rgbaSampledBuffer = new u8[requiredSize];
		rgbaSampledBufferSize = requiredSize;
	}

	ConvertYUV420ToRGBASampled(frame, rgbaSampledBuffer, step, step, sw, sh);

	// Coded orientation by contract (see header): rotationDegrees is NOT
	// applied here -- per-pixel stats don't care, and skipping the rotate
	// keeps this tap cheap.
	outW = sw;
	outH = sh;
	return rgbaSampledBuffer;
}

// ============================================================================
// ConvertYUV420ToRGBA
// ============================================================================
void CVideoPlayer::ConvertYUV420ToRGBA(const DecodedFrame &frame, u8 *outRGBA)
{
	ConvertYUV420ToRGBASampled(frame, outRGBA, 1, 1, frame.width, frame.height);
}

// ============================================================================
// ConvertYUV420ToRGBASampled - shared decimating conversion core
// ============================================================================
void CVideoPlayer::ConvertYUV420ToRGBASampled(const DecodedFrame &frame, u8 *outRGBA,
                                              int stepX, int stepY, int outW, int outH)
{
	// Select color matrix coefficients from the NORMALIZED color space value
	// (VPX_CS_* convention: 1 = BT.601, 2 = BT.709, 5 = BT.2020 ncl). CM-E:
	// kept in step with CVideoYUVShader's matrix branches -- this converter
	// feeds the histogram tap and the CPU display fallback, and it must
	// describe the same conversion the screen uses -- WITH ONE STATED
	// EXCEPTION since S-5 Phase 5: for an HDR clip this always applies the
	// TONE-MAPPED arm, even when the screen is showing the float one. That is
	// deliberate, not an oversight. This tap's output is 8-bit RGBA by
	// contract and its consumers (the video histogram, and GetCurrentFrameRGBA's
	// CPU display fallback) are display-referred, so there is nowhere for an
	// above-white value to go. The histogram therefore describes the clip as it
	// would look WITHOUT headroom, which is the honest reading of an 8-bit
	// histogram; the alternative -- describing values the 8-bit buckets cannot
	// represent -- would be worse.
	// BT.601:  Y' =  0.299R + 0.587G + 0.114B
	// BT.709:  Y' = 0.2126R + 0.7152G + 0.0722B
	// BT.2020: Y' = 0.2627R + 0.6780G + 0.0593B

	bool isBT709 = (vpxColorSpace == 2);   // VPX_CS_BT_709
	bool isBT2020 = (vpxColorSpace == 5);  // VPX_CS_BT_2020 (ncl)
	bool isFullRange = (vpxColorRange == 1); // VPX_CR_FULL_RANGE

	// Fixed-point coefficients (shifted by 16 bits)
	// BT.601 limited range:
	//   R = 1.164*(Y-16) + 1.596*(V-128)
	//   G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
	//   B = 1.164*(Y-16) + 2.017*(U-128)
	// BT.709 limited range:
	//   R = 1.164*(Y-16) + 1.793*(V-128)
	//   G = 1.164*(Y-16) - 0.213*(U-128) - 0.533*(V-128)
	//   B = 1.164*(Y-16) + 2.112*(U-128)
	// Full range: just use Y directly (no 16 offset, no 1.164 scaling)

	// Integer coefficients scaled by 65536 (1 << 16)
	int cY, cRV, cGU, cGV, cBU;
	int yOffset;

	if (isFullRange)
	{
		yOffset = 0;
		if (isBT2020)
		{
			cY  = 65536;  // 1.0
			cRV = 96640;  // 1.4746
			cGU = -10785; // -0.16455
			cGV = -37444; // -0.57135
			cBU = 123304; // 1.8814
		}
		else if (isBT709)
		{
			cY  = 65536;  // 1.0
			cRV = 103206; // 1.5748
			cGU = -12276; // -0.1873
			cGV = -30679; // -0.4681
			cBU = 121608; // 1.8556
		}
		else
		{
			// BT.601 full range
			cY  = 65536;  // 1.0
			cRV = 91881;  // 1.402
			cGU = -22554; // -0.3441
			cGV = -46802; // -0.7141
			cBU = 116130; // 1.772
		}
	}
	else
	{
		// Limited range
		yOffset = 16;
		if (isBT2020)
		{
			cY  = 76309;  // 1.164
			cRV = 110025; // 1.6787  (1.4746 * 255/224)
			cGU = -12276; // -0.1873 (0.16455 * 255/224)
			cGV = -42630; // -0.6504 (0.57135 * 255/224)
			cBU = 140364; // 2.1418  (1.8814 * 255/224)
		}
		else if (isBT709)
		{
			cY  = 76309;  // 1.164
			cRV = 117489; // 1.793
			cGU = -13975; // -0.213
			cGV = -34925; // -0.533
			cBU = 138438; // 2.112
		}
		else
		{
			// BT.601 limited range (default)
			cY  = 76309;  // 1.164
			cRV = 104597; // 1.596
			cGU = -25675; // -0.392
			cGV = -53279; // -0.813
			cBU = 132201; // 2.017
		}
	}

	// Task 5's EmitFrame can hand back 8-bit or 10-bit-in-16 samples, and
	// either 4:2:0 (chroma vertically halved) or 4:2:2 (chroma full-height)
	// subsampling, plus NV12's interleaved (rather than planar) U/V. Handle
	// all combinations here so this stays the single CPU YUV->RGBA path.
	const bool is10Bit = (frame.bytesPerSample == 2);
	const bool isNV12 = frame.chromaInterleaved;

	// Hoisted out of the pixel loop: one branch per FRAME, not per sample.
	// Zero means "not HDR, do nothing" and keeps the SDR path byte-identical
	// -- the integer matrix above is untouched for every BT.601/709 clip.
	const int hdrTrcForCpuConvert =
		VideoTransfer::IsHdrTrc(colorTrc.load(std::memory_order_relaxed))
			? colorTrc.load(std::memory_order_relaxed) : 0;
	// Matches the shader's uniform. VID_GetRenderBackend() can be NULL on the
	// headless/CPU path, where 1.0 makes the curve exactly the identity.
	float cpuToneMapHeadroom = 1.0f;
	if (hdrTrcForCpuConvert != 0)
	{
		CRenderBackend *b = VID_GetRenderBackend();
		if (b != NULL)
			cpuToneMapHeadroom = b->GetDisplayHdrHeadroom();
	}

	for (int orow = 0; orow < outH; orow++)
	{
		const int row = orow * stepY;
		const u8 *yRowBytes = frame.yPlane + row * frame.yStride;
		int cRow = frame.chromaFullHeight ? row : (row / 2);
		const u8 *uRowBytes = frame.uPlane + cRow * frame.uStride;
		const u8 *vRowBytes = isNV12 ? nullptr : (frame.vPlane + cRow * frame.vStride);
		const u8 *aRow = (frame.hasAlpha && frame.aPlane) ? (frame.aPlane + row * frame.aStride) : nullptr;
		u8 *dst = outRGBA + (size_t)orow * outW * 4;

		for (int ocol = 0; ocol < outW; ocol++)
		{
			const int col = ocol * stepX;
			int ySample, uSample, vSample;

			if (is10Bit)
			{
				// 10-bit little-endian samples packed in 16-bit words -- take
				// the high 8 bits (>>2) as the 8-bit fallback RGBA display
				// value (lossy, but this is the CPU/no-GPU-upload path).
				const u16 *y16 = reinterpret_cast<const u16 *>(yRowBytes);
				ySample = (int)(y16[col] >> 2);
			}
			else
			{
				ySample = (int)yRowBytes[col];
			}

			int cCol = col / 2;
			if (isNV12)
			{
				// 8-bit only: interleaved U,V pairs.
				uSample = (int)uRowBytes[cCol * 2 + 0];
				vSample = (int)uRowBytes[cCol * 2 + 1];
			}
			else if (is10Bit)
			{
				const u16 *u16p = reinterpret_cast<const u16 *>(uRowBytes);
				const u16 *v16p = reinterpret_cast<const u16 *>(vRowBytes);
				uSample = (int)(u16p[cCol] >> 2);
				vSample = (int)(v16p[cCol] >> 2);
			}
			else
			{
				uSample = (int)uRowBytes[cCol];
				vSample = (int)vRowBytes[cCol];
			}

			int y = ySample - yOffset;
			int u = uSample - 128;
			int v = vSample - 128;

			int r = (cY * y + cRV * v + 32768) >> 16;
			int g = (cY * y + cGU * u + cGV * v + 32768) >> 16;
			int b = (cY * y + cBU * u + 32768) >> 16;

			// S-5 Phase 5: THE SAME TRANSFER THE SCREEN APPLIES.
			//
			// This converter's contract (see GetCurrentFrameRGBASampled's
			// comment) is that it "must describe the same conversion the
			// screen uses" -- and the moment the shader learned the PQ/HLG
			// EOTF, that contract broke SILENTLY: the histogram would have gone
			// on describing the old washed-out picture while the screen showed
			// the corrected one, and a wrong histogram looks exactly like a
			// histogram. There is no way to notice by looking.
			//
			// So the same maths runs here. This is the 8-bit arm in every case
			// -- the tap's output is 8-bit RGBA by contract, and its consumers
			// (the video histogram, and GetCurrentFrameRGBA's CPU display
			// fallback) both want display-referred bytes -- so it mirrors the
			// shader's tone-mapped branch, not its float one.
			if (hdrTrcForCpuConvert != 0)
			{
				// Table-driven, same as the poster lane's bulk converter. The
				// tap is decimated so the volume is far lower than a poster's,
				// but it runs EVERY FRAME while an HDR clip plays, which the
				// poster does not -- so the powf cost would be paid 25-60
				// times a second instead of once.
				//
				// The samples arrive 0..255 here (the 10-bit planes are
				// shifted down before the matrix above), so they are scaled to
				// the table's 16-bit index. Clamped because the integer matrix
				// can overshoot either end before the clamp below.
				const int ri = (r < 0) ? 0 : ((r > 255) ? 65535 : r * 257);
				const int gi = (g < 0) ? 0 : ((g > 255) ? 65535 : g * 257);
				const int bi = (b < 0) ? 0 : ((b > 255) ? 65535 : b * 257);
				const u16 code[3] = { (u16)ri, (u16)gi, (u16)bi };
				float lin[3];
				VideoTransfer::HdrCodeToLinearSrgb(code, hdrTrcForCpuConvert, lin);
				for (int c = 0; c < 3; c++)
				{
					const float t = VideoTransfer::ToneMapReinhard(lin[c], cpuToneMapHeadroom);
					lin[c] = SrgbExtendedEncode(t);
				}
				r = (int)(lin[0] * 255.0f + 0.5f);
				g = (int)(lin[1] * 255.0f + 0.5f);
				b = (int)(lin[2] * 255.0f + 0.5f);
			}

			// Clamp to [0, 255]
			if (r < 0) r = 0; else if (r > 255) r = 255;
			if (g < 0) g = 0; else if (g > 255) g = 255;
			if (b < 0) b = 0; else if (b > 255) b = 255;

			dst[0] = static_cast<u8>(r);
			dst[1] = static_cast<u8>(g);
			dst[2] = static_cast<u8>(b);
			dst[3] = aRow ? aRow[col] : 255;
			dst += 4;
		}
	}
}

// ============================================================================
// Ring buffer management
// ============================================================================
void CVideoPlayer::ClearRingBuffer()
{
	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		frameBuffer[i].stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_relaxed);
		frameBuffer[i].generation = 0;
		frameBuffer[i].absIdx = 0;
	}
	frameWriteIdx.store(0, std::memory_order_release);
	frameReadIdx.store(0, std::memory_order_release);
}

// ============================================================================
// FreeResources
// ============================================================================
void CVideoPlayer::FreeResources()
{
	LOGD("CVideoPlayer::FreeResources");

	// Destroy the video source (nestegg/vpx/opus, or whichever source is active)
	if (source)
	{
		source->Close();
		delete source;
		source = nullptr;
	}

	// Leak guard for the test-only seam: an injected source that was never
	// consumed by Open() (SetTestVideoSource() called, then Close()/destructor
	// without an Open()) is abandoned -- we own it, so free it. Open() itself
	// consumes (and clears) testVideoSource BEFORE its internal Close() call,
	// so a normal injected-Open() never hits this path.
	if (testVideoSource)
	{
		delete testVideoSource;
		testVideoSource = nullptr;
	}

	// Leak guard for the async-open seam: a preopened source that was never
	// consumed by Open() (SetPreopenedSource() called, then Close()/destructor
	// without an Open()) is abandoned -- we own it, so free it. Open() itself
	// consumes (and clears) preopenedSource BEFORE its internal Close() call,
	// so a normal preopened-Open() never hits this path.
	if (preopenedSource)
	{
		delete preopenedSource;
		preopenedSource = nullptr;
	}

	// Free frame buffer planes
	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		delete[] frameBuffer[i].yPlane;  frameBuffer[i].yPlane = nullptr;
		delete[] frameBuffer[i].uPlane;  frameBuffer[i].uPlane = nullptr;
		delete[] frameBuffer[i].vPlane;  frameBuffer[i].vPlane = nullptr;
		delete[] frameBuffer[i].aPlane;  frameBuffer[i].aPlane = nullptr;
		frameBuffer[i].yCapacity = frameBuffer[i].uCapacity = frameBuffer[i].vCapacity = frameBuffer[i].aCapacity = 0;
		frameBuffer[i].stateWord.store(DecodedFrame::SLOT_EMPTY, std::memory_order_relaxed);
		frameBuffer[i].generation = 0;
		frameBuffer[i].absIdx = 0;
	}
	frameWriteIdx.store(0, std::memory_order_release);
	frameReadIdx.store(0, std::memory_order_release);

	// The last-displayed snapshot owns whichever plane buffers DisplayFrame()
	// swapped out of the ring (Task 2) -- free them here too.
	delete[] displaySnapshot.yPlane;  displaySnapshot.yPlane = nullptr;
	delete[] displaySnapshot.uPlane;  displaySnapshot.uPlane = nullptr;
	delete[] displaySnapshot.vPlane;  displaySnapshot.vPlane = nullptr;
	delete[] displaySnapshot.aPlane;  displaySnapshot.aPlane = nullptr;
	displaySnapshot.yCapacity = displaySnapshot.uCapacity = displaySnapshot.vCapacity = displaySnapshot.aCapacity = 0;
	displaySnapshot.width = 0;
	displaySnapshot.height = 0;
	displaySnapshotValid = false;

	// Destroy GPU textures
	DestroyGPUTextures();

	// Destroy RGBA render-to-texture resources (Task 10). outputRGBATexture
	// itself is left alone (like enableGPUUpload, it's a mode the caller set
	// before Open() and expects to persist across Close()/Open() cycles).
	DestroyRGBATextures();
	if (rgbaTarget)
	{
		rgbaTarget->Destroy();
		delete rgbaTarget;
		rgbaTarget = nullptr;
	}
	if (rgbaShader)
	{
		delete rgbaShader;
		rgbaShader = nullptr;
	}
	// CM-E: the colour LUT dies here, with the player -- NOT in
	// DestroyRGBATextures() (see SetColorLut3D's comment: that also fires on
	// the per-frame needsRecreate path, which would destroy a just-installed
	// LUT on the first frame after every Open).
	if (lutTexture != NULL)
	{
		CRenderBackend *backend = VID_GetRenderBackend();
		if (backend != NULL)
			backend->DeleteLutTexture3D(lutTexture);
		lutTexture = NULL;
	}
	lutEdge = 0;
	rgbaTexWidth = 0;
	rgbaTexHeight = 0;
	rgbaTexMode = EYUVShaderMode::YUV420_3Plane;
	rgbaTexHasAlpha = false;

	// Remove audio channel from mixer (if still registered) before deleting
	if (audioChannel)
	{
		if (audioChannel->isActive)
		{
			SND_RemoveChannel(audioChannel);
		}
		delete audioChannel;
		audioChannel = nullptr;
	}

	// Free RGBA fallback buffers
	if (rgbaBuffer)
	{
		delete[] rgbaBuffer;
		rgbaBuffer = nullptr;
		rgbaBufferSize = 0;
	}
	if (rotatedBuffer)
	{
		delete[] rotatedBuffer;
		rotatedBuffer = nullptr;
		rotatedBufferSize = 0;
	}
	if (rgbaSampledBuffer)
	{
		delete[] rgbaSampledBuffer;
		rgbaSampledBuffer = nullptr;
		rgbaSampledBufferSize = 0;
	}

	// Reset metadata
	hasAudio = false;
	hasAlpha = false;
	videoWidth = 0;
	videoHeight = 0;
	duration = 0.0;
	fps = 30.0;
	currentTime = 0.0;
	rotationDegrees = 0;
}

// ============================================================================
// GetCurrentTime -- locked snapshot of the playback clock (Task 2)
// The clock triplet is written by BOTH the render thread (Update()'s A/V-sync
// advance) and the decode thread (a serviced seek re-bases it), so a bare read
// of a non-atomic double would be a data race. `mutex` is the innermost lock
// here -- never call this while holding it.
// ============================================================================
double CVideoPlayer::GetCurrentTime() const
{
	mutex->Lock();
	double t = currentTime;
	mutex->Unlock();
	return t;
}

// ============================================================================
// GetWallTime
// ============================================================================
double CVideoPlayer::GetWallTime() const
{
	auto now = std::chrono::high_resolution_clock::now();
	auto epoch = now.time_since_epoch();
	return std::chrono::duration<double>(epoch).count();
}
