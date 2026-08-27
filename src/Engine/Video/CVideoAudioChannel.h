#ifndef _CVIDEOCHANNEL_H_
#define _CVIDEOCHANNEL_H_

#include "CAudioChannel.h"
#include <vector>
#include <atomic>
#include <cstdint>

// Interpolation used when resampling video audio to the engine sample rate.
// Values are persisted by hosts (PhotoCruise setting) -- do not renumber.
enum class EVideoAudioInterpolation : int
{
	Nearest = 0,
	Linear  = 1,
	Cubic   = 2,   // 4-point Catmull-Rom
	Sinc    = 3    // 8-point Hann-windowed sinc
};

class CVideoAudioChannel : public CAudioChannel
{
public:
	CVideoAudioChannel();
	virtual ~CVideoAudioChannel();

	// Called by CVideoPlayer decode thread -- push decoded PCM (float
	// interleaved) at sourceSampleRate. sourceSampleRate <= 0 falls back to
	// 48000 (the historic assumption; kept as the default arg for source
	// compatibility with pre-2026-07 callers).
	void PushSamples(const float *interleavedSamples, int numFrames, int numChannels,
	                 int sourceSampleRate = 48000);

	// Resample interpolation quality. Safe to call from the main thread while
	// the decode thread pushes (atomic; takes effect on the next push block).
	void SetInterpolation(EVideoAudioInterpolation m) { interpolation.store((int)m, std::memory_order_relaxed); }
	EVideoAudioInterpolation GetInterpolation() const { return (EVideoAudioInterpolation)interpolation.load(std::memory_order_relaxed); }

	// Called by engine audio mixer -- S16 stereo interleaved (packed pairs in int*)
	virtual void MixIn(int *mixBuffer, u32 numSamples, int numAudioChannels) override;

	// --- Seek-pending gate -------------------------------------------------
	// Raised by CVideoPlayer::SubmitSeekCommandLocked() the instant a seek lands
	// in the mailbox, dropped by ServiceSeekOnDecodeThread() once the seek has
	// been serviced and no NEWER request is outstanding. Seeks are asynchronous:
	// between the submit (render thread) and the service (decode thread) the ring
	// still holds PRE-seek PCM, which must not be audible -- the user has already
	// left that position. While the gate is up MixIn() behaves exactly like the
	// muted path: it still CONSUMES the ring (the A/V clock behind
	// GetPlaybackPosition() keeps its semantics, and dropping the gate never
	// replays a backlog) but contributes SILENCE.
	//
	// Independent of isMuted -- the host's sticky mute is a separate, frozen
	// contract; either one alone silences the mix-in.
	void SetSeekPending(bool p) { seekPending.store(p, std::memory_order_release); }
	bool IsSeekPending() const { return seekPending.load(std::memory_order_acquire); }

	// Playback position for A/V sync (seconds): audio CONSUMED since the last
	// Reset(). Derived from the ring counters (readPos - epochBase), not from a
	// separate tally -- see the epochBase note below. FROZEN public contract.
	double GetPlaybackPosition();

	// Seconds of PCM currently BUFFERED (pushed but not yet consumed) --
	// (writePos - readPos), the mixer's own backlog. Used by the decode
	// thread's audio pump-ahead (2026-07-18): when this runs low while the
	// video ring is full, the source demuxes forward for audio only, so a
	// lazily-interleaved container (real-world ASF/WMV) can't starve the
	// consumption-driven A/V clock. ANY THREAD; a Reset() racing the two
	// loads can produce a torn pair -- guarded exactly like MixIn()'s
	// framesAvail (anything over capacity reads as 0, i.e. "low", which at
	// worst pumps once more).
	double GetBufferedSeconds();

	// Reset ring buffer (for seek). DECODE-THREAD ONLY -- see the note on
	// scratchL/scratchR below.
	void Reset();

	// TEST-ONLY probe. Positions are monotonically increasing counters (indexed
	// mod ringBufferCapacity), so the ring invariant is
	//   epochBase <= readPos <= writePos  &&  writePos - readPos <= ringBufferCapacity.
	// Only meaningful when QUIESCENT (no MixIn()/PushSamples()/Reset() in
	// flight): Reset() re-bases three separate atomics, so a concurrent observer
	// can legitimately catch a torn intermediate state. Used by PhotoCruise's
	// CTestVideoSeek to prove that a Reset() racing a MixIn() quantum can never
	// leave a stale readPos standing against a re-based writePos.
	bool DebugRingInvariantHolds() const;

	// TEST-ONLY stall hook. When set, MixIn() calls it once per quantum in the
	// window a decode-thread Reset() has to race: AFTER the entry load of
	// readPos/writePos and the mix/drain, and immediately BEFORE the publishing
	// CAS. PhotoCruise's CTestVideoSeek uses it to make that interleaving
	// DETERMINISTIC (the epoch-boundary case is a handful of instructions wide;
	// a spin loop reproduces it only by luck).
	//
	// Guarded by PC_TEST_STAT_COUNTER -- PhotoCruise's own build-local
	// test-seam macro (see that repo's CMakeLists.txt PC_ENABLE_TEST_SEAMS
	// option and its Xcode/VS Debug configs) -- so this setter carries NO
	// symbol at all in a store-shipped PhotoCruise build: a public setter
	// that installs a callback invoked on the audio mixer thread has no
	// business existing in a commercial, App-Store-shipped binary.
	//
	// Defined INLINE, right here, rather than declared here and defined
	// out-of-line in the .cpp: this engine is built ONCE, separately, as a
	// prebuilt static library, and the engine's own build never defines
	// PC_TEST_STAT_COUNTER (that macro belongs to PhotoCruise's build, not
	// this one). An out-of-line definition guarded the same way would either
	// always be compiled out of the engine .a regardless of which
	// PhotoCruise config later links against it (defeating the point of a
	// test build ever having it), or -- if some other engine TU could
	// somehow see it differently -- an ODR mismatch between what the
	// prebuilt .a contains and what a PhotoCruise TU's header view expects.
	// Living entirely inline sidesteps that: the code exists only in
	// whichever PhotoCruise TUs compile it, matching PhotoCruise's own
	// PC_TEST_STAT_COUNTER state, never the engine's.
	//
	// debugPreCasHook/debugPreCasHookArg below, and the MixIn() call site,
	// stay UNCONDITIONAL (not similarly guarded) -- see the member comment
	// for why gating those would hit the same cross-build hazard, but for
	// class layout instead of a function symbol.
#ifdef PC_TEST_STAT_COUNTER
	void SetDebugMixInPreCasHook(void (*fn)(void *userData), void *userData)
	{
		if (fn)
		{
			// SETTING: publish the payload (arg) before the flag (fn) -- this
			// mirrors MixIn()'s load order (fn acquire, then arg acquire), so
			// a mixer that observes the new fn is guaranteed to also observe
			// the new arg underneath it.
			debugPreCasHookArg.store(userData, std::memory_order_release);
			debugPreCasHook.store(fn, std::memory_order_release);
		}
		else
		{
			// CLEARING: drop the flag (fn) FIRST, then the payload (arg) --
			// the REVERSE of SETTING, deliberately. A mixer that already
			// loaded the old, live fn (acquire) before this call runs must
			// never be able to then load a cleared/dead arg out from under
			// it: storing fn first means that once a mixer can no longer
			// observe a non-null fn, the arg behind it is free to change.
			// (Storing arg first -- the bug this replaces -- lets a mixer
			// that already has the old non-null fn load the NEW null arg and
			// invoke the hook with a null userData.)
			debugPreCasHook.store(nullptr, std::memory_order_release);
			debugPreCasHookArg.store(nullptr, std::memory_order_release);
		}
	}
#endif

private:
	// Ring buffer stores stereo S16 samples at engine sample rate
	// Layout: [L0, R0, L1, R1, ...] as int16_t values
	std::vector<int16_t> ringBuffer;

	// STRICTLY MONOTONIC counters of int16_t samples, indexed mod
	// ringBufferCapacity. They are NOT zeroed by Reset(): Reset() RE-BASES them
	// forward (see Reset()), and that is load-bearing, not cosmetic --
	//
	//   MixIn() publishes its consumption with a CAS against the readPos it read
	//   at ENTRY, so that a Reset() landing mid-quantum makes the mixer abandon
	//   the quantum instead of storing an old epoch's readPos over a re-based
	//   ring. A CAS is only as good as the uniqueness of the value it compares:
	//   with a ZEROING Reset() the mixer's rpEntry == 0 is ambiguous (it is both
	//   "fresh epoch" and "old epoch that had consumed nothing yet" -- which is
	//   the state of EVERY epoch for its whole first consuming quantum), and
	//   Reset() storing 0 over an already-0 readPos leaves the CAS unable to
	//   tell them apart. It then SUCCEEDS across the Reset() and strands
	//   readPos > writePos: hasSpace()'s (wp - rp) underflows, PushSamples()
	//   drops every sample, and the clip is silent until a later seek heals it.
	//   Two seeks in quick succession (an arrow-key seek burst) is all it takes.
	//
	//   Because every Reset() jumps the pair PAST the old writePos, no position
	//   value is ever revisited, so a stale rpEntry can never compare equal to a
	//   live readPos and the CAS always fails when it must.
	std::atomic<uint64_t> writePos{0};
	std::atomic<uint64_t> readPos{0};

	// readPos at the start of the current epoch (i.e. as of the last Reset()).
	// The A/V clock is (readPos - epochBase) / 2 frames -- one derived value, so
	// there is no second tally to keep in sync with the mixer's CAS. (There used
	// to be a separate framesPlayed atomic holding exactly readPos / 2; nothing
	// ordered its fetch_add against Reset()'s store, so a mixer preempted between
	// the two could resurrect a quantum into a freshly zeroed clock and leave the
	// video running permanently ahead of the audio.)
	//
	// WRITE ORDER in Reset(): epochBase, then writePos, then readPos, all
	// release. READ ORDER everywhere else: readPos (acquire) FIRST, then
	// writePos / epochBase (acquire). Observing a re-based readPos therefore
	// implies both of the others are visible -- a reader can never pair a new
	// readPos with a stale writePos or a stale base.
	std::atomic<uint64_t> epochBase{0};

	size_t ringBufferCapacity = 0;  // in int16_t samples (frames * 2)

	int engineSampleRate = 0;
	int lastSourceSampleRate = 0;   // ratio rebuilt when a push's rate differs
	double resampleRatio = 1.0;     // source frames advanced per output frame
	double resamplePos = 0.0;       // fractional read position, in source frames,
	                                 // relative to the first frame of the CURRENT
	                                 // push block (may be negative: still inside
	                                 // the history carried from the previous block)
	bool needsResample = false;

	std::atomic<int> interpolation{(int)EVideoAudioInterpolation::Cubic};

	// kHistoryFrames source frames carried across PushSamples() blocks so
	// Cubic (1 left / 2 right taps) and Sinc (3 left / 4 right taps) stay
	// continuous at block boundaries. 8 >= 3 (max left) + 4 (max right) + 1.
	//
	// DECODE-THREAD-OWNED, and so are lastSourceSampleRate / resampleRatio /
	// resamplePos / needsResample above: the ONLY writers are PushSamples() and
	// ResetResampleState(), and the only caller of the latter is Reset(), which
	// CVideoPlayer now invokes from ServiceSeekOnDecodeThread() -- i.e. the same
	// thread as PushSamples(). Deliberately NOT atomic and NOT locked. (Before
	// the async-seek work the UI thread's blocking Seek() mutated this state; it
	// was safe only because that Seek() first JOINED the decode thread. With the
	// decode thread now persistent, that cross-thread mutation would have become
	// a real data race -- hence the move to decode-thread ownership. The mixer
	// thread never touches any of it; MixIn() needs the atomic gate instead.)
	static constexpr int kHistoryFrames = 8;
	std::vector<float> scratchL, scratchR;  // [history | current block]

	// Audible-silence gate (see SetSeekPending). Mixer-thread READS only; the
	// writers are the render thread (submit) and the decode thread (service),
	// both serialized under CVideoPlayer::cmdMutex.
	std::atomic<bool> seekPending{false};

	// TEST-ONLY payload for SetDebugMixInPreCasHook() above (guarded there by
	// PC_TEST_STAT_COUNTER). Null in production. UNCONDITIONAL on purpose,
	// unlike the setter: gating these would make this class's layout depend
	// on whether the TU compiling this header is the engine's own build
	// (which never defines PC_TEST_STAT_COUNTER) or a PhotoCruise TU that
	// does -- i.e. the engine's prebuilt .a and a PhotoCruise test TU could
	// disagree on sizeof(CVideoAudioChannel), corrupting anything laid out
	// after these members. Two atomics and a null check in MixIn() is a
	// negligible, already-accepted cost (see the call site) -- not worth
	// that hazard.
	std::atomic<void (*)(void *)> debugPreCasHook{nullptr};
	std::atomic<void *> debugPreCasHookArg{nullptr};

	void ResetResampleState();
};

#endif
