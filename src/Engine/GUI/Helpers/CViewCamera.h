#ifndef _CViewCamera_h_
#define _CViewCamera_h_

#include "CGuiViewMovingPaneImage.h"
#include "CCameraCapture.h"
#include <vector>

#define kDefaultCameraWidth  640
#define kDefaultCameraHeight 480

class CViewCamera : public CGuiViewMovingPaneImage
{
public:
	CViewCamera(const char *name, float posX, float posY, float posZ,
				float sizeX, float sizeY);
	virtual ~CViewCamera();

	virtual bool UpdateImageData() override;
	virtual void RenderImGui() override;
	virtual bool HasContextMenuItems() override;
	virtual void RenderContextMenuItems() override;

	// Camera control
	void SelectDevice(int deviceIndex);
	void RefreshDeviceList();
	std::vector<CCameraDevice> &GetDevices();
	int GetSelectedDeviceIndex();

	// Layout persistence
	virtual void SerializeLayout(CByteBuffer *byteBuffer) override;
	virtual bool DeserializeLayout(CByteBuffer *byteBuffer, int version) override;

	// Visibility change
	virtual void SetVisible(bool isVisible) override;

private:
	CCameraCapture *cameraCapture;
	std::vector<CCameraDevice> devices;
	int selectedDeviceIndex;
	char selectedDeviceUniqueId[256];

	u8 *cameraFrameBuffer;
	int cameraFrameBufferWidth;
	int cameraFrameBufferHeight;

	bool cameraWasCapturing;

	void StartCamera();
	void StopCamera();
	void AllocFrameBuffer(int width, int height);
	void FreeFrameBuffer();
	void InitCameraImage(int width, int height);

	enum CameraStatus { CAMERA_OK, CAMERA_NO_DEVICES, CAMERA_UNAVAILABLE, CAMERA_DISCONNECTED };
	CameraStatus cameraStatus;
};

#endif
