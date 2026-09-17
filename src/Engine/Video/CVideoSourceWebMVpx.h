#ifndef _CVIDEOSOURCEWEBMVPX_H_
#define _CVIDEOSOURCEWEBMVPX_H_

#pragma once

#include "IVideoSource.h"
#include "MT_MediaPacket.h"
#include "CAudioDecoderAACNative.h"
#include <cstdio>
#include <deque>
#include <functional>
#include <memory>
#include <vector>

class IVideoPacketDecoder;

// Forward declarations for nestegg (C library)
struct nestegg;
struct nestegg_packet;

// Forward declarations for vpx (C library)
typedef struct vpx_codec_ctx vpx_codec_ctx_t;
typedef struct vpx_image vpx_image_t;

// Forward declaration for Opus decoder (opaque type from opus.h)
struct OpusDecoder;

// THE MATROSKA LANE. nestegg demuxes; what decodes depends on the track.
//
// The name is historical -- WebM is a Matroska profile, and this class began
// as WebM/VP9/Opus only. It now carries two decode paths over ONE demuxer,
// because the demuxer was never the limitation:
//
//   VP9 video  -> libvpx (BSD-3)       | Opus audio -> libopus (BSD-3)
//   H.264/HEVC -> IVideoPacketDecoder  | AAC audio  -> CAudioDecoderAACNative
//
// WHY THE SECOND PATH EXISTS. On macOS a store build carries no FFmpeg, and
// AVFoundation does not read Matroska at all -- so before this, .mkv simply
// did not play, which the maintainer ruled unacceptable (tracker 3.7). The
// premise behind proposing to drop it was wrong in an instructive way:
// CONTAINERS ARE NOT THE LICENSING PROBLEM, CODECS ARE. Matroska is a file
// format; parsing it needs no patent licence, and nestegg (ISC) was already
// vendored and already parsed V_MPEG4/ISO/AVC, V_MPEGH/ISO/HEVC and A_AAC.
// The lane was VP9+Opus by OUR choice, not the library's. So we demux under a
// permissive licence and hand the packets to decoders the OS licenses --
// which is the same argument that makes the whole store build possible.
//
// Extracted verbatim from the original monolithic CVideoPlayer, and the VP9
// path is still that code unchanged: CVideoPlayer drives this source from its
// decode thread instead of talking to nestegg/vpx/opus directly.
class CVideoSourceWebMVpx : public IVideoSource
{
public:
	CVideoSourceWebMVpx();
	virtual ~CVideoSourceWebMVpx();

	// Cheap container probe for the routing chain (CVideoPlayer::
	// CreateAndOpenSource), matching CVideoSourceAVFoundation::CanProbablyOpen.
	// Extension-based on purpose: opening the file to find out is what made the
	// old FFmpeg probe stall for an entire download on an iCloud-evicted file,
	// and the folder scanner routes per file while scanning.
	static bool CanProbablyOpen(const char *filePath);

	bool Open(const char *filePath) override;
	const SVideoInfo &Info() const override { return info; }
	bool ReadVideoFrame(SDecodedVideoFrame &out) override;
	bool ReadAudio(SDecodedAudio &out) override;
	bool Seek(double seconds) override;

	// FORWARDED TO THE PACKET DECODER, and on Windows that is not optional:
	// CVideoDecoderHEVCMF blocks on two condvar waits of up to 10 SECONDS
	// inside a single DecodePacket(). Without this, a shutdown or a superseded
	// seek would sit through them -- exactly the freeze the abort plumbing
	// exists to remove. The libvpx path needs neither (its decode is bounded),
	// which is why this class had no override until the packet path arrived.
	void SetAbortPredicate(std::function<bool()> pred) override;
	void WakeAbort() override;
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
	// Vorbis-in-WebM, decoded by the vendored Tremor. Its three setup headers
	// arrive as nestegg codec_data items rather than as Ogg pages, which is the
	// only thing that differs from CSlrMusicFileOgg's use of the same library.
	bool InitVorbisAudioDecoder(unsigned int track);
	void DecodeAudioPacketVorbis(nestegg_packet *packet);

	// The packet-decoder path (H.264/HEVC video, AAC audio). Separate
	// functions rather than branches inside the vpx ones: the two decode
	// models have genuinely different frame lifetimes -- libvpx hands back
	// images that stay valid until the next vpx_codec_decode, an
	// IVideoPacketDecoder hands back one frame valid until its next call --
	// and interleaving them in one function is how that gets confused.
	bool InitPacketVideoDecoder(const u8 *codecPrivate, size_t codecPrivateLen);
	bool InitAacAudioDecoder(const u8 *asc, size_t ascLen);
	bool ReadVideoFrameVpx(SDecodedVideoFrame &out);
	bool ReadVideoFramePacket(SDecodedVideoFrame &out);
	void DecodeAudioPacketAAC(nestegg_packet *packet);

	// Pull the next demuxed packet, routing audio to whichever audio decoder
	// this file needs and handing video back to the caller. Shared by both
	// video paths, because demuxing is the half that does NOT differ.
	// Returns NULL at EOF or on error (errorReason distinguishes them).
	nestegg_packet *ReadNextVideoPacket();

	// Take ownership of a demuxed video packet and read its chunk count,
	// timestamp and keyframe flag. Both video paths decode chunks lazily.
	void AdoptVideoPacket(nestegg_packet *packet);

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
	// Exactly one video decoder and at most one audio decoder are live at a
	// time; which pair depends on the track codecs found at Open().
	vpx_codec_ctx_t *vpxDecoder = nullptr;
	OpusDecoder *opusDecoder = nullptr;
	// VP8 or VP9; both go through libvpx and differ only in the decoder
	// interface handed to vpx_codec_dec_init.
	bool vpxIsVP8 = false;
	// Opaque so Tremor's headers stay out of every consumer's include graph.
	struct SVorbisState;
	SVorbisState *vorbisState = nullptr;

	EMediaCodecId videoCodec = EMediaCodecId::Unknown;   // Unknown == the VP9/libvpx path
	std::unique_ptr<IVideoPacketDecoder> packetDecoder;
#ifdef MT_HAVE_NATIVE_AAC
	std::unique_ptr<CAudioDecoderAACNative> aacDecoder;
#endif
	// nestegg owns the CodecPrivate bytes only while the context lives, and
	// Init() wants them after; copied once at Open() rather than trusted.
	std::vector<u8> videoCodecPrivate;
	// The AAC AudioSpecificConfig, kept for the same reason: a seek re-inits
	// the decoder, and nestegg's copy is not guaranteed to outlive the context.
	std::vector<u8> audioCodecPrivate;

	// Kept so a decoder built later still gets it; see InitPacketVideoDecoder.
	std::function<bool()> abortPredicate;

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
	bool currentVideoPacketKeyframe = false;

	// Annex-B scratch, reused across packets so the decode thread does not
	// allocate per frame. Only the packet-decoder path that WantsAnnexB()
	// touches it (Media Foundation); VideoToolbox takes the container's
	// length-prefixed layout directly.
	std::vector<u8> annexBParameterSets;
	std::vector<u8> annexBScratch;
	int nalLengthSize = 4;
	bool wantsAnnexB = false;
};

#endif
//_CVIDEOSOURCEWEBMVPX_H_
