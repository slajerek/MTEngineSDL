#ifndef _CCameraLinux_h_
#define _CCameraLinux_h_

#include "CCameraCapture.h"
#include <mutex>
#include <atomic>
#include <thread>

class CCameraLinux : public CCameraCapture
{
public:
	CCameraLinux();
	virtual ~CCameraLinux();

	std::vector<CCameraDevice> EnumerateDevices() override;
	bool StartCapture(int deviceIndex, int requestedWidth, int requestedHeight) override;
	void StopCapture() override;
	bool IsCapturing() override;
	bool IsNewFrameReady() override;
	bool GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight) override;
	int GetFrameWidth() override;
	int GetFrameHeight() override;

private:
	static const int NUM_BUFFERS = 4;

	struct MappedBuffer
	{
		void *start;
		size_t length;
	};

	int fd;
	MappedBuffer mappedBuffers[NUM_BUFFERS];
	int numMappedBuffers;

	int captureWidth;
	int captureHeight;

	u8 *internalBuffer;
	std::mutex frameMutex;
	std::atomic<bool> newFrame;
	std::atomic<bool> capturing;

	std::thread captureThread;
	std::atomic<bool> threadRunning;

	void CaptureThreadFunc();
	void UnmapBuffers();
	bool ConvertYUYVToRGBA(const u8 *src, int width, int height);
};

#endif
