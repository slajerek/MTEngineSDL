#ifndef _CCameraCapture_h_
#define _CCameraCapture_h_

#include "SYS_Defs.h"
#include <vector>

struct CCameraDevice
{
	int index;
	char name[256];
	char uniqueId[256];
};

class CCameraCapture
{
public:
	virtual ~CCameraCapture();

	static CCameraCapture *Instance();

	virtual std::vector<CCameraDevice> EnumerateDevices() = 0;

	virtual bool StartCapture(int deviceIndex, int requestedWidth, int requestedHeight) = 0;
	virtual void StopCapture() = 0;
	virtual bool IsCapturing() = 0;

	virtual bool IsNewFrameReady() = 0;
	virtual bool GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight) = 0;

	virtual int GetFrameWidth() = 0;
	virtual int GetFrameHeight() = 0;
};

// Dummy/stub implementation — no camera support
class CCameraDummy : public CCameraCapture
{
public:
	virtual ~CCameraDummy() {}
	std::vector<CCameraDevice> EnumerateDevices() override { return {}; }
	bool StartCapture(int deviceIndex, int requestedWidth, int requestedHeight) override { return false; }
	void StopCapture() override {}
	bool IsCapturing() override { return false; }
	bool IsNewFrameReady() override { return false; }
	bool GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight) override { return false; }
	int GetFrameWidth() override { return 0; }
	int GetFrameHeight() override { return 0; }
};

#endif
