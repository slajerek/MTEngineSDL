// Native WMV-family decode via the in-box WMVideo Decoder MFT -- see the
// header for the full rationale. Everything pipeline-shaped lives in the
// base class (CVideoDecoderHEVCMF's ECodec seam); this file only maps codec
// ids and implements the availability probe.

#include "CVideoDecoderWMVMF.h"

#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "MFComThreadGuard.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

using MFComThreadGuard::EnsureComInitializedForThisThread;
using MFComThreadGuard::EnsureMFStarted;

namespace
{
	CVideoDecoderHEVCMF::ECodec CodecFromAVCodecID(int avCodecId)
	{
		switch (avCodecId)
		{
			case AV_CODEC_ID_WMV1: return CVideoDecoderHEVCMF::ECodec::WMV1;
			case AV_CODEC_ID_WMV2: return CVideoDecoderHEVCMF::ECodec::WMV2;
			case AV_CODEC_ID_WMV3: return CVideoDecoderHEVCMF::ECodec::WMV3;
			case AV_CODEC_ID_VC1:  return CVideoDecoderHEVCMF::ECodec::WVC1;
			default:               return CVideoDecoderHEVCMF::ECodec::WMV3;
		}
	}
}

CVideoDecoderWMVMF::CVideoDecoderWMVMF(int avCodecId)
	: CVideoDecoderHEVCMF(CodecFromAVCodecID(avCodecId))
{
}

// ============================================================================
// IsWMVDecodeAvailable -- existence probe, same two-tier enumeration shape as
// IsHEVCDecodeAvailable (CVideoDecoderHEVCMF.cpp). WMV3 is the family's
// canonical subtype: wmvdecod.dll registers one decoder MFT for all four
// WMV-family input subtypes, so probing any one of them answers for all.
// Effectively always true on non-N Windows; false on an N edition without
// the Media Feature Pack -- CVideoSourceFFmpeg then falls through to FFmpeg
// software decode (full builds) or the edition refusal (commercial builds).
// ============================================================================
namespace
{
	constexpr UINT32 kWmvHardwareFlags = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;
	constexpr UINT32 kWmvSoftwareFlags = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_SORTANDFILTER;

	bool AnyWMVDecoderActivate(UINT32 flags)
	{
		MFT_REGISTER_TYPE_INFO inInfo = {};
		inInfo.guidMajorType = MFMediaType_Video;
		inInfo.guidSubtype = MFVideoFormat_WMV3;

		IMFActivate **activates = nullptr;
		UINT32 count = 0;
		HRESULT hr = MFTEnumEx(MFT_CATEGORY_VIDEO_DECODER, flags, &inInfo, nullptr, &activates, &count);

		bool found = SUCCEEDED(hr) && count > 0;
		if (activates)
		{
			for (UINT32 i = 0; i < count; i++)
				if (activates[i]) activates[i]->Release();
			CoTaskMemFree(activates);
		}
		return found;
	}
}

bool IVideoPacketDecoder::IsWMVDecodeAvailable()
{
	EnsureComInitializedForThisThread();
	EnsureMFStarted();

	return AnyWMVDecoderActivate(kWmvHardwareFlags) || AnyWMVDecoderActivate(kWmvSoftwareFlags);
}

#endif // _WIN32 && MT_ENABLE_FFMPEG
