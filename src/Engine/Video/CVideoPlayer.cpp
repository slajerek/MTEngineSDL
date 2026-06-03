#include "CVideoPlayer.h"
#include "CVideoAudioChannel.h"
#include "SYS_Threading.h"
#include "DBG_Log.h"
#include "SND_SoundEngine.h"
#include "SND_Main.h"

#include <cstring>
#include <cmath>
#include <algorithm>
#include <chrono>

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

// ============================================================================
// Constructor / Destructor
// ============================================================================
CVideoPlayer::CVideoPlayer()
{
	mutex = new CSlrMutex("CVideoPlayer");
	// DecodedFrame members have default initializers (nullptr, 0, false) — no memset needed
}

CVideoPlayer::~CVideoPlayer()
{
	Close();

	if (mutex)
	{
		delete mutex;
		mutex = nullptr;
	}
}

// ============================================================================
// Open
// ============================================================================
bool CVideoPlayer::Open(const char *filePath)
{
	LOGD("CVideoPlayer::Open: %s", filePath);

	// Close any previously opened file
	Close();

	// Open file
	fileHandle = fopen(filePath, "rb");
	if (!fileHandle)
	{
		LOGError("CVideoPlayer::Open: failed to open file '%s'", filePath);
		state = EVideoPlayerState::Error;
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
		LOGError("CVideoPlayer::Open: nestegg_init failed for '%s'", filePath);
		fclose(fileHandle);
		fileHandle = nullptr;
		state = EVideoPlayerState::Error;
		return false;
	}

	// Get duration (in nanoseconds)
	uint64_t durationNs = 0;
	if (nestegg_duration(demuxCtx, &durationNs) == 0)
	{
		duration = static_cast<double>(durationNs) / 1000000000.0;
	}
	else
	{
		duration = 0.0;
		LOGWarning("CVideoPlayer::Open: could not determine duration");
	}

	// Enumerate tracks
	unsigned int numTracks = 0;
	nestegg_track_count(demuxCtx, &numTracks);

	LOGD("CVideoPlayer::Open: found %u tracks, duration=%.2f sec", numTracks, duration);

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
				videoWidth = static_cast<int>(videoParams.width);
				videoHeight = static_cast<int>(videoParams.height);

				// Check for alpha mode in video params
				if (videoParams.alpha_mode != 0)
				{
					hasAlpha = true;
				}

				// Estimate FPS from default duration
				uint64_t defaultDuration = 0;
				if (nestegg_track_default_duration(demuxCtx, i, &defaultDuration) == 0 && defaultDuration > 0)
				{
					fps = 1000000000.0 / static_cast<double>(defaultDuration);
				}

				LOGD("CVideoPlayer::Open: video track %d: %dx%d, fps=%.2f, alpha=%d",
					 videoTrack, videoWidth, videoHeight, fps, hasAlpha ? 1 : 0);
			}
			else
			{
				LOGWarning("CVideoPlayer::Open: video track %u has unsupported codec %d (only VP9 supported)", i, trackCodecId);
			}
		}
		else if (trackType == NESTEGG_TRACK_AUDIO && audioTrack < 0)
		{
			if (trackCodecId == NESTEGG_CODEC_OPUS)
			{
				audioTrack = static_cast<int>(i);
				hasAudio = true;

				nestegg_audio_params audioParams;
				nestegg_track_audio_params(demuxCtx, i, &audioParams);
				audioChannelCount = static_cast<int>(audioParams.channels);
				audioSampleRate = static_cast<u32>(audioParams.rate);

				LOGD("CVideoPlayer::Open: audio track %d: %d channels, rate=%u",
					 audioTrack, audioChannelCount, audioSampleRate);
			}
			else
			{
				LOGWarning("CVideoPlayer::Open: audio track %u has unsupported codec %d (only Opus supported)", i, trackCodecId);
			}
		}
	}

	if (videoTrack < 0)
	{
		LOGError("CVideoPlayer::Open: no VP9 video track found in '%s'", filePath);
		nestegg_destroy(demuxCtx);
		demuxCtx = nullptr;
		fclose(fileHandle);
		fileHandle = nullptr;
		state = EVideoPlayerState::Error;
		return false;
	}

	// Initialize decoders
	if (!InitVideoDecoder())
	{
		LOGError("CVideoPlayer::Open: failed to initialize VP9 decoder");
		FreeResources();
		state = EVideoPlayerState::Error;
		return false;
	}

	if (hasAudio)
	{
		if (!InitAudioDecoder())
		{
			LOGWarning("CVideoPlayer::Open: failed to initialize Opus decoder, continuing without audio");
			hasAudio = false;
			audioTrack = -1;
		}
	}

	// Allocate frame ring buffer planes
	AllocateFrameBuffers();

	state = EVideoPlayerState::Idle;
	currentTime = 0.0;

	LOGD("CVideoPlayer::Open: successfully opened '%s'", filePath);
	return true;
}

// ============================================================================
// InitVideoDecoder
// ============================================================================
bool CVideoPlayer::InitVideoDecoder()
{
	LOGD("CVideoPlayer::InitVideoDecoder");

	vpxDecoder = new vpx_codec_ctx_t();
	memset(vpxDecoder, 0, sizeof(vpx_codec_ctx_t));

	vpx_codec_dec_cfg_t cfg;
	memset(&cfg, 0, sizeof(cfg));
	cfg.threads = 2;
	cfg.w = static_cast<unsigned int>(videoWidth);
	cfg.h = static_cast<unsigned int>(videoHeight);

	vpx_codec_err_t err = vpx_codec_dec_init(vpxDecoder, vpx_codec_vp9_dx(), &cfg, 0);
	if (err != VPX_CODEC_OK)
	{
		LOGError("CVideoPlayer::InitVideoDecoder: vpx_codec_dec_init failed: %s", vpx_codec_err_to_string(err));
		delete vpxDecoder;
		vpxDecoder = nullptr;
		return false;
	}

	LOGD("CVideoPlayer::InitVideoDecoder: VP9 decoder initialized (%dx%d, 2 threads)", videoWidth, videoHeight);
	return true;
}

// ============================================================================
// InitAudioDecoder
// ============================================================================
bool CVideoPlayer::InitAudioDecoder()
{
	LOGD("CVideoPlayer::InitAudioDecoder: channels=%d, sampleRate=%u", audioChannelCount, audioSampleRate);

	if (audioChannelCount < 1 || audioChannelCount > 2)
	{
		LOGError("CVideoPlayer::InitAudioDecoder: unsupported channel count %d (only mono/stereo supported)", audioChannelCount);
		return false;
	}

	int opusErr = 0;
	opusDecoder = opus_decoder_create(48000, audioChannelCount, &opusErr);
	if (opusErr != OPUS_OK || !opusDecoder)
	{
		LOGError("CVideoPlayer::InitAudioDecoder: opus_decoder_create failed: %s", opus_strerror(opusErr));
		opusDecoder = nullptr;
		return false;
	}

	// Create audio channel for the sound engine
	audioChannel = new CVideoAudioChannel();

	LOGD("CVideoPlayer::InitAudioDecoder: Opus decoder initialized");
	return true;
}

// ============================================================================
// AllocateFrameBuffers
// ============================================================================
void CVideoPlayer::AllocateFrameBuffers()
{
	// Use ceiling division for UV planes to handle odd dimensions correctly
	int uvWidth = (videoWidth + 1) / 2;
	int uvHeight = (videoHeight + 1) / 2;
	int ySize = videoWidth * videoHeight;
	int uvSize = uvWidth * uvHeight;

	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		frameBuffer[i].yPlane = new u8[ySize];
		frameBuffer[i].uPlane = new u8[uvSize];
		frameBuffer[i].vPlane = new u8[uvSize];
		frameBuffer[i].yStride = videoWidth;
		frameBuffer[i].uStride = uvWidth;
		frameBuffer[i].vStride = uvWidth;
		frameBuffer[i].width = videoWidth;
		frameBuffer[i].height = videoHeight;
		frameBuffer[i].allocWidth = videoWidth;
		frameBuffer[i].allocHeight = videoHeight;
		frameBuffer[i].ready.store(false, std::memory_order_relaxed);
		frameBuffer[i].hasAlpha = false;
		frameBuffer[i].aPlane = nullptr;
		frameBuffer[i].aStride = 0;

		if (hasAlpha)
		{
			frameBuffer[i].aPlane = new u8[ySize];
			frameBuffer[i].aStride = videoWidth;
		}
	}
}

// ============================================================================
// Play / Pause / Stop / Close
// ============================================================================
void CVideoPlayer::Play()
{
	LOGD("CVideoPlayer::Play");

	if (state == EVideoPlayerState::Error)
		return;

	if (!demuxCtx)
	{
		LOGError("CVideoPlayer::Play: no file opened");
		return;
	}

	// Record wall-clock start time for A/V sync
	playbackStartWallTime = GetWallTime();
	playbackStartOffset = currentTime;

	state = EVideoPlayerState::Playing;
	StartDecodeThread();
}

void CVideoPlayer::Pause()
{
	LOGD("CVideoPlayer::Pause");

	if (state == EVideoPlayerState::Playing)
	{
		state = EVideoPlayerState::Paused;
	}
}

void CVideoPlayer::Stop()
{
	LOGD("CVideoPlayer::Stop");

	StopDecodeThread();
	endOfStream.store(false, std::memory_order_relaxed);

	// Reset audio channel ring buffer
	if (audioChannel)
	{
		audioChannel->Reset();
	}

	ClearRingBuffer();

	currentTime = 0.0;
	state = EVideoPlayerState::Idle;

	// Seek back to start
	if (demuxCtx && videoTrack >= 0)
	{
		nestegg_track_seek(demuxCtx, static_cast<unsigned int>(videoTrack), 0);
	}

	if (opusDecoder)
	{
		opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
	}
}

void CVideoPlayer::Close()
{
	LOGD("CVideoPlayer::Close");
	StopDecodeThread();
	FreeResources();
	state = EVideoPlayerState::Idle;
}

// ============================================================================
// Seek
// ============================================================================
void CVideoPlayer::Seek(double timeSeconds)
{
	LOGD("CVideoPlayer::Seek: %.3f sec", timeSeconds);

	if (!demuxCtx || videoTrack < 0)
		return;

	// Clamp to valid range
	if (timeSeconds < 0.0)
		timeSeconds = 0.0;
	if (duration > 0.0 && timeSeconds > duration)
		timeSeconds = duration;

	// Stop decode thread entirely to safely manipulate demuxer/decoder state
	// (avoids race condition where thread is mid-decode while we seek)
	bool wasPlaying = (state == EVideoPlayerState::Playing);
	StopDecodeThread();
	endOfStream.store(false, std::memory_order_relaxed);

	// Seek the demuxer (timestamp in nanoseconds)
	uint64_t tstampNs = static_cast<uint64_t>(timeSeconds * 1000000000.0);
	int ret = nestegg_track_seek(demuxCtx, static_cast<unsigned int>(videoTrack), tstampNs);
	if (ret != 0)
	{
		LOGWarning("CVideoPlayer::Seek: nestegg_track_seek failed for time %.3f", timeSeconds);
	}

	// Reset Opus decoder state to avoid glitches
	if (opusDecoder)
	{
		opus_decoder_ctl(opusDecoder, OPUS_RESET_STATE);
	}

	// Clear the ring buffer
	ClearRingBuffer();

	// Reset audio channel ring buffer
	if (audioChannel)
	{
		audioChannel->Reset();
	}

	// Update playback time
	currentTime = timeSeconds;
	playbackStartOffset = timeSeconds;
	playbackStartWallTime = GetWallTime();

	// Resume decode thread if we were playing
	if (wasPlaying)
	{
		state = EVideoPlayerState::Playing;
		StartDecodeThread();
	}
	else
	{
		state = EVideoPlayerState::Idle;
	}
}

// ============================================================================
// Update (main thread)
// ============================================================================
bool CVideoPlayer::Update(float deltaTime)
{
	if (state != EVideoPlayerState::Playing)
		return false;

	// Check if decode thread signaled EOF and ring buffer is fully drained
	if (endOfStream.load(std::memory_order_acquire))
	{
		uint32_t ri = frameReadIdx.load(std::memory_order_acquire);
		uint32_t wi = frameWriteIdx.load(std::memory_order_acquire);
		if (ri == wi)
		{
			// All buffered frames consumed — transition to Finished
			LOGD("CVideoPlayer::Update: buffer drained after EOF, finishing");
			state = EVideoPlayerState::Finished;
			if (onFinished) onFinished();
			return false;
		}
	}

	// Compute current playback time
	if (hasAudio && audioChannel)
	{
		double audioPos = audioChannel->GetPlaybackPosition();
		if (audioPos > 0.001)
		{
			// Use audio clock for A/V sync once audio is actually playing
			currentTime = playbackStartOffset + audioPos;
		}
		else
		{
			// Audio not yet playing (e.g., not registered with SND mixer yet, or first frame)
			// Fall back to wall clock to avoid blocking video on audio startup
			double wallNow = GetWallTime();
			currentTime = playbackStartOffset + (wallNow - playbackStartWallTime);
		}
	}
	else
	{
		// Wall-clock fallback
		double wallNow = GetWallTime();
		currentTime = playbackStartOffset + (wallNow - playbackStartWallTime);
	}

	// Check if we have frames in the ring buffer
	uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);
	uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);

	if (readIdx == writeIdx)
	{
		// No frames available
		return false;
	}

	// Find the best frame to display: skip frames whose PTS < currentTime (catch up)
	// Use -1 sentinel via int64_t to avoid signed/unsigned issues with uint32_t
	int64_t bestFrameIdx = -1;
	while (readIdx != writeIdx)
	{
		uint32_t bufIdx = readIdx % VIDEO_BUFFER_FRAMES;
		DecodedFrame &frame = frameBuffer[bufIdx];

		if (!frame.ready.load(std::memory_order_acquire))
			break;

		if (frame.pts <= currentTime)
		{
			// This frame is at or before current time -- it's a candidate
			if (bestFrameIdx >= 0)
			{
				// We're skipping the previous best frame (it was too old)
				uint32_t prevBufIdx = (uint32_t)bestFrameIdx % VIDEO_BUFFER_FRAMES;
				frameBuffer[prevBufIdx].ready.store(false, std::memory_order_relaxed);
			}
			bestFrameIdx = (int64_t)readIdx;
			readIdx++;
		}
		else
		{
			// This frame is in the future; stop here
			break;
		}
	}

	if (bestFrameIdx < 0)
		return false;

	uint32_t bufIdx = (uint32_t)bestFrameIdx % VIDEO_BUFFER_FRAMES;
	DecodedFrame &displayFrame = frameBuffer[bufIdx];

	// Update video dimensions from frame
	if (displayFrame.width != videoWidth || displayFrame.height != videoHeight)
	{
		videoWidth = displayFrame.width;
		videoHeight = displayFrame.height;
		if (enableGPUUpload) DestroyGPUTextures();
	}

	// Create GPU textures and upload (only when GPU upload is enabled)
	if (enableGPUUpload)
	{
		if (!gpuTexturesCreated)
		{
			CreateGPUTextures(videoWidth, videoHeight);
		}
		UploadYUVToGPU(displayFrame);
	}

	lastDisplayedFrameIdx = (int)bestFrameIdx;

	// Mark consumed frames as not ready and advance read index
	displayFrame.ready.store(false, std::memory_order_relaxed);
	frameReadIdx.store((uint32_t)(bestFrameIdx + 1), std::memory_order_release);

	return true;
}

// ============================================================================
// Decode thread
// ============================================================================
void CVideoPlayer::StartDecodeThread()
{
	if (threadRunning.load())
		return;

	LOGD("CVideoPlayer::StartDecodeThread");
	shouldStop = false;
	threadRunning = true;
	decodeThread = std::thread(&CVideoPlayer::DecodeThreadFunc, this);
}

void CVideoPlayer::StopDecodeThread()
{
	if (!threadRunning.load())
		return;

	LOGD("CVideoPlayer::StopDecodeThread");
	shouldStop = true;

	if (decodeThread.joinable())
	{
		decodeThread.join();
	}

	threadRunning = false;
}

void CVideoPlayer::DecodeThreadFunc()
{
	LOGD("CVideoPlayer::DecodeThreadFunc: started");

	while (!shouldStop.load())
	{
		if (state == EVideoPlayerState::Playing)
		{
			// Check ring buffer space
			uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
			uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);

			if (writeIdx - readIdx >= VIDEO_BUFFER_FRAMES)
			{
				// Ring buffer is full, wait
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}

			bool gotPacket = DemuxAndDecodeNextPacket();
			if (!gotPacket)
			{
				// EOF — signal main thread to drain remaining frames before finishing
				LOGD("CVideoPlayer::DecodeThreadFunc: end of stream, signaling endOfStream");
				endOfStream.store(true, std::memory_order_release);
				break;
			}
		}
		else
		{
			// Not playing, sleep to avoid spinning
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
	}

	LOGD("CVideoPlayer::DecodeThreadFunc: exiting");
}

// ============================================================================
// DemuxAndDecodeNextPacket
// ============================================================================
bool CVideoPlayer::DemuxAndDecodeNextPacket()
{
	nestegg_packet *packet = nullptr;

	int ret = nestegg_read_packet(demuxCtx, &packet);
	if (ret <= 0 || !packet)
	{
		// ret == 0 means EOF, ret < 0 means error
			return false;
	}

	unsigned int track = 0;
	nestegg_packet_track(packet, &track);

	if (static_cast<int>(track) == videoTrack)
	{
		DecodeVideoPacket(packet);
	}
	else if (static_cast<int>(track) == audioTrack && hasAudio)
	{
		DecodeAudioPacket(packet);
	}

	nestegg_free_packet(packet);
	return true;
}

// ============================================================================
// DecodeVideoPacket
// ============================================================================
void CVideoPlayer::DecodeVideoPacket(nestegg_packet *packet)
{
	unsigned int numChunks = 0;
	nestegg_packet_count(packet, &numChunks);

	// Get packet timestamp (nanoseconds)
	uint64_t tstampNs = 0;
	nestegg_packet_tstamp(packet, &tstampNs);
	double pts = static_cast<double>(tstampNs) / 1000000000.0;

	for (unsigned int i = 0; i < numChunks; i++)
	{
		unsigned char *data = nullptr;
		size_t dataLen = 0;
		int ret = nestegg_packet_data(packet, i, &data, &dataLen);
		if (ret != 0 || !data || dataLen == 0)
			continue;

		vpx_codec_err_t err = vpx_codec_decode(vpxDecoder, data, static_cast<unsigned int>(dataLen), nullptr, 0);
		if (err != VPX_CODEC_OK)
		{
			LOGError("CVideoPlayer::DecodeVideoPacket: vpx_codec_decode failed: %s", vpx_codec_err_to_string(err));
			continue;
		}

		// Retrieve decoded frames
		vpx_codec_iter_t iter = nullptr;
		vpx_image_t *img = nullptr;
		while ((img = vpx_codec_get_frame(vpxDecoder, &iter)) != nullptr)
		{
			StoreDecodedFrame(img, pts);
		}
	}
}

// ============================================================================
// DecodeAudioPacket
// ============================================================================
void CVideoPlayer::DecodeAudioPacket(nestegg_packet *packet)
{
	if (!opusDecoder || !audioChannel)
		return;

	unsigned int numChunks = 0;
	nestegg_packet_count(packet, &numChunks);

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
			LOGError("CVideoPlayer::DecodeAudioPacket: opus_decode_float failed: %s", opus_strerror(samplesDecoded));
			continue;
		}

		if (samplesDecoded > 0 && audioChannel)
		{
			audioChannel->PushSamples(pcmBuffer, samplesDecoded, audioChannelCount);
		}
	}
}

// ============================================================================
// StoreDecodedFrame
// ============================================================================
void CVideoPlayer::StoreDecodedFrame(void *vpxImagePtr, double pts)
{
	vpx_image_t *img = static_cast<vpx_image_t *>(vpxImagePtr);

	// Check ring buffer space
	uint32_t writeIdx = frameWriteIdx.load(std::memory_order_acquire);
	uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);

	if (writeIdx - readIdx >= VIDEO_BUFFER_FRAMES)
	{
		// Ring buffer full, drop this frame
		LOGWarning("CVideoPlayer::StoreDecodedFrame: ring buffer full, dropping frame at pts=%.3f", pts);
		return;
	}

	uint32_t bufIdx = writeIdx % VIDEO_BUFFER_FRAMES;
	DecodedFrame &frame = frameBuffer[bufIdx];

	int imgWidth = static_cast<int>(img->d_w);
	int imgHeight = static_cast<int>(img->d_h);

	// Guard against mid-stream resolution changes exceeding allocated buffer
	if (imgWidth > frame.allocWidth || imgHeight > frame.allocHeight)
	{
		LOGWarning("CVideoPlayer::StoreDecodedFrame: resolution changed to %dx%d (allocated %dx%d), dropping frame",
				   imgWidth, imgHeight, frame.allocWidth, frame.allocHeight);
		return;
	}

	// Copy Y plane
	int ySrcStride = img->stride[VPX_PLANE_Y];
	int yDstStride = imgWidth;
	for (int row = 0; row < imgHeight; row++)
	{
		memcpy(frame.yPlane + row * yDstStride,
			   img->planes[VPX_PLANE_Y] + row * ySrcStride,
			   static_cast<size_t>(imgWidth));
	}

	// Copy U plane (half resolution)
	int uvWidth = (imgWidth + 1) / 2;
	int uvHeight = (imgHeight + 1) / 2;
	int uSrcStride = img->stride[VPX_PLANE_U];
	int uDstStride = uvWidth;
	for (int row = 0; row < uvHeight; row++)
	{
		memcpy(frame.uPlane + row * uDstStride,
			   img->planes[VPX_PLANE_U] + row * uSrcStride,
			   static_cast<size_t>(uvWidth));
	}

	// Copy V plane (half resolution)
	int vSrcStride = img->stride[VPX_PLANE_V];
	int vDstStride = uvWidth;
	for (int row = 0; row < uvHeight; row++)
	{
		memcpy(frame.vPlane + row * vDstStride,
			   img->planes[VPX_PLANE_V] + row * vSrcStride,
			   static_cast<size_t>(uvWidth));
	}

	// Copy alpha plane if present
	// VP9 alpha in WebM: alpha_mode flag + VPX_PLANE_ALPHA (plane index 3)
	bool frameHasAlpha = false;
	if (hasAlpha && img->planes[VPX_PLANE_ALPHA] != nullptr)
	{
		frameHasAlpha = true;
		if (frame.aPlane)
		{
			int aSrcStride = img->stride[VPX_PLANE_ALPHA];
			int aDstStride = imgWidth;
			for (int row = 0; row < imgHeight; row++)
			{
				memcpy(frame.aPlane + row * aDstStride,
					   img->planes[VPX_PLANE_ALPHA] + row * aSrcStride,
					   static_cast<size_t>(imgWidth));
			}
		}
	}

	// Store color space and range info
	vpxColorSpace = img->cs;
	vpxColorRange = img->range;

	frame.yStride = yDstStride;
	frame.uStride = uDstStride;
	frame.vStride = vDstStride;
	frame.aStride = frameHasAlpha ? imgWidth : 0;
	frame.width = imgWidth;
	frame.height = imgHeight;
	frame.pts = pts;
	frame.hasAlpha = frameHasAlpha;
	frame.ready.store(true, std::memory_order_release);

	frameWriteIdx.store(writeIdx + 1, std::memory_order_release);
}

// ============================================================================
// GPU texture management
// ============================================================================
void CVideoPlayer::CreateGPUTextures(int width, int height)
{
	LOGD("CVideoPlayer::CreateGPUTextures: %dx%d", width, height);

	int uvWidth = (width + 1) / 2;
	int uvHeight = (height + 1) / 2;

	// Y texture (full resolution)
	glGenTextures(1, &texY);
	glBindTexture(GL_TEXTURE_2D, texY);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

	// U texture (half resolution)
	glGenTextures(1, &texU);
	glBindTexture(GL_TEXTURE_2D, texU);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uvWidth, uvHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

	// V texture (half resolution)
	glGenTextures(1, &texV);
	glBindTexture(GL_TEXTURE_2D, texV);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, uvWidth, uvHeight, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);

	// Alpha texture (full resolution, created lazily only if alpha is present)
	if (hasAlpha)
	{
		glGenTextures(1, &texA);
		glBindTexture(GL_TEXTURE_2D, texA);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, width, height, 0, GL_RED, GL_UNSIGNED_BYTE, nullptr);
	}

	glBindTexture(GL_TEXTURE_2D, 0);
	gpuTexturesCreated = true;
}

void CVideoPlayer::UploadYUVToGPU(DecodedFrame &frame)
{
	int uvWidth = (frame.width + 1) / 2;
	int uvHeight = (frame.height + 1) / 2;

	// GL_R8 textures are 1 byte per pixel — need alignment of 1 (default is 4)
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

	// Y plane
	glBindTexture(GL_TEXTURE_2D, texY);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.yStride);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
					GL_RED, GL_UNSIGNED_BYTE, frame.yPlane);

	// U plane
	glBindTexture(GL_TEXTURE_2D, texU);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.uStride);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
					GL_RED, GL_UNSIGNED_BYTE, frame.uPlane);

	// V plane
	glBindTexture(GL_TEXTURE_2D, texV);
	glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.vStride);
	glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, uvWidth, uvHeight,
					GL_RED, GL_UNSIGNED_BYTE, frame.vPlane);

	// Alpha plane (if present)
	if (frame.hasAlpha && texA != 0 && frame.aPlane)
	{
		glBindTexture(GL_TEXTURE_2D, texA);
		glPixelStorei(GL_UNPACK_ROW_LENGTH, frame.aStride);
		glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, frame.width, frame.height,
						GL_RED, GL_UNSIGNED_BYTE, frame.aPlane);
	}

	// Reset GL state
	glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 4);  // Restore default
	glBindTexture(GL_TEXTURE_2D, 0);
}

void CVideoPlayer::DestroyGPUTextures()
{
	if (texY != 0) { glDeleteTextures(1, &texY); texY = 0; }
	if (texU != 0) { glDeleteTextures(1, &texU); texU = 0; }
	if (texV != 0) { glDeleteTextures(1, &texV); texV = 0; }
	if (texA != 0) { glDeleteTextures(1, &texA); texA = 0; }
	gpuTexturesCreated = false;
}

// ============================================================================
// GetCurrentFrameRGBA - CPU fallback
// ============================================================================
u8 *CVideoPlayer::GetCurrentFrameRGBA()
{
	// Find the most recently displayed frame
	uint32_t readIdx = frameReadIdx.load(std::memory_order_acquire);
	int targetIdx = readIdx - 1;
	if (targetIdx < 0)
		return nullptr;

	int bufIdx = targetIdx % VIDEO_BUFFER_FRAMES;
	DecodedFrame &frame = frameBuffer[bufIdx];

	if (frame.width <= 0 || frame.height <= 0)
		return nullptr;

	int requiredSize = frame.width * frame.height * 4;
	if (rgbaBuffer == nullptr || rgbaBufferSize < requiredSize)
	{
		delete[] rgbaBuffer;
		rgbaBuffer = new u8[requiredSize];
		rgbaBufferSize = requiredSize;
	}

	ConvertYUV420ToRGBA(frame, rgbaBuffer);
	return rgbaBuffer;
}

// ============================================================================
// ConvertYUV420ToRGBA
// ============================================================================
void CVideoPlayer::ConvertYUV420ToRGBA(const DecodedFrame &frame, u8 *outRGBA)
{
	// Select color matrix coefficients based on color space
	// VPX_CS_BT_709 = 1, VPX_CS_BT_601 = 5 (or default)
	// BT.601: Y'=  0.299R + 0.587G + 0.114B
	// BT.709: Y'= 0.2126R + 0.7152G + 0.0722B

	bool isBT709 = (vpxColorSpace == 2); // VPX_CS_BT_709 = 2 (VPX_CS_BT_601 = 1)
	bool isFullRange = (vpxColorRange == 1); // VPX_CR_FULL_RANGE

	// Fixed-point coefficients (shifted by 16 bits)
	// BT.601 limited range:
	//   R = 1.164*(Y-16) + 1.596*(V-128)
	//   G = 1.164*(Y-16) - 0.392*(U-128) - 0.813*(V-128)
	//   B = 1.164*(Y-16) + 2.017*(U-128)
	// BT.709 limited range:
	//   R = 1.164*(Y-16) + 1.793*(V-128)
	//   G = 1.164*(Y-16) - 0.213*(U-128) - 0.533*(V-128)
	//   B = 1.164*(Y-16) + 2.112*(U-128)
	// Full range: just use Y directly (no 16 offset, no 1.164 scaling)

	// Integer coefficients scaled by 65536 (1 << 16)
	int cY, cRV, cGU, cGV, cBU;
	int yOffset;

	if (isFullRange)
	{
		yOffset = 0;
		if (isBT709)
		{
			cY  = 65536;  // 1.0
			cRV = 103206; // 1.5748
			cGU = -12276; // -0.1873
			cGV = -30679; // -0.4681
			cBU = 121608; // 1.8556
		}
		else
		{
			// BT.601 full range
			cY  = 65536;  // 1.0
			cRV = 91881;  // 1.402
			cGU = -22554; // -0.3441
			cGV = -46802; // -0.7141
			cBU = 116130; // 1.772
		}
	}
	else
	{
		// Limited range
		yOffset = 16;
		if (isBT709)
		{
			cY  = 76309;  // 1.164
			cRV = 117489; // 1.793
			cGU = -13975; // -0.213
			cGV = -34925; // -0.533
			cBU = 138438; // 2.112
		}
		else
		{
			// BT.601 limited range (default)
			cY  = 76309;  // 1.164
			cRV = 104597; // 1.596
			cGU = -25675; // -0.392
			cGV = -53279; // -0.813
			cBU = 132201; // 2.017
		}
	}

	int width = frame.width;
	int height = frame.height;

	for (int row = 0; row < height; row++)
	{
		const u8 *yRow = frame.yPlane + row * frame.yStride;
		const u8 *uRow = frame.uPlane + (row / 2) * frame.uStride;
		const u8 *vRow = frame.vPlane + (row / 2) * frame.vStride;
		const u8 *aRow = (frame.hasAlpha && frame.aPlane) ? (frame.aPlane + row * frame.aStride) : nullptr;
		u8 *dst = outRGBA + row * width * 4;

		for (int col = 0; col < width; col++)
		{
			int y = static_cast<int>(yRow[col]) - yOffset;
			int u = static_cast<int>(uRow[col / 2]) - 128;
			int v = static_cast<int>(vRow[col / 2]) - 128;

			int r = (cY * y + cRV * v + 32768) >> 16;
			int g = (cY * y + cGU * u + cGV * v + 32768) >> 16;
			int b = (cY * y + cBU * u + 32768) >> 16;

			// Clamp to [0, 255]
			if (r < 0) r = 0; else if (r > 255) r = 255;
			if (g < 0) g = 0; else if (g > 255) g = 255;
			if (b < 0) b = 0; else if (b > 255) b = 255;

			dst[0] = static_cast<u8>(r);
			dst[1] = static_cast<u8>(g);
			dst[2] = static_cast<u8>(b);
			dst[3] = aRow ? aRow[col] : 255;
			dst += 4;
		}
	}
}

// ============================================================================
// Ring buffer management
// ============================================================================
void CVideoPlayer::ClearRingBuffer()
{
	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		frameBuffer[i].ready.store(false, std::memory_order_relaxed);
	}
	frameWriteIdx.store(0, std::memory_order_release);
	frameReadIdx.store(0, std::memory_order_release);
}

// ============================================================================
// FreeResources
// ============================================================================
void CVideoPlayer::FreeResources()
{
	LOGD("CVideoPlayer::FreeResources");

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

	// Free frame buffer planes
	for (int i = 0; i < VIDEO_BUFFER_FRAMES; i++)
	{
		delete[] frameBuffer[i].yPlane;  frameBuffer[i].yPlane = nullptr;
		delete[] frameBuffer[i].uPlane;  frameBuffer[i].uPlane = nullptr;
		delete[] frameBuffer[i].vPlane;  frameBuffer[i].vPlane = nullptr;
		delete[] frameBuffer[i].aPlane;  frameBuffer[i].aPlane = nullptr;
		frameBuffer[i].ready.store(false, std::memory_order_relaxed);
	}
	frameWriteIdx.store(0, std::memory_order_release);
	frameReadIdx.store(0, std::memory_order_release);

	// Destroy GPU textures
	DestroyGPUTextures();

	// Remove audio channel from mixer (if still registered) before deleting
	if (audioChannel)
	{
		if (audioChannel->isActive)
		{
			SND_RemoveChannel(audioChannel);
		}
		delete audioChannel;
		audioChannel = nullptr;
	}

	// Free RGBA fallback buffer
	if (rgbaBuffer)
	{
		delete[] rgbaBuffer;
		rgbaBuffer = nullptr;
		rgbaBufferSize = 0;
	}

	// Reset metadata
	videoTrack = -1;
	audioTrack = -1;
	hasAudio = false;
	hasAlpha = false;
	videoWidth = 0;
	videoHeight = 0;
	duration = 0.0;
	fps = 30.0;
	currentTime = 0.0;
	lastDisplayedFrameIdx = -1;
}

// ============================================================================
// GetWallTime
// ============================================================================
double CVideoPlayer::GetWallTime() const
{
	auto now = std::chrono::high_resolution_clock::now();
	auto epoch = now.time_since_epoch();
	return std::chrono::duration<double>(epoch).count();
}
