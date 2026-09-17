#include "CAudioDecoderCompressedMF.h"

#if defined(_WIN32)

#include "MT_AudioFrames.h"
#include "MFComThreadGuard.h"
#include "DBG_Log.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>
#include <mferror.h>
#include <wmcodecdsp.h>
#include <cstring>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")

using MFComThreadGuard::EnsureComInitializedForThisThread;
using MFComThreadGuard::EnsureMFStarted;

CAudioDecoderCompressedMF::CAudioDecoderCompressedMF() {}
CAudioDecoderCompressedMF::~CAudioDecoderCompressedMF() { Teardown(); }

void CAudioDecoderCompressedMF::Teardown()
{
	if (decoderMFT) { decoderMFT->Release(); decoderMFT = nullptr; }
	outputChannels = 0;
	outputSampleRate = 0;
	carry.clear();
}

bool CAudioDecoderCompressedMF::Init(EMediaCodecId codec, u32 sampleRate, int channels)
{
	Teardown();
	errorReason.clear();
	codecId = codec;

	GUID subtype = {};
	switch (codec)
	{
		case EMediaCodecId::MP3: subtype = MFAudioFormat_MP3; break;
		case EMediaCodecId::MP2: subtype = MFAudioFormat_MPEG; break;
		case EMediaCodecId::AC3: subtype = MFAudioFormat_Dolby_AC3; break;
		default:
			errorReason = "CAudioDecoderCompressedMF: unsupported codec";
			return false;
	}
	if (sampleRate == 0 || channels <= 0)
	{
		errorReason = "CAudioDecoderCompressedMF: container gave no rate or channel count";
		return false;
	}

	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	// ENUMERATED, NOT A FIXED CLSID. There is no single documented CLSID for
	// "the MP3 decoder" the way CLSID_CMSAACDecMFT names the AAC one, and
	// enumerating also answers honestly on an edition where the decoder is
	// absent instead of failing later with a confusing error.
	MFT_REGISTER_TYPE_INFO inInfo = { MFMediaType_Audio, subtype };
	IMFActivate **activates = nullptr;
	UINT32 count = 0;
	HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER, MFT_ENUM_FLAG_SYNCMFT,
						   &inInfo, nullptr, &activates, &count);
	if (FAILED(hr) || count == 0)
	{
		if (activates) CoTaskMemFree(activates);
		errorReason = "no Media Foundation decoder for this audio codec on this machine";
		return false;
	}
	hr = activates[0]->ActivateObject(IID_PPV_ARGS(&decoderMFT));
	for (UINT32 i = 0; i < count; i++) if (activates[i]) activates[i]->Release();
	CoTaskMemFree(activates);
	if (FAILED(hr) || !decoderMFT)
	{
		decoderMFT = nullptr;
		errorReason = "IMFActivate::ActivateObject failed for the audio decoder";
		return false;
	}

	IMFMediaType *inputType = nullptr;
	hr = MFCreateMediaType(&inputType);
	if (SUCCEEDED(hr))
	{
		inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		inputType->SetGUID(MF_MT_SUBTYPE, subtype);
		inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
		inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
		hr = decoderMFT->SetInputType(0, inputType, 0);
	}
	if (inputType) inputType->Release();
	if (FAILED(hr))
	{
		errorReason = "IMFTransform::SetInputType failed";
		Teardown();
		return false;
	}

	if (!SetOutputTypePCM())
	{
		Teardown();
		return false;
	}
	return true;
}

// Pick the MFT's first 16-bit PCM output type, exactly as CAudioDecoderAACMF
// does, and LEARN rate/channels from it rather than assuming the container's.
bool CAudioDecoderCompressedMF::SetOutputTypePCM()
{
	if (!decoderMFT) return false;
	for (DWORD i = 0; ; i++)
	{
		IMFMediaType *candidate = nullptr;
		if (FAILED(decoderMFT->GetOutputAvailableType(0, i, &candidate)) || !candidate)
			break;
		GUID sub = {};
		UINT32 bits = 0;
		const bool isPcm16 = SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &sub)) &&
							 sub == MFAudioFormat_PCM &&
							 SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) &&
							 bits == 16;
		if (isPcm16 && SUCCEEDED(decoderMFT->SetOutputType(0, candidate, 0)))
		{
			UINT32 ch = 0, rate = 0;
			if (SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &ch)) && ch > 0)
				outputChannels = (int)ch;
			if (SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &rate)) && rate > 0)
				outputSampleRate = (u32)rate;
			candidate->Release();
			return true;
		}
		candidate->Release();
	}
	errorReason = "no 16-bit PCM output type offered by the audio decoder MFT";
	return false;
}

bool CAudioDecoderCompressedMF::DecodeOneFrame(const u8 *data, int size, std::vector<s16> &outPCM)
{
	IMFSample *inputSample = nullptr;
	IMFMediaBuffer *inputBuffer = nullptr;
	HRESULT hr = MFCreateSample(&inputSample);
	if (SUCCEEDED(hr)) hr = MFCreateMemoryBuffer((DWORD)size, &inputBuffer);
	if (SUCCEEDED(hr))
	{
		BYTE *raw = nullptr;
		hr = inputBuffer->Lock(&raw, nullptr, nullptr);
		if (SUCCEEDED(hr))
		{
			memcpy(raw, data, (size_t)size);
			inputBuffer->Unlock();
			inputBuffer->SetCurrentLength((DWORD)size);
			hr = inputSample->AddBuffer(inputBuffer);
		}
	}
	if (SUCCEEDED(hr)) hr = decoderMFT->ProcessInput(0, inputSample, 0);
	if (inputBuffer) inputBuffer->Release();
	if (inputSample) inputSample->Release();
	if (FAILED(hr))
	{
		errorReason = "IMFTransform::ProcessInput failed";
		return false;
	}

	MFT_OUTPUT_STREAM_INFO streamInfo = {};
	decoderMFT->GetOutputStreamInfo(0, &streamInfo);
	bool mftProvidesSamples = (streamInfo.dwFlags &
		(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

	for (;;)
	{
		IMFSample *outputSample = nullptr;
		if (!mftProvidesSamples)
		{
			IMFMediaBuffer *outputBuffer = nullptr;
			if (FAILED(MFCreateSample(&outputSample)) ||
				FAILED(MFCreateMemoryBuffer(streamInfo.cbSize, &outputBuffer)))
			{
				if (outputSample) outputSample->Release();
				errorReason = "failed to allocate MFT output sample/buffer";
				return false;
			}
			outputSample->AddBuffer(outputBuffer);
			outputBuffer->Release();
		}

		MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
		outputDataBuffer.dwStreamID = 0;
		outputDataBuffer.pSample = outputSample;
		DWORD statusFlags = 0;
		HRESULT hrOut = decoderMFT->ProcessOutput(0, 1, &outputDataBuffer, &statusFlags);

		if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			break;   // this frame is drained -- normal
		}
		if (hrOut == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			if (!SetOutputTypePCM())
			{
				errorReason = "output type renegotiation failed after MF_E_TRANSFORM_STREAM_CHANGE";
				return false;
			}
			decoderMFT->GetOutputStreamInfo(0, &streamInfo);
			mftProvidesSamples = (streamInfo.dwFlags &
				(MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;
			continue;
		}
		if (FAILED(hrOut))
		{
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			errorReason = "IMFTransform::ProcessOutput failed";
			return false;
		}

		IMFSample *resultSample = outputDataBuffer.pSample ? outputDataBuffer.pSample : outputSample;
		IMFMediaBuffer *contiguous = nullptr;
		if (resultSample && SUCCEEDED(resultSample->ConvertToContiguousBuffer(&contiguous)))
		{
			BYTE *raw = nullptr;
			DWORD rawLen = 0;
			if (SUCCEEDED(contiguous->Lock(&raw, nullptr, &rawLen)))
			{
				const size_t sampleCount = (size_t)rawLen / sizeof(s16);
				const size_t oldSize = outPCM.size();
				outPCM.resize(oldSize + sampleCount);
				memcpy(outPCM.data() + oldSize, raw, (size_t)rawLen);
				contiguous->Unlock();
			}
			contiguous->Release();
		}
		if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
		if (outputDataBuffer.pSample) outputDataBuffer.pSample->Release();
		if (outputSample && outputSample != outputDataBuffer.pSample) outputSample->Release();
	}
	return true;
}

bool CAudioDecoderCompressedMF::DecodeChunk(const u8 *data, int size, std::vector<s16> &outPCM,
											int &outChannels, u32 &outSampleRate)
{
	outPCM.clear();
	outChannels = outputChannels;
	outSampleRate = outputSampleRate;

	if (!decoderMFT)
	{
		errorReason = "transform not initialized";
		return false;
	}
	if (!data || size <= 0)
		return true;

	const u8 *buf;
	size_t avail;
	std::vector<u8> joined;
	if (carry.empty()) { buf = data; avail = (size_t)size; }
	else
	{
		joined.reserve(carry.size() + (size_t)size);
		joined.insert(joined.end(), carry.begin(), carry.end());
		joined.insert(joined.end(), data, data + size);
		buf = joined.data(); avail = joined.size();
	}

	const bool isAc3 = (codecId == EMediaCodecId::AC3);
	size_t pos = 0;
	for (;;)
	{
		pos += isAc3 ? MT_AudioFrames::FindAc3Sync(buf + pos, avail - pos)
					 : MT_AudioFrames::FindMpegSync(buf + pos, avail - pos);
		if (pos >= avail) break;
		const size_t frameSize = isAc3
			? MT_AudioFrames::Ac3FrameSize(buf + pos, avail - pos)
			: MT_AudioFrames::MpegFrameSize(buf + pos, avail - pos);
		if (frameSize == 0) break;
		if (!DecodeOneFrame(buf + pos, (int)frameSize, outPCM))
			return false;
		pos += frameSize;
	}

	carry.assign(buf + pos, buf + avail);
	if (carry.size() > 64 * 1024) carry.clear();

	outChannels = outputChannels;
	outSampleRate = outputSampleRate;
	return true;
}

#endif // _WIN32
