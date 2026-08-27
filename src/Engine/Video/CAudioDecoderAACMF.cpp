// Native AAC decode via Windows Media Foundation's in-box AAC decoder MFT.
#include "CAudioDecoderAACMF.h"

#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "AacAscParser.h"
#include "DBG_Log.h"
#include "MFComThreadGuard.h"

#include <cstring>

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
// IMFTransform's methods, MFT_OUTPUT_DATA_BUFFER, MFT_OUTPUT_STREAM_INFO,
// MFT_OUTPUT_STREAM_PROVIDES_SAMPLES -- declared here, not reliably pulled in
// transitively via mfapi.h/mfidl.h/wmcodecdsp.h. The HEVC sibling
// (CVideoDecoderHEVCMF.cpp) already includes this explicitly for the same
// reason.
#include <mftransform.h>
// CLSID_CMSAACDecMFT (the in-box "Microsoft AAC Decoder" MFT's CLSID) is
// declared in wmcodecdsp.h alongside the other built-in Media Foundation
// transform CLSIDs (e.g. CLSID_CMSH264DecoderMFT) -- verify against the
// actual Windows SDK on first compile; if it has moved, grep the SDK's
// Include/**/um for "CMSAACDecMFT".
#include <wmcodecdsp.h>

// ============================================================================
// Thread-affine COM + process-wide MF lifetime
// ============================================================================
// Hoisted into MFComThreadGuard.h (Task 6) once CVideoDecoderHEVCMF needed
// the identical mechanism -- see that header for the full comment. Using
// declarations below keep every call site in this file unchanged
// (EnsureComInitializedForThisThread()/EnsureMFStarted(), no namespace
// prefix).
using MFComThreadGuard::EnsureComInitializedForThisThread;
using MFComThreadGuard::EnsureMFStarted;

// ============================================================================
// Construction / teardown
// ============================================================================
CAudioDecoderAACMF::CAudioDecoderAACMF()
{
}

CAudioDecoderAACMF::~CAudioDecoderAACMF()
{
	Teardown();
}

void CAudioDecoderAACMF::Teardown()
{
	if (decoderMFT)
	{
		decoderMFT->Release();
		decoderMFT = nullptr;
	}
	outputChannels = 0;
	outputSampleRate = 0;
	inputChannels = 0;
}

// ============================================================================
// Init -- AudioSpecificConfig -> IMFTransform (input MFAudioFormat_AAC, output MFAudioFormat_PCM)
// ============================================================================
bool CAudioDecoderAACMF::Init(const u8 *asc, int ascSize)
{
	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	Teardown();
	errorReason.clear();

	if (!asc || ascSize <= 0)
	{
		errorReason = "no AudioSpecificConfig (extradata) supplied";
		return false;
	}

	// Same hand-parse the Apple twin uses (AacAscParser.h, hoisted out of
	// CAudioDecoderAACApple.cpp specifically so both decoders share it) --
	// the MFT's input media type needs the channel count and the (possibly
	// SBR-extended) sample rate declared explicitly, same as AudioConverter's
	// input AudioStreamBasicDescription on the Apple side.
	SAacAscInfo parsed = AacParseAudioSpecificConfig(asc, ascSize);
	if (!parsed.ok)
	{
		errorReason = "failed to parse AudioSpecificConfig (unsupported audioObjectType/channelConfiguration, or malformed extradata)";
		return false;
	}

	inputChannels = parsed.channels;
	u32 declaredRate = (parsed.sbrPresent && parsed.sbrSampleRate > 0)
							? (u32)parsed.sbrSampleRate
							: (u32)parsed.coreSampleRate;

	HRESULT hr = CoCreateInstance(CLSID_CMSAACDecMFT, nullptr, CLSCTX_INPROC_SERVER,
								   IID_PPV_ARGS(&decoderMFT));
	if (FAILED(hr) || !decoderMFT)
	{
		errorReason = "CoCreateInstance(CLSID_CMSAACDecMFT) failed";
		decoderMFT = nullptr;
		return false;
	}

	IMFMediaType *inputType = nullptr;
	hr = MFCreateMediaType(&inputType);
	if (SUCCEEDED(hr))
	{
		inputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
		inputType->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
		inputType->SetUINT32(MF_MT_AAC_PAYLOAD_TYPE, 0); // 0 == raw AAC access units (no ADTS/LOAS framing) -- DecodePacket() strips ADTS itself before ever reaching here, same as the Apple twin
		inputType->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, (UINT32)parsed.channels);
		inputType->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, (UINT32)declaredRate);

		// MF_MT_USER_DATA: the documented AAC-decoder-MFT input type carries
		// HEAACWAVEINFO's tail fields (everything after the struct's embedded
		// WAVEFORMATEX, which MF_MT_USER_DATA's blob replaces entirely --
		// there is no separate WAVEFORMATEX to set under Media Foundation's
		// IMFMediaType model) followed by the raw AudioSpecificConfig bytes,
		// all little-endian (native on Windows):
		//   wPayloadType(u16)=0                    -- raw AU, matches MF_MT_AAC_PAYLOAD_TYPE above
		//   wAudioProfileLevelIndication(u16)=0xFE  -- "unknown/unspecified" per HEAACWAVEINFO's documented convention
		//   wStructType(u16)=0
		//   wReserved1(u16)=0
		//   dwReserved2(u32)=0
		// = 12 bytes, then the ASC verbatim.
		std::vector<u8> userData;
		userData.reserve(12 + (size_t)ascSize);
		auto pushU16 = [&userData](unsigned short v) {
			userData.push_back((u8)(v & 0xFF));
			userData.push_back((u8)((v >> 8) & 0xFF));
		};
		auto pushU32 = [&userData](unsigned int v) {
			userData.push_back((u8)(v & 0xFF));
			userData.push_back((u8)((v >> 8) & 0xFF));
			userData.push_back((u8)((v >> 16) & 0xFF));
			userData.push_back((u8)((v >> 24) & 0xFF));
		};
		pushU16(0);      // wPayloadType
		pushU16(0xFE);   // wAudioProfileLevelIndication
		pushU16(0);      // wStructType
		pushU16(0);      // wReserved1
		pushU32(0);      // dwReserved2
		userData.insert(userData.end(), asc, asc + ascSize);

		hr = inputType->SetBlob(MF_MT_USER_DATA, userData.data(), (UINT32)userData.size());
		if (SUCCEEDED(hr))
			hr = decoderMFT->SetInputType(0, inputType, 0);

		inputType->Release();
	}

	if (FAILED(hr))
	{
		errorReason = "IMFTransform::SetInputType failed";
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
// SetOutputTypePCM -- (re)negotiate MFAudioFormat_PCM, 16-bit, on the MFT's
// output stream. Called from Init() and again from DecodePacket() whenever
// ProcessOutput reports MF_E_TRANSFORM_STREAM_CHANGE (HE-AAC SBR rate
// change -- mirrors the Apple twin's
// kAudioConverterCurrentOutputStreamDescription read-back after the fact,
// except here the MFT requires an explicit re-SetOutputType rather than a
// passive property read).
// ============================================================================
bool CAudioDecoderAACMF::SetOutputTypePCM()
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
// DecodePacket
// ============================================================================
bool CAudioDecoderAACMF::DecodePacket(const u8 *data, int size, std::vector<s16> &outPCM,
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

	// TS-carried AAC is ADTS-framed (each packet: 7-byte header, or 9 with
	// the optional CRC, syncword 0xFFF); mp4/mkv hand us raw access units
	// with the AudioSpecificConfig supplied out-of-band instead (Init()'s
	// asc). MF_MT_AAC_PAYLOAD_TYPE=0 above declares raw-AU input, so any
	// ADTS header must be stripped per-packet before handing the payload
	// over -- identical logic to the Apple twin's DecodePacket().
	const u8 *payload = data;
	int payloadSize = size;
	if (size >= 7 && data[0] == 0xFF && (data[1] & 0xF0) == 0xF0)
	{
		bool protectionAbsent = (data[1] & 0x01) != 0;
		int headerSize = protectionAbsent ? 7 : 9;
		if (size >= headerSize)
		{
			payload = data + headerSize;
			payloadSize = size - headerSize;
		}
	}

	IMFSample *inputSample = nullptr;
	IMFMediaBuffer *inputBuffer = nullptr;
	HRESULT hr = MFCreateSample(&inputSample);
	if (SUCCEEDED(hr))
		hr = MFCreateMemoryBuffer((DWORD)payloadSize, &inputBuffer);
	if (SUCCEEDED(hr))
	{
		BYTE *raw = nullptr;
		hr = inputBuffer->Lock(&raw, nullptr, nullptr);
		if (SUCCEEDED(hr))
		{
			memcpy(raw, payload, (size_t)payloadSize);
			inputBuffer->Unlock();
			inputBuffer->SetCurrentLength((DWORD)payloadSize);
			hr = inputSample->AddBuffer(inputBuffer);
		}
	}
	if (SUCCEEDED(hr))
		hr = decoderMFT->ProcessInput(0, inputSample, 0);

	if (inputBuffer) inputBuffer->Release();
	if (inputSample) inputSample->Release();

	if (FAILED(hr))
	{
		errorReason = "IMFTransform::ProcessInput failed";
		return false;
	}

	// Loop ProcessOutput until the MFT reports it has drained everything
	// this one ProcessInput() call fed it (MF_E_TRANSFORM_NEED_MORE_INPUT)
	// -- may be zero iterations (priming: encoder delay, or the MFT
	// buffering internally -- normal, not an error, same contract as the
	// Apple twin's AudioConverterFillComplexBuffer returning
	// ioOutputDataPackets==0).
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
		outputDataBuffer.pSample = outputSample; // nullptr if the MFT provides its own (mftProvidesSamples)

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
			// HE-AAC SBR rate change (or first-ever output type resolution
			// on some MFT implementations): re-fetch/re-negotiate the
			// output type, then retry ProcessOutput without consuming more
			// input -- mirrors the Apple twin re-reading
			// kAudioConverterCurrentOutputStreamDescription, except the MFT
			// requires an explicit SetOutputType call rather than a passive
			// property read.
			if (outputSample) outputSample->Release();
			if (outputDataBuffer.pEvents) outputDataBuffer.pEvents->Release();
			if (!SetOutputTypePCM())
			{
				errorReason = "IMFTransform output type renegotiation failed after MF_E_TRANSFORM_STREAM_CHANGE";
				return false;
			}
			// Output stream info (cbSize/dwFlags) may have changed with the
			// new type -- refresh before allocating the next output buffer.
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
		// outputDataBuffer.pSample (when the MFT provided its own sample, as
		// opposed to the one we allocated into outputSample above) is owned
		// by the MFT's returned MFT_OUTPUT_DATA_BUFFER per the ProcessOutput
		// contract -- release our own reference to it same as any other
		// COM out-param.
		if (outputDataBuffer.pSample) outputDataBuffer.pSample->Release();
		if (outputSample && outputSample != outputDataBuffer.pSample) outputSample->Release();
	}

	outChannels = outputChannels;
	outSampleRate = outputSampleRate;
	return true; // true regardless of whether outPCM ended up empty (priming), per class contract
}

#endif // _WIN32 && MT_ENABLE_FFMPEG
