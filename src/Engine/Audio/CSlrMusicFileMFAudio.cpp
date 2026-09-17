#include "CSlrMusicFileMFAudio.h"

#if defined(_WIN32) && (!defined(MT_CAP_MUSIC_PLAYER) || MT_CAP_MUSIC_PLAYER)

#include "MFComThreadGuard.h"
#include "SND_SoundEngine.h"
#include "DBG_Log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <propvarutil.h>
#include <cctype>
#include <cstring>
#include <algorithm>

#pragma comment(lib, "mfreadwrite.lib")

struct CSlrMusicFileMFAudio::SImpl { IMFSourceReader *reader = nullptr; };

bool CSlrMusicFileMFAudio::CanPlay(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0') return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos) return false;
	std::string ext = p.substr(dot + 1);
	for (char &c : ext) c = (char)tolower((unsigned char)c);
	// Matches the macOS twin's set minus .caf/.aiff, which MF does not read.
	// .wma is NOT here: it belongs to the video-side WMA decoder's territory
	// and adding it would put two owners on one extension.
	return ext == "m4a" || ext == "aac";
}

CSlrMusicFileMFAudio::CSlrMusicFileMFAudio(const char *filePath)
: CSlrMusicFileResampled(filePath)
{
	impl = new SImpl();
	this->shouldBeDestroyedByEngine = false;
	this->shouldBeRemovedByEngine = false;
	if (filePath != NULL) Init(filePath);
}

CSlrMusicFileMFAudio::~CSlrMusicFileMFAudio()
{
	if (impl) { if (impl->reader) impl->reader->Release(); delete impl; impl = nullptr; }
}

bool CSlrMusicFileMFAudio::IsOpen() const { return impl != nullptr && impl->reader != nullptr; }

bool CSlrMusicFileMFAudio::Init(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0') return false;
	path = filePath;

	MFComThreadGuard::EnsureComInitializedForThisThread();
	MFComThreadGuard::EnsureMFStarted();

	int wlen = MultiByteToWideChar(CP_UTF8, 0, filePath, -1, NULL, 0);
	std::wstring wpath((size_t)(wlen > 0 ? wlen - 1 : 0), L'\0');
	if (wlen > 0) MultiByteToWideChar(CP_UTF8, 0, filePath, -1, &wpath[0], wlen);

	IMFSourceReader *reader = nullptr;
	if (FAILED(MFCreateSourceReaderFromURL(wpath.c_str(), NULL, &reader)) || reader == nullptr)
	{
		LOGError("CSlrMusicFileMFAudio: Media Foundation could not open '%s'", filePath);
		return false;
	}
	impl->reader = reader;

	// The FILE's own rate: read it before asking for PCM, so the decoded output
	// stays in native frames and the base's sample invariant holds.
	IMFMediaType *native = nullptr;
	if (SUCCEEDED(reader->GetNativeMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &native)) && native)
	{
		UINT32 rate = 0, ch = 0;
		native->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate);
		native->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch);
		sourceSampleRate = (u32)rate;
		sourceChannels = (int)ch;
		native->Release();
	}
	if (sourceSampleRate == 0) sourceSampleRate = SND_GetEngineSampleRate();
	if (sourceChannels <= 0) sourceChannels = 2;

	IMFMediaType *want = nullptr;
	if (FAILED(MFCreateMediaType(&want))) return false;
	want->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
	want->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
	want->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
	want->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);          // the base wants stereo
	want->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, sourceSampleRate);
	HRESULT hr = reader->SetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM, NULL, want);
	want->Release();
	if (FAILED(hr))
	{
		LOGError("CSlrMusicFileMFAudio: no Media Foundation decoder for '%s' "
				 "(Windows N/KN ships none for AAC -- tracker 4.7)", filePath);
		return false;
	}

	PROPVARIANT var; PropVariantInit(&var);
	if (SUCCEEDED(reader->GetPresentationAttribute((DWORD)MF_SOURCE_READER_MEDIASOURCE, MF_PD_DURATION, &var)))
	{
		const double seconds = (double)var.uhVal.QuadPart / 10000000.0;
		lengthFrames = (u64)(seconds * (double)sourceSampleRate);
		PropVariantClear(&var);
	}
	cursorFrames = 0;
	ResetResampler();
	return true;
}

u32 CSlrMusicFileMFAudio::DecodeNativeStereo(s16 *out, u32 frames)
{
	if (!IsOpen() || out == NULL || frames == 0) return 0;
	u32 written = 0;
	while (written < frames)
	{
		if (carryPos < carry.size())
		{
			const size_t avail = (carry.size() - carryPos) / 2;
			const size_t take = std::min((size_t)(frames - written), avail);
			memcpy(out + (size_t)written * 2, carry.data() + carryPos, take * 2 * sizeof(s16));
			carryPos += take * 2;
			written += (u32)take;
			continue;
		}
		DWORD flags = 0; LONGLONG ts = 0; IMFSample *sample = nullptr;
		if (FAILED(impl->reader->ReadSample((DWORD)MF_SOURCE_READER_FIRST_AUDIO_STREAM,
											0, NULL, &flags, &ts, &sample)))
			break;
		if ((flags & MF_SOURCE_READERF_ENDOFSTREAM) || sample == nullptr)
		{
			if (sample) sample->Release();
			break;
		}
		IMFMediaBuffer *buf = nullptr;
		if (SUCCEEDED(sample->ConvertToContiguousBuffer(&buf)) && buf)
		{
			BYTE *data = nullptr; DWORD maxLen = 0, curLen = 0;
			if (SUCCEEDED(buf->Lock(&data, &maxLen, &curLen)))
			{
				carry.resize(curLen / sizeof(s16));
				if (!carry.empty()) memcpy(carry.data(), data, carry.size() * sizeof(s16));
				carryPos = 0;
				buf->Unlock();
			}
			buf->Release();
		}
		sample->Release();
		if (carry.empty()) break;
	}
	cursorFrames += written;
	return written;
}

void CSlrMusicFileMFAudio::FillBuffer(int *mixBuffer, u32 numSamples)
{
	if (!IsOpen() || !playing)
	{
		memset((signed short *)mixBuffer, 0, (size_t)numSamples * 2 * sizeof(signed short));
		return;
	}
	FillResampled(mixBuffer, numSamples);
	if (lengthFrames > 0 && cursorFrames >= lengthFrames) playing = false;
}

void CSlrMusicFileMFAudio::Mix(int *mixBuffer, u32 numSamples) { FillBuffer(mixBuffer, numSamples); }
void CSlrMusicFileMFAudio::Play()  { if (IsOpen()) playing = true; }
void CSlrMusicFileMFAudio::Pause() { playing = false; }
void CSlrMusicFileMFAudio::Stop()  { playing = false; Rewind(); }
bool CSlrMusicFileMFAudio::IsPlaying() { return playing; }
void CSlrMusicFileMFAudio::Rewind() { SeekToSample(0); }
u64 CSlrMusicFileMFAudio::GetCurrentSampleNum() { return cursorFrames; }
u64 CSlrMusicFileMFAudio::GetLengthSamples()    { return lengthFrames; }
u32 CSlrMusicFileMFAudio::ResourceGetSize()     { return (u32)(lengthFrames * 4); }

void CSlrMusicFileMFAudio::SeekToSample(u64 sampleNum)
{
	if (!IsOpen() || sourceSampleRate == 0) return;
	if (lengthFrames > 0 && sampleNum > lengthFrames) sampleNum = lengthFrames;
	// Native frames in, 100ns units out -- via the FILE's rate, never the engine's.
	PROPVARIANT pos; PropVariantInit(&pos);
	pos.vt = VT_I8;
	pos.hVal.QuadPart = (LONGLONG)((double)sampleNum / (double)sourceSampleRate * 10000000.0);
	impl->reader->SetCurrentPosition(GUID_NULL, pos);
	PropVariantClear(&pos);
	carry.clear(); carryPos = 0;
	cursorFrames = sampleNum;
	ResetResampler();
}

#endif
