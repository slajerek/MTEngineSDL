#ifndef _CVIDEODECODERWMVMF_H_
#define _CVIDEODECODERWMVMF_H_

#pragma once

// Native WMV-family decode (WMV1/WMV2/WMV3/VC-1) via Windows Media
// Foundation's in-box WMVideo Decoder MFT (wmvdecod.dll) -- the 2026-07-18
// WMV spec's Windows decode path, used in BOTH build modes (the commercial
// FFmpeg carries no WMV/VC-1 software decoders at all; full builds prefer
// this path too so it is exercised dev-side, with FFmpeg software decode as
// the N-edition fallback).
//
// Deliberately a THIN SUBCLASS of CVideoDecoderHEVCMF rather than a parallel
// implementation: that class's Topology/IMFMediaSession pipeline, abort
// plumbing, private serial work queue, and frame copy-out are all
// codec-agnostic (see its ECodec seam) -- the only WMV-specific facts are
// the compressed input subtype, the extradata convention (the ASF sequence
// header goes into MF_MT_USER_DATA as-is; no Annex-B conditioning, so
// WantsAnnexB() is false and CVideoSourceFFmpeg never builds a bitstream
// filter in front of us), and the 8-bit-only output (no P010 fallback).
// Compiles to nothing anywhere except a Windows FFmpeg-enabled build.
#if defined(_WIN32) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "CVideoDecoderHEVCMF.h"

class CVideoDecoderWMVMF : public CVideoDecoderHEVCMF
{
public:
	// avCodecId is the plain AVCodecID value (AV_CODEC_ID_WMV1/WMV2/WMV3/
	// VC1), kept as int so this header needs no FFmpeg includes -- exactly
	// the four values CVideoSourceFFmpeg's isWMVFamily gate admits. Anything
	// else falls back to WMV3 (the most common), where the MFT's own input
	// negotiation then fails Init() cleanly if the bits don't match.
	explicit CVideoDecoderWMVMF(int avCodecId);

	// ASF hands out complete frames and the MFT takes the sequence header
	// via MF_MT_USER_DATA -- no Annex-B anywhere in the WMV family.
	bool WantsAnnexB() const override { return false; }
};

#endif // _WIN32 && MT_ENABLE_FFMPEG

#endif
//_CVIDEODECODERWMVMF_H_
