#ifndef _CVIDEOSOURCEWEBMVPX_H_
#define _CVIDEOSOURCEWEBMVPX_H_

#pragma once

#include "IVideoSource.h"
#include <cstdio>
#include <deque>

// Forward declarations for nestegg (C library)
struct nestegg;
struct nestegg_packet;

// Forward declarations for vpx (C library)
typedef struct vpx_codec_ctx vpx_codec_ctx_t;
typedef struct vpx_image vpx_image_t;

// Forward declaration for Opus decoder (opaque type from opus.h)
struct OpusDecoder;

// Legacy WebM (nestegg demux) + VP9 (libvpx) + Opus video source.
//
// Extracted verbatim from the original monolithic CVideoPlayer so behavior is
// unchanged: still nestegg for demuxing, libvpx for VP9 (with optional alpha
// plane) video decode, and libopus for audio decode. CVideoPlayer drives this
// source from its decode thread instead of talking to nestegg/vpx/opus directly.
class CVideoSourceWebMVpx : public IVideoSource
{
public:
	CVideoSourceWebMVpx();
	virtual ~CVideoSourceWebMVpx();

	bool Open(const char *filePath) override;
	const SVideoInfo &Info() const override { return info; }
	bool ReadVideoFrame(SDecodedVideoFrame &out) override;
	bool ReadAudio(SDecodedAudio &out) override;
	bool Seek(double seconds) override;
	const std::string &GetErrorReason() const override { return errorReason; }
	void Close() override;

private:
	// A vpx_image_t decoded ahead of the caller's pull. vpx_codec_get_frame() can
	// return more than one image per packet; these stay valid (no further
	// vpx_codec_decode() call) until they've all been handed out via ReadVideoFrame.
	struct SPendingVideoImage
	{
		vpx_image_t *img = nullptr;
		double pts = 0.0;
	};

	bool InitVideoDecoder();
	bool InitAudioDecoder();

	// Decode the next chunk of the current video packet (one vpx_codec_decode call);
	// queues resulting images in pendingVideoImages. Chunks are decoded lazily, one
	// per call, because vpx_codec_decode invalidates images from the previous call --
	// all queued images must be handed out via ReadVideoFrame before decoding again.
	void DecodeNextVideoChunk();
	// Decode all chunks of an audio packet; queues resulting PCM in pendingAudioFrames.
	void DecodeAudioPacket(nestegg_packet *packet);

	void FreeCurrentVideoPacket();
	void FreeResources();

	SVideoInfo info;
	std::string errorReason;

	// --- Demuxer ---
	nestegg *demuxCtx = nullptr;
	FILE *fileHandle = nullptr;
	int videoTrack = -1;
	int audioTrack = -1;
	int audioChannelCount = 0;

	// --- Decoders ---
	vpx_codec_ctx_t *vpxDecoder = nullptr;
	OpusDecoder *opusDecoder = nullptr;

	// Color space info from vpx_image (VPX_CS_* / VPX_CR_* enums stored as int),
	// mirrored into info.colorSpace/info.fullRange as frames are decoded.
	int vpxColorSpace = 0;
	int vpxColorRange = 0;

	// Decoded ahead of the caller; drained one at a time by ReadVideoFrame/ReadAudio.
	std::deque<SPendingVideoImage> pendingVideoImages;
	std::deque<SDecodedAudio> pendingAudioFrames;

	// Video packet whose chunks are being decoded lazily (see DecodeNextVideoChunk).
	nestegg_packet *currentVideoPacket = nullptr;
	unsigned int currentVideoPacketChunk = 0;
	unsigned int currentVideoPacketNumChunks = 0;
	double currentVideoPacketPts = 0.0;
};

#endif
//_CVIDEOSOURCEWEBMVPX_H_
