#include "CVideoSourceMediaFoundation.h"

#if defined(_WIN32) && (!defined(MT_CAP_VIDEO_PLAYBACK) || MT_CAP_VIDEO_PLAYBACK)

#include "MFComThreadGuard.h"
#include "DBG_Log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <cstring>
#include <cmath>

#pragma comment(lib, "mfreadwrite.lib")

struct CVideoSourceMediaFoundation::SImpl
{
	IMFSourceReader *reader = nullptr;
	DWORD videoStream = (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM;
	DWORD audioStream = (DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM;
	bool haveAudio = false;
};

bool CVideoSourceMediaFoundation::CanProbablyOpen(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = p.substr(dot);
	for (char &c : ext)
		c = (char)tolower((unsigned char)c);

	// LONGER than the macOS list, deliberately -- see the header. MPEG-TS and
	// ASF are demuxed by Media Foundation and by nothing Apple ships, so a
	// Windows store build keeps formats a macOS store build loses. MKV and AVI
	// are absent from BOTH: Media Foundation reads neither without a
	// third-party splitter we would then be shipping.
	return ext == ".mp4"  || ext == ".m4v"  || ext == ".mov" ||
		   ext == ".3gp"  || ext == ".3g2"  ||
		   ext == ".mts"  || ext == ".m2ts" || ext == ".ts"  ||
		   ext == ".wmv"  || ext == ".asf";
}

CVideoSourceMediaFoundation::CVideoSourceMediaFoundation()  { impl = new SImpl(); }
CVideoSourceMediaFoundation::~CVideoSourceMediaFoundation() { Close(); delete impl; impl = nullptr; }

// Ask the reader to decode into NV12 -- the one format the engine's shader and
// CPU paths already take, and what every Media Foundation decoder produces
// natively, so this adds no conversion of its own.
//
// 10-BIT IS NOT REQUESTED HERE, unlike the AVFoundation source. P010 output
// from the MF pipeline needs a different media subtype and a different plane
// walk, and neither can be verified from macOS; an HDR clip therefore decodes
// at 8 bits on Windows until somebody with the machine adds it. That is a
// recorded gap in HANDOVER-WINDOWS.md, not an oversight.
bool CVideoSourceMediaFoundation::ConfigureStreams()
{
	HRESULT hr = S_OK;

	IMFMediaType *want = nullptr;
	hr = MFCreateMediaType(&want);
	if (FAILED(hr)) { errorReason = "MFCreateMediaType failed"; return false; }
	want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
	want->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
	hr = impl->reader->SetCurrentMediaType(impl->videoStream, NULL, want);
	want->Release();
	if (FAILED(hr))
	{
		errorReason = "this build has no Media Foundation decoder for that video codec";
		return false;
	}

	IMFMediaType *wantA = nullptr;
	if (SUCCEEDED(MFCreateMediaType(&wantA)))
	{
		// THE OS DECODES THE AUDIO TOO -- the Windows half of what keeps AAC,
		// MP3, WMA and PCM working with no FFmpeg linked.
		wantA->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		wantA->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
		wantA->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
		if (SUCCEEDED(impl->reader->SetCurrentMediaType(impl->audioStream, NULL, wantA)))
			impl->haveAudio = true;
		wantA->Release();
	}
	return true;
}

bool CVideoSourceMediaFoundation::FillInfoFromReader()
{
	IMFMediaType *mt = nullptr;
	if (FAILED(impl->reader->GetCurrentMediaType(impl->videoStream, &mt)) || mt == nullptr)
	{
		errorReason = "no video stream";
		return false;
	}

	UINT32 w = 0, h = 0;
	MFGetAttributeSize(mt, MF_MT_FRAME_SIZE, &w, &h);
	info.width = (int)w;
	info.height = (int)h;

	UINT32 num = 0, den = 0;
	if (SUCCEEDED(MFGetAttributeRatio(mt, MF_MT_FRAME_RATE, &num, &den)) && den != 0)
		info.fps = (double)num / (double)den;

	// ROTATION. MF_MT_VIDEO_ROTATION is MFVideoRotationFormat, which counts
	// degrees COUNTER-clockwise -- the same direction FFmpeg's display matrix
	// reports, so unlike AVFoundation's preferredTransform this needs NO sign
	// flip. That asymmetry between the two OS sources is exactly the sort of
	// thing that shipped wrong on macOS once, so it is stated rather than
	// assumed: the macOS value is negated, this one is not. UNVERIFIED here.
	UINT32 rotation = 0;
	if (SUCCEEDED(mt->GetUINT32(MF_MT_VIDEO_ROTATION, &rotation)))
		info.rotationDegrees = (int)(rotation % 360);

	GUID sub = GUID_NULL;
	if (SUCCEEDED(mt->GetGUID(MF_MT_SUBTYPE, &sub)))
	{
		if (sub == MFVideoFormat_H264)      info.videoCodecName = "h264";
		else if (sub == MFVideoFormat_HEVC) info.videoCodecName = "hevc";
		else if (sub == MFVideoFormat_WMV3) info.videoCodecName = "wmv3";
		else if (sub == MFVideoFormat_WVC1) info.videoCodecName = "vc1";
		else if (sub == MFVideoFormat_MPEG2) info.videoCodecName = "mpeg2video";
	}
	mt->Release();

	PROPVARIANT var;
	PropVariantInit(&var);
	if (SUCCEEDED(impl->reader->GetPresentationAttribute(
			(DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
	{
		info.duration = (double)var.uhVal.QuadPart / 10000000.0;   // 100ns units
		PropVariantClear(&var);
	}

	if (impl->haveAudio)
	{
		IMFMediaType *amt = nullptr;
		if (SUCCEEDED(impl->reader->GetCurrentMediaType(impl->audioStream, &amt)) && amt != nullptr)
		{
			UINT32 ch = 0, rate = 0;
			amt->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
			amt->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
			info.hasAudio = true;
			info.audioChannels = (int)(ch ? ch : 2);
			info.audioSampleRate = (u32)(rate ? rate : 44100);
			info.audioCodecName = "pcm";   // post-decode; the source codec is gone by here
			amt->Release();
		}
	}

	// Colour is left at "unspecified" rather than guessed. MF exposes
	// MF_MT_TRANSFER_FUNCTION / MF_MT_VIDEO_PRIMARIES as MFVideo* enums whose
	// mapping onto the AVCOL numbering SVideoInfo carries is NOT the identity,
	// and getting it wrong is how an SDR clip gets treated as HDR. Better an
	// honest "unknown" -- which the engine already handles -- than a wrong
	// definite value. Recorded in the handover as owed work.
	info.colorSpace = VideoColor_NormalizeMatrix(2 /* AVCOL_SPC_UNSPECIFIED */,
												 info.width, info.height);
	return true;
}

bool CVideoSourceMediaFoundation::Open(const char *filePath)
{
	Close();
	errorReason.clear();
	info = SVideoInfo();

	if (filePath == NULL || filePath[0] == '\0') { errorReason = "empty path"; return false; }

	// COM/MF on THIS thread. CVideoPlayer's decode thread is not initialised by
	// anything else, and an MF demuxer makes that unavoidable rather than
	// incidental -- see MFComThreadGuard.h.
	MFComThreadGuard::EnsureComInitializedForThisThread();
	MFComThreadGuard::EnsureMFStarted();

	int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath, -1, NULL, 0);
	std::wstring wpath((size_t)(wlen > 0 ? wlen - 1 : 0), L'\0');
	if (wlen > 0)
		MultiByteToWideChar(CP_UTF8, 0, filePath, -1, &wpath[0], wlen);

	IMFAttributes *attrs = nullptr;
	if (SUCCEEDED(MFCreateAttributes(&attrs, 1)))
		attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

	IMFSourceReader *reader = nullptr;
	HRESULT hr = MFCreateSourceReaderFromURL(wpath.c_str(), attrs, &reader);
	if (attrs) attrs->Release();
	if (FAILED(hr) || reader == nullptr)
	{
		errorReason = "Media Foundation could not open this file";
		return false;
	}
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		impl->reader = reader;
	}

	if (!ConfigureStreams()) { Close(); return false; }
	if (!FillInfoFromReader()) { Close(); return false; }
	return true;
}

bool CVideoSourceMediaFoundation::ReadVideoFrame(SDecodedVideoFrame &out)
{
	errorReason.clear();
	IMFSourceReader *r = nullptr;
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		r = impl->reader;
	}
	if (r == nullptr) { errorReason = "read before open"; return false; }
	if (IsAborted()) return false;

	while (true)
	{
		if (IsAborted()) return false;

		DWORD flags = 0;
		LONGLONG ts = 0;
		IMFSample *sample = nullptr;
		HRESULT hr = r->ReadSample(impl->videoStream, 0, NULL, &flags, &ts, &sample);
		if (FAILED(hr))
		{
			errorReason = "Media Foundation ReadSample failed";
			if (sample) sample->Release();
			return false;
		}
		if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
		{
			if (sample) sample->Release();
			return false;   // EOS: false with an EMPTY errorReason
		}
		if (sample == nullptr)
			continue;   // a gap or a format change carrying no data

		lastVideoPts = (double)ts / 10000000.0;

		IMFMediaBuffer *buf = nullptr;
		if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || buf == nullptr)
		{
			sample->Release();
			continue;
		}

		BYTE *data = nullptr;
		DWORD maxLen = 0, curLen = 0;
		if (FAILED(buf->Lock(&data, &maxLen, &curLen)))
		{
			buf->Release(); sample->Release();
			continue;
		}

		// NV12: Y plane of height rows, then interleaved CbCr of height/2 rows,
		// both at the frame width. ReadSample's contiguous buffer is packed at
		// exactly that stride.
		frameWidth = info.width;
		frameHeight = info.height;
		strideY = frameWidth;
		strideUV = frameWidth;
		const size_t ySize = (size_t)strideY * frameHeight;
		const size_t uvSize = (size_t)strideUV * (frameHeight / 2);
		if (curLen >= ySize + uvSize)
		{
			planeY.resize(ySize);
			planeUV.resize(uvSize);
			memcpy(planeY.data(), data, ySize);
			memcpy(planeUV.data(), data + ySize, uvSize);
		}
		buf->Unlock();
		buf->Release();
		sample->Release();

		out.width = frameWidth;
		out.height = frameHeight;
		out.pixelFormat = EVideoPixelFormat::NV12;
		out.plane[0] = planeY.data();  out.stride[0] = strideY;
		out.plane[1] = planeUV.data(); out.stride[1] = strideUV;
		out.plane[2] = nullptr;        out.stride[2] = 0;
		out.plane[3] = nullptr;        out.stride[3] = 0;
		out.pts = lastVideoPts;
		return true;
	}
}

bool CVideoSourceMediaFoundation::ReadAudio(SDecodedAudio &out)
{
	IMFSourceReader *r = nullptr;
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		r = impl->reader;
	}
	if (r == nullptr || !impl->haveAudio) return false;
	if (IsAborted()) return false;

	DWORD flags = 0;
	LONGLONG ts = 0;
	IMFSample *sample = nullptr;
	if (FAILED(r->ReadSample(impl->audioStream, 0, NULL, &flags, &ts, &sample)) ||
		(flags & MF_SOURCE_READERF_ENDOFSTREAM) || sample == nullptr)
	{
		if (sample) sample->Release();
		return false;
	}

	IMFMediaBuffer *buf = nullptr;
	if (FAILED(sample->ConvertToContiguousBuffer(&buf)) || buf == nullptr)
	{
		sample->Release();
		return false;
	}
	BYTE *data = nullptr;
	DWORD maxLen = 0, curLen = 0;
	if (FAILED(buf->Lock(&data, &maxLen, &curLen)))
	{
		buf->Release(); sample->Release();
		return false;
	}
	audioPcm.resize(curLen / sizeof(s16));
	if (!audioPcm.empty())
		memcpy(audioPcm.data(), data, audioPcm.size() * sizeof(s16));
	buf->Unlock();
	buf->Release();
	sample->Release();

	out.pcm = audioPcm;
	out.channels = info.audioChannels;
	out.sampleRate = info.audioSampleRate;
	out.pts = (double)ts / 10000000.0;
	return true;
}

bool CVideoSourceMediaFoundation::Seek(double seconds)
{
	errorReason.clear();
	IMFSourceReader *r = nullptr;
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		r = impl->reader;
	}
	if (r == nullptr) { errorReason = "seek before open"; return false; }

	// Clear the cancel flag FIRST -- a Seek IS the new work, and the flag was
	// raised on its behalf. Honouring it here made every seek fail on the macOS
	// twin; the same shape would do the same here.
	cancelRequested.store(false, std::memory_order_release);
	if (abortPredicate && abortPredicate())
		return false;

	double target = seconds;
	if (target < 0.0) target = 0.0;
	if (info.duration > 0.0 && target > info.duration) target = info.duration;

	PROPVARIANT pos;
	PropVariantInit(&pos);
	pos.vt = VT_I8;
	pos.hVal.QuadPart = (LONGLONG)(target * 10000000.0);
	HRESULT hr = r->SetCurrentPosition(GUID_NULL, pos);
	PropVariantClear(&pos);
	if (FAILED(hr))
	{
		errorReason = "Media Foundation seek failed";
		return false;
	}
	lastVideoPts = target;
	return true;
}

void CVideoSourceMediaFoundation::SetAbortPredicate(std::function<bool()> pred)
{
	abortPredicate = std::move(pred);
}

void CVideoSourceMediaFoundation::WakeAbort()
{
	// Cooperative, exactly as on macOS and for the same measured reason: tearing
	// the reader down from another thread while ReadSample is in flight is not
	// safe. Raise the flag; the read loops honour it between reads.
	//
	// The macOS note about this costing at most one read's latency applies with
	// LESS force here: a synchronous MF ReadSample can take much longer than an
	// AVAssetReader read, and the HEVC pipeline has been measured waiting up to
	// ten seconds per step elsewhere in this engine. If a Close is observed to
	// hang on Windows, THIS is the first place to look, and the fix is async
	// ReadSample (IMFSourceReaderCallback) rather than a destructive cancel.
	cancelRequested.store(true, std::memory_order_release);
}

void CVideoSourceMediaFoundation::Close()
{
	IMFSourceReader *doomed = nullptr;
	{
		std::lock_guard<std::mutex> lock(readerMutex);
		doomed = impl ? impl->reader : nullptr;
		if (impl) { impl->reader = nullptr; impl->haveAudio = false; }
	}
	if (doomed) doomed->Release();

	planeY.clear(); planeUV.clear(); audioPcm.clear();
	strideY = strideUV = 0;
	frameWidth = frameHeight = 0;
	lastVideoPts = 0.0;
	cancelRequested.store(false, std::memory_order_release);
	errorReason.clear();
}

#endif // _WIN32 && MT_CAP_VIDEO_PLAYBACK
