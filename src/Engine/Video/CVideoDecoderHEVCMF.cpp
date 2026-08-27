// Native HEVC decode via Windows Media Foundation. Drives the HEVC decoder
// through the Topology/IMFMediaSession pipeline (Sample Grabber Sink) rather
// than a raw IMFTransform push/pull loop.
//
// WHY THE TOPOLOGY/SESSION APPROACH (not raw IMFTransform): an earlier
// version of this class drove the decoder MFT directly with
// ProcessInput()/ProcessOutput(), self-allocating output samples per
// MFT_OUTPUT_STREAM_INFO. On this VM's "HEVC Video Extensions" software MFT,
// that consistently produced a sample that ProcessOutput() reported as
// successfully filled, yet had zero buffers attached by the time it reached
// the caller (confirmed with GetBufferCount()==0 immediately after a
// successful ProcessOutput() call that returned our own sample pointer
// unchanged) -- across three independent, individually well-reasoned fixes
// (restricting the "MFT provides its own samples" check to the mandatory
// MFT_OUTPUT_STREAM_PROVIDES_SAMPLES flag; switching self-allocation from a
// flat MFCreateMemoryBuffer to a stride-aware MFCreate2DMediaBuffer; falling
// back to IMFSample::GetBufferByIndex(0) instead of ConvertToContiguousBuffer)
// with MF_TRANSFORM_ASYNC confirmed 0 (genuinely synchronous, ruling out the
// async-MFT-driven-synchronously failure mode too). Rather than keep
// guessing at this specific MFT's quirk, this version instead uses Media
// Foundation's own high-level pipeline (Topology + IMFMediaSession + a
// built-in Sample Grabber Sink), which resolves the decoder MFT, negotiates
// its output type and allocates/owns its buffers internally -- the same
// machinery Windows' own media playback stack relies on. The Sample Grabber
// Sink's OnProcessSample() callback also hands decoded frames back as a
// flat `const BYTE*`/`DWORD size` pair directly, sidestepping the
// IMFSample/IMFMediaBuffer/IMF2DBuffer2 lock dance entirely.
//
// PIPELINE SHAPE: CVideoSourceFFmpeg demuxes the container and hands this
// class Annex-B HEVC access units one at a time (DecodePacket()'s existing
// per-packet contract, unchanged). Since IMFMediaSession/Topology expects to
// pull compressed samples FROM a real IMFMediaSource rather than being fed
// packets from outside, this file implements a minimal custom push-mode
// IMFMediaSource (CHevcPushSource + CHevcPushStream) that hands DecodePacket's
// packets to the pipeline on demand (via IMFMediaStream::RequestSample()),
// keeping FFmpeg as the only demuxer -- Windows' own container parsers are
// never invoked. See the mutex/condvar-based handoff in SPipeline below for
// how DecodePacket() stays synchronous (one packet in, at most one frame out
// per call, matching IVideoPacketDecoder's contract) despite the
// Session driving RequestSample()/OnProcessSample() from its own internal
// worker threads.
//
// FLUSH: MFT_MESSAGE_COMMAND_FLUSH (the raw-IMFTransform version's Flush()
// mechanism) has no direct Topology/Session equivalent for discarding
// internally-buffered reordered frames. Flush() here instead tears down and
// rebuilds the entire pipeline (source, topology, session, sink) from
// scratch -- more expensive than a flush message, but correctness-safe and
// cheap enough for a Seek()-frequency operation.
#include "CVideoDecoderHEVCMF.h"

#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "MFComThreadGuard.h"
#include "CVideoTransferFunctions.h"
#include "DBG_Log.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <chrono>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfobjects.h>
#include <propvarutil.h>

using MFComThreadGuard::EnsureComInitializedForThisThread;
using MFComThreadGuard::EnsureMFStarted;

namespace
{
	// ============================================================================
	// CUnknownBase -- shared IUnknown refcount plumbing for the small set of
	// COM objects below; each derived class still implements its own
	// QueryInterface (the supported interface set differs per class).
	// ============================================================================
	class CUnknownBase
	{
	public:
		CUnknownBase() : refCount(1) {}
		virtual ~CUnknownBase() = default;

	protected:
		ULONG AddRefImpl() { return (ULONG)InterlockedIncrement(&refCount); }
		ULONG ReleaseImpl()
		{
			LONG c = InterlockedDecrement(&refCount);
			if (c == 0)
				delete this;
			return (ULONG)c;
		}

	private:
		LONG refCount;
	};

	// Forward declarations -- CVideoDecoderHEVCMF::SPipeline (defined right
	// below, out-of-line, providing the header's forward-declared nested
	// type) only needs POINTER members to these, so their full definitions
	// can come later in this file, once SPipeline itself is a complete type.
	class CHevcPushStream;
	class CHevcPushSource;
	class CHevcSampleGrabberCallback;
	class CHevcSessionEventCallback;

	using SPipeline = CVideoDecoderHEVCMF::SPipeline;
}

// ============================================================================
// CVideoDecoderHEVCMF::SPipeline -- out-of-line definition of the PIMPL type
// forward-declared in the header: the COM objects plus the mutex/condvar
// state that lets synchronous DecodePacket() calls hand packets to, and
// receive frames from, the Session's own worker threads. Defined here (not
// inside the anonymous namespace) so it stays exactly the type the header
// declares; must be complete before any code below does `pipeline->member`.
// ============================================================================
struct CVideoDecoderHEVCMF::SPipeline
{
	std::mutex mtx;
	std::condition_variable cv;

	// Packet handoff: DecodePacket() (producer) -> CHevcPushStream::RequestSample (consumer).
	bool packetAvailable = false;
	std::vector<u8> pktDataBuf;
	LONGLONG pktPts100ns = 0;
	bool pktIsEOS = false;
	// True while RequestSample() is blocked waiting for the *next*
	// packet -- DecodePacket() treats a fresh true here (after it just
	// handed over a packet) as "the pipeline wants more input before it
	// can produce a frame for what I just gave it", the Topology/Session
	// equivalent of the old code's MF_E_TRANSFORM_NEED_MORE_INPUT.
	bool streamWaitingForPacket = false;

	// Decoded output handoff: CHevcSampleGrabberCallback::OnProcessSample (producer) -> DecodePacket() (consumer).
	std::vector<u8> outDataBuf;
	LONGLONG outPts100ns = 0;
	bool hasOutput = false;

	bool shutdown = false;      // pipeline is tearing down -- all waits should give up
	bool sourceError = false;   // an async/session error occurred; see asyncErrorReason
	bool sessionEnded = false;  // MESessionEnded observed (EOS drained all the way through)
	std::string asyncErrorReason;

	IMFMediaSession *session = nullptr;
	IMFTopology *topology = nullptr;
	CHevcPushSource *source = nullptr;
	CHevcSampleGrabberCallback *grabberCallback = nullptr;
	CHevcSessionEventCallback *eventCallback = nullptr;
};

namespace
{
	// ============================================================================
	// CHevcPushStream -- the push source's single (and only) video stream.
	// RequestSample() is called by the Session's internal worker threads
	// whenever the pipeline wants the next compressed access unit; it blocks
	// (with a generous timeout, purely as a hang guard -- nothing else
	// contends for this thread pool in this single-source/single-sink
	// topology) on SPipeline's condvar until DecodePacket() supplies one.
	// ============================================================================
	class CHevcPushStream : public IMFMediaStream, public CUnknownBase
	{
	public:
		CHevcPushStream(std::shared_ptr<SPipeline> p, CHevcPushSource *src, IMFStreamDescriptor *sd)
			: pipeline(std::move(p)), source(src), streamDesc(sd)
		{
			streamDesc->AddRef();
			// `source` is intentionally a raw, non-AddRef'd back-pointer:
			// source owns (and outlives) this stream, so AddRef'ing it here
			// would create an uncollectable reference cycle.
		}

		~CHevcPushStream() override
		{
			streamDesc->Release();
			if (eventQueue)
				eventQueue->Release();
		}

		HRESULT InitInstance() { return MFCreateEventQueue(&eventQueue); }
		void ShutdownStream()
		{
			if (eventQueue)
				eventQueue->Shutdown();
		}

		// IUnknown
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaStream))
				*ppv = static_cast<IMFMediaStream *>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
		ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

		// IMFMediaEventGenerator
		HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent **ppEvent) override
		{
			return eventQueue->GetEvent(flags, ppEvent);
		}
		HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override
		{
			return eventQueue->BeginGetEvent(cb, state);
		}
		HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult *result, IMFMediaEvent **ppEvent) override
		{
			return eventQueue->EndGetEvent(result, ppEvent);
		}
		HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID extType, HRESULT status, const PROPVARIANT *value) override
		{
			return eventQueue->QueueEventParamVar(met, extType, status, value);
		}

		// IMFMediaStream
		HRESULT STDMETHODCALLTYPE GetMediaSource(IMFMediaSource **ppSource) override;
		HRESULT STDMETHODCALLTYPE GetStreamDescriptor(IMFStreamDescriptor **ppDesc) override
		{
			*ppDesc = streamDesc;
			streamDesc->AddRef();
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE RequestSample(IUnknown *pToken) override;

	private:
		// Co-owns the pipeline (see CVideoDecoderHEVCMF::pipeline's header note):
		// RequestSample() runs on an MF worker thread and touches pipeline->mtx/cv,
		// so the struct must outlive any in-flight call even after TeardownPipeline()
		// drops the decoder's own reference.
		std::shared_ptr<SPipeline> pipeline;
		CHevcPushSource *source; // raw back-pointer -- see constructor comment
		IMFMediaEventQueue *eventQueue = nullptr;
		IMFStreamDescriptor *streamDesc;
	};

	// ============================================================================
	// CHevcPushSource -- minimal push-mode IMFMediaSource wrapping exactly
	// one CHevcPushStream. MFMEDIASOURCE_IS_LIVE (no seek capability
	// reported): CVideoSourceFFmpeg does its own container-level seeking and
	// calls this class's Flush() (a full pipeline rebuild -- see this file's
	// header comment) rather than expecting the source itself to seek.
	// ============================================================================
	class CHevcPushSource : public IMFMediaSource, public CUnknownBase
	{
	public:
		CHevcPushSource(std::shared_ptr<SPipeline> p, IMFStreamDescriptor *sd, IMFPresentationDescriptor *pd)
			: pipeline(std::move(p)), streamDesc(sd), presDesc(pd)
		{
			streamDesc->AddRef();
			presDesc->AddRef();
		}

		~CHevcPushSource() override
		{
			if (stream) stream->Release();
			streamDesc->Release();
			presDesc->Release();
			if (eventQueue) eventQueue->Release();
		}

		HRESULT InitInstance()
		{
			HRESULT hr = MFCreateEventQueue(&eventQueue);
			if (FAILED(hr))
				return hr;
			stream = new CHevcPushStream(pipeline, this, streamDesc); // stream co-owns the pipeline (shared_ptr copy)
			return stream->InitInstance();
		}

		IMFStreamDescriptor *StreamDescriptorRaw() const { return streamDesc; }
		IMFPresentationDescriptor *PresentationDescriptorRaw() const { return presDesc; }
		CHevcPushStream *StreamRaw() const { return stream; }

		// IUnknown
		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IUnknown || riid == __uuidof(IMFMediaEventGenerator) || riid == __uuidof(IMFMediaSource))
				*ppv = static_cast<IMFMediaSource *>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
		ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

		// IMFMediaEventGenerator
		HRESULT STDMETHODCALLTYPE GetEvent(DWORD flags, IMFMediaEvent **ppEvent) override
		{
			return eventQueue->GetEvent(flags, ppEvent);
		}
		HRESULT STDMETHODCALLTYPE BeginGetEvent(IMFAsyncCallback *cb, IUnknown *state) override
		{
			return eventQueue->BeginGetEvent(cb, state);
		}
		HRESULT STDMETHODCALLTYPE EndGetEvent(IMFAsyncResult *result, IMFMediaEvent **ppEvent) override
		{
			return eventQueue->EndGetEvent(result, ppEvent);
		}
		HRESULT STDMETHODCALLTYPE QueueEvent(MediaEventType met, REFGUID extType, HRESULT status, const PROPVARIANT *value) override
		{
			return eventQueue->QueueEventParamVar(met, extType, status, value);
		}

		// IMFMediaSource
		HRESULT STDMETHODCALLTYPE GetCharacteristics(DWORD *pdw) override
		{
			*pdw = MFMEDIASOURCE_IS_LIVE;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE CreatePresentationDescriptor(IMFPresentationDescriptor **out) override
		{
			return presDesc->Clone(out);
		}
		HRESULT STDMETHODCALLTYPE Start(IMFPresentationDescriptor *, const GUID *, const PROPVARIANT *) override
		{
			PROPVARIANT empty;
			PropVariantInit(&empty);
			QueueEvent(MESourceStarted, GUID_NULL, S_OK, &empty);

			PROPVARIANT streamVar;
			PropVariantInit(&streamVar);
			streamVar.vt = VT_UNKNOWN;
			streamVar.punkVal = stream;
			stream->AddRef();
			QueueEvent(MENewStream, GUID_NULL, S_OK, &streamVar);
			PropVariantClear(&streamVar);

			// MEStreamStarted, queued on the STREAM's own event queue (not the
			// source's), is the documented signal that tells the pipeline this
			// particular stream is now active and ready to be pulled from --
			// without it, the session sets up an event listener on the stream
			// (confirmed via BeginGetEvent tracing) but never calls
			// RequestSample() on it.
			stream->QueueEvent(MEStreamStarted, GUID_NULL, S_OK, &empty);
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Stop() override
		{
			PROPVARIANT empty;
			PropVariantInit(&empty);
			QueueEvent(MESourceStopped, GUID_NULL, S_OK, &empty);
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Pause() override { return MF_E_INVALID_STATE_TRANSITION; }
		HRESULT STDMETHODCALLTYPE Shutdown() override
		{
			if (stream) stream->ShutdownStream();
			if (eventQueue) eventQueue->Shutdown();
			return S_OK;
		}

	private:
		std::shared_ptr<SPipeline> pipeline; // co-owned -- passed on to the stream; see CVideoDecoderHEVCMF::pipeline's header note
		IMFMediaEventQueue *eventQueue = nullptr;
		IMFStreamDescriptor *streamDesc;
		IMFPresentationDescriptor *presDesc;
		CHevcPushStream *stream = nullptr;
	};

	HRESULT STDMETHODCALLTYPE CHevcPushStream::GetMediaSource(IMFMediaSource **ppSource)
	{
		*ppSource = source;
		source->AddRef();
		return S_OK;
	}

	// ============================================================================
	// CHevcSampleGrabberCallback -- receives decoded NV12/P010 frames as flat
	// bytes directly from the Sample Grabber Sink, no IMFSample/IMFMediaBuffer
	// handling needed on our side at all.
	// ============================================================================
	class CHevcSampleGrabberCallback : public IMFSampleGrabberSinkCallback, public CUnknownBase
	{
	public:
		explicit CHevcSampleGrabberCallback(std::shared_ptr<SPipeline> p) : pipeline(std::move(p)) {}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IUnknown || riid == __uuidof(IMFClockStateSink))
				*ppv = static_cast<IMFClockStateSink *>(this);
			else if (riid == __uuidof(IMFSampleGrabberSinkCallback))
				*ppv = static_cast<IMFSampleGrabberSinkCallback *>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
		ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

		// IMFClockStateSink
		HRESULT STDMETHODCALLTYPE OnClockStart(MFTIME, LONGLONG) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnClockStop(MFTIME) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnClockPause(MFTIME) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnClockRestart(MFTIME) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnClockSetRate(MFTIME, float) override { return S_OK; }

		// IMFSampleGrabberSinkCallback
		HRESULT STDMETHODCALLTYPE OnSetPresentationClock(IMFPresentationClock *) override { return S_OK; }
		HRESULT STDMETHODCALLTYPE OnProcessSample(REFGUID, DWORD, LONGLONG llSampleTime, LONGLONG,
												   const BYTE *pSampleBuffer, DWORD dwSampleSize) override;
		HRESULT STDMETHODCALLTYPE OnShutdown() override { return S_OK; }

	private:
		// Co-owns the pipeline: OnProcessSample() runs on an MF worker thread and
		// touches pipeline->mtx/cv/outDataBuf, so the struct must outlive any
		// in-flight call even after TeardownPipeline() drops the decoder's ref.
		std::shared_ptr<SPipeline> pipeline;
	};

	HRESULT STDMETHODCALLTYPE CHevcSampleGrabberCallback::OnProcessSample(
		REFGUID, DWORD, LONGLONG llSampleTime, LONGLONG,
		const BYTE *pSampleBuffer, DWORD dwSampleSize)
	{
		std::lock_guard<std::mutex> lock(pipeline->mtx);
		pipeline->outDataBuf.assign(pSampleBuffer, pSampleBuffer + dwSampleSize);
		pipeline->outPts100ns = llSampleTime;
		pipeline->hasOutput = true;
		pipeline->cv.notify_all();
		return S_OK;
	}

	// ============================================================================
	// CHevcSessionEventCallback -- pumps IMFMediaSession's event queue
	// (required for MESessionTopologyStatus/MESessionEnded/error visibility;
	// actual sample flow happens on MF's own worker threads regardless of
	// whether this pump runs, but without it we'd never see topology-ready
	// to call Start(), nor any error/end-of-stream status).
	//
	// LIFETIME (this is the fix for the observed 0xC0000005 in MFCORE reached via
	// this callback's Invoke() -> session->BeginGetEvent):
	//
	//   * `session` is a CO-OWNED reference: AddRef'd in the constructor, Release'd
	//     in the destructor. MF holds its OWN reference on THIS callback object for
	//     the duration of any dispatched Invoke() (COM async-callback convention),
	//     so the destructor -- and thus the session Release -- cannot run until
	//     every in-flight Invoke() has returned. Therefore the session OBJECT is
	//     alive throughout any Invoke(), even after another thread's
	//     TeardownPipeline() has Shutdown()+Release()'d the pipeline's own session
	//     reference. Calls on a post-Shutdown() session return MF_E_SHUTDOWN (the
	//     documented contract) -- a clean failure, not a fault. (The pre-fix code
	//     stored `session` as a bare raw pointer, so TeardownPipeline()'s
	//     session->Release() could destroy the session object out from under an
	//     Invoke() that had already been dispatched -- the UAF that crashed.)
	//
	//   * `pipeline` is a shared_ptr, co-owning the SPipeline for the same reason
	//     (Invoke touches pipeline->mtx/cv/flags). See CVideoDecoderHEVCMF::pipeline.
	//
	//   * `retired` is a fast-exit optimization + belt, NOT the correctness fence:
	//     TeardownPipeline() release-stores it before session->Shutdown(); Invoke()
	//     acquire-loads it at entry and bails immediately when set. Correctness does
	//     NOT depend on it -- the two owning references above already make every
	//     access in Invoke() safe regardless of ordering.
	// ============================================================================
	class CHevcSessionEventCallback : public IMFAsyncCallback, public CUnknownBase
	{
	public:
		CHevcSessionEventCallback(std::shared_ptr<SPipeline> p, IMFMediaSession *s, DWORD workQueue)
			: pipeline(std::move(p)), session(s), workQueue(workQueue)
		{
			if (session)
				session->AddRef(); // CO-OWN the session -- keeps the OBJECT alive across any in-flight Invoke()
		}

		~CHevcSessionEventCallback() override
		{
			// Runs only when the LAST reference to this callback drops -- i.e. after
			// MF's in-flight dispatch (which holds its own ref) has returned -- so this
			// Release can never race an Invoke() that is still touching `session`.
			if (session)
				session->Release();
		}

		HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override
		{
			if (!ppv) return E_POINTER;
			if (riid == IID_IUnknown || riid == __uuidof(IMFAsyncCallback))
				*ppv = static_cast<IMFAsyncCallback *>(this);
			else
			{
				*ppv = nullptr;
				return E_NOINTERFACE;
			}
			AddRef();
			return S_OK;
		}
		ULONG STDMETHODCALLTYPE AddRef() override { return AddRefImpl(); }
		ULONG STDMETHODCALLTYPE Release() override { return ReleaseImpl(); }

		// Route OUR event dispatch onto the decoder's private serial work queue
		// (Fix 2). MF calls this when we BeginGetEvent() to learn which work queue
		// should run the resulting Invoke(); returning our own id keeps our
		// dispatches off MF's process-wide shared platform queue. `workQueue == 0`
		// (MFASYNC_CALLBACK_QUEUE_UNDEFINED) means none was allocated -- fall back
		// to E_NOTIMPL, i.e. MF's default dispatch, exactly as before this fix.
		HRESULT STDMETHODCALLTYPE GetParameters(DWORD *pdwFlags, DWORD *pdwQueue) override
		{
			if (!pdwFlags || !pdwQueue)
				return E_POINTER;
			if (workQueue == 0)
				return E_NOTIMPL;
			*pdwFlags = 0;
			*pdwQueue = workQueue;
			return S_OK;
		}
		HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult *pResult) override;

		// Set by TeardownPipeline() BEFORE session->Shutdown() (release), read at
		// Invoke() entry (acquire). See the class comment: fast-exit belt only.
		void MarkRetired() { retired.store(true, std::memory_order_release); }

	private:
		std::shared_ptr<SPipeline> pipeline; // co-owned -- see class comment
		IMFMediaSession *session;            // co-owned via AddRef/Release -- see class comment
		std::atomic<bool> retired{false};
		DWORD workQueue;                     // decoder's private serial work queue id (0 == none); see GetParameters (Fix 2)
	};

	HRESULT STDMETHODCALLTYPE CHevcSessionEventCallback::Invoke(IMFAsyncResult *pResult)
	{
		// Fast-exit belt (NOT the correctness fence -- the co-owned `session` and
		// `pipeline` refs are). If teardown has already retired us, skip all work.
		if (retired.load(std::memory_order_acquire))
			return S_OK;

		IMFMediaEvent *event = nullptr;
		HRESULT hr = session->EndGetEvent(pResult, &event);
		if (FAILED(hr))
			return S_OK; // MF_E_SHUTDOWN (or any failure): session torn down -- terminal, do NOT re-arm, surface nothing

		MediaEventType type = MEUnknown;
		event->GetType(&type);
		HRESULT eventStatus = S_OK;
		event->GetStatus(&eventStatus);

		bool terminal = false;
		{
			std::lock_guard<std::mutex> lock(pipeline->mtx);
			if (FAILED(eventStatus))
			{
				pipeline->sourceError = true;
				pipeline->asyncErrorReason = "IMFMediaSession event reported failure";
				pipeline->cv.notify_all();
			}

			if (type == MESessionTopologyStatus)
			{
				UINT32 topoStatus = 0;
				event->GetUINT32(MF_EVENT_TOPOLOGY_STATUS, &topoStatus);
				if (topoStatus == MF_TOPOSTATUS_READY)
				{
					PROPVARIANT var;
					PropVariantInit(&var);
					session->Start(&GUID_NULL, &var);
				}
			}
			else if (type == MESessionEnded)
			{
				pipeline->sessionEnded = true;
				pipeline->cv.notify_all();
			}
			else if (type == MESessionClosed)
			{
				terminal = true;
				pipeline->shutdown = true;
				pipeline->cv.notify_all();
			}
		}
		event->Release();

		if (!terminal)
		{
			// Re-arm. Our AddRef keeps the session OBJECT alive even after another
			// thread released the pipeline's own session reference, so a session that
			// has since been Shutdown() returns MF_E_SHUTDOWN here (documented
			// contract) instead of faulting. Treat ANY failure as terminal: no
			// re-arm, no error surfaced -- teardown is (or soon will be) draining us.
			HRESULT hrArm = session->BeginGetEvent(this, nullptr);
			(void)hrArm;
		}
		return S_OK;
	}

	HRESULT STDMETHODCALLTYPE CHevcPushStream::RequestSample(IUnknown *pToken)
	{
		std::unique_lock<std::mutex> lock(pipeline->mtx);
		for (;;)
		{
			if (pipeline->shutdown)
				return MF_E_SHUTDOWN;
			if (pipeline->packetAvailable)
				break;

			pipeline->streamWaitingForPacket = true;
			pipeline->cv.notify_all();
			bool got = pipeline->cv.wait_for(lock, std::chrono::seconds(10),
											  [&] { return pipeline->packetAvailable || pipeline->shutdown; });
			if (pipeline->shutdown)
				return MF_E_SHUTDOWN;
			if (!got || !pipeline->packetAvailable)
			{
				pipeline->sourceError = true;
				pipeline->asyncErrorReason = "push source RequestSample timed out waiting for a packet";
				pipeline->cv.notify_all();
				return E_FAIL;
			}
		}

		pipeline->streamWaitingForPacket = false;
		bool isEOS = pipeline->pktIsEOS;
		std::vector<u8> localData;
		LONGLONG localPts = 0;
		if (!isEOS)
		{
			localData = pipeline->pktDataBuf;
			localPts = pipeline->pktPts100ns;
		}
		pipeline->packetAvailable = false;
		pipeline->cv.notify_all();
		lock.unlock();

		if (isEOS)
		{
			QueueEvent(MEEndOfStream, GUID_NULL, S_OK, nullptr);
			return S_OK;
		}

		IMFSample *sample = nullptr;
		IMFMediaBuffer *buf = nullptr;
		HRESULT hr = MFCreateSample(&sample);
		if (SUCCEEDED(hr))
			hr = MFCreateMemoryBuffer((DWORD)localData.size(), &buf);
		if (SUCCEEDED(hr))
		{
			BYTE *raw = nullptr;
			hr = buf->Lock(&raw, nullptr, nullptr);
			if (SUCCEEDED(hr))
			{
				memcpy(raw, localData.data(), localData.size());
				buf->Unlock();
				buf->SetCurrentLength((DWORD)localData.size());
				hr = sample->AddBuffer(buf);
			}
		}
		if (SUCCEEDED(hr))
		{
			sample->SetSampleTime(localPts);
			sample->SetSampleDuration(0);
			if (pToken)
				sample->SetUnknown(MFSampleExtension_Token, pToken);
		}
		if (buf) buf->Release();

		if (FAILED(hr))
		{
			if (sample) sample->Release();
			std::lock_guard<std::mutex> lg(pipeline->mtx);
			pipeline->sourceError = true;
			pipeline->asyncErrorReason = "push source failed to build a compressed IMFSample";
			pipeline->cv.notify_all();
			return hr;
		}

		PROPVARIANT var;
		PropVariantInit(&var);
		var.vt = VT_UNKNOWN;
		var.punkVal = sample; // transfers our ref -- QueueEventParamVar copies its own
		QueueEvent(MEMediaSample, GUID_NULL, S_OK, &var);
		PropVariantClear(&var);
		return S_OK;
	}

	HRESULT CreateGrabberSinkActivate(const std::shared_ptr<SPipeline> &pipeline, int width, int height, const GUID &subtype, IMFActivate *&outActivate)
	{
		outActivate = nullptr;

		IMFMediaType *type = nullptr;
		HRESULT hr = MFCreateMediaType(&type);
		if (SUCCEEDED(hr))
		{
			type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
			type->SetGUID(MF_MT_SUBTYPE, subtype);
			MFSetAttributeSize(type, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
			type->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
			type->SetUINT32(MF_MT_ALL_SAMPLES_INDEPENDENT, TRUE);
			MFSetAttributeRatio(type, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
		}

		if (SUCCEEDED(hr))
		{
			pipeline->grabberCallback = new CHevcSampleGrabberCallback(pipeline);
			hr = MFCreateSampleGrabberSinkActivate(type, pipeline->grabberCallback, &outActivate);
			if (FAILED(hr))
			{
				pipeline->grabberCallback->Release();
				pipeline->grabberCallback = nullptr;
			}
		}
		if (type) type->Release();
		return hr;
	}
}

// ============================================================================
// Local helpers -- MFT enumeration shared by IsHEVCDecodeAvailable() (existence
// check only; unrelated to the Topology/Session pipeline).
// ============================================================================
namespace
{
	constexpr UINT32 kHardwareFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
	constexpr UINT32 kSoftwareFlags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

	IMFActivate *EnumFirstHEVCDecoderActivate(UINT32 flags)
	{
		MFT_REGISTER_TYPE_INFO inInfo = {};
		inInfo.guidMajorType = MFMediaType_Video;
		inInfo.guidSubtype = MFVideoFormat_HEVC;

		IMFActivate **activates = nullptr;
		UINT32 count = 0;
		HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr, &activates, &count);

		IMFActivate *result = nullptr;
		if (SUCCEEDED(hr) && count > 0 && activates && activates[0])
		{
			result = activates[0];
			result->AddRef();
		}
		if (activates)
		{
			for (UINT32 i = 0; i < count; i++)
				if (activates[i]) activates[i]->Release();
			CoTaskMemFree(activates);
		}
		return result;
	}

	IMFActivate *EnumFirstHEVCDecoderActivateEitherTier()
	{
		IMFActivate *activate = EnumFirstHEVCDecoderActivate(kHardwareFlags);
		if (!activate)
			activate = EnumFirstHEVCDecoderActivate(kSoftwareFlags);
		return activate;
	}
}

bool IVideoPacketDecoder::IsHEVCDecodeAvailable()
{
	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	IMFActivate *activate = EnumFirstHEVCDecoderActivateEitherTier();
	if (!activate)
		return false;

	activate->Release();
	return true;
}

// ============================================================================
// Construction / teardown
// ============================================================================
CVideoDecoderHEVCMF::CVideoDecoderHEVCMF()
	: CVideoDecoderHEVCMF(ECodec::HEVC)
{
}

CVideoDecoderHEVCMF::CVideoDecoderHEVCMF(ECodec codec)
	: inputCodec(codec)
{
}

CVideoDecoderHEVCMF::~CVideoDecoderHEVCMF()
{
	TeardownPipeline();

	// Release this instance's private serial work queue (Fix 2). Safe here:
	// TeardownPipeline() has already Shutdown() the session, so no new dispatch
	// will be scheduled onto this queue, and MFUnlockWorkQueue does not free the
	// queue out from under a work item that is still executing (the platform
	// keeps it live until outstanding items drain). Matched 1:1 with the single
	// MFAllocateSerialWorkQueue in Init(); 0 means none was ever allocated.
	if (serialWorkQueue != 0)
	{
		MFUnlockWorkQueue(serialWorkQueue);
		serialWorkQueue = 0;
	}
}

// Abort (Task 5; contract in IVideoPacketDecoder.h). Runs on the RENDER thread,
// concurrently with a DecodePacket() blocked on one of its 10-second waits on
// the DECODE thread.
//
// PURE WAKE -- it publishes NOTHING. The state a woken wait re-evaluates is the
// player's own (shouldStop / seekGeneration), already published by the caller
// before it reached here (CVideoPlayer::StopDecodeThread() /
// SubmitSeekCommandLocked(): state first, wake second), and read through the
// LEVEL-triggered abortPredicate that both wait predicates below join in.
//
// No wake can be lost, in either interleaving:
//   * we take pipeline->mtx BEFORE the waiter evaluates its predicate -- the
//     waiter then evaluates it after we are done, reads the predicate as true and
//     never sleeps at all;
//   * the waiter is already asleep -- wait_for() released pipeline->mtx only once
//     committed to sleeping, so notify_all() below reaches it, and it
//     RE-EVALUATES the predicate on wakeup and sees the truth.
// And a wake that lands when NOTHING is in flight is simply redundant (there is
// no flag left behind to be cleared, and none needed: the next DecodePacket()
// re-reads the predicate itself).
void CVideoDecoderHEVCMF::Abort()
{
	// The lifetime mutex keeps `pipeline` alive across the notify (a concurrent
	// TeardownPipeline() swaps the pointer out under this same mutex). Nothing
	// slow runs under it -- see the header's note.
	std::lock_guard<std::mutex> life(pipelineLifetimeMutex);
	if (!pipeline)
		return;   // no pipeline, nothing to wake
	{
		std::lock_guard<std::mutex> lock(pipeline->mtx);
	}
	pipeline->cv.notify_all();
}

void CVideoDecoderHEVCMF::TeardownPipeline()
{
	// SWAP THE POINTER OUT under the lifetime mutex, then tear the local copy down
	// OUTSIDE it. IMFMediaSession::Shutdown() blocks until MF's worker threads wind
	// down, and this whole function runs on the decode thread on EVERY seek
	// (Flush() -> BuildPipeline() -> TeardownPipeline()); holding the lifetime
	// mutex across it would make a concurrent Abort() -- render thread, holding
	// cmdMutex -- block for that long. That is a render stall on a key-repeat seek
	// burst: a smaller copy of the very freeze this task removes.
	//
	// An Abort() that races this and wins the mutex first simply wakes the pipeline
	// we are about to shut down (harmless -- its waiters give up on the predicate);
	// one that arrives after sees `pipeline == nullptr` and does nothing, which is
	// CORRECT rather than merely benign: `shutdown` below is published before the
	// pointer is dropped, and it releases every waiter unconditionally.
	std::shared_ptr<SPipeline> p;
	{
		std::lock_guard<std::mutex> life(pipelineLifetimeMutex);
		p = std::move(pipeline);   // decoder-side reference handed to the local
	}
	if (!p)
		return;

	{
		std::lock_guard<std::mutex> lock(p->mtx);
		p->shutdown = true;
		p->cv.notify_all();
	}

	// Retire the event callback BEFORE Shutdown() (release-store; Invoke()
	// acquire-loads at entry). This is the fast-exit belt; it is NOT what makes
	// the releases below safe. What makes them safe is co-ownership: the event
	// callback holds its own AddRef on `session`, and every MF-worker-facing
	// helper (event callback, grabber, push source/stream) co-owns `*p` via its
	// own shared_ptr. So an Invoke()/OnProcessSample()/RequestSample() already
	// dispatched keeps both the session OBJECT and this SPipeline alive until it
	// returns -- the Release()/`p`-drop below can never pull the rug out.
	if (p->eventCallback)
		p->eventCallback->MarkRetired();

	if (p->session)
		p->session->Shutdown();
	if (p->source)
		p->source->Shutdown();

	if (p->eventCallback) p->eventCallback->Release();
	if (p->grabberCallback) p->grabberCallback->Release();
	if (p->source) p->source->Release();
	if (p->topology) p->topology->Release();
	if (p->session) p->session->Release();

	// No `delete p`. `p` is the last DECODER-side reference; the SPipeline is
	// freed only once this local drops AND every co-owning COM helper still held
	// alive by an in-flight MF dispatch has been destroyed -- i.e. after the last
	// worker-thread callback has returned. The raw COM member pointers inside the
	// struct were released just above and are inert by the time the (plain)
	// struct destructor runs; it never touches them.
}

// ============================================================================
// BuildPipeline -- push source (single HEVC stream) -> Sample Grabber Sink
// (NV12, falling back to P010), letting the Topology loader auto-insert
// whichever decoder MFT it resolves for HEVC -> NV12/P010 between them.
// ============================================================================
bool CVideoDecoderHEVCMF::BuildPipeline()
{
	TeardownPipeline();
	{
		// Under the lifetime mutex for the same reason TeardownPipeline() is: a
		// concurrent Abort() must never observe a half-published pointer.
		std::lock_guard<std::mutex> life(pipelineLifetimeMutex);
		pipeline = std::make_shared<SPipeline>();
	}

	// Compressed input subtype per this instance's codec (the subclass seam
	// -- see ECodec in the header). The session's topology loader resolves
	// whatever decoder MFT registers for this subtype: the HEVC decoder MFT
	// (hardware or the Store's "HEVC Video Extensions") for HEVC, the in-box
	// WMVideo Decoder MFT (wmvdecod.dll, registered for all four WMV-family
	// subtypes) for the CVideoDecoderWMVMF subclass.
	GUID inputSubtype = MFVideoFormat_HEVC;
	switch (inputCodec)
	{
		case ECodec::HEVC: inputSubtype = MFVideoFormat_HEVC; break;
		case ECodec::WMV1: inputSubtype = MFVideoFormat_WMV1; break;
		case ECodec::WMV2: inputSubtype = MFVideoFormat_WMV2; break;
		case ECodec::WMV3: inputSubtype = MFVideoFormat_WMV3; break;
		case ECodec::WVC1: inputSubtype = MFVideoFormat_WVC1; break;
	}

	IMFMediaType *compressedType = nullptr;
	HRESULT hr = MFCreateMediaType(&compressedType);
	if (SUCCEEDED(hr))
	{
		compressedType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		compressedType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
		MFSetAttributeSize(compressedType, MF_MT_FRAME_SIZE, (UINT32)width, (UINT32)height);
		compressedType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
		// savedExtradata semantics per codec (both delivered the same way):
		// HEVC -- ANNEX-B (start-code-prefixed VPS/SPS/PPS), not hvcC; see
		// WantsAnnexB()'s doc comment and CVideoSourceFFmpeg.cpp's bsf setup.
		// WMV family -- the ASF sequence header (codecpar->extradata) as-is,
		// which is exactly the codec private data the WMV decoder MFT
		// documents in MF_MT_USER_DATA.
		if (!savedExtradata.empty())
			compressedType->SetBlob(MF_MT_USER_DATA, savedExtradata.data(), (UINT32)savedExtradata.size());
	}

	IMFStreamDescriptor *streamDesc = nullptr;
	if (SUCCEEDED(hr))
		hr = MFCreateStreamDescriptor(0, 1, &compressedType, &streamDesc);
	if (SUCCEEDED(hr))
	{
		IMFMediaTypeHandler *handler = nullptr;
		hr = streamDesc->GetMediaTypeHandler(&handler);
		if (SUCCEEDED(hr))
		{
			hr = handler->SetCurrentMediaType(compressedType);
			handler->Release();
		}
	}

	IMFPresentationDescriptor *presDesc = nullptr;
	if (SUCCEEDED(hr))
		hr = MFCreatePresentationDescriptor(1, &streamDesc, &presDesc);
	if (SUCCEEDED(hr))
		hr = presDesc->SelectStream(0);

	if (compressedType) compressedType->Release();

	if (FAILED(hr))
	{
		if (streamDesc) streamDesc->Release();
		if (presDesc) presDesc->Release();
		errorReason = "failed to build HEVC push-source stream/presentation descriptors";
		TeardownPipeline();
		return false;
	}

	pipeline->source = new CHevcPushSource(pipeline, streamDesc, presDesc);
	hr = pipeline->source->InitInstance();
	streamDesc->Release();
	presDesc->Release();
	if (FAILED(hr))
	{
		errorReason = "failed to initialize HEVC push source";
		TeardownPipeline();
		return false;
	}

	// OUTPUT BIT DEPTH IS CHOSEN FROM THE TRANSFER FUNCTION (S-5 Phase 5).
	//
	// This REPLACES the original "v1 policy" (which mirrored the old
	// NegotiateOutputType()): prefer 8-bit NV12 always, and treat P010 purely
	// as a capability fallback for an MFT that cannot downconvert Main10/HLG
	// to 8 bits itself. That policy existed to avoid ever needing a 10-bit
	// display path, and Phase 5 builds exactly that path -- so the reasoning
	// is void. Worse, it was lossy in the one case it was meant to serve: an
	// MFT that CAN downconvert throws the HDR range away, and PQ quantised to
	// 8 bits bands in the shadows by construction, whatever the shader does.
	//
	// So the request order is now trc-driven: PQ (16) and HLG (18) ask for
	// P010 FIRST, everything else asks for NV12 first exactly as before. The
	// capability fallback is kept in BOTH directions -- a clip that decodes at
	// the wrong depth still beats a clip that does not decode.
	//
	// The P010 arm stays HEVC-only. The WMV family is 8-bit throughout, so a
	// WMV pipeline that cannot do NV12 is simply broken and must fail rather
	// than retry at a bit depth the codec does not have.
	const bool wants10Bit = (inputCodec == ECodec::HEVC) && VideoTransfer::IsHdrTrc(colorTrc);

	IMFActivate *sinkActivate = nullptr;
	use10Bit = wants10Bit;
	hr = CreateGrabberSinkActivate(pipeline, width, height,
								   wants10Bit ? MFVideoFormat_P010 : MFVideoFormat_NV12,
								   sinkActivate);
	if (FAILED(hr) && inputCodec == ECodec::HEVC)
	{
		use10Bit = !wants10Bit;
		hr = CreateGrabberSinkActivate(pipeline, width, height,
									   wants10Bit ? MFVideoFormat_NV12 : MFVideoFormat_P010,
									   sinkActivate);
	}
	if (FAILED(hr))
	{
		errorReason = "failed to create decode Sample Grabber Sink (NV12/P010 not accepted)";
		TeardownPipeline();
		return false;
	}

	hr = MFCreateTopology(&pipeline->topology);
	IMFTopologyNode *sourceNode = nullptr;
	IMFTopologyNode *outputNode = nullptr;
	if (SUCCEEDED(hr)) hr = MFCreateTopologyNode(MF_TOPOLOGY_SOURCESTREAM_NODE, &sourceNode);
	if (SUCCEEDED(hr)) hr = sourceNode->SetUnknown(MF_TOPONODE_SOURCE, pipeline->source);
	if (SUCCEEDED(hr)) hr = sourceNode->SetUnknown(MF_TOPONODE_PRESENTATION_DESCRIPTOR, pipeline->source->PresentationDescriptorRaw());
	if (SUCCEEDED(hr)) hr = sourceNode->SetUnknown(MF_TOPONODE_STREAM_DESCRIPTOR, pipeline->source->StreamDescriptorRaw());
	if (SUCCEEDED(hr)) hr = pipeline->topology->AddNode(sourceNode);
	if (SUCCEEDED(hr)) hr = MFCreateTopologyNode(MF_TOPOLOGY_OUTPUT_NODE, &outputNode);
	if (SUCCEEDED(hr)) hr = outputNode->SetObject(sinkActivate);
	if (SUCCEEDED(hr)) hr = outputNode->SetUINT32(MF_TOPONODE_STREAMID, 0);
	if (SUCCEEDED(hr)) hr = outputNode->SetUINT32(MF_TOPONODE_NOSHUTDOWN_ON_REMOVE, FALSE);
	if (SUCCEEDED(hr)) hr = pipeline->topology->AddNode(outputNode);
	if (SUCCEEDED(hr)) hr = sourceNode->ConnectOutput(0, outputNode, 0);

	if (sourceNode) sourceNode->Release();
	if (outputNode) outputNode->Release();
	sinkActivate->Release();

	if (FAILED(hr))
	{
		errorReason = "failed to build HEVC decode topology";
		TeardownPipeline();
		return false;
	}

	hr = MFCreateMediaSession(nullptr, &pipeline->session);
	if (SUCCEEDED(hr))
		hr = pipeline->session->SetTopology(0, pipeline->topology); // 0 (not IMMEDIATE) -- let the session resolve this partial topology (auto-insert the HEVC decoder MFT) itself
	if (FAILED(hr))
	{
		errorReason = "failed to create HEVC decode media session / set topology";
		TeardownPipeline();
		return false;
	}

	pipeline->eventCallback = new CHevcSessionEventCallback(pipeline, pipeline->session, serialWorkQueue);
	pipeline->session->BeginGetEvent(pipeline->eventCallback, nullptr);

	// Nothing further to do here: CHevcSessionEventCallback::Invoke() calls
	// session->Start() once MESessionTopologyStatus reports MF_TOPOSTATUS_READY;
	// RequestSample() will then begin firing on the Session's own worker
	// threads as soon as it does.
	return true;
}

// ============================================================================
// Init
// ============================================================================
bool CVideoDecoderHEVCMF::Init(const u8 *extradata, int extradataSize, int w, int h, int colorTrc)
{
	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	// Kept as member state, like savedExtradata, because Flush() rebuilds the
	// whole pipeline and must negotiate the SAME bit depth -- a rebuild that
	// silently dropped to 8-bit would band a PQ clip from the first seek
	// onward while the first play-through looked right (S-5 Phase 5).
	this->colorTrc = colorTrc;

	// Allocate this instance's private serial work queue once (Fix 2). Serial so
	// our event dispatches stay one-at-a-time; layered over the standard
	// multithreaded platform queue. A finite process resource -- exactly one
	// alloc per decoder instance, released in the destructor (including error
	// paths -- see ~CVideoDecoderHEVCMF). Idempotent: a re-Init() on an
	// already-initialized instance keeps the queue it already holds. Best-effort:
	// on failure we leave it 0 and GetParameters() returns E_NOTIMPL, i.e. MF's
	// default dispatch -- no worse than before this fix.
	if (serialWorkQueue == 0)
	{
		DWORD q = 0;
		HRESULT hrq = MFAllocateSerialWorkQueue(MFASYNC_CALLBACK_QUEUE_MULTITHREADED, &q);
		if (SUCCEEDED(hrq))
			serialWorkQueue = q;
		else
			LOGD("CVideoDecoderHEVCMF::Init: MFAllocateSerialWorkQueue failed (0x%08lx); using MF default dispatch", (unsigned long)hrq);
	}

	errorReason.clear();
	width = w;
	height = h;
	use10Bit = false;
	outFrame = SPendingFrame();

	savedExtradata.assign(extradata, extradata + (extradata && extradataSize > 0 ? extradataSize : 0));

	if (!BuildPipeline())
	{
		if (errorReason.empty())
			errorReason = "HEVC decode pipeline build failed";
		return false;
	}

	LOGD("CVideoDecoderHEVCMF::Init: HEVC decode pipeline built (%dx%d, requesting %s)",
		 width, height, use10Bit ? "P010" : "NV12");
	return true;
}

// ============================================================================
// DecodePacket -- one packet in, at most one frame out per call (see the
// SPipeline doc comment above for the full handoff protocol).
// ============================================================================
bool CVideoDecoderHEVCMF::DecodePacket(const AVPacket *pkt, SDecodedVideoFrame &out)
{
	// LEVEL-TRIGGERED ABORT (Task 5), checked at entry and joined into both waits
	// below. Generation scoping is the PREDICATE's, not ours: it reads true exactly
	// while the operation the decode thread is servicing is condemned (shutdown, or
	// superseded by a newer seek), so a call that starts while it is true has
	// nothing to do -- there is no flag to clear, and therefore nothing that a wake
	// landing between two DecodePacket() calls could erase. Aborted is neither a
	// failure nor EOS: false with an EMPTY errorReason, classified by the caller
	// (CVideoSourceFFmpeg's loops, which triggered the abort).
	if (IsAborted())
	{
		errorReason.clear();
		return false;
	}

	if (!pipeline)
	{
		errorReason = "decode pipeline not initialized";
		return false;
	}

	errorReason.clear();

	std::unique_lock<std::mutex> lock(pipeline->mtx);
	if (pipeline->shutdown)
	{
		errorReason = "decode pipeline already shut down";
		return false;
	}

	// Offer this packet (or, if pkt == nullptr, the EOS marker -- see
	// IVideoPacketDecoder.h / CVideoSourceFFmpeg's EOS drain contract) to
	// CHevcPushStream::RequestSample() and wait for it to be picked up.
	pipeline->pktIsEOS = (pkt == nullptr);
	if (pkt)
	{
		double ptsSeconds = (pkt->pts != AV_NOPTS_VALUE) ? (double)pkt->pts / (double)AV_TIME_BASE : 0.0;
		pipeline->pktPts100ns = (LONGLONG)std::llround(ptsSeconds * 10000000.0);
		pipeline->pktDataBuf.assign(pkt->data, pkt->data + pkt->size);
	}
	pipeline->hasOutput = false;
	pipeline->packetAvailable = true;
	pipeline->cv.notify_all();

	bool consumed = pipeline->cv.wait_for(lock, std::chrono::seconds(10),
										   [&] { return !pipeline->packetAvailable || pipeline->shutdown ||
														pipeline->sourceError || IsAborted(); });
	// ABORT FIRST, before the shutdown/timeout classification below: an aborted
	// wait is NEITHER a failure NOR EOS, so it returns false with an EMPTY
	// errorReason (IVideoPacketDecoder's contract) and CVideoSourceFFmpeg's loops
	// -- which triggered the abort -- classify it by re-checking their own copy of
	// the same predicate. Surfacing "did not accept the packet in time" here would
	// latch EVideoPlayerState::Error for a seek the user merely superseded.
	//
	// The pipeline is left mid-handoff (packetAvailable may still be true, the
	// Session may still be chewing on the previous sample). That is deliberate and
	// safe: EVERY abort is immediately followed by either
	// CVideoSourceFFmpeg::Seek()'s hevcDecoder->Flush() -- which
	// BuildPipeline()s a brand-new SPipeline, discarding all of it -- or by
	// Close()/TeardownPipeline().
	if (IsAborted())
	{
		errorReason.clear();
		return false;
	}
	if (pipeline->shutdown || !consumed)
	{
		errorReason = "HEVC push source did not accept the packet in time";
		return false;
	}
	if (pipeline->sourceError)
	{
		errorReason = "HEVC decode pipeline reported an error: " + pipeline->asyncErrorReason;
		return false;
	}

	// Now wait for either a decoded frame (hasOutput), or the pipeline
	// asking for ANOTHER packet before it can produce one (streamWaitingForPacket
	// becoming true again -- B-frame reorder/priming, the Topology/Session
	// equivalent of the old code's MF_E_TRANSFORM_NEED_MORE_INPUT), or EOS
	// (sessionEnded), or an error.
	bool progressed = pipeline->cv.wait_for(lock, std::chrono::seconds(10), [&] {
		return pipeline->hasOutput || pipeline->streamWaitingForPacket ||
			   pipeline->sessionEnded || pipeline->shutdown || pipeline->sourceError ||
			   IsAborted();
	});

	// Abort first, same classification as the first wait above -- and this is the
	// wait that actually bites: it is where DecodePacket() sits for up to ten
	// seconds while the Session decodes, i.e. what a Close() or a superseding seek
	// would otherwise have to wait out.
	if (IsAborted())
	{
		errorReason.clear();
		return false;
	}

	if (pipeline->shutdown)
	{
		errorReason = "decode pipeline shut down while waiting for output";
		return false;
	}
	if (pipeline->sourceError)
	{
		errorReason = "HEVC decode pipeline reported an error: " + pipeline->asyncErrorReason;
		return false;
	}
	if (!progressed)
	{
		errorReason = "timed out waiting for HEVC decode pipeline output";
		return false;
	}

	if (pipeline->hasOutput)
	{
		std::vector<u8> localData = std::move(pipeline->outDataBuf);
		LONGLONG localPts = pipeline->outPts100ns;
		pipeline->hasOutput = false;
		lock.unlock();

		double pts = (double)localPts / 10000000.0;
		bool got = CopyFrameOut(localData.data(), localData.size(), pts);
		return got ? EmitOutFrame(out) : false;
	}

	// streamWaitingForPacket (no output yet -- keep feeding) or sessionEnded
	// (clean EOS drained with nothing further, matching the old code's "false
	// + empty errorReason == clean EOS" contract) both return false here with
	// no error set.
	return false;
}

// ============================================================================
// Flush -- see this file's header comment: no Topology/Session equivalent of
// MFT_MESSAGE_COMMAND_FLUSH, so this rebuilds the entire pipeline instead.
// ============================================================================
void CVideoDecoderHEVCMF::Flush()
{
	outFrame = SPendingFrame();
	if (!pipeline)
		return;
	BuildPipeline(); // best-effort rebuild; on failure errorReason is set and subsequent DecodePacket() calls fail cleanly (pipeline stays non-null but freshly torn-down-and-rebuilt state)
}

// ============================================================================
// CopyFrameOut -- copy Y/UV planes out of the Sample Grabber's flat buffer
// into outFrame. Tightly-packed stride assumption (pitch == width, or
// width*2 for 10-bit): the Sample Grabber Sink receives whatever the
// resolved decoder MFT's output type negotiation converged on for the exact
// width/height/subtype BuildPipeline() requested, and MF's own topology
// resolution -- unlike this class's earlier hand-rolled buffer allocation --
// is expected to hand the grabber a buffer matching that declared type's
// default (tightly packed) stride.
// ============================================================================
bool CVideoDecoderHEVCMF::CopyFrameOut(const u8 *data, size_t dataSize, double pts)
{
	if (!data)
		return false;

	const int yHeight = height;
	const int cHeight = (height + 1) / 2;
	const int cWidth = (width + 1) / 2;
	const int pitch = width * (use10Bit ? 2 : 1);

	const size_t expected = (size_t)pitch * yHeight + (size_t)pitch * cHeight;
	if (dataSize < expected)
	{
		errorReason = "HEVC decoded sample smaller than expected NV12/P010 frame size";
		return false;
	}

	SPendingFrame pf;
	pf.pts = pts;
	pf.is10Bit = use10Bit;
	pf.width = width;
	pf.height = height;

	if (!use10Bit)
	{
		pf.strideY = width;
		pf.y.resize((size_t)pf.strideY * yHeight);
		for (int r = 0; r < yHeight; r++)
			memcpy(pf.y.data() + (size_t)r * pf.strideY, data + (size_t)r * pitch, (size_t)pf.strideY);

		const u8 *uvBase = data + (size_t)pitch * yHeight;
		pf.strideUV = cWidth * 2;
		pf.uv.resize((size_t)pf.strideUV * cHeight);
		// Copy only the bytes the SOURCE chroma row actually holds. The dest
		// stride is the rounded-up cWidth*2, kept for downstream layout; for ODD
		// width that is width+1, one byte wider than the source row pitch (==
		// `pitch` == width per the tightly-packed layout `expected` assumes), so
		// clamp to min(pitch, strideUV) to avoid a 1-byte over-read off the final
		// row. Even width (all real HEVC coded sizes) has pitch == strideUV, so
		// this is byte-for-byte identical to before.
		const size_t uvCopyBytes = (size_t)pf.strideUV < (size_t)pitch ? (size_t)pf.strideUV : (size_t)pitch;
		for (int r = 0; r < cHeight; r++)
			memcpy(pf.uv.data() + (size_t)r * pf.strideUV, uvBase + (size_t)r * pitch, uvCopyBytes);
	}
	else
	{
		// P010: same biplanar shape as NV12 but 16-bit LE samples, 10-bit
		// value MSB-justified (value << 6) -- de-interleave + right-shift-by-6
		// to reach the LSB-justified layout CVideoSourceFFmpeg's
		// AV_PIX_FMT_YUV420P10LE path expects (identical conversion to
		// CVideoDecoderHEVCVT::OnFrameDecoded's 10-bit path).
		pf.strideY10 = width * 2;
		pf.y.resize((size_t)pf.strideY10 * yHeight);
		for (int r = 0; r < yHeight; r++)
		{
			const u16 *srcRow = reinterpret_cast<const u16 *>(data + (size_t)r * pitch);
			u16 *dstRow = reinterpret_cast<u16 *>(pf.y.data() + (size_t)r * pf.strideY10);
			for (int c = 0; c < width; c++)
				dstRow[c] = (u16)(srcRow[c] >> 6);
		}

		const u8 *uvBase = data + (size_t)pitch * yHeight;
		pf.strideC10 = cWidth * 2;
		pf.u10.resize((size_t)pf.strideC10 * cHeight);
		pf.v10.resize((size_t)pf.strideC10 * cHeight);
		// Read only COMPLETE UV pairs the source row holds. Each pair consumes two
		// u16 samples; the source chroma row is `pitch` bytes == pitch/2 samples,
		// so at most pitch/4 pairs. For ODD width that is cWidth-1, one pair short
		// of cWidth -- reading the full cWidth pairs would fetch srcRow[width], one
		// u16 past the row. Even width (all real HEVC) yields pitch/4 == cWidth, so
		// the bound is unchanged. The unwritten last dest column stays zero.
		const int srcPairs = (int)(pitch / 4);
		const int uvPairs = cWidth < srcPairs ? cWidth : srcPairs;
		for (int r = 0; r < cHeight; r++)
		{
			const u16 *srcRow = reinterpret_cast<const u16 *>(uvBase + (size_t)r * pitch);
			u16 *dstU = reinterpret_cast<u16 *>(pf.u10.data() + (size_t)r * pf.strideC10);
			u16 *dstV = reinterpret_cast<u16 *>(pf.v10.data() + (size_t)r * pf.strideC10);
			for (int c = 0; c < uvPairs; c++)
			{
				dstU[c] = (u16)(srcRow[c * 2 + 0] >> 6);
				dstV[c] = (u16)(srcRow[c * 2 + 1] >> 6);
			}
		}
	}

	outFrame = std::move(pf);
	return true;
}

// ============================================================================
// EmitOutFrame -- map outFrame's owned planes to SDecodedVideoFrame, same
// shape as CVideoDecoderHEVCVT::EmitOutFrame.
// ============================================================================
bool CVideoDecoderHEVCMF::EmitOutFrame(SDecodedVideoFrame &out)
{
	out.width = outFrame.width;
	out.height = outFrame.height;
	out.pts = outFrame.pts;

	if (outFrame.is10Bit)
	{
		out.pixelFormat = EVideoPixelFormat::YUV420P10;
		out.plane[0] = outFrame.y.data();   out.stride[0] = outFrame.strideY10;
		out.plane[1] = outFrame.u10.data(); out.stride[1] = outFrame.strideC10;
		out.plane[2] = outFrame.v10.data(); out.stride[2] = outFrame.strideC10;
		out.plane[3] = nullptr;             out.stride[3] = 0;
	}
	else
	{
		out.pixelFormat = EVideoPixelFormat::NV12;
		out.plane[0] = outFrame.y.data();  out.stride[0] = outFrame.strideY;
		out.plane[1] = outFrame.uv.data(); out.stride[1] = outFrame.strideUV;
		out.plane[2] = nullptr;            out.stride[2] = 0;
		out.plane[3] = nullptr;            out.stride[3] = 0;
	}
	return true;
}

#endif // _WIN32 && MT_ENABLE_FFMPEG
