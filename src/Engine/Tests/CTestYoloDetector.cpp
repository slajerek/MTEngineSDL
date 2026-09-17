#include "CTestYoloDetector.h"

#include "MT_Capabilities.h"
#include "MT_YoloDetector.h"
#include "DBG_Log.h"

#include <cstdlib>
#include <cstring>
#include <string>

#if MT_ENABLE_YOLO
#include "CYoloModelManager.h"
#include <opencv2/imgproc.hpp>
#include <filesystem>
#endif

CTestYoloDetector::CTestYoloDetector() {}
CTestYoloDetector::~CTestYoloDetector() {}
void CTestYoloDetector::Cancel() { isRunning = false; }

#if MT_ENABLE_YOLO
// Where the test model comes from. It CANNOT be committed: it is 34 MB and the
// capability's whole premise is that no weights ship. So an environment
// variable wins, and otherwise the shared dependency work root, where the
// spike that proved this capability left a copy.
static std::string YoloTestModelPath()
{
	const char *fromEnv = getenv("MT_YOLO_TEST_MODEL");
	if (fromEnv != NULL && fromEnv[0] != '\0')
		return std::string(fromEnv);

	const char *home = getenv("HOME");
	if (home == NULL || home[0] == '\0')
		return std::string();
	return std::string(home) + "/.cache/mtengine/_deps/work/opencv/models/yolox.onnx";
}
#endif

void CTestYoloDetector::Run(ITestCallback *callback)
{
	this->callback = callback;
	isRunning = true;

#if MT_ENABLE_YOLO

	if (!MT_Yolo_IsAvailable())
	{
		TestCompleted(false, "MT_ENABLE_YOLO is set but MT_Yolo_IsAvailable() said no");
		return;
	}
	StepCompleted(1, true, "the detector reports itself available");

	// ---- Step 2: a missing model FAILS, and says why ----
	//
	// This is the one assertion that needs no model file, so it runs on every
	// machine including CI.
	{
		MT_YoloDetector detector;
		if (detector.LoadModel("/definitely/not/here/nothing.onnx", 640))
		{
			TestCompleted(false, "loading a nonexistent model reported success");
			return;
		}
		if (detector.IsLoaded() || strlen(detector.GetLastError()) == 0)
		{
			TestCompleted(false, "a failed load left no error message");
			return;
		}
		// And a nonsense input size is refused before anything is read.
		if (detector.LoadModel("/tmp/whatever.onnx", 33))
		{
			TestCompleted(false, "an input size that is not a multiple of 32 was accepted");
			return;
		}
	}
	StepCompleted(2, true, "a missing model and a bad input size both fail with a message");

	// ---- Step 3: THE LICENCE POLICY ----
	//
	// A shipped catalogue row under AGPL-3.0 would put the obligation to
	// open-source the whole application on every host that copies this
	// template, at the moment a user clicked Download. That must fail here.
	{
		CYoloModelManager manager;
		const std::vector<MT_YoloModelDef> &models = manager.GetDefaultModels();
		if (models.empty())
		{
			TestCompleted(false, "the shipped catalogue is empty");
			return;
		}

		static const char *kPermitted[] = { "Apache-2.0", "MIT", "BSD-3-Clause", "BSD-2-Clause" };
		for (size_t i = 0; i < models.size(); i++)
		{
			const MT_YoloModelDef &m = models[i];
			if (m.id.empty() || m.displayName.empty() || m.licence.empty())
			{
				TestCompleted(false, "a catalogue row is missing its id, name or licence");
				return;
			}

			bool permitted = false;
			for (size_t k = 0; k < sizeof(kPermitted) / sizeof(kPermitted[0]); k++)
			{
				if (m.licence == kPermitted[k])
				{
					permitted = true;
					break;
				}
			}
			if (!permitted)
			{
				char buf[220];
				snprintf(buf, sizeof(buf),
						 "catalogue row '%s' is licensed '%s', which is not in the permissive set. "
						 "A model that cannot ship in a closed product belongs behind Browse, not here.",
						 m.id.c_str(), m.licence.c_str());
				TestCompleted(false, buf);
				return;
			}

			const std::string url = manager.GetDownloadUrl(m.id);
			if (url.rfind("https://huggingface.co/", 0) != 0)
			{
				TestCompleted(false, "a catalogue row's download URL is not an https HuggingFace URL");
				return;
			}
		}
	}
	StepCompleted(3, true, "every shipped model is permissively licensed and fetched over https");

	// ---- Step 4: the decode, against KNOWN coordinates ----
	//
	// Needs the model. Without it this reports SKIPPED and says what went
	// unverified, because a skip counted as a pass is the lie this suite's
	// TestSkipped exists to avoid.
	{
		const std::string modelPath = YoloTestModelPath();
		std::error_code ec;
		if (modelPath.empty() || !std::filesystem::exists(modelPath, ec) || ec)
		{
			TestSkipped("no ONNX model on this machine, so the box decode was never exercised. "
						"Set MT_YOLO_TEST_MODEL to a YOLOX .onnx to run it.");
			return;
		}

		MT_YoloDetector detector;
		if (!detector.LoadModel(modelPath.c_str(), 640))
		{
			char buf[256];
			snprintf(buf, sizeof(buf), "the model failed to load: %s", detector.GetLastError());
			TestCompleted(false, buf);
			return;
		}
		StepCompleted(4, true, "the model loads and its output shape is understood");

		// A scene with a few coarse shapes. What is asserted below does NOT
		// depend on the model recognising any of them -- see the note on the
		// threshold.
		const int W = 640, H = 480;
		cv::Mat rgba(H, W, CV_8UC4, cv::Scalar(220, 220, 220, 255));
		cv::rectangle(rgba, cv::Rect(200, 120, 150, 300), cv::Scalar(20, 20, 30, 255), cv::FILLED);
		cv::circle(rgba, cv::Point(275, 95), 45, cv::Scalar(20, 20, 30, 255), cv::FILLED);

		// THE THRESHOLD IS ZERO ON PURPOSE, and it is what makes this a test of
		// the DECODE rather than of the model's opinion.
		//
		// A COCO detector is entitled to find nothing in a synthetic scene, and
		// an earlier version of this step skipped whenever it did -- a test
		// that never runs. Raising the sensitivity does not help either: this
		// model's objectness is far below 0.01 across the whole grid here, so
		// even 0.01 returned almost nothing. At zero every one of the 8400
		// anchors becomes a candidate, which is exactly what is wanted, because
		// the geometry below separates a decoded head from an undecoded one
		// without asking the model to recognise anything:
		//
		//   decoded    -- centres spread across the full 640x480, boxes tens of
		//                 pixels across, because gx/gy and the stride put them
		//                 there.
		//   undecoded  -- every centre within a few pixels of the origin and
		//                 every box about one pixel across, because raw_x is a
		//                 sub-cell offset and raw_w is a logarithm.
		//
		// So this fails loudly if the grid/stride step is ever dropped, without
		// asking the model to recognise a drawing.
		std::vector<MT_YoloDetection> found = detector.Detect(rgba, 0.0f, 0.99f);

		if (found.size() < 64)
		{
			char buf[200];
			snprintf(buf, sizeof(buf),
					 "the head returned %d candidates at a zero threshold, where a "
					 "three-level 640 pyramid has 8400 anchors -- the forward pass, not "
					 "the decode, is wrong",
					 (int)found.size());
			TestCompleted(false, buf);
			return;
		}

		float maxCentreX = 0.0f, maxCentreY = 0.0f, maxSide = 0.0f;
		for (size_t i = 0; i < found.size(); i++)
		{
			const cv::Rect2f &b = found[i].box;
			const float cx = b.x + b.width * 0.5f;
			const float cy = b.y + b.height * 0.5f;
			if (cx > maxCentreX) maxCentreX = cx;
			if (cy > maxCentreY) maxCentreY = cy;
			if (b.width > maxSide) maxSide = b.width;
			if (b.height > maxSide) maxSide = b.height;
		}

		LOGD("CTestYoloDetector: %d candidates, max centre (%.0f,%.0f), max side %.0f",
			 (int)found.size(), maxCentreX, maxCentreY, maxSide);

		if (maxCentreX < W * 0.4f || maxCentreY < H * 0.4f)
		{
			char buf[220];
			snprintf(buf, sizeof(buf),
					 "every box centre sits in the top-left corner (max %.0f,%.0f of %dx%d), "
					 "which is what a missing grid/stride decode looks like",
					 maxCentreX, maxCentreY, W, H);
			TestCompleted(false, buf);
			return;
		}
		if (maxSide < 8.0f)
		{
			TestCompleted(false, "every box is about a pixel across, which is what a missing "
								 "exp() on the box size looks like");
			return;
		}

		StepCompleted(5, true, "boxes are spread across the frame at a plausible size, "
							   "so the grid and stride decode ran");
	}

	TestCompleted(true, "the detector loads, refuses what it should, and decodes boxes");

#else

	if (MT_Yolo_IsAvailable())
	{
		TestCompleted(false, "MT_ENABLE_YOLO is 0 but MT_Yolo_IsAvailable() said yes");
		return;
	}
	StepCompleted(1, true, "the detector reports itself unavailable");

	if (strstr(MT_GetCapabilityManifest(), "MT_CAP_YOLO=0") == NULL)
	{
		TestCompleted(false, "the resolved manifest does not record MT_CAP_YOLO=0");
		return;
	}
	StepCompleted(2, true, "the resolved manifest records MT_CAP_YOLO=0");

	TestCompleted(true, "object detection is absent because MT_CAP_YOLO=0");

#endif
}
