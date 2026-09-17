#include "CVideoSourceWebMVpx.h"
// The whole TU is capability-gated (unification plan Phase 2, 2026-08-31):
// at MT_ENABLE_WEBM_VPX=0 the video-codecs archive is a stub with no vpx/opus
// symbols, and the nestegg/vpx include dirs may not exist at all.
#if MT_ENABLE_WEBM_VPX
#include "DBG_Log.h"

#include <cstring>

// nestegg (WebM demuxer)
extern "C" {
#include <nestegg/nestegg.h>
}

// libvpx (VP9 decoder)
#include <vpx/vpx_codec.h>
#include <vpx/vpx_decoder.h>
#include <vpx/vpx_image.h>
#include <vpx/vp8dx.h>

// Opus audio decoder
#include <opus/opus.h>

// Vorbis audio decoder -- the SAME vendored Tremor that CSlrMusicFileOgg uses
// for .ogg files, driven through its low-level synthesis API because WebM
// delivers the three setup headers as codec_data rather than as Ogg pages.
extern "C" {
#include "tremor-ivorbiscodec.h"
}

// The packet-decoder path. Both headers compile to nothing off their platform,
// and the members they back are guarded the same way.
#include "IVideoPacketDecoder.h"
#include "MT_AnnexB.h"
#if defined(__APPLE__)
#include "CVideoDecoderHEVCVT.h"
#include "CAudioDecoderAACApple.h"
#elif defined(_WIN32)
#include "CVideoDecoderHEVCMF.h"
#include "CAudioDecoderAACMF.h"
#endif

// ============================================================================
// Platform-compatible 64-bit file I/O
// ============================================================================
#ifdef _WIN32
  #define ne_fseek(fp, offset, whence) _fseeki64(fp, offset, whence)
  #define ne_ftell(fp) _ftelli64(fp)
#else
  #define ne_fseek(fp, offset, whence) fseeko(fp, offset, whence)
  #define ne_ftell(fp) ftello(fp)
#endif

// ============================================================================
// nestegg I/O callbacks
// ============================================================================
static int nestegg_io_read(void *buffer, size_t length, void *userdata)
{
	FILE *fp = static_cast<FILE *>(userdata);
	size_t bytesRead = fread(buffer, 1, length, fp);
	if (bytesRead == length)
		return 1;
	if (feof(fp))
		return 0;
	return -1;
}

static int nestegg_io_seek(int64_t offset, int whence, void *userdata)
{
	FILE *fp = static_cast<FILE *>(userdata);
	int result = ne_fseek(fp, offset, whence);
	return (result == 0) ? 0 : -1;
}

static int64_t nestegg_io_tell(void *userdata)
{
	FILE *fp = static_cast<FILE *>(userdata);
	return static_cast<int64_t>(ne_ftell(fp));
}

// Clamp float to [-1.0, 1.0] range and convert to S16 (mirrors CVideoAudioChannel's
// own FloatToS16 helper -- kept local here since SDecodedAudio hands out s16 PCM).
static inline s16 FloatToS16(float sample)
{
	float clamped = sample;
	if (clamped > 1.0f) clamped = 1.0f;
	if (clamped < -1.0f) clamped = -1.0f;
	return (s16)(clamped * 32767.0f);
}

// ============================================================================
// Constructor / Destructor
// ============================================================================
CVideoSourceWebMVpx::CVideoSourceWebMVpx()
{
}

CVideoSourceWebMVpx::~CVideoSourceWebMVpx()
{
	Close();
}

// ============================================================================
// Open
// ============================================================================
bool CVideoSourceWebMVpx::CanProbablyOpen(const char *filePath)
{
	if (filePath == NULL || filePath[0] == '\0')
		return false;
	std::string p(filePath);
	size_t dot = p.find_last_of('.');
	if (dot == std::string::npos)
		return false;
	std::string ext = p.substr(dot);
	for (char &c : ext)
		c = (char)tolower((unsigned char)c);
	// .mkv IS claimed now (tracker 3.10). It was deliberately absent while this
	// source wired up only VP9 and Opus, because claiming it would have routed
	// a perfectly good H.264-in-Matroska file to a lane that could not decode
	// it. The lane is wider than that now -- see the class comment -- so the
	// claim is honest. Open() still refuses a file whose codecs it cannot
	// decode, and refusing is what lets CVideoPlayer's chain fall through to
	// FFmpeg in a private build.
	return ext == ".webm" || ext == ".mkv" || ext == ".mka";
}

bool CVideoSourceWebMVpx::Open(const char *filePath)
{
	LOGD("CVideoSourceWebMVpx::Open: %s", filePath);

	Close();

	// Open file
	fileHandle = fopen(filePath, "rb");
	if (!fileHandle)
	{
		LOGError("CVideoSourceWebMVpx::Open: failed to open file '%s'", filePath);
		errorReason = "failed to open file";
		return false;
	}

	// Setup nestegg I/O
	nestegg_io io;
	io.read = nestegg_io_read;
	io.seek = nestegg_io_seek;
	io.tell = nestegg_io_tell;
	io.userdata = fileHandle;

	// Initialize nestegg demuxer
	int ret = nestegg_init(&demuxCtx, io, nullptr, -1);
	if (ret != 0)
	{
		LOGError("CVideoSourceWebMVpx::Open: nestegg_init failed for '%s'", filePath);
		fclose(fileHandle);
		fileHandle = nullptr;
		errorReason = "nestegg_init failed";
		return false;
	}

	// Get duration (in nanoseconds)
	uint64_t durationNs = 0;
	if (nestegg_duration(demuxCtx, &durationNs) == 0)
	{
		info.duration = static_cast<double>(durationNs) / 1000000000.0;
	}
	else
	{
		info.duration = 0.0;
		LOGWarning("CVideoSourceWebMVpx::Open: could not determine duration");
	}

	// Enumerate tracks
	unsigned int numTracks = 0;
	nestegg_track_count(demuxCtx, &numTracks);

	LOGD("CVideoSourceWebMVpx::Open: found %u tracks, duration=%.2f sec", numTracks, info.duration);

	videoTrack = -1;
	audioTrack = -1;

	for (unsigned int i = 0; i < numTracks; i++)
	{
		int trackType = nestegg_track_type(demuxCtx, i);
		int trackCodecId = nestegg_track_codec_id(demuxCtx, i);

		if (trackType == NESTEGG_TRACK_VIDEO && videoTrack < 0)
		{
			// H.264 / HEVC in Matroska: demuxed here, decoded by the OS. The
			// CodecPrivate for these track types IS the avcC/hvcC
			// configuration record -- the exact bytes VideoToolbox and Media
			// Foundation want -- which is why this needs no bitstream surgery.
			const bool isPacketCodec = (trackCodecId == NESTEGG_CODEC_AVC ||
										trackCodecId == NESTEGG_CODEC_HEVC);
			if (isPacketCodec)
			{
				nestegg_video_params videoParams;
				memset(&videoParams, 0, sizeof(videoParams));
				nestegg_track_video_params(demuxCtx, i, &videoParams);

				unsigned char *cp = nullptr;
				size_t cpLen = 0;
				if (nestegg_track_codec_data(demuxCtx, i, 0, &cp, &cpLen) != 0 ||
					cp == nullptr || cpLen == 0)
				{
					// Annex-B-in-Matroska exists but is rare and neither OS
					// decoder takes it without a configuration record. Refuse
					// the TRACK, not silently the picture.
					LOGWarning("CVideoSourceWebMVpx::Open: video track %u has no CodecPrivate "
							   "(raw Annex-B in Matroska is not supported)", i);
				}
				else
				{
					videoTrack = static_cast<int>(i);
					videoCodec = (trackCodecId == NESTEGG_CODEC_AVC)
							   ? EMediaCodecId::H264 : EMediaCodecId::HEVC;
					videoCodecPrivate.assign(cp, cp + cpLen);

					info.width = static_cast<int>(videoParams.width);
					info.height = static_cast<int>(videoParams.height);
					uint64_t defaultDuration = 0;
					if (nestegg_track_default_duration(demuxCtx, i, &defaultDuration) == 0 &&
						defaultDuration > 0)
					{
						info.fps = 1000000000.0 / static_cast<double>(defaultDuration);
					}
					info.videoCodecName = (videoCodec == EMediaCodecId::H264) ? "h264" : "hevc";

					// The bitstream carries the real colour description and
					// neither OS decoder reports it back here, so this is the
					// same resolution heuristic the vpx path falls back to
					// when the stream says UNKNOWN -- least-wrong, and stated
					// rather than pretended.
					info.colorSpace = (info.height >= 720 || info.width >= 1280) ? 2 : 1;
					info.fullRange = false;

					LOGD("CVideoSourceWebMVpx::Open: video track %d: %s %dx%d, fps=%.2f, "
						 "%zu bytes of %s", videoTrack, info.videoCodecName.c_str(),
						 info.width, info.height, info.fps, cpLen,
						 (videoCodec == EMediaCodecId::H264) ? "avcC" : "hvcC");
				}
			}
			else if (trackCodecId == NESTEGG_CODEC_VP9 || trackCodecId == NESTEGG_CODEC_VP8)
			{
				// VP8 IS ACCEPTED NOW, and it is free. The lane refused it for
				// years because libvpx was configured `--disable-vp8-decoder`
				// -- a licensing decision applied to the wrong library. libvpx
				// is BSD-3 and VP8 carries Google's irrevocable royalty-free
				// grant, so it is store-safe by exactly the standard that
				// admits the VP9 beside it. (Tracker 2.11.)
				vpxIsVP8 = (trackCodecId == NESTEGG_CODEC_VP8);
				videoTrack = static_cast<int>(i);

				nestegg_video_params videoParams;
				nestegg_track_video_params(demuxCtx, i, &videoParams);
				info.width = static_cast<int>(videoParams.width);
				info.height = static_cast<int>(videoParams.height);

				// Check for alpha mode in video params
				if (videoParams.alpha_mode != 0)
				{
					info.hasAlpha = true;
				}

				// Estimate FPS from default duration
				uint64_t defaultDuration = 0;
				if (nestegg_track_default_duration(demuxCtx, i, &defaultDuration) == 0 && defaultDuration > 0)
				{
					info.fps = 1000000000.0 / static_cast<double>(defaultDuration);
				}

				info.videoCodecName = vpxIsVP8 ? "vp8" : "vp9";

				LOGD("CVideoSourceWebMVpx::Open: video track %d: %s %dx%d, fps=%.2f, alpha=%d",
					 videoTrack, info.videoCodecName.c_str(), info.width, info.height,
					 info.fps, info.hasAlpha ? 1 : 0);
			}
			else
			{
				LOGWarning("CVideoSourceWebMVpx::Open: video track %u has unsupported codec %d "
						   "(this lane decodes VP9, H.264 and HEVC)", i, trackCodecId);
			}
		}
		else if (trackType == NESTEGG_TRACK_AUDIO && audioTrack < 0)
		{
			if (trackCodecId == NESTEGG_CODEC_OPUS)
			{
				audioTrack = static_cast<int>(i);
				info.hasAudio = true;

				nestegg_audio_params audioParams;
				nestegg_track_audio_params(demuxCtx, i, &audioParams);
				audioChannelCount = static_cast<int>(audioParams.channels);
				info.audioChannels = audioChannelCount;
				info.audioSampleRate = static_cast<u32>(audioParams.rate);
				info.audioCodecName = "opus";

				LOGD("CVideoSourceWebMVpx::Open: audio track %d: %d channels, rate=%u",
					 audioTrack, audioChannelCount, info.audioSampleRate);
			}
			else if (trackCodecId == NESTEGG_CODEC_VORBIS)
			{
				// Vorbis is free and Tremor is already vendored AND already
				// linked -- CSlrMusicFileOgg plays .ogg with it. Refusing it
				// here meant a VP8+Vorbis WebM, the most permissively licensed
				// video file that exists, needed FFmpeg to play. (Tracker 2.11.)
				nestegg_audio_params audioParams;
				memset(&audioParams, 0, sizeof(audioParams));
				nestegg_track_audio_params(demuxCtx, i, &audioParams);
				audioChannelCount = static_cast<int>(audioParams.channels);

				if (InitVorbisAudioDecoder(i))
				{
					audioTrack = static_cast<int>(i);
					info.hasAudio = true;
					info.audioChannels = audioChannelCount;
					info.audioSampleRate = static_cast<u32>(audioParams.rate);
					info.audioCodecName = "vorbis";
					LOGD("CVideoSourceWebMVpx::Open: audio track %d: Vorbis, %d channels, rate=%u",
						 audioTrack, audioChannelCount, info.audioSampleRate);
				}
				else
				{
					audioChannelCount = 0;
					LOGWarning("CVideoSourceWebMVpx::Open: Vorbis track %u has unusable setup "
							   "headers; continuing without audio", i);
				}
			}
			else if (trackCodecId == NESTEGG_CODEC_AAC)
			{
				// A_AAC's CodecPrivate IS the AudioSpecificConfig, which is
				// exactly what CAudioDecoderAACNative::Init wants. This is the
				// case that has no ADTS header to hand-parse and no esds box
				// to walk -- the reason tracker 4.6's ASC synthesis was never
				// needed here.
				unsigned char *cp = nullptr;
				size_t cpLen = 0;
				nestegg_audio_params audioParams;
				memset(&audioParams, 0, sizeof(audioParams));
				nestegg_track_audio_params(demuxCtx, i, &audioParams);

				if (nestegg_track_codec_data(demuxCtx, i, 0, &cp, &cpLen) == 0 &&
					cp != nullptr && cpLen > 0 && InitAacAudioDecoder(cp, cpLen))
				{
					audioTrack = static_cast<int>(i);
					info.hasAudio = true;
					audioChannelCount = static_cast<int>(audioParams.channels);
					info.audioChannels = audioChannelCount;
					info.audioSampleRate = static_cast<u32>(audioParams.rate);
					info.audioCodecName = "aac";
					LOGD("CVideoSourceWebMVpx::Open: audio track %d: AAC, %d channels, rate=%u",
						 audioTrack, audioChannelCount, info.audioSampleRate);
				}
				else
				{
					// Silent video beats no video: the picture is the point,
					// and a machine with no OS AAC decoder (Windows N) should
					// still see it.
					LOGWarning("CVideoSourceWebMVpx::Open: AAC track %u has no usable "
							   "AudioSpecificConfig or no OS decoder; continuing without audio", i);
				}
			}
			else
			{
				LOGWarning("CVideoSourceWebMVpx::Open: audio track %u has unsupported codec %d "
						   "(this lane decodes Opus, Vorbis and AAC)", i, trackCodecId);
			}
		}
	}

	if (videoTrack < 0)
	{
		LOGError("CVideoSourceWebMVpx::Open: no decodable video track found in '%s'", filePath);
		nestegg_destroy(demuxCtx);
		demuxCtx = nullptr;
		fclose(fileHandle);
		fileHandle = nullptr;
		errorReason = "no decodable video track found";
		return false;
	}

	// Initialize decoders
	const bool usesPacketDecoder = (videoCodec != EMediaCodecId::Unknown);
	if (usesPacketDecoder)
	{
		if (!InitPacketVideoDecoder(videoCodecPrivate.data(), videoCodecPrivate.size()))
		{
			LOGError("CVideoSourceWebMVpx::Open: no OS decoder for %s", info.videoCodecName.c_str());
			errorReason = "no OS decoder for " + info.videoCodecName;
			FreeResources();
			return false;
		}
	}
	else if (!InitVideoDecoder())
	{
		LOGError("CVideoSourceWebMVpx::Open: failed to initialize VP9 decoder");
		errorReason = "failed to initialize VP9 decoder";
		FreeResources();
		return false;
	}

	// The AAC decoder was built during track enumeration (it has to be, to
	// know whether the track is usable at all); only Opus is initialised here.
	if (info.hasAudio && info.audioCodecName == "opus")
	{
		if (!InitAudioDecoder())
		{
			LOGWarning("CVideoSourceWebMVpx::Open: failed to initialize Opus decoder, continuing without audio");
			info.hasAudio = false;
			audioTrack = -1;
		}
	}

	errorReason.clear();

	LOGD("CVideoSourceWebMVpx::Open: successfully opened '%s'", filePath);
	return true;
}

// ============================================================================
// InitVideoDecoder
// ============================================================================
bool CVideoSourceWebMVpx::InitVideoDecoder()
{
	LOGD("CVideoSourceWebMVpx::InitVideoDecoder");

	vpxDecoder = new vpx_codec_ctx_t();
	memset(vpxDecoder, 0, sizeof(vpx_codec_ctx_t));

	vpx_codec_dec_cfg_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.threads = 2;
	cfg.w = static_cast<unsigned int>(info.width);
	cfg.h = static_cast<unsigned int>(info.height);

	vpx_codec_err_t err = vpx_codec_dec_init(
		vpxDecoder, vpxIsVP8 ? vpx_codec_vp8_dx() : vpx_codec_vp9_dx(), &cfg, 0);
	if (err != VPX_CODEC_OK)
	{
		LOGError("CVideoSourceWebMVpx::InitVideoDecoder: vpx_codec_dec_init failed: %s", vpx_codec_err_to_string(err));
		delete vpxDecoder;
		vpxDecoder = nullptr;
		return false;
	}

	LOGD("CVideoSourceWebMVpx::InitVideoDecoder: %s decoder initialized (%dx%d, 2 threads)",
		 vpxIsVP8 ? "VP8" : "VP9", info.width, info.height);
	return true;
}

// ============================================================================
// InitAudioDecoder
// ============================================================================
bool CVideoSourceWebMVpx::InitAudioDecoder()
{
	LOGD("CVideoSourceWebMVpx::InitAudioDecoder: channels=%d, sampleRate=%u", audioChannelCount, info.audioSampleRate);

	if (audioChannelCount < 1 || audioChannelCount > 2)
	{
		LOGError("CVideoSourceWebMVpx::InitAudioDecoder: unsupported channel count %d (only mono/stereo supported)", audioChannelCount);
		return false;
	}

	int opusErr = 0;
	opusDecoder = opus_decoder_create(48000, audioChannelCount, &opusErr);
	if (opusErr != OPUS_OK || !opusDecoder)
	{
		LOGError("CVideoSourceWebMVpx::InitAudioDecoder: opus_decoder_create failed: %s", opus_strerror(opusErr));
		opusDecoder = nullptr;
		return false;
	}

	LOGD("CVideoSourceWebMVpx::InitAudioDecoder: Opus decoder initialized");
	return true;
}

// ============================================================================
// ReadVideoFrame
// ============================================================================
bool CVideoSourceWebMVpx::ReadVideoFrame(SDecodedVideoFrame &out)
{
	// One demuxer, two decode models -- dispatched once, here, rather than
	// branched at every step below.
	return (videoCodec == EMediaCodecId::Unknown) ? ReadVideoFrameVpx(out)
												  : ReadVideoFramePacket(out);
}

// ============================================================================
// ReadNextVideoPacket -- the half that does NOT differ between the two paths
// ============================================================================
nestegg_packet *CVideoSourceWebMVpx::ReadNextVideoPacket()
{
	for (;;)
	{
		nestegg_packet *packet = nullptr;
		int ret = nestegg_read_packet(demuxCtx, &packet);
		if (ret <= 0 || !packet)
		{
			// ret == 0 means EOF, ret < 0 means error. An empty errorReason is
			// how the caller tells clean EOS from a failure.
			errorReason = (ret < 0) ? "nestegg_read_packet failed" : "";
			return nullptr;
		}

		unsigned int track = 0;
		nestegg_packet_track(packet, &track);
		if (static_cast<int>(track) == videoTrack)
			return packet;

		if (static_cast<int>(track) == audioTrack && info.hasAudio)
		{
			if (info.audioCodecName == "aac")
				DecodeAudioPacketAAC(packet);
			else if (info.audioCodecName == "vorbis")
				DecodeAudioPacketVorbis(packet);
			else
				DecodeAudioPacket(packet);
		}
		nestegg_free_packet(packet);
	}
}

bool CVideoSourceWebMVpx::ReadVideoFrameVpx(SDecodedVideoFrame &out)
{
	// Demux and decode packets until we have at least one video frame queued,
	// or we hit real end-of-stream/error. Audio packets encountered along the
	// way are decoded and queued for ReadAudio().
	while (pendingVideoImages.empty())
	{
		// First finish decoding chunks of the current video packet, if any
		if (currentVideoPacket)
		{
			if (currentVideoPacketChunk < currentVideoPacketNumChunks)
			{
				DecodeNextVideoChunk();
				continue;
			}
			FreeCurrentVideoPacket();
		}

		nestegg_packet *packet = ReadNextVideoPacket();
		if (!packet)
			return false;

		// Adopt as the current video packet; chunks are decoded lazily above
		AdoptVideoPacket(packet);
	}

	SPendingVideoImage pending = pendingVideoImages.front();
	pendingVideoImages.pop_front();

	vpx_image_t *img = pending.img;

	// Store color space/range info (only known once the bitstream is decoded).
	// The vpx value is ALREADY the engine's normalized convention (VPX_CS_*)
	// -- do NOT route it through VideoColor_NormalizeMatrix, whose input is
	// the FFmpeg convention (the two disagree on the meaning of 1 and 2).
	// CM-E: only the UNKNOWN(0) -> resolution-heuristic step applies here.
	vpxColorSpace = img->cs;
	vpxColorRange = img->range;
	// SMPTE_240 (4) folds into the 709 family, matching the FFmpeg-side fold
	// (its coefficients are 709's, not 601's); SMPTE_170 (3) and SRGB (7)
	// land on the consumers' 601 else-branch, which is right for 170 and the
	// least-wrong answer for the never-seen-in-practice sRGB-tagged YUV case.
	info.colorSpace = (vpxColorSpace == 4)
						  ? 2
						  : (vpxColorSpace != 0)
						  ? vpxColorSpace
						  : (((int)img->d_h >= 720 || (int)img->d_w >= 1280) ? 2 : 1);
	info.fullRange = (vpxColorRange == 1);

	bool frameHasAlpha = (info.hasAlpha && img->planes[VPX_PLANE_ALPHA] != nullptr);

	out.plane[0] = img->planes[VPX_PLANE_Y];
	out.plane[1] = img->planes[VPX_PLANE_U];
	out.plane[2] = img->planes[VPX_PLANE_V];
	out.plane[3] = frameHasAlpha ? img->planes[VPX_PLANE_ALPHA] : nullptr;
	out.stride[0] = img->stride[VPX_PLANE_Y];
	out.stride[1] = img->stride[VPX_PLANE_U];
	out.stride[2] = img->stride[VPX_PLANE_V];
	out.stride[3] = frameHasAlpha ? img->stride[VPX_PLANE_ALPHA] : 0;
	out.width = static_cast<int>(img->d_w);
	out.height = static_cast<int>(img->d_h);
	out.pixelFormat = frameHasAlpha ? EVideoPixelFormat::YUVA420P : EVideoPixelFormat::YUV420P;
	out.pts = pending.pts;

	return true;
}

// ============================================================================
// ReadAudio
// ============================================================================
bool CVideoSourceWebMVpx::ReadAudio(SDecodedAudio &out)
{
	if (pendingAudioFrames.empty())
		return false;

	out = std::move(pendingAudioFrames.front());
	pendingAudioFrames.pop_front();
	return true;
}

// ============================================================================
// DecodeNextVideoChunk
// ============================================================================
void CVideoSourceWebMVpx::DecodeNextVideoChunk()
{
	unsigned int i = currentVideoPacketChunk++;

	unsigned char *data = nullptr;
	size_t dataLen = 0;
	int ret = nestegg_packet_data(currentVideoPacket, i, &data, &dataLen);
	if (ret != 0 || !data || dataLen == 0)
		return;

	vpx_codec_err_t err = vpx_codec_decode(vpxDecoder, data, static_cast<unsigned int>(dataLen), nullptr, 0);
	if (err != VPX_CODEC_OK)
	{
		LOGError("CVideoSourceWebMVpx::DecodeNextVideoChunk: vpx_codec_decode failed: %s", vpx_codec_err_to_string(err));
		return;
	}

	// Retrieve decoded frames (valid until the next vpx_codec_decode call)
	vpx_codec_iter_t iter = nullptr;
	vpx_image_t *img = nullptr;
	while ((img = vpx_codec_get_frame(vpxDecoder, &iter)) != nullptr)
	{
		pendingVideoImages.push_back({img, currentVideoPacketPts});
	}
}

// ============================================================================
// FreeCurrentVideoPacket
// ============================================================================
void CVideoSourceWebMVpx::FreeCurrentVideoPacket()
{
	if (currentVideoPacket)
	{
		nestegg_free_packet(currentVideoPacket);
		currentVideoPacket = nullptr;
	}
	currentVideoPacketChunk = 0;
	currentVideoPacketNumChunks = 0;
	currentVideoPacketPts = 0.0;
}

// ============================================================================
// DecodeAudioPacket
// ============================================================================
void CVideoSourceWebMVpx::DecodeAudioPacket(nestegg_packet *packet)
{
	if (!opusDecoder)
		return;

	unsigned int numChunks = 0;
	nestegg_packet_count(packet, &numChunks);

	// Get packet timestamp (nanoseconds)
	uint64_t tstampNs = 0;
	nestegg_packet_tstamp(packet, &tstampNs);
	double pts = static_cast<double>(tstampNs) / 1000000000.0;

	// Opus decode buffer: max frame size is 120ms at 48kHz = 5760 samples per channel
	// Support up to 2 channels (mono/stereo); >2 channels rejected at init time
	static constexpr int MAX_OPUS_FRAME_SAMPLES = 5760;
	static constexpr int MAX_OPUS_CHANNELS = 2;
	float pcmBuffer[MAX_OPUS_FRAME_SAMPLES * MAX_OPUS_CHANNELS];

	for (unsigned int i = 0; i < numChunks; i++)
	{
		unsigned char *data = nullptr;
		size_t dataLen = 0;
		int ret = nestegg_packet_data(packet, i, &data, &dataLen);
		if (ret != 0 || !data || dataLen == 0)
			continue;

		int samplesDecoded = opus_decode_float(
			opusDecoder,
			data,
			static_cast<opus_int32>(dataLen),
			pcmBuffer,
			MAX_OPUS_FRAME_SAMPLES,
			0  // no FEC
		);

		if (samplesDecoded < 0)
		{
			LOGError("CVideoSourceWebMVpx::DecodeAudioPacket: opus_decode_float failed: %s", opus_strerror(samplesDecoded));
			continue;
		}

		if (samplesDecoded > 0)
		{
			SDecodedAudio audio;
			audio.channels = audioChannelCount;
			audio.sampleRate = 48000;
			audio.pts = pts;

			int totalSamples = samplesDecoded * audioChannelCount;
			audio.pcm.resize(static_cast<size_t>(totalSamples));
			for (int s = 0; s < totalSamples; s++)
			{
				audio.pcm[s] = FloatToS16(pcmBuffer[s]);
			}

			pendingAudioFrames.push_back(std::move(audio));
		}
	}
}


// ============================================================================
// AdoptVideoPacket
// ============================================================================
void CVideoSourceWebMVpx::AdoptVideoPacket(nestegg_packet *packet)
{
	currentVideoPacket = packet;
	currentVideoPacketChunk = 0;
	currentVideoPacketNumChunks = 0;
	nestegg_packet_count(packet, &currentVideoPacketNumChunks);

	uint64_t tstampNs = 0;
	nestegg_packet_tstamp(packet, &tstampNs);
	currentVideoPacketPts = static_cast<double>(tstampNs) / 1000000000.0;

	// UNKNOWN counts as "not a keyframe". The flag is advisory to every
	// decoder here -- both OS decoders find their own IDR -- so the safe
	// reading is the one that never claims a random frame is seekable.
	currentVideoPacketKeyframe =
		(nestegg_packet_has_keyframe(packet) == NESTEGG_PACKET_HAS_KEYFRAME_TRUE);
}

// ============================================================================
// InitPacketVideoDecoder -- H.264 / HEVC through the OS
// ============================================================================
bool CVideoSourceWebMVpx::InitPacketVideoDecoder(const u8 *codecPrivate, size_t codecPrivateLen)
{
	packetDecoder.reset();
	wantsAnnexB = false;
	nalLengthSize = 4;
	annexBParameterSets.clear();

#if defined(__APPLE__)
	// One VideoToolbox class for both codecs: the session, sample-buffer and
	// output plumbing are identical, only the CMVideoCodecType and the
	// configuration-record atom name differ.
	CVideoDecoderHEVCVT *vt = new CVideoDecoderHEVCVT();
	vt->SetCodec(videoCodec);
	packetDecoder.reset(vt);
#elif defined(_WIN32)
	packetDecoder.reset(new CVideoDecoderHEVCMF(
		(videoCodec == EMediaCodecId::H264) ? CVideoDecoderHEVCMF::ECodec::H264
											: CVideoDecoderHEVCMF::ECodec::HEVC));
#else
	// Linux has no OS video decoder to route to. Returning false here is what
	// makes CVideoPlayer's chain fall through to FFmpeg in a private build,
	// and what makes a store build say so instead of showing a black frame.
	(void)codecPrivate; (void)codecPrivateLen;
	LOGWarning("CVideoSourceWebMVpx: no OS video decoder on this platform for Matroska H.264/HEVC");
	return false;
#endif

	if (!packetDecoder)
		return false;

	// The predicate may have been handed to this source BEFORE the file was
	// opened -- CVideoPlayer does exactly that -- so it is installed here as
	// well as in SetAbortPredicate(). Whichever happens second wins, and both
	// orders end with the decoder holding it.
	if (abortPredicate)
		packetDecoder->SetAbortPredicate(abortPredicate);

	const bool isHevc = (videoCodec == EMediaCodecId::HEVC);
	const u8 *initData = codecPrivate;
	int initSize = (int)codecPrivateLen;

	// ANNEX-B DECODERS GET A CONVERTED STREAM. Media Foundation wants
	// start-code-prefixed parameter sets and packets; Matroska stores
	// length-prefixed NALs and an avcC/hvcC record. CVideoSourceFFmpeg solved
	// this with libavcodec's mp4toannexb bitstream filter, which a store build
	// does not have -- MT_AnnexB is that conversion, written to be testable
	// without the platform that needs it.
	wantsAnnexB = packetDecoder->WantsAnnexB();
	if (wantsAnnexB)
	{
		if (!MT_AnnexB::ParseConfigRecord(codecPrivate, codecPrivateLen, isHevc,
										  annexBParameterSets, nalLengthSize))
		{
			LOGError("CVideoSourceWebMVpx: could not parse the %s configuration record",
					 isHevc ? "hvcC" : "avcC");
			packetDecoder.reset();
			return false;
		}
		initData = annexBParameterSets.data();
		initSize = (int)annexBParameterSets.size();
	}
	else
	{
		// Even the non-Annex-B path needs the prefix width, because a Seek()
		// or a later WantsAnnexB() decoder would otherwise guess it.
		std::vector<u8> ignored;
		MT_AnnexB::ParseConfigRecord(codecPrivate, codecPrivateLen, isHevc, ignored, nalLengthSize);
	}

	// colorTrc: Matroska CAN carry a Colour element, but nestegg does not
	// expose it, so this is honestly unspecified rather than falsely SDR-
	// tagged. The decoder's own 8/10-bit fallback covers the HDR case badly
	// but not destructively; see the tracker note on 3.10.
	if (!packetDecoder->Init(initData, initSize, info.width, info.height, 2))
	{
		LOGError("CVideoSourceWebMVpx: %s decoder init failed: %s",
				 info.videoCodecName.c_str(), packetDecoder->GetErrorReason().c_str());
		packetDecoder.reset();
		return false;
	}

	LOGD("CVideoSourceWebMVpx: %s decoded by the OS (%s, nalLengthSize=%d)",
		 info.videoCodecName.c_str(), wantsAnnexB ? "Annex-B" : "length-prefixed", nalLengthSize);
	return true;
}

// ============================================================================
// InitAacAudioDecoder
// ============================================================================
bool CVideoSourceWebMVpx::InitAacAudioDecoder(const u8 *asc, size_t ascLen)
{
#ifdef MT_HAVE_NATIVE_AAC
	audioCodecPrivate.assign(asc, asc + ascLen);
	aacDecoder.reset(new CAudioDecoderAACNative());
	if (!aacDecoder->Init(audioCodecPrivate.data(), (int)audioCodecPrivate.size()))
	{
		LOGWarning("CVideoSourceWebMVpx: AAC decoder init failed: %s",
				   aacDecoder->GetErrorReason().c_str());
		aacDecoder.reset();
		return false;
	}
	return true;
#else
	(void)asc; (void)ascLen;
	return false;
#endif
}

// ============================================================================
// ReadVideoFramePacket -- H.264 / HEVC
// ============================================================================
bool CVideoSourceWebMVpx::ReadVideoFramePacket(SDecodedVideoFrame &out)
{
	if (!packetDecoder)
		return false;

	for (;;)
	{
		// Finish the current packet's chunks before demuxing another. Matroska
		// lacing is rare for video, but the vpx path already honours it and a
		// path that silently dropped laced frames would produce a subtly short
		// clip rather than an error anyone would notice.
		if (currentVideoPacket)
		{
			if (currentVideoPacketChunk < currentVideoPacketNumChunks)
			{
				unsigned int i = currentVideoPacketChunk++;
				unsigned char *data = nullptr;
				size_t dataLen = 0;
				if (nestegg_packet_data(currentVideoPacket, i, &data, &dataLen) != 0 ||
					!data || dataLen == 0)
					continue;

				const u8 *feed = (const u8 *)data;
				int feedSize = (int)dataLen;
				if (wantsAnnexB)
				{
					if (!MT_AnnexB::ConvertPacket((const u8 *)data, dataLen, nalLengthSize, annexBScratch))
						continue;   // corrupt sample: skip it, do not feed half a NAL
					feed = annexBScratch.data();
					feedSize = (int)annexBScratch.size();
				}

				SVideoPacket pkt(feed, feedSize,
								 (i64)(currentVideoPacketPts * 1000000.0),
								 currentVideoPacketKeyframe);
				if (packetDecoder->DecodePacket(&pkt, out))
					return true;

				// An EMPTY reason means "no frame yet, keep feeding" (the
				// reorder window is not full); a non-empty one is a genuine
				// failure the caller must not read as clean EOS.
				const std::string &err = packetDecoder->GetErrorReason();
				if (!err.empty())
				{
					errorReason = err;
					return false;
				}
				continue;
			}
			FreeCurrentVideoPacket();
		}

		nestegg_packet *packet = ReadNextVideoPacket();
		if (!packet)
		{
			if (!errorReason.empty())
				return false;
			// Clean EOF: drain whatever the decoder still holds, one frame per
			// call, exactly as the FFmpeg source does.
			return packetDecoder->DecodePacket(NULL, out);
		}
		AdoptVideoPacket(packet);
	}
}

// ============================================================================
// DecodeAudioPacketAAC
// ============================================================================
void CVideoSourceWebMVpx::DecodeAudioPacketAAC(nestegg_packet *packet)
{
#ifdef MT_HAVE_NATIVE_AAC
	if (!aacDecoder)
		return;

	unsigned int numChunks = 0;
	nestegg_packet_count(packet, &numChunks);

	uint64_t tstampNs = 0;
	nestegg_packet_tstamp(packet, &tstampNs);
	const double pts = static_cast<double>(tstampNs) / 1000000000.0;

	for (unsigned int i = 0; i < numChunks; i++)
	{
		unsigned char *data = nullptr;
		size_t dataLen = 0;
		if (nestegg_packet_data(packet, i, &data, &dataLen) != 0 || !data || dataLen == 0)
			continue;

		std::vector<s16> pcm;
		int channels = 0;
		u32 rate = 0;
		if (!aacDecoder->DecodePacket((const u8 *)data, (int)dataLen, pcm, channels, rate))
		{
			LOGError("CVideoSourceWebMVpx::DecodeAudioPacketAAC: %s",
					 aacDecoder->GetErrorReason().c_str());
			continue;
		}
		if (pcm.empty())
			continue;   // still priming -- not an error

		SDecodedAudio audio;
		audio.channels = (channels > 0) ? channels : audioChannelCount;
		audio.sampleRate = (rate > 0) ? rate : info.audioSampleRate;
		audio.pts = pts;
		audio.pcm = std::move(pcm);

		// THE DECODER'S RATE WINS, not the container's. HE-AAC's SBR doubles
		// the output rate and Matroska records the CORE rate, so a clip
		// mastered at 22.05 kHz + SBR decodes at 44.1 and would play an octave
		// low if we believed the header.
		info.audioSampleRate = audio.sampleRate;
		info.audioChannels = audio.channels;

		pendingAudioFrames.push_back(std::move(audio));
	}
#else
	(void)packet;
#endif
}


// ============================================================================
// Vorbis, through Tremor
//
// WebM hands the three Vorbis setup headers (identification, comment, setup)
// as nestegg codec_data items 0..2 rather than as Ogg pages, so this drives
// Tremor's LOW-LEVEL synthesis API directly instead of ov_open/ov_read the way
// CSlrMusicFileOgg does. Same library, same vendored copy, no new dependency.
// ============================================================================

struct CVideoSourceWebMVpx::SVorbisState
{
	vorbis_info    vi;
	vorbis_comment vc;
	vorbis_dsp_state vd;
	vorbis_block   vb;
	bool dspReady = false;
	bool blockReady = false;
};

bool CVideoSourceWebMVpx::InitVorbisAudioDecoder(unsigned int track)
{
	unsigned int count = 0;
	if (nestegg_track_codec_data_count(demuxCtx, track, &count) != 0 || count != 3)
	{
		LOGError("CVideoSourceWebMVpx: Vorbis track has %u codec_data items, expected 3", count);
		return false;
	}

	SVorbisState *st = new SVorbisState();
	vorbis_info_init(&st->vi);
	vorbis_comment_init(&st->vc);

	for (unsigned int i = 0; i < 3; i++)
	{
		unsigned char *data = nullptr;
		size_t len = 0;
		if (nestegg_track_codec_data(demuxCtx, track, i, &data, &len) != 0 || !data || len == 0)
		{
			LOGError("CVideoSourceWebMVpx: Vorbis setup header %u missing", i);
			vorbis_comment_clear(&st->vc);
			vorbis_info_clear(&st->vi);
			delete st;
			return false;
		}
		ogg_packet op;
		memset(&op, 0, sizeof(op));
		op.packet = data;
		op.bytes = (long)len;
		op.b_o_s = (i == 0) ? 1 : 0;
		op.packetno = (ogg_int64_t)i;
		const int r = vorbis_synthesis_headerin(&st->vi, &st->vc, &op);
		if (r < 0)
		{
			LOGError("CVideoSourceWebMVpx: vorbis_synthesis_headerin(%u) failed: %d", i, r);
			vorbis_comment_clear(&st->vc);
			vorbis_info_clear(&st->vi);
			delete st;
			return false;
		}
	}

	if (vorbis_synthesis_init(&st->vd, &st->vi) != 0)
	{
		LOGError("CVideoSourceWebMVpx: vorbis_synthesis_init failed");
		vorbis_comment_clear(&st->vc);
		vorbis_info_clear(&st->vi);
		delete st;
		return false;
	}
	st->dspReady = true;
	vorbis_block_init(&st->vd, &st->vb);
	st->blockReady = true;

	// THE STREAM'S OWN CHANNEL COUNT AND RATE, not the container's. Matroska's
	// audio element is written by a muxer; the identification header is written
	// by the encoder, and it is the one the decoded samples actually match.
	if (st->vi.channels > 0)
		audioChannelCount = st->vi.channels;
	if (st->vi.rate > 0)
		info.audioSampleRate = (u32)st->vi.rate;

	vorbisState = st;
	return true;
}

void CVideoSourceWebMVpx::DecodeAudioPacketVorbis(nestegg_packet *packet)
{
	SVorbisState *st = vorbisState;
	if (st == nullptr || !st->dspReady)
		return;

	unsigned int numChunks = 0;
	nestegg_packet_count(packet, &numChunks);

	uint64_t tstampNs = 0;
	nestegg_packet_tstamp(packet, &tstampNs);
	const double pts = static_cast<double>(tstampNs) / 1000000000.0;

	const int channels = (audioChannelCount > 0) ? audioChannelCount : st->vi.channels;

	for (unsigned int i = 0; i < numChunks; i++)
	{
		unsigned char *data = nullptr;
		size_t dataLen = 0;
		if (nestegg_packet_data(packet, i, &data, &dataLen) != 0 || !data || dataLen == 0)
			continue;

		ogg_packet op;
		memset(&op, 0, sizeof(op));
		op.packet = data;
		op.bytes = (long)dataLen;
		op.granulepos = -1;
		if (vorbis_synthesis(&st->vb, &op) != 0)
			continue;              // an undecodable packet is skipped, not fatal
		vorbis_synthesis_blockin(&st->vd, &st->vb);

		ogg_int32_t **pcm = nullptr;
		int samples;
		while ((samples = vorbis_synthesis_pcmout(&st->vd, &pcm)) > 0)
		{
			SDecodedAudio audio;
			audio.channels = channels;
			audio.sampleRate = info.audioSampleRate;
			audio.pts = pts;
			audio.pcm.resize((size_t)samples * (size_t)channels);

			// TREMOR IS FIXED POINT: its output is 8.24-ish and the canonical
			// conversion to s16 is >>9 with a clamp, which is what
			// ov_read()/CSlrMusicFileOgg get for free from the same library.
			for (int ch = 0; ch < channels; ch++)
			{
				const ogg_int32_t *src = pcm[ch];
				for (int sIdx = 0; sIdx < samples; sIdx++)
				{
					int v = (int)(src[sIdx] >> 9);
					if (v > 32767) v = 32767;
					else if (v < -32768) v = -32768;
					audio.pcm[(size_t)sIdx * (size_t)channels + (size_t)ch] = (s16)v;
				}
			}
			vorbis_synthesis_read(&st->vd, samples);
			pendingAudioFrames.push_back(std::move(audio));
		}
	}
}

void CVideoSourceWebMVpx::SetAbortPredicate(std::function<bool()> pred)
{
	abortPredicate = pred;
	if (packetDecoder)
		packetDecoder->SetAbortPredicate(pred);
}

void CVideoSourceWebMVpx::WakeAbort()
{
	if (packetDecoder)
		packetDecoder->Abort();
}

// ============================================================================
// Seek
// ============================================================================
bool CVideoSourceWebMVpx::Seek(double seconds)
{
	// Clip-relative time contract (IVideoSource.h): `seconds` here and every
	// pts this source emits (ReadVideoFrame/ReadAudio) are already
	// clip-relative with no translation needed -- nestegg/WebM has no
	// container-level start_time offset (unlike MPEG-TS/AVCHD; see
	// CVideoSourceFFmpeg), so this class's startTime is implicitly always 0.
	if (!demuxCtx || videoTrack < 0)
		return false;

	if (seconds < 0.0)
		seconds = 0.0;
	if (info.duration > 0.0 && seconds > info.duration)
		seconds = info.duration;

	uint64_t tstampNs = static_cast<uint64_t>(seconds * 1000000000.0);
	int ret = nestegg_track_seek(demuxCtx, static_cast<unsigned int>(videoTrack), tstampNs);
	if (ret != 0)
	{
		LOGWarning("CVideoSourceWebMVpx::Seek: nestegg_track_seek failed for time %.3f", seconds);
	}

	// Reset Opus decoder state to avoid glitches
	if (opusDecoder)
	{
		opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
	}

	if (vorbisState != nullptr && vorbisState->dspReady)
		vorbis_synthesis_restart(&vorbisState->vd);

	// The packet decoders carry a reorder queue and, for AAC, a converter
	// priming state. Both describe the stream BEFORE the seek; keeping either
	// across the discontinuity emits frames from the old position.
	if (packetDecoder)
		packetDecoder->Flush();
#ifdef MT_HAVE_NATIVE_AAC
	if (aacDecoder && !audioCodecPrivate.empty())
	{
		// Re-callable by contract (CAudioDecoderAACApple::Init): tears down the
		// converter and rebuilds it, which is the documented way to drop its
		// priming state across a discontinuity.
		aacDecoder->Init(audioCodecPrivate.data(), (int)audioCodecPrivate.size());
	}
#endif

	// Discard anything queued ahead of the seek point
	FreeCurrentVideoPacket();
	pendingVideoImages.clear();
	pendingAudioFrames.clear();
	errorReason.clear();

	return (ret == 0);
}

// ============================================================================
// Close
// ============================================================================
void CVideoSourceWebMVpx::Close()
{
	FreeResources();
}

// ============================================================================
// FreeResources
// ============================================================================
void CVideoSourceWebMVpx::FreeResources()
{
	// Free any partially decoded packet before tearing down the demuxer
	FreeCurrentVideoPacket();

	// Destroy VPX decoder
	if (vpxDecoder)
	{
		vpx_codec_destroy(vpxDecoder);
		delete vpxDecoder;
		vpxDecoder = nullptr;
	}

	// Destroy Opus decoder
	if (opusDecoder)
	{
		opus_decoder_destroy(opusDecoder);
		opusDecoder = nullptr;
	}

	if (vorbisState != nullptr)
	{
		if (vorbisState->blockReady) vorbis_block_clear(&vorbisState->vb);
		if (vorbisState->dspReady)   vorbis_dsp_clear(&vorbisState->vd);
		vorbis_comment_clear(&vorbisState->vc);
		vorbis_info_clear(&vorbisState->vi);
		delete vorbisState;
		vorbisState = nullptr;
	}
	vpxIsVP8 = false;

	packetDecoder.reset();
#ifdef MT_HAVE_NATIVE_AAC
	aacDecoder.reset();
#endif
	videoCodec = EMediaCodecId::Unknown;
	videoCodecPrivate.clear();
	audioCodecPrivate.clear();
	annexBParameterSets.clear();
	annexBScratch.clear();
	nalLengthSize = 4;
	wantsAnnexB = false;
	currentVideoPacketKeyframe = false;

	// Destroy nestegg demuxer
	if (demuxCtx)
	{
		nestegg_destroy(demuxCtx);
		demuxCtx = nullptr;
	}

	// Close file
	if (fileHandle)
	{
		fclose(fileHandle);
		fileHandle = nullptr;
	}

	pendingVideoImages.clear();
	pendingAudioFrames.clear();

	videoTrack = -1;
	audioTrack = -1;
	audioChannelCount = 0;
	vpxColorSpace = 0;
	vpxColorRange = 0;

	info = SVideoInfo();
	errorReason.clear();
}
#endif // MT_ENABLE_WEBM_VPX
