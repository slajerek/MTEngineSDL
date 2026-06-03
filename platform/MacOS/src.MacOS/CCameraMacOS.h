#ifndef _CCameraMacOS_h_
#define _CCameraMacOS_h_

#include "CCameraCapture.h"

class CCameraMacOS : public CCameraCapture
{
public:
	CCameraMacOS();
	virtual ~CCameraMacOS();

	std::vector<CCameraDevice> EnumerateDevices() override;
	bool StartCapture(int deviceIndex, int requestedWidth, int requestedHeight) override;
	void StopCapture() override;
	bool IsCapturing() override;
	bool IsNewFrameReady() override;
	bool GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight) override;
	int GetFrameWidth() override;
	int GetFrameHeight() override;

private:
	void *captureHelper; // Objective-C helper object (CCameraMacOSHelper *)
};

#endif
