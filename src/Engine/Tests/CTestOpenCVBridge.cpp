#include "CTestOpenCVBridge.h"

#include "MT_OpenCV.h"
#include "MT_Capabilities.h"
#include "CImageData.h"
#include "DBG_Log.h"

#include <cstring>

#if MT_ENABLE_OPENCV
#include <opencv2/imgproc.hpp>
#include <opencv2/features.hpp>
#if MT_ENABLE_OPENCV_NONFREE
#include <opencv2/xfeatures2d.hpp>
#include <opencv2/xfeatures2d/nonfree.hpp>
#endif
#endif

CTestOpenCVBridge::CTestOpenCVBridge() {}
CTestOpenCVBridge::~CTestOpenCVBridge() {}
void CTestOpenCVBridge::Cancel() { isRunning = false; }

void CTestOpenCVBridge::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

#if MT_ENABLE_OPENCV

	if (!MT_OpenCV_IsAvailable())
	{
		TestCompleted(false, "MT_ENABLE_OPENCV is set but MT_OpenCV_IsAvailable() said no");
		return;
	}
	if (strncmp(MT_OpenCV_Version(), "5.", 2) != 0)
	{
		TestCompleted(false, "expected an OpenCV 5.x runtime");
		return;
	}
	StepCompleted(1, true, "the bridge reports OpenCV 5 available");

	// ---- Step 2: an RGBA round trip, every pixel distinct ----
	//
	// Distinct values matter: a transposed, sheared or channel-swapped copy of
	// a uniform image is indistinguishable from a correct one.
	CImageData *rgba = new CImageData(7, 5, IMG_TYPE_RGBA);
	rgba->AllocImage(false, true);
	for (int y = 0; y < 5; y++)
	{
		for (int x = 0; x < 7; x++)
		{
			u8 *p = rgba->resultData + ((size_t)y * 7 + (size_t)x) * 4;
			p[0] = (u8)(x * 10);
			p[1] = (u8)(y * 20);
			p[2] = (u8)(x + y);
			p[3] = 255;
		}
	}

	cv::Mat m = MT_OpenCV_MatFromImageData(rgba);
	if (m.type() != CV_8UC4 || m.cols != 7 || m.rows != 5)
	{
		delete rgba;
		TestCompleted(false, "an RGBA CImageData did not become a 7x5 CV_8UC4 Mat");
		return;
	}
	cv::Vec4b corner = m.at<cv::Vec4b>(4, 6);
	if (corner[0] != 60 || corner[1] != 80 || corner[2] != 10 || corner[3] != 255)
	{
		delete rgba;
		TestCompleted(false, "the far-corner pixel did not survive the conversion");
		return;
	}

	CImageData *back = MT_OpenCV_ImageDataFromMat(m);
	if (back == NULL || back->width != 7 || back->height != 5
		|| memcmp(back->resultData, rgba->resultData, (size_t)7 * 5 * 4) != 0)
	{
		delete back;
		delete rgba;
		TestCompleted(false, "the RGBA round trip was not byte-identical");
		return;
	}
	delete back;
	delete rgba;
	StepCompleted(2, true, "RGBA round-trips byte-identically, corner pixel included");

	// ---- Step 3: a PADDED ROI -- the case the 2021 bridge got wrong ----
	//
	// A ROI's step is the PARENT's row length, so cols * elemSize is not the
	// stride. Copying the whole buffer in one memcpy shears the image by
	// exactly that difference, and on a uniform test image nothing shows.
	{
		cv::Mat big(9, 9, CV_8UC4, cv::Scalar(1, 2, 3, 4));
		cv::Mat roi = big(cv::Rect(1, 1, 5, 3));
		roi.at<cv::Vec4b>(2, 4) = cv::Vec4b(9, 8, 7, 6);

		if (roi.isContinuous())
		{
			TestCompleted(false, "the ROI came out continuous, so this test proves nothing");
			return;
		}

		CImageData *fromRoi = MT_OpenCV_ImageDataFromMat(roi);
		if (fromRoi == NULL || fromRoi->width != 5 || fromRoi->height != 3)
		{
			delete fromRoi;
			TestCompleted(false, "a discontinuous ROI did not convert to a 5x3 image");
			return;
		}
		const u8 *last = fromRoi->resultData + ((size_t)2 * 5 + 4) * 4;
		bool ok = (last[0] == 9 && last[1] == 8 && last[2] == 7 && last[3] == 6);
		delete fromRoi;
		if (!ok)
		{
			TestCompleted(false, "the ROI's last pixel landed in the wrong place (stride bug)");
			return;
		}
	}
	StepCompleted(3, true, "a discontinuous ROI converts without shearing");

	// ---- Step 4: the depth-carrying types keep their depth ----
	{
		CImageData *g16 = new CImageData(4, 4, IMG_TYPE_GRAYSCALE_16BIT);
		g16->AllocImage(false, true);
		((unsigned short *)g16->resultData)[15] = 65535;
		cv::Mat m16 = MT_OpenCV_MatFromImageData(g16);
		bool ok = (m16.type() == CV_16UC1 && m16.at<unsigned short>(3, 3) == 65535);
		delete g16;
		if (!ok)
		{
			TestCompleted(false, "16-bit grayscale lost its depth crossing the bridge");
			return;
		}

		CImageData *h = new CImageData(2, 2, IMG_TYPE_RGBA_16F);
		h->AllocImage(false, true);
		cv::Mat mh = MT_OpenCV_MatFromImageData(h);
		ok = (mh.type() == CV_16FC4 && mh.elemSize() == 8);
		delete h;
		if (!ok)
		{
			TestCompleted(false, "IMG_TYPE_RGBA_16F did not become CV_16FC4");
			return;
		}

		// An unsupported type must be refused, not silently mangled.
		CImageData *lab = new CImageData(2, 2, IMG_TYPE_CIELAB);
		lab->AllocImage(false, true);
		cv::Mat mlab = MT_OpenCV_MatFromImageData(lab);
		ok = mlab.empty();
		delete lab;
		if (!ok)
		{
			TestCompleted(false, "an unsupported image type produced a Mat instead of an error");
			return;
		}
	}
	StepCompleted(4, true, "16-bit, half-float and unsupported types all behave");

	// ---- Step 5: the blit writes a corner and nothing else ----
	//
	// This is the contract CGuiViewMovingPaneImage needs: its raster is
	// power-of-two, so the result is always smaller than the buffer.
	{
		CImageData *raster = new CImageData(8, 8, IMG_TYPE_RGBA);
		raster->AllocImage(false, true);
		memset(raster->resultData, 0xAA, (size_t)8 * 8 * 4);

		// NOT `small`: the Windows SDK's rpcndr.h does `#define small char`, so
		// that name turns into `cv::Mat char(...)` and only fails on Windows.
		cv::Mat smallMat(5, 7, CV_8UC4, cv::Scalar(1, 2, 3, 4));
		bool ok = MT_OpenCV_BlitMatToImageData(smallMat, raster);
		ok = ok && raster->resultData[0] == 1 && raster->resultData[3] == 4;
		// the row padding past column 6, and every row below row 4
		ok = ok && raster->resultData[7 * 4] == 0xAA;
		ok = ok && raster->resultData[(size_t)5 * 8 * 4] == 0xAA;

		cv::Mat tooBig(9, 9, CV_8UC4, cv::Scalar(0));
		bool refused = !MT_OpenCV_BlitMatToImageData(tooBig, raster);

		delete raster;
		if (!ok)
		{
			TestCompleted(false, "the blit did not write exactly the top-left corner");
			return;
		}
		if (!refused)
		{
			TestCompleted(false, "the blit accepted a source larger than the destination");
			return;
		}
	}
	StepCompleted(5, true, "the blit fills the corner, leaves the padding, refuses an oversized source");

	// ---- Step 6: one call into each linked module ----
	//
	// So a missing or half-merged archive fails HERE, in the engine's own
	// suite, rather than in whichever app happens to call a matcher first.
	{
		cv::Mat scene(240, 320, CV_8UC4, cv::Scalar(235, 235, 235, 255));
		for (int i = 0; i < 8; i++)
		{
			cv::circle(scene, cv::Point(30 + i * 35, 120), 10 + i,
					   cv::Scalar(25, 25, 25, 255), cv::FILLED, cv::LINE_AA);
		}

		cv::Mat gray;
		cv::cvtColor(scene, gray, cv::COLOR_RGBA2GRAY);          // imgproc
		if (gray.type() != CV_8UC1)
		{
			TestCompleted(false, "cvtColor did not yield CV_8UC1");
			return;
		}

		std::vector<cv::KeyPoint> kp;
		cv::ORB::create(500)->detect(gray, kp);                  // features
		if (kp.empty())
		{
			TestCompleted(false, "ORB found no key points in the test scene");
			return;
		}

#if MT_ENABLE_OPENCV_NONFREE
		std::vector<cv::KeyPoint> surfKp;
		cv::xfeatures2d::SURF::create(300)->detect(gray, surfKp); // xfeatures2d
		if (surfKp.empty())
		{
			TestCompleted(false, "SURF found no key points in the test scene");
			return;
		}
		StepCompleted(6, true, "imgproc, features and xfeatures2d all answer");
#else
		if (strstr(MT_GetCapabilityManifest(), "MT_CAP_OPENCV_NONFREE=0") == NULL)
		{
			TestCompleted(false, "the non-free capability is compiled out but the manifest does not say so");
			return;
		}
		StepCompleted(6, true, "imgproc and features answer; xfeatures2d is absent and the manifest agrees");
#endif
	}

	TestCompleted(true, "the OpenCV bridge behaves in an OpenCV-enabled build");

#else

	// THE OFF BRANCH HAS ITS OWN ASSERTIONS, rather than an absence of them.
	if (MT_OpenCV_IsAvailable())
	{
		TestCompleted(false, "MT_ENABLE_OPENCV is 0 but MT_OpenCV_IsAvailable() said yes");
		return;
	}
	if (MT_OpenCV_Version()[0] != '\0')
	{
		TestCompleted(false, "MT_OpenCV_Version() returned a version in a build without OpenCV");
		return;
	}
	StepCompleted(1, true, "the bridge reports OpenCV unavailable");

	if (strstr(MT_GetCapabilityManifest(), "MT_CAP_OPENCV=0") == NULL)
	{
		TestCompleted(false, "the resolved manifest does not record MT_CAP_OPENCV=0");
		return;
	}
	StepCompleted(2, true, "the resolved manifest records MT_CAP_OPENCV=0");

	TestCompleted(true, "the OpenCV bridge reports absent in a build without OpenCV");

#endif
}
