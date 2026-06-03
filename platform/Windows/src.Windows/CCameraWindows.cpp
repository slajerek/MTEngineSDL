#include "CCameraWindows.h"
#include "DBG_Log.h"
#include <cstring>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <mftransform.h>
#include <wmcodecdsp.h>

#pragma comment(lib, "mf.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mfreadwrite.lib")

CCameraWindows::CCameraWindows()
: sourceReader(NULL)
, captureWidth(0)
, captureHeight(0)
, internalBuffer(NULL)
, newFrame(false)
, capturing(false)
, threadRunning(false)
{
	HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
	if (FAILED(hr))
	{
		LOGD("CCameraWindows: MFStartup failed, hr=0x%08X", (unsigned)hr);
	}
}

CCameraWindows::~CCameraWindows()
{
	StopCapture();
	if (internalBuffer)
	{
		delete[] internalBuffer;
		internalBuffer = NULL;
	}
	MFShutdown();
}

std::vector<CCameraDevice> CCameraWindows::EnumerateDevices()
{
	std::vector<CCameraDevice> result;

	IMFAttributes *pAttribs = NULL;
	HRESULT hr = MFCreateAttributes(&pAttribs, 1);
	if (FAILED(hr))
	{
		LOGD("CCameraWindows: MFCreateAttributes failed, hr=0x%08X", (unsigned)hr);
		return result;
	}

	hr = pAttribs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
	                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if (FAILED(hr))
	{
		LOGD("CCameraWindows: SetGUID failed, hr=0x%08X", (unsigned)hr);
		pAttribs->Release();
		return result;
	}

	IMFActivate **ppDevices = NULL;
	UINT32 count = 0;
	hr = MFEnumDeviceSources(pAttribs, &ppDevices, &count);
	pAttribs->Release();

	if (FAILED(hr))
	{
		LOGD("CCameraWindows: MFEnumDeviceSources failed, hr=0x%08X", (unsigned)hr);
		return result;
	}

	for (UINT32 i = 0; i < count; i++)
	{
		CCameraDevice d;
		d.index = (int)i;

		// Friendly name
		WCHAR *szFriendlyName = NULL;
		UINT32 cchName = 0;
		hr = ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME,
		                                      &szFriendlyName, &cchName);
		if (SUCCEEDED(hr) && szFriendlyName)
		{
			WideCharToMultiByte(CP_UTF8, 0, szFriendlyName, -1,
			                    d.name, sizeof(d.name) - 1, NULL, NULL);
			d.name[sizeof(d.name) - 1] = '\0';
			CoTaskMemFree(szFriendlyName);
		}
		else
		{
			snprintf(d.name, sizeof(d.name), "Camera %u", i);
		}

		// Symbolic link as uniqueId
		WCHAR *szSymLink = NULL;
		UINT32 cchLink = 0;
		hr = ppDevices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
		                                      &szSymLink, &cchLink);
		if (SUCCEEDED(hr) && szSymLink)
		{
			WideCharToMultiByte(CP_UTF8, 0, szSymLink, -1,
			                    d.uniqueId, sizeof(d.uniqueId) - 1, NULL, NULL);
			d.uniqueId[sizeof(d.uniqueId) - 1] = '\0';
			CoTaskMemFree(szSymLink);
		}
		else
		{
			snprintf(d.uniqueId, sizeof(d.uniqueId), "vidcap:%u", i);
		}

		result.push_back(d);
		ppDevices[i]->Release();
	}

	CoTaskMemFree(ppDevices);
	return result;
}

bool CCameraWindows::StartCapture(int deviceIndex, int requestedWidth, int requestedHeight)
{
	StopCapture();

	// Enumerate devices to find the one at deviceIndex
	IMFAttributes *pAttribs = NULL;
	HRESULT hr = MFCreateAttributes(&pAttribs, 1);
	if (FAILED(hr))
	{
		LOGD("CCameraWindows: StartCapture MFCreateAttributes failed, hr=0x%08X", (unsigned)hr);
		return false;
	}

	hr = pAttribs->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE,
	                       MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
	if (FAILED(hr))
	{
		pAttribs->Release();
		return false;
	}

	IMFActivate **ppDevices = NULL;
	UINT32 count = 0;
	hr = MFEnumDeviceSources(pAttribs, &ppDevices, &count);
	pAttribs->Release();

	if (FAILED(hr) || (UINT32)deviceIndex >= count)
	{
		LOGD("CCameraWindows: device index %d not found (count=%u)", deviceIndex, count);
		if (ppDevices)
			CoTaskMemFree(ppDevices);
		return false;
	}

	// Activate source
	IMFMediaSource *pSource = NULL;
	hr = ppDevices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&pSource));

	for (UINT32 i = 0; i < count; i++)
		ppDevices[i]->Release();
	CoTaskMemFree(ppDevices);

	if (FAILED(hr) || !pSource)
	{
		LOGD("CCameraWindows: ActivateObject failed, hr=0x%08X", (unsigned)hr);
		return false;
	}

	// Create source reader
	IMFAttributes *pReaderAttribs = NULL;
	hr = MFCreateAttributes(&pReaderAttribs, 1);
	if (FAILED(hr))
	{
		pSource->Release();
		return false;
	}

	IMFSourceReader *pReader = NULL;
	hr = MFCreateSourceReaderFromMediaSource(pSource, pReaderAttribs, &pReader);
	pReaderAttribs->Release();
	pSource->Release();

	if (FAILED(hr) || !pReader)
	{
		LOGD("CCameraWindows: MFCreateSourceReaderFromMediaSource failed, hr=0x%08X", (unsigned)hr);
		return false;
	}

	// Request RGB32 output
	IMFMediaType *pType = NULL;
	hr = MFCreateMediaType(&pType);
	if (SUCCEEDED(hr))
	{
		pType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
		pType->SetGUID(MF_MT_SUBTYPE,    MFVideoFormat_RGB32);
		MFSetAttributeSize(pType, MF_MT_FRAME_SIZE,
		                   (UINT32)requestedWidth, (UINT32)requestedHeight);
		hr = pReader->SetCurrentMediaType(
		        (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, NULL, pType);
		pType->Release();
	}

	if (FAILED(hr))
	{
		LOGD("CCameraWindows: SetCurrentMediaType RGB32 failed (hr=0x%08X), using default", (unsigned)hr);
	}

	// Read back actual negotiated size
	IMFMediaType *pActualType = NULL;
	hr = pReader->GetCurrentMediaType((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, &pActualType);
	if (SUCCEEDED(hr) && pActualType)
	{
		UINT32 w = 0, h = 0;
		MFGetAttributeSize(pActualType, MF_MT_FRAME_SIZE, &w, &h);
		if (w > 0 && h > 0)
		{
			captureWidth  = (int)w;
			captureHeight = (int)h;
		}
		else
		{
			captureWidth  = requestedWidth;
			captureHeight = requestedHeight;
		}
		pActualType->Release();
	}
	else
	{
		captureWidth  = requestedWidth;
		captureHeight = requestedHeight;
	}

	// Select the first video stream
	pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_ALL_STREAMS, FALSE);
	pReader->SetStreamSelection((DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

	sourceReader = pReader;

	// Allocate internal RGBA buffer
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		if (internalBuffer)
			delete[] internalBuffer;
		internalBuffer = new u8[captureWidth * captureHeight * 4];
	}

	newFrame      = false;
	capturing     = true;
	threadRunning = true;
	captureThread = std::thread(&CCameraWindows::CaptureThreadFunc, this);

	LOGD("CCameraWindows: started capture on device %d at %dx%d", deviceIndex, captureWidth, captureHeight);
	return true;
}

void CCameraWindows::StopCapture()
{
	if (!capturing.load())
		return;

	capturing     = false;
	threadRunning = false;

	if (captureThread.joinable())
		captureThread.join();

	if (sourceReader)
	{
		sourceReader->Release();
		sourceReader = NULL;
	}

	newFrame = false;
}

bool CCameraWindows::IsCapturing()
{
	return capturing.load();
}

bool CCameraWindows::IsNewFrameReady()
{
	return newFrame.load();
}

bool CCameraWindows::GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (!internalBuffer || captureWidth != bufferWidth || captureHeight != bufferHeight)
		return false;

	memcpy(destBuffer, internalBuffer, bufferWidth * bufferHeight * 4);
	newFrame = false;
	return true;
}

int CCameraWindows::GetFrameWidth()
{
	return captureWidth;
}

int CCameraWindows::GetFrameHeight()
{
	return captureHeight;
}

void CCameraWindows::CaptureThreadFunc()
{
	while (threadRunning.load())
	{
		if (!sourceReader)
			break;

		IMFSample *pSample = NULL;
		DWORD streamIndex = 0;
		DWORD flags = 0;
		LONGLONG llTimeStamp = 0;

		HRESULT hr = sourceReader->ReadSample(
		    (DWORD)MF_SOURCE_READER_FIRST_VIDEO_STREAM,
		    0,
		    &streamIndex,
		    &flags,
		    &llTimeStamp,
		    &pSample);

		if (FAILED(hr))
		{
			LOGD("CCameraWindows: ReadSample failed, hr=0x%08X", (unsigned)hr);
			break;
		}

		if (flags & MF_SOURCE_READERF_ENDOFSTREAM)
		{
			LOGD("CCameraWindows: end of stream");
			break;
		}

		if (!pSample)
			continue;

		// Lock the media buffer and get raw pixel data
		IMFMediaBuffer *pMediaBuffer = NULL;
		hr = pSample->ConvertToContiguousBuffer(&pMediaBuffer);
		if (SUCCEEDED(hr) && pMediaBuffer)
		{
			BYTE *pData = NULL;
			DWORD cbCurrentLen = 0;
			hr = pMediaBuffer->Lock(&pData, NULL, &cbCurrentLen);
			if (SUCCEEDED(hr) && pData)
			{
				int w = captureWidth;
				int h = captureHeight;
				int expectedBytes = w * h * 4;

				if ((int)cbCurrentLen >= expectedBytes)
				{
					std::lock_guard<std::mutex> lock(frameMutex);
					if (internalBuffer)
					{
						// MF RGB32 is bottom-up BGRA.
						// Convert BGR->RGBA and flip vertically.
						for (int y = 0; y < h; y++)
						{
							// Source row: bottom-up, so row 0 in src = bottom row
							const BYTE *srcRow = pData + (h - 1 - y) * w * 4;
							u8         *dstRow = internalBuffer + y * w * 4;
							for (int x = 0; x < w; x++)
							{
								int si = x * 4;
								int di = x * 4;
								dstRow[di + 0] = srcRow[si + 2]; // R  (src byte 2 = R in BGRA)
								dstRow[di + 1] = srcRow[si + 1]; // G
								dstRow[di + 2] = srcRow[si + 0]; // B  (src byte 0 = B in BGRA)
								dstRow[di + 3] = 255;            // A
							}
						}
						newFrame = true;
					}
				}

				pMediaBuffer->Unlock();
			}
			pMediaBuffer->Release();
		}

		pSample->Release();
	}
}

CCameraCapture *SYS_CreatePlatformCameraCapture()
{
	return new CCameraWindows();
}
