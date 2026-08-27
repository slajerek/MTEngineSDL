// Native WMA decode via Windows Media Foundation's in-box WMAudio Decoder --
// see the header for the full rationale. Structure mirrors
// CAudioDecoderAACMF.cpp method-for-method; divergences are marked WMA-DIVERGENCE.
#include "CAudioDecoderWMAMF.h"

#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "DBG_Log.h"
#include "MFComThreadGuard.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <cstring>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mftransform.h>
// CLSID_CWMADecMediaObject (the in-box WMAudio Decoder DMO/MFT's CLSID) --
// declared in wmcodecdsp.h alongside the other built-in transform CLSIDs
// (same header the AAC twin pulls CLSID_CMSAACDecMFT from).
#include <wmcodecdsp.h>

using MFComThreadGuard::EnsureComInitializedForThisThread;
using MFComThreadGuard::EnsureMFStarted;

namespace
{
	// WMA-DIVERGENCE: input subtypes are built from the WAVE_FORMAT tag via
	// the documented MFAudioFormat_Base convention ({tag}-0000-0010-8000-
	// 00AA00389B71) instead of naming per-flavor SDK GUID constants -- the
	// three tags are stable ABI (WAVE_FORMAT_MSAUDIO1/WMAUDIO2/WMAUDIO3),
	// while the SDK's per-flavor GUID *names* have shifted across header
	// generations.
	GUID WmaSubtypeForCodecId(int avCodecId)
	{
		unsigned int waveTag;
		switch (avCodecId)
		{
			case AV_CODEC_ID_WMAV1:  waveTag = 0x0160; break; // WAVE_FORMAT_MSAUDIO1 (WMA v1)
			case AV_CODEC_ID_WMAV2:  waveTag = 0x0161; break; // WAVE_FORMAT_WMAUDIO2 (WMA v2 / "standard")
			case AV_CODEC_ID_WMAPRO: waveTag = 0x0162; break; // WAVE_FORMAT_WMAUDIO3 (WMA Pro)
			default:                 waveTag = 0x0161; break;
		}
		GUID subtype = MFAudioFormat_Base;
		subtype.Data1 = waveTag;
		return subtype;
	}

	// Resolve the decoder MFT by input subtype (hardware tier is meaningless
	// for WMA -- software flags only), falling back to direct instantiation
	// of the documented in-box CLSID. Returns an AddRef'd IMFTransform or
	// null.
	IMFTransform *CreateWmaDecoderMFT(const GUID &inputSubtype)
	{
		MFT_REGISTER_TYPE_INFO inInfo = {};
		inInfo.guidMajorType = MFMediaType_Audio;
		inInfo.guidSubtype = inputSubtype;

		IMFTransform *result = nullptr;

		IMFActivate **activates = nullptr;
		UINT32 count = 0;
		HRESULT hr = MFTEnumEx(MFT_CATEGORY_AUDIO_DECODER,
							   MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER,
							   &inInfo, nullptr, &activates, &count);
		if (SUCCEEDED(hr) && count > 0 && activates && activates[0])
			activates[0]->ActivateObject(IID_PPV_ARGS(&result));
		if (activates)
		{
			for (UINT32 i = 0; i < count; i++)
				if (activates[i]) activates[i]->Release();
			CoTaskMemFree(activates);
		}
		if (result)
			return result;

		// Fallback: the in-box WMAudio Decoder DMO/MFT, instantiated directly.
		if (SUCCEEDED(CoCreateInstance(CLSID_CWMADecMediaObject, nullptr, CLSCTX_INPROC_SERVER,
									   IID_PPV_ARGS(&result))))
			return result;
		return nullptr;
	}
}

// ============================================================================
// Construction / teardown
// ============================================================================
CAudioDecoderWMAMF::CAudioDecoderWMAMF()
{
}

CAudioDecoderWMAMF::~CAudioDecoderWMAMF()
{
	Teardown();
}

void CAudioDecoderWMAMF::Teardown()
{
	if (decoderMFT)
	{
		decoderMFT->Release();
		decoderMFT = nullptr;
	}
	outputChannels = 0;
	outputSampleRate = 0;
}

// ============================================================================
// Init -- WAVEFORMATEX-style params + codec-private extradata -> IMFTransform
// ============================================================================
bool CAudioDecoderWMAMF::Init(int avCodecId, const u8 *extradata, int extradataSize,
							  int channels, int sampleRate, int blockAlign, int avgBytesPerSec)
{
	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	Teardown();
	errorReason.clear();

	if (channels <= 0 || sampleRate <= 0)
	{
		errorReason = "invalid WMA stream parameters (channels/sampleRate)";
		return false;
	}

	const GUID inputSubtype = WmaSubtypeForCodecId(avCodecId);
	decoderMFT = CreateWmaDecoderMFT(inputSubtype);
	if (!decoderMFT)
	{
		errorReason = "no WMAudio decoder MFT available (N edition without the Media Feature Pack?)";
		return false;
	}

	IMFMediaType *inputType = nullptr;
	HRESULT hr = MFCreateMediaType(&inputType);
	if (SUCCEEDED(hr))
	{
		inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		inputType->SetGUID(MF_MT_SUBTYPE, inputSubtype);
		inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)channels);
		inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)sampleRate);
		// WMA-DIVERGENCE: nBlockAlign and nAvgBytesPerSec are LOAD-BEARING for
		// the WMA decoder (they parameterize the superframe layout; the MFT
		// rejects the type or mis-decodes without them). FFmpeg's asf demuxer
		// carries both through AVCodecParameters verbatim.
		if (blockAlign > 0)
			inputType->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, (UINT32)blockAlign);
		if (avgBytesPerSec > 0)
			inputType->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, (UINT32)avgBytesPerSec);
		// MF_MT_USER_DATA carries the WAVEFORMATEX cbSize tail verbatim (the
		// same bytes FFmpeg exposes as codecpar->extradata: 4 for v1, 10 for
		// v2, 18 for Pro) -- the documented MFInitMediaTypeFromWaveFormatEx
		// mapping, hand-rolled here because we start from AVCodecParameters,
		// not a WAVEFORMATEX.
		if (extradata && extradataSize > 0)
			inputType->SetBlob(MF_MT_USER_DATA, extradata, (UINT32)extradataSize);

		hr = decoderMFT->SetInputType(0, inputType, 0);
		inputType->Release();
	}

	if (FAILED(hr))
	{
		errorReason = "IMFTransform::SetInputType (WMA) failed";
		Teardown();
		return false;
	}

	if (!SetOutputTypePCM())
	{
		errorReason = "IMFTransform output type negotiation (16-bit PCM) failed";
		Teardown();
		return false;
	}

	return true;
}

// ============================================================================
// SetOutputTypePCM -- verbatim mechanism from CAudioDecoderAACMF (see that
// file's doc comment).
// ============================================================================
bool CAudioDecoderWMAMF::SetOutputTypePCM()
{
	if (!decoderMFT)
		return false;

	for (DWORD typeIndex = 0;; typeIndex++)
	{
		IMFMediaType *candidate = nullptr;
		HRESULT hr = decoderMFT->GetOutputAvailableType(0, typeIndex, &candidate);
		if (hr == MF_E_NO_MORE_TYPES || FAILED(hr))
			break;

		GUID subtype = {};
		UINT32 bits = 0;
		bool isPcm16 = SUCCEEDED(candidate->GetGUID(MF_MT_SUBTYPE, &subtype)) &&
					   subtype == MFAudioFormat_PCM &&
					   SUCCEEDED(candidate->GetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, &bits)) &&
					   bits == 16;

		if (isPcm16 && SUCCEEDED(decoderMFT->SetOutputType(0, candidate, 0)))
		{
			UINT32 channels = 0, sampleRate = 0;
			candidate->GetUINT32(MF_MT_AUDIO_NUM_CHANNELS, &channels);
			candidate->GetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, &sampleRate);
			outputChannels = (int)channels;
			outputSampleRate = (u32)sampleRate;
			candidate->Release();
			return true;
		}

		candidate->Release();
	}

	return false;
}

// ============================================================================
// DecodePacket -- verbatim ProcessInput/ProcessOutput loop from
// CAudioDecoderAACMF::DecodePacket, minus the ADTS strip (WMA-DIVERGENCE:
// ASF audio payloads carry no framing header to remove).
// ============================================================================
bool CAudioDecoderWMAMF::DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
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
	{
		errorReason = "empty packet";
		return false;
	}

	IMFSample *inputSample = nullptr;
	IMFMediaBuffer *inputBuffer = nullptr;
	HRESULT hr = MFCreateSample(&inputSample);
	if (SUCCEEDED(hr))
		hr = MFCreateMemoryBuffer((DWORD)size, &inputBuffer);
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
	if (SUCCEEDED(hr))
		hr = decoderMFT->ProcessInput(0, inputSample, 0);

	if (inputBuffer) inputBuffer->Release();
	if (inputSample) inputSample->Release();

	if (FAILED(hr))
	{
		errorReason = "IMFTransform::ProcessInput (WMA) failed";
		return false;
	}

	MFT_OUTPUT_STREAM_INFO streamInfo = {};
	decoderMFT->GetOutputStreamInfo(0, &streamInfo);
	bool mftProvidesSamples = (streamInfo.dwFlags &
							   (MFT_OUTPUT_STREAM_PROVIDES_SAMPLES | MFT_OUTPUT_STREAM_CAN_PROVIDE_SAMPLES)) != 0;

	while (true)
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
			outputBuffer->Release(); // sample holds its own ref via AddBuffer
		}

		MFT_OUTPUT_DATA_BUFFER outputDataBuffer = {};
		outputDataBuffer.dwStreamID = 0;
		outputDataBuffer.pSample = outputSample; // nullptr if the MFT provides its own

		DWORD statusFlags = 0;
		HRESULT hrOut = decoderMFT->ProcessOutput(0, 1, &outputDataBuffer, &statusFlags);

		if (hrOut == MF_E_TRANSFORM_NEED_MORE_INPUT)
		{
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			break; // this packet's output is fully drained -- normal end, not an error
		}

		if (hrOut == MF_E_TRANSFORM_STREAM_CHANGE)
		{
			// First-output-type resolution on some MFT implementations (WMA
			// has no mid-stream rate change, but the handling is identical
			// to the AAC twin's).
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			if (!SetOutputTypePCM())
			{
				errorReason = "IMFTransform output type renegotiation failed after MF_E_TRANSFORM_STREAM_CHANGE";
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
			errorReason = "IMFTransform::ProcessOutput (WMA) failed";
			return false;
		}

		// Success: append this call's decoded interleaved s16 PCM.
		IMFSample *resultSample = outputDataBuffer.pSample ? outputDataBuffer.pSample : outputSample;
		IMFMediaBuffer *contiguous = nullptr;
		if (resultSample && SUCCEEDED(resultSample->ConvertToContiguousBuffer(&contiguous)))
		{
			BYTE *raw = nullptr;
			DWORD rawLen = 0;
			if (SUCCEEDED(contiguous->Lock(&raw, nullptr, &rawLen)))
			{
				size_t sampleCount = (size_t)rawLen / sizeof(s16);
				size_t oldSize = outPCM.size();
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

	outChannels = outputChannels;
	outSampleRate = outputSampleRate;
	return true; // true regardless of whether outPCM ended up empty (priming), per class contract
}

#endif // _WIN32 && MT_ENABLE_FFMPEG
