#include "CVideoDecoderHEVCVT.h"
#include "CVideoTransferFunctions.h"

#if defined(__APPLE__) && (!defined(MT_ENABLE_FFMPEG) || (MT_ENABLE_FFMPEG))

#include "DBG_Log.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
}

#include <algorithm>
#include <cstring>

// ============================================================================
// Output callback trampoline
// ============================================================================
namespace
{
	void HEVCVTOutputCallback(void *decompressionOutputRefCon,
							   void * /*sourceFrameRefCon*/,
							   OSStatus status,
							   VTDecodeInfoFlags /*infoFlags*/,
							   CVImageBufferRef imageBuffer,
							   CMTime presentationTimeStamp,
							   CMTime /*presentationDuration*/)
	{
		CVideoDecoderHEVCVT *self = reinterpret_cast<CVideoDecoderHEVCVT *>(decompressionOutputRefCon);
		double pts = CMTIME_IS_NUMERIC(presentationTimeStamp) ? CMTimeGetSeconds(presentationTimeStamp) : 0.0;
		self->OnFrameDecoded(imageBuffer, pts, status);
	}
}

// ============================================================================
// Construction / teardown
// ============================================================================
CVideoDecoderHEVCVT::CVideoDecoderHEVCVT()
{
}

CVideoDecoderHEVCVT::~CVideoDecoderHEVCVT()
{
	TeardownSession();
}

void CVideoDecoderHEVCVT::TeardownSession()
{
	if (session)
	{
		VTDecompressionSessionInvalidate(session);
		CFRelease(session);
		session = nullptr;
	}
	if (formatDesc)
	{
		CFRelease(formatDesc);
		formatDesc = nullptr;
	}
	reorderQueue.clear();
}

// ============================================================================
// Init -- hvcC extradata -> CMVideoFormatDescription -> VTDecompressionSession
// ============================================================================
bool CVideoDecoderHEVCVT::Init(const u8 *extradata, int extradataSize, int w, int h, int colorTrc)
{
	TeardownSession();
	errorReason.clear();
	width = w;
	height = h;
	use10Bit = false;
	sessionRecreateAttempted = false;
	this->colorTrc = colorTrc;

	if (!extradata || extradataSize <= 0)
	{
		// Annex-B/TS-carried HEVC (no hvcC configuration record) is out of
		// scope for v1 -- refuse cleanly rather than guess a format.
		errorReason = "HEVC stream has no hvcC extradata (raw Annex-B HEVC not supported)";
		return false;
	}

	CFDataRef hvcCData = CFDataCreate(kCFAllocatorDefault, extradata, extradataSize);
	if (!hvcCData)
	{
		errorReason = "CFDataCreate failed for hvcC extradata";
		return false;
	}

	CFStringRef hvccKey = CFSTR("hvcC");
	CFDictionaryRef atoms = CFDictionaryCreate(kCFAllocatorDefault,
												(const void **)&hvccKey, (const void **)&hvcCData, 1,
												&kCFCopyStringDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(hvcCData);

	CFStringRef extKey = kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms;
	CFDictionaryRef extensions = CFDictionaryCreate(kCFAllocatorDefault,
													 (const void **)&extKey, (const void **)&atoms, 1,
													 &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(atoms);

	OSStatus status = CMVideoFormatDescriptionCreate(kCFAllocatorDefault, kCMVideoCodecType_HEVC,
													  width, height, extensions, &formatDesc);
	CFRelease(extensions);

	if (status != noErr || !formatDesc)
	{
		errorReason = "CMVideoFormatDescriptionCreate failed for HEVC hvcC extradata";
		formatDesc = nullptr;
		return false;
	}

	// OUTPUT BIT DEPTH IS CHOSEN FROM THE TRANSFER FUNCTION (S-5 Phase 5).
	//
	// This REPLACES the original "v1 policy", which requested 8-bit NV12 for
	// every clip and let VT downconvert Main10/HLG sources, explicitly so that
	// no 10-bit render or display path would ever be needed. That reasoning is
	// void: Phase 5 builds exactly that path, and the old policy was actively
	// destroying the picture it was meant to simplify -- VideoToolbox
	// TRUNCATES to 8 bits rather than tone-mapping, so a PQ clip's range died
	// here, before any shader could see it. Even a perfect shader cannot undo
	// it: PQ allocates its code space assuming 10 bits or more, so quantising
	// it to 8 bands visibly in the shadows by construction.
	//
	// So: PQ (16) and HLG (18) ask for 10-bit biplanar FIRST. Everything else
	// -- which is almost every clip -- is unchanged, still 8-bit NV12, because
	// widening SDR too would double the decode bandwidth of the common path
	// for no picture at all.
	//
	// The fallback runs in BOTH directions, because either request can be
	// refused by an older MFT/GPU pairing and a clip that decodes at the wrong
	// depth is still enormously better than a clip that does not decode.
	const bool wants10Bit = VideoTransfer::IsHdrTrc(colorTrc);

	const uint32_t preferred = wants10Bit
		? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
		: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	const uint32_t fallback = wants10Bit
		? kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
		: kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange;

	use10Bit = wants10Bit;
	if (CreateSession(preferred))
	{
		LOGD("CVideoDecoderHEVCVT::Init: using %s output (%dx%d, trc=%d)",
			 wants10Bit ? "10-bit biplanar" : "8-bit NV12", width, height, colorTrc);
		return true;
	}

	LOGWarning("CVideoDecoderHEVCVT::Init: %s output refused by VideoToolbox (trc=%d), falling back to %s",
			   wants10Bit ? "10-bit biplanar" : "8-bit NV12", colorTrc,
			   wants10Bit ? "8-bit NV12" : "10-bit biplanar");
	use10Bit = !wants10Bit;
	if (CreateSession(fallback))
	{
		LOGD("CVideoDecoderHEVCVT::Init: using %s output (%dx%d, fallback)",
			 use10Bit ? "10-bit biplanar" : "8-bit NV12", width, height);
		return true;
	}

	errorReason = "VTDecompressionSessionCreate failed for both 8-bit and 10-bit output formats";
	CFRelease(formatDesc);
	formatDesc = nullptr;
	return false;
}

bool CVideoDecoderHEVCVT::CreateSession(uint32_t pixelFormat)
{
	const void *specKeys[] = { kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder };
	const void *specValues[] = { kCFBooleanTrue };
	CFDictionaryRef decoderSpec = CFDictionaryCreate(kCFAllocatorDefault, specKeys, specValues, 1,
													  &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	SInt32 fmtValue = (SInt32)pixelFormat;
	CFNumberRef fmtNum = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &fmtValue);
	const void *attrKeys[] = { kCVPixelBufferPixelFormatTypeKey };
	const void *attrValues[] = { fmtNum };
	CFDictionaryRef destAttrs = CFDictionaryCreate(kCFAllocatorDefault, attrKeys, attrValues, 1,
													&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

	VTDecompressionOutputCallbackRecord cb;
	cb.decompressionOutputCallback = HEVCVTOutputCallback;
	cb.decompressionOutputRefCon = this;

	VTDecompressionSessionRef newSession = nullptr;
	OSStatus status = VTDecompressionSessionCreate(kCFAllocatorDefault, formatDesc,
													decoderSpec, destAttrs, &cb, &newSession);

	CFRelease(decoderSpec);
	CFRelease(fmtNum);
	CFRelease(destAttrs);

	if (status != noErr || !newSession)
		return false;

	if (session)
	{
		VTDecompressionSessionInvalidate(session);
		CFRelease(session);
	}
	session = newSession;
	return true;
}

// ============================================================================
// DecodePacket
// ============================================================================
bool CVideoDecoderHEVCVT::DecodePacket(const AVPacket *pkt, SDecodedVideoFrame &out)
{
	if (!pkt)
	{
		// EOS drain signal (mirrors avcodec_send_packet(ctx, nullptr)): no
		// more packets are coming, so just hand back whatever's left in the
		// reorder queue, oldest pts first, one per call.
		if (reorderQueue.empty())
			return false;
		outFrame = std::move(reorderQueue.front());
		reorderQueue.pop_front();
		return EmitOutFrame(out);
	}

	// Each real packet gets a fresh errorReason: GetErrorReason() must reflect
	// only this call's outcome (IVideoPacketDecoder's contract: false+non-empty
	// == genuine failure). A stale reason left over from an earlier call must
	// not "leak" into a later, successful one and falsely trip the caller's
	// error propagation (CVideoSourceFFmpeg::ReadVideoFrame / CVideoPlayer's
	// decode thread).
	errorReason.clear();

	if (!session)
	{
		errorReason = "decoder session not initialized";
		return false;
	}

	CMBlockBufferRef blockBuffer = nullptr;
	// kCFAllocatorNull as the block allocator: the memory block stays owned
	// by the caller's AVPacket (unreffed only after this call returns), we
	// never want CMBlockBuffer to try to free or copy it.
	OSStatus status = CMBlockBufferCreateWithMemoryBlock(kCFAllocatorDefault, pkt->data, (size_t)pkt->size,
														  kCFAllocatorNull, nullptr, 0, (size_t)pkt->size, 0, &blockBuffer);
	if (status != noErr || !blockBuffer)
	{
		errorReason = "CMBlockBufferCreateWithMemoryBlock failed";
		return false;
	}

	double ptsSeconds = (pkt->pts != AV_NOPTS_VALUE) ? (double)pkt->pts / (double)AV_TIME_BASE : 0.0;
	CMSampleTimingInfo timing;
	timing.duration = kCMTimeInvalid;
	timing.presentationTimeStamp = CMTimeMakeWithSeconds(ptsSeconds, 1000000);
	timing.decodeTimeStamp = kCMTimeInvalid;

	size_t sampleSize = (size_t)pkt->size;
	CMSampleBufferRef sampleBuffer = nullptr;
	status = CMSampleBufferCreateReady(kCFAllocatorDefault, blockBuffer, formatDesc,
										1, 1, &timing, 1, &sampleSize, &sampleBuffer);
	CFRelease(blockBuffer);
	if (status != noErr || !sampleBuffer)
	{
		errorReason = "CMSampleBufferCreateReady failed";
		return false;
	}

	status = VTDecompressionSessionDecodeFrame(session, sampleBuffer, 0, nullptr, nullptr);

	if (status == kVTInvalidSessionErr && !sessionRecreateAttempted)
	{
		// Observed after system sleep/wake: the session VT handed us is no
		// longer valid. Tear down + recreate once per Init() lifetime and
		// retry this same sample buffer; a second failure (this attempt or
		// any later kVTInvalidSessionErr this Init() lifetime) falls straight
		// through to the errorReason/failure path below instead of looping.
		sessionRecreateAttempted = true;
		LOGWarning("CVideoDecoderHEVCVT::DecodePacket: kVTInvalidSessionErr -- recreating VT session once and retrying");
		uint32_t pixelFormat = use10Bit ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange
										 : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
		if (CreateSession(pixelFormat))
			status = VTDecompressionSessionDecodeFrame(session, sampleBuffer, 0, nullptr, nullptr);
	}

	CFRelease(sampleBuffer);
	if (status != noErr)
	{
		// Genuine decode failure: surfaced to the caller (CVideoSourceFFmpeg::
		// ReadVideoFrame) via the non-empty errorReason + false return, which
		// now propagates as a real error instead of being silently skipped
		// toward a false "clean EOS" (Plan-2 Task 2).
		errorReason = "VTDecompressionSessionDecodeFrame failed";
	}

	// Brief-mandated simplicity: force synchronous behavior so every decode
	// call's output (if any) is already in reorderQueue by the time we get
	// here, rather than dealing with a truly async callback + separate
	// polling API.
	VTDecompressionSessionWaitForAsynchronousFrames(session);

	if (reorderQueue.size() > kReorderWindow)
	{
		outFrame = std::move(reorderQueue.front());
		reorderQueue.pop_front();
		return EmitOutFrame(out);
	}
	return false;
}

// ============================================================================
// Flush -- called on Seek(); discards buffered (now-stale) reordered frames.
// ============================================================================
void CVideoDecoderHEVCVT::Flush()
{
	if (session)
		VTDecompressionSessionWaitForAsynchronousFrames(session);
	reorderQueue.clear();
}

// ============================================================================
// OnFrameDecoded -- VT output callback; copy planes out, insert pts-sorted.
// ============================================================================
void CVideoDecoderHEVCVT::OnFrameDecoded(CVPixelBufferRef pixelBuffer, double pts, OSStatus status)
{
	if (status != noErr || !pixelBuffer)
	{
		LOGWarning("CVideoDecoderHEVCVT::OnFrameDecoded: dropped frame (status=%d, pixelBuffer=%p)",
				   (int)status, (void *)pixelBuffer);
		return;
	}

	CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	SPendingFrame pf;
	pf.pts = pts;
	pf.is10Bit = use10Bit;

	int yWidth = (int)CVPixelBufferGetWidthOfPlane(pixelBuffer, 0);
	int yHeight = (int)CVPixelBufferGetHeightOfPlane(pixelBuffer, 0);
	int ySrcStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0);
	const u8 *ySrc = (const u8 *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0);

	int cWidth = (int)CVPixelBufferGetWidthOfPlane(pixelBuffer, 1);   // chroma sample pairs per row
	int cHeight = (int)CVPixelBufferGetHeightOfPlane(pixelBuffer, 1);
	int cSrcStride = (int)CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1);
	const u8 *cSrc = (const u8 *)CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1);

	pf.width = yWidth;
	pf.height = yHeight;

	if (!use10Bit)
	{
		// kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange: plane0 = 8-bit Y,
		// plane1 = interleaved 8-bit Cb/Cr pairs -- exactly NV12.
		pf.strideY = yWidth;
		pf.y.resize((size_t)pf.strideY * yHeight);
		for (int r = 0; r < yHeight; r++)
			memcpy(pf.y.data() + (size_t)r * pf.strideY, ySrc + (size_t)r * ySrcStride, (size_t)pf.strideY);

		pf.strideUV = cWidth * 2;
		pf.uv.resize((size_t)pf.strideUV * cHeight);
		for (int r = 0; r < cHeight; r++)
			memcpy(pf.uv.data() + (size_t)r * pf.strideUV, cSrc + (size_t)r * cSrcStride, (size_t)pf.strideUV);
	}
	else
	{
		// kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange: plane0 = 16-bit
		// LE Y samples with the 10-bit value MSB-justified (value << 6),
		// plane1 = interleaved 16-bit LE Cb/Cr pairs, same justification
		// (this is VideoToolbox/P010's convention). CVideoSourceFFmpeg's
		// existing YUV420P10 path (AV_PIX_FMT_YUV420P10LE) expects
		// LSB-justified samples (value in the low 10 bits) instead, so every
		// sample is shifted right by 6 while copying, and Cb/Cr are
		// de-interleaved into separate planar buffers to match YUV420P10's
		// 3-plane shape.
		pf.strideY10 = yWidth * 2;
		pf.y.resize((size_t)pf.strideY10 * yHeight);
		for (int r = 0; r < yHeight; r++)
		{
			const u16 *srcRow = reinterpret_cast<const u16 *>(ySrc + (size_t)r * ySrcStride);
			u16 *dstRow = reinterpret_cast<u16 *>(pf.y.data() + (size_t)r * pf.strideY10);
			for (int c = 0; c < yWidth; c++)
				dstRow[c] = (u16)(srcRow[c] >> 6);
		}

		pf.strideC10 = cWidth * 2;
		pf.u10.resize((size_t)pf.strideC10 * cHeight);
		pf.v10.resize((size_t)pf.strideC10 * cHeight);
		for (int r = 0; r < cHeight; r++)
		{
			const u16 *srcRow = reinterpret_cast<const u16 *>(cSrc + (size_t)r * cSrcStride);
			u16 *dstU = reinterpret_cast<u16 *>(pf.u10.data() + (size_t)r * pf.strideC10);
			u16 *dstV = reinterpret_cast<u16 *>(pf.v10.data() + (size_t)r * pf.strideC10);
			for (int c = 0; c < cWidth; c++)
			{
				dstU[c] = (u16)(srcRow[c * 2 + 0] >> 6);
				dstV[c] = (u16)(srcRow[c * 2 + 1] >> 6);
			}
		}
	}

	CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);

	InsertSorted(reorderQueue, std::move(pf));
}

bool CVideoDecoderHEVCVT::InsertSorted(std::deque<SPendingFrame> &q, SPendingFrame &&f)
{
	auto it = std::lower_bound(q.begin(), q.end(), f.pts,
								[](const SPendingFrame &a, double pts) { return a.pts < pts; });
	q.insert(it, std::move(f));
	return true;
}

bool CVideoDecoderHEVCVT::EmitOutFrame(SDecodedVideoFrame &out)
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

// ============================================================================
// IVideoPacketDecoder::IsHEVCDecodeAvailable (Apple definition)
// ============================================================================
bool IVideoPacketDecoder::IsHEVCDecodeAvailable()
{
	// VideoToolbox has provided HEVC decode on every Mac since 10.13
	// (hardware-accelerated where the SoC supports it, software-decoded --
	// and still Apple-licensed, not bundled-FFmpeg -- otherwise).
	return true;
}

#endif // __APPLE__ && MT_ENABLE_FFMPEG
