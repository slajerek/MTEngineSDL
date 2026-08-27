#ifndef _CVIDEODECODERHEVCVT_H_
#define _CVIDEODECODERHEVCVT_H_

#pragma once

// Native HEVC decode via Apple VideoToolbox. This is the zero-bundled-HEVC-
// decoder path: the bundled FFmpeg carries no HEVC decoder (licensing), so
// HEVC packets demuxed by CVideoSourceFFmpeg are handed to this class, which
// decodes them with the OS-provided (and OS-licensed) VideoToolbox decoder.
// Compiles to nothing anywhere this isn't Apple + the bundled-FFmpeg build.
#if defined(__APPLE__) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "IVideoPacketDecoder.h"
#include <CoreVideo/CoreVideo.h>
#include <CoreMedia/CoreMedia.h>
#include <VideoToolbox/VideoToolbox.h>
#include <deque>
#include <vector>

class CVideoDecoderHEVCVT : public IVideoPacketDecoder
{
public:
	CVideoDecoderHEVCVT();
	~CVideoDecoderHEVCVT() override;

	bool Init(const u8 *extradata, int extradataSize, int width, int height,
			  int colorTrc = 2 /* AVCOL_TRC_UNSPECIFIED */) override;
	bool DecodePacket(const AVPacket *pkt, SDecodedVideoFrame &out) override;
	void Flush() override;
	const std::string &GetErrorReason() const override { return errorReason; }

	// Called from the VTDecompressionSession output callback trampoline.
	// Public only because a free C function needs to reach it via refcon.
	void OnFrameDecoded(CVPixelBufferRef pixelBuffer, double pts, OSStatus status);

private:
	// Owned copy of one decoded frame's planes -- VT's CVPixelBuffer is only
	// valid for the duration of the output callback, so every plane is
	// copied out into buffers we own before the callback returns. Kept
	// around (as `outFrame`, below) until the next DecodePacket()/Flush()
	// call so the SDecodedVideoFrame pointers handed to the caller stay
	// valid that long, per IVideoPacketDecoder's contract.
	struct SPendingFrame
	{
		double pts = 0.0;
		int width = 0, height = 0;
		bool is10Bit = false;

		// 8-bit NV12 path (kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange).
		std::vector<u8> y;
		std::vector<u8> uv;
		int strideY = 0, strideUV = 0;

		// 10-bit fallback path (kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange),
		// de-interleaved into planar U/V and shifted from VT's MSB-justified
		// 16-bit samples to the LSB-justified convention CVideoSourceFFmpeg's
		// AV_PIX_FMT_YUV420P10LE path already uses (see EmitFrame()).
		std::vector<u8> u10, v10;
		int strideY10 = 0, strideC10 = 0;
	};

	bool CreateSession(uint32_t pixelFormat);
	void TeardownSession();
	bool EmitOutFrame(SDecodedVideoFrame &out);

	VTDecompressionSessionRef session = nullptr;
	CMVideoFormatDescriptionRef formatDesc = nullptr;
	std::string errorReason;
	int width = 0, height = 0;
	bool use10Bit = false;

	// The clip's AVCOL_TRC, taken in Init(). Drives the OUTPUT pixel format:
	// PQ (16) and HLG (18) decode at 10 bits, everything else at 8, because
	// VideoToolbox truncates rather than tone-maps and PQ in 8 bits bands in
	// the shadows by construction (S-5 Phase 5).
	int colorTrc = 2;   // AVCOL_TRC_UNSPECIFIED

	// Plan-2 Task 2: kVTInvalidSessionErr recovery. VT sessions can be
	// invalidated out from under us (observed after system sleep/wake); one
	// recreate-and-retry attempt per Init() lifetime gives that case a chance
	// to self-heal without masking a second, genuine failure. Reset in Init().
	bool sessionRecreateAttempted = false;

	// Small pts-ordered reorder window: VTDecompressionSessionDecodeFrame()
	// delivers frames in decode order (identical to the bitstream's DTS
	// order), not display order -- B-frames must be re-sorted by pts before
	// handing them back through IVideoPacketDecoder. Sized generously above
	// typical consumer HEVC encodes' max reorder depth (verified against the
	// project's own hevc_aac.mp4 fixture, encoded with x265 defaults: 4
	// consecutive B-frames between P-frames, i.e. reorder depth 4).
	static constexpr size_t kReorderWindow = 8;
	std::deque<SPendingFrame> reorderQueue;
	SPendingFrame outFrame;

	static bool InsertSorted(std::deque<SPendingFrame> &q, SPendingFrame &&f);
};

#endif // __APPLE__ && MT_ENABLE_FFMPEG

#endif
//_CVIDEODECODERHEVCVT_H_
