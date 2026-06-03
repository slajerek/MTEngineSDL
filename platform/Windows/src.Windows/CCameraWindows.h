#ifndef _CCameraWindows_h_
#define _CCameraWindows_h_

#include "CCameraCapture.h"
#include <mutex>
#include <atomic>
#include <thread>

// Forward-declare MF types to avoid pulling in all Media Foundation headers here
struct IMFSourceReader;

class CCameraWindows : public CCameraCapture
{
public:
	CCameraWindows();
	virtual ~CCameraWindows();

	std::vector<CCameraDevice> EnumerateDevices() override;
	bool StartCapture(int deviceIndex, int requestedWidth, int requestedHeight) override;
	void StopCapture() override;
	bool IsCapturing() override;
	bool IsNewFrameReady() override;
	bool GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight) override;
	int GetFrameWidth() override;
	int GetFrameHeight() override;

private:
	IMFSourceReader *sourceReader;

	int captureWidth;
	int captureHeight;

	u8 *internalBuffer;
	std::mutex frameMutex;
	std::atomic<bool> newFrame;
	std::atomic<bool> capturing;

	std::thread captureThread;
	std::atomic<bool> threadRunning;

	void CaptureThreadFunc();
};

#endif
