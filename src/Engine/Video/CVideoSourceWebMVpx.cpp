#include "CVideoSourceWebMVpx.h"
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
			if (trackCodecId == NESTEGG_CODEC_VP9)
			{
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

				info.videoCodecName = "vp9";

				LOGD("CVideoSourceWebMVpx::Open: video track %d: %dx%d, fps=%.2f, alpha=%d",
					 videoTrack, info.width, info.height, info.fps, info.hasAlpha ? 1 : 0);
			}
			else
			{
				LOGWarning("CVideoSourceWebMVpx::Open: video track %u has unsupported codec %d (only VP9 supported)", i, trackCodecId);
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
			else
			{
				LOGWarning("CVideoSourceWebMVpx::Open: audio track %u has unsupported codec %d (only Opus supported)", i, trackCodecId);
			}
		}
	}

	if (videoTrack < 0)
	{
		LOGError("CVideoSourceWebMVpx::Open: no VP9 video track found in '%s'", filePath);
		nestegg_destroy(demuxCtx);
		demuxCtx = nullptr;
		fclose(fileHandle);
		fileHandle = nullptr;
		errorReason = "no VP9 video track found";
		return false;
	}

	// Initialize decoders
	if (!InitVideoDecoder())
	{
		LOGError("CVideoSourceWebMVpx::Open: failed to initialize VP9 decoder");
		errorReason = "failed to initialize VP9 decoder";
		FreeResources();
		return false;
	}

	if (info.hasAudio)
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

	vpx_codec_err_t err = vpx_codec_dec_init(vpxDecoder, vpx_codec_vp9_dx(), &cfg, 0);
	if (err != VPX_CODEC_OK)
	{
		LOGError("CVideoSourceWebMVpx::InitVideoDecoder: vpx_codec_dec_init failed: %s", vpx_codec_err_to_string(err));
		delete vpxDecoder;
		vpxDecoder = nullptr;
		return false;
	}

	LOGD("CVideoSourceWebMVpx::InitVideoDecoder: VP9 decoder initialized (%dx%d, 2 threads)", info.width, info.height);
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

		nestegg_packet *packet = nullptr;
		int ret = nestegg_read_packet(demuxCtx, &packet);
		if (ret <= 0 || !packet)
		{
			// ret == 0 means EOF, ret < 0 means error
			errorReason = (ret < 0) ? "nestegg_read_packet failed" : "";
			return false;
		}

		unsigned int track = 0;
		nestegg_packet_track(packet, &track);

		if (static_cast<int>(track) == videoTrack)
		{
			// Adopt as the current video packet; chunks are decoded lazily above
			currentVideoPacket = packet;
			currentVideoPacketChunk = 0;
			currentVideoPacketNumChunks = 0;
			nestegg_packet_count(packet, &currentVideoPacketNumChunks);

			uint64_t tstampNs = 0;
			nestegg_packet_tstamp(packet, &tstampNs);
			currentVideoPacketPts = static_cast<double>(tstampNs) / 1000000000.0;
		}
		else
		{
			if (static_cast<int>(track) == audioTrack && info.hasAudio)
			{
				DecodeAudioPacket(packet);
			}
			nestegg_free_packet(packet);
		}
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
