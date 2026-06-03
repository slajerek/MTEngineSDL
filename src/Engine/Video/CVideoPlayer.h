#ifndef _CVIDEOPLAYER_H_
#define _CVIDEOPLAYER_H_

#pragma once

#include "SYS_Defs.h"
#include <string>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdio>

// OpenGL
#ifdef __APPLE__
#include <OpenGL/gl3.h>
#else
#include <GL/glew.h>
#endif

// Forward declarations for nestegg (C library)
struct nestegg;
struct nestegg_packet;

// Forward declarations for vpx (C library)
typedef struct vpx_codec_ctx vpx_codec_ctx_t;

// Forward declaration for Opus decoder (opaque type from opus.h)
struct OpusDecoder;

class CSlrMutex;
class CVideoAudioChannel;

enum class EVideoPlayerState
{
	Idle,
	Playing,
	Paused,
	Finished,
	Error
};

class CVideoPlayer
{
public:
	CVideoPlayer();
	~CVideoPlayer();

	// --- Public API ---

	// Open a .webm file, parse tracks, prepare decoders
	bool Open(const char *filePath);

	// Playback control
	void Play();
	void Pause();
	void Stop();
	void Seek(double timeSeconds);
	void Close();

	// Call from main/render thread each frame. Returns true if a new video frame was uploaded to GPU.
	bool Update(float deltaTime);

	// GPU texture handles (GL_R8 luminance textures for YUV shader)
	GLuint GetYTexture() const { return texY; }
	GLuint GetUTexture() const { return texU; }
	GLuint GetVTexture() const { return texV; }
	GLuint GetATexture() const { return texA; }

	// Video metadata
	bool HasAlpha() const { return hasAlpha; }
	int GetColorSpace() const { return vpxColorSpace; }
	int GetColorRange() const { return vpxColorRange; }
	int GetVideoWidth() const { return videoWidth; }
	int GetVideoHeight() const { return videoHeight; }
	double GetDuration() const { return duration; }
	double GetCurrentTime() const { return currentTime; }
	double GetFps() const { return fps; }

	// State
	bool IsPlaying() const { return state == EVideoPlayerState::Playing; }
	bool IsFinished() const { return state == EVideoPlayerState::Finished; }
	EVideoPlayerState GetState() const { return state; }

	// Audio channel for SND_AddChannel
	CVideoAudioChannel *GetAudioChannel() const { return audioChannel; }

	// CPU fallback: returns RGBA pixel buffer (width * height * 4 bytes), or nullptr
	u8 *GetCurrentFrameRGBA();

	// Set to false to skip GPU texture creation/upload (e.g., for headless tests)
	// When false, frames are still decoded and available via GetCurrentFrameRGBA()
	bool enableGPUUpload = true;

	// Callback invoked when playback finishes (called from main thread in Update())
	std::function<void()> onFinished;

private:
	// --- Decoded frame ring buffer ---
	static constexpr int VIDEO_BUFFER_FRAMES = 4;

	struct DecodedFrame
	{
		u8 *yPlane = nullptr;
		u8 *uPlane = nullptr;
		u8 *vPlane = nullptr;
		u8 *aPlane = nullptr;
		int yStride = 0;
		int uStride = 0;
		int vStride = 0;
		int aStride = 0;
		int width = 0;
		int height = 0;
		int allocWidth = 0;  // Allocated plane dimensions (may be >= decoded dimensions)
		int allocHeight = 0;
		double pts = 0.0;
		std::atomic<bool> ready{false};
		bool hasAlpha = false;
	};

	DecodedFrame frameBuffer[VIDEO_BUFFER_FRAMES];
	std::atomic<uint32_t> frameWriteIdx{0};
	std::atomic<uint32_t> frameReadIdx{0};

	// --- GPU textures ---
	GLuint texY = 0;
	GLuint texU = 0;
	GLuint texV = 0;
	GLuint texA = 0;
	bool gpuTexturesCreated = false;

	// --- Video metadata ---
	int videoWidth = 0;
	int videoHeight = 0;
	double duration = 0.0;
	double fps = 30.0;
	double currentTime = 0.0;
	bool hasAlpha = false;

	// Color space info from vpx_image (VPX_CS_* and VPX_CR_* enums stored as int)
	int vpxColorSpace = 0;
	int vpxColorRange = 0;

	// --- Demuxer ---
	nestegg *demuxCtx = nullptr;
	FILE *fileHandle = nullptr;
	int videoTrack = -1;
	int audioTrack = -1;
	bool hasAudio = false;
	int audioChannelCount = 0;
	u32 audioSampleRate = 48000;

	// --- Decoders ---
	vpx_codec_ctx_t *vpxDecoder = nullptr;
	OpusDecoder *opusDecoder = nullptr;

	// --- Audio ---
	CVideoAudioChannel *audioChannel = nullptr;

	// --- Playback state ---
	std::atomic<EVideoPlayerState> state{EVideoPlayerState::Idle};
	double playbackStartWallTime = 0.0;
	double playbackStartOffset = 0.0;

	// --- Decode thread ---
	std::thread decodeThread;
	std::atomic<bool> shouldStop{false};
	std::atomic<bool> threadRunning{false};
	std::atomic<bool> endOfStream{false};
	CSlrMutex *mutex = nullptr;

	// --- CPU fallback RGBA buffer ---
	u8 *rgbaBuffer = nullptr;
	int rgbaBufferSize = 0;

	// Last displayed frame index (for CPU fallback)
	int lastDisplayedFrameIdx = -1;

	// --- Private methods ---
	bool InitVideoDecoder();
	bool InitAudioDecoder();
	void AllocateFrameBuffers();

	void StartDecodeThread();
	void StopDecodeThread();
	void DecodeThreadFunc();

	bool DemuxAndDecodeNextPacket();
	void DecodeVideoPacket(nestegg_packet *packet);
	void DecodeAudioPacket(nestegg_packet *packet);
	void StoreDecodedFrame(void *vpxImage, double pts);

	void CreateGPUTextures(int width, int height);
	void UploadYUVToGPU(DecodedFrame &frame);
	void DestroyGPUTextures();

	void ConvertYUV420ToRGBA(const DecodedFrame &frame, u8 *outRGBA);

	void FreeResources();
	void ClearRingBuffer();

	double GetWallTime() const;
};

#endif
//_CVIDEOPLAYER_H_
