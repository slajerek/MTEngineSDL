#include "CCameraCapture.h"

#if defined(MT_CAMERA_CAPTURE_ENABLED)
extern CCameraCapture *SYS_CreatePlatformCameraCapture();
#endif

static CCameraCapture *cameraInstance = NULL;

CCameraCapture::~CCameraCapture()
{
}

CCameraCapture *CCameraCapture::Instance()
{
	if (cameraInstance == NULL)
	{
#if defined(MT_CAMERA_CAPTURE_ENABLED)
		cameraInstance = SYS_CreatePlatformCameraCapture();
#else
		cameraInstance = new CCameraDummy();
#endif
	}
	return cameraInstance;
}
