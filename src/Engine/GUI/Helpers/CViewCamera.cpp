#include "CViewCamera.h"
#include "CGuiMain.h"
#include "VID_ImageBinding.h"
#include "CSlrImage.h"
#include "CImageData.h"
#include "CByteBuffer.h"
#include "DBG_Log.h"
#include "SYS_Defs.h"
#include <cstring>

CViewCamera::CViewCamera(const char *name, float posX, float posY, float posZ,
						  float sizeX, float sizeY)
: CGuiViewMovingPaneImage(name, posX, posY, posZ, sizeX, sizeY,
						   "Camera", "camera.view")
{
	cameraCapture = CCameraCapture::Instance();
	selectedDeviceIndex = -1;
	selectedDeviceUniqueId[0] = '\0';
	cameraFrameBuffer = NULL;
	cameraFrameBufferWidth = 0;
	cameraFrameBufferHeight = 0;
	cameraWasCapturing = false;
	cameraStatus = CAMERA_NO_DEVICES;

	RefreshDeviceList();
}

CViewCamera::~CViewCamera()
{
	StopCamera();
	FreeFrameBuffer();
}

void CViewCamera::RefreshDeviceList()
{
	devices = cameraCapture->EnumerateDevices();
	if (devices.empty())
	{
		cameraStatus = CAMERA_NO_DEVICES;
	}
}

std::vector<CCameraDevice> &CViewCamera::GetDevices()
{
	return devices;
}

int CViewCamera::GetSelectedDeviceIndex()
{
	return selectedDeviceIndex;
}

void CViewCamera::AllocFrameBuffer(int width, int height)
{
	FreeFrameBuffer();
	cameraFrameBufferWidth = width;
	cameraFrameBufferHeight = height;
	cameraFrameBuffer = new u8[width * height * 4];
	memset(cameraFrameBuffer, 0, width * height * 4);
}

void CViewCamera::FreeFrameBuffer()
{
	if (cameraFrameBuffer)
	{
		delete[] cameraFrameBuffer;
		cameraFrameBuffer = NULL;
	}
	cameraFrameBufferWidth = 0;
	cameraFrameBufferHeight = 0;
}

void CViewCamera::InitCameraImage(int width, int height)
{
	guiMain->LockMutex();

	if (this->image && shouldDeallocImage)
	{
		VID_PostImageDealloc(this->image);
	}
	this->image = NULL;

	if (this->imageData)
	{
		delete this->imageData;
	}
	this->imageData = NULL;

	CreateEmptyImageData(width, height);

	shouldDeallocImage = true;
	image = new CSlrImage(true, false);
	image->LoadImageForRebinding(imageData, RESOURCE_PRIORITY_STATIC);
	VID_PostImageBinding(image, NULL, BINDING_MODE_DONT_FREE_IMAGEDATA);

	renderTextureStartX = 0.0f;
	renderTextureEndX = ((float)paneWidth / (float)rasterWidth);
	renderTextureStartY = 0.0f;
	renderTextureEndY = ((float)paneHeight / (float)rasterHeight);

	SetKeepAspectRatio(true, (float)width / (float)height);

	guiMain->UnlockMutex();
}

void CViewCamera::StartCamera()
{
	if (selectedDeviceIndex < 0 || selectedDeviceIndex >= (int)devices.size())
		return;

	if (cameraCapture->IsCapturing())
	{
		cameraCapture->StopCapture();
	}

	bool started = cameraCapture->StartCapture(selectedDeviceIndex, kDefaultCameraWidth, kDefaultCameraHeight);
	if (!started)
	{
		cameraStatus = CAMERA_UNAVAILABLE;
		return;
	}

	// Use default dimensions initially — actual frame size will be applied
	// when the first frame arrives in UpdateImageData()
	int w = kDefaultCameraWidth;
	int h = kDefaultCameraHeight;

	AllocFrameBuffer(w, h);
	InitCameraImage(w, h);

	cameraStatus = CAMERA_OK;
	cameraWasCapturing = true;
}

void CViewCamera::StopCamera()
{
	if (cameraCapture->IsCapturing())
	{
		cameraCapture->StopCapture();
	}
	cameraWasCapturing = false;
}

void CViewCamera::SelectDevice(int deviceIndex)
{
	if (deviceIndex < 0 || deviceIndex >= (int)devices.size())
		return;

	StopCamera();

	selectedDeviceIndex = deviceIndex;
	strncpy(selectedDeviceUniqueId, devices[deviceIndex].uniqueId, sizeof(selectedDeviceUniqueId) - 1);
	selectedDeviceUniqueId[sizeof(selectedDeviceUniqueId) - 1] = '\0';

	if (visible)
	{
		StartCamera();
	}
}

void CViewCamera::SetVisible(bool isVisible)
{
	CGuiViewMovingPaneImage::SetVisible(isVisible);

	if (isVisible)
	{
		// Auto-select first device if none selected
		if (selectedDeviceIndex < 0)
		{
			RefreshDeviceList();
			if (!devices.empty())
			{
				selectedDeviceIndex = 0;
				strncpy(selectedDeviceUniqueId, devices[0].uniqueId, sizeof(selectedDeviceUniqueId) - 1);
				selectedDeviceUniqueId[sizeof(selectedDeviceUniqueId) - 1] = '\0';
			}
		}

		if (!cameraCapture->IsCapturing() && selectedDeviceIndex >= 0)
		{
			StartCamera();
		}
	}
	else
	{
		StopCamera();
	}
}

bool CViewCamera::UpdateImageData()
{
	if (!cameraCapture->IsCapturing())
	{
		if (cameraWasCapturing)
		{
			cameraStatus = CAMERA_DISCONNECTED;
			cameraWasCapturing = false;
		}
		return false;
	}

	if (!cameraCapture->IsNewFrameReady())
		return false;

	int w = cameraCapture->GetFrameWidth();
	int h = cameraCapture->GetFrameHeight();
	if (w != cameraFrameBufferWidth || h != cameraFrameBufferHeight)
	{
		AllocFrameBuffer(w, h);
		InitCameraImage(w, h);
	}

	if (!cameraCapture->GetFrameRGBA(cameraFrameBuffer, cameraFrameBufferWidth, cameraFrameBufferHeight))
		return false;

	int srcStride = cameraFrameBufferWidth * 4;
	int dstStride = rasterWidth * 4;
	u8 *dst = imageData->resultData;
	u8 *src = cameraFrameBuffer;

	for (int y = 0; y < cameraFrameBufferHeight; y++)
	{
		memcpy(dst + y * dstStride, src + y * srcStride, srcStride);
	}

	return true;
}

void CViewCamera::RenderImGui()
{
	if (image && cameraStatus == CAMERA_OK && cameraCapture->IsCapturing())
	{
		CGuiViewMovingPaneImage::RenderImGui();
		return;
	}

	PreRenderImGui();

	const char *statusText = "No camera";
	switch (cameraStatus)
	{
		case CAMERA_NO_DEVICES:   statusText = "No camera found"; break;
		case CAMERA_UNAVAILABLE:  statusText = "Camera unavailable"; break;
		case CAMERA_DISCONNECTED: statusText = "Camera disconnected"; break;
		case CAMERA_OK:           statusText = "Camera starting..."; break;
	}

	ImVec2 windowSize = ImGui::GetContentRegionAvail();
	ImVec2 textSize = ImGui::CalcTextSize(statusText);
	ImGui::SetCursorPos(ImVec2(
		(windowSize.x - textSize.x) * 0.5f,
		(windowSize.y - textSize.y) * 0.5f
	));
	ImGui::TextDisabled("%s", statusText);

	PostRenderImGui();
}

bool CViewCamera::HasContextMenuItems()
{
	return true;
}

void CViewCamera::RenderContextMenuItems()
{
	if (ImGui::MenuItem("Reset zoom and pan"))
	{
		ClearZoom();
	}
}

void CViewCamera::SerializeLayout(CByteBuffer *byteBuffer)
{
	CGuiViewMovingPaneImage::SerializeLayout(byteBuffer);

	byteBuffer->PutU16(1);
	byteBuffer->PutString(selectedDeviceUniqueId);
	byteBuffer->PutI32(selectedDeviceIndex);
}

bool CViewCamera::DeserializeLayout(CByteBuffer *byteBuffer, int version)
{
	if (!CGuiViewMovingPaneImage::DeserializeLayout(byteBuffer, version))
		return false;

	u16 cameraVersion = byteBuffer->GetU16();
	if (cameraVersion >= 1)
	{
		char *savedUniqueId = byteBuffer->GetString();
		int savedDeviceIndex = byteBuffer->GetI32();

		if (savedUniqueId)
		{
			strncpy(selectedDeviceUniqueId, savedUniqueId, sizeof(selectedDeviceUniqueId) - 1);
			selectedDeviceUniqueId[sizeof(selectedDeviceUniqueId) - 1] = '\0';

			RefreshDeviceList();
			selectedDeviceIndex = -1;
			for (int i = 0; i < (int)devices.size(); i++)
			{
				if (strcmp(devices[i].uniqueId, selectedDeviceUniqueId) == 0)
				{
					selectedDeviceIndex = i;
					break;
				}
			}

			if (selectedDeviceIndex < 0 && savedDeviceIndex >= 0 && savedDeviceIndex < (int)devices.size())
			{
				selectedDeviceIndex = savedDeviceIndex;
				strncpy(selectedDeviceUniqueId, devices[savedDeviceIndex].uniqueId, sizeof(selectedDeviceUniqueId) - 1);
				selectedDeviceUniqueId[sizeof(selectedDeviceUniqueId) - 1] = '\0';
			}

			STRFREE(savedUniqueId);

			if (visible && selectedDeviceIndex >= 0)
			{
				StartCamera();
			}
		}
	}

	return true;
}
