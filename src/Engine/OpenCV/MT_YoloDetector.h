#ifndef _MT_YoloDetector_h_
#define _MT_YoloDetector_h_

// Object detection through OpenCV 5's dnn module.
//
// The engine owns the DETECTOR and the model catalogue; a host owns its own
// views. That split is deliberate and is the one difference from the LLM
// capability, which keeps its loader view in the engine too: this is the first
// host to want detection, and an engine view is far harder to reshape later
// than an app one.
//
// NO WEIGHTS ARE SHIPPED, here or anywhere in the engine. CYoloModelManager
// holds URLs and the host downloads on demand, exactly as the LLM capability
// does with GGUF files.

#include "MT_Capabilities.h"

// Declared in BOTH branches so a host can ask without an #if.
bool MT_Yolo_IsAvailable();

#if MT_ENABLE_YOLO

#include <opencv2/core.hpp>
#include <string>
#include <vector>

struct MT_YoloDetection
{
	// In SOURCE image pixels, not in the model's letterboxed coordinates. The
	// detector undoes the letterbox so a caller never has to know the model's
	// input size.
	cv::Rect2f box;
	int classId = -1;
	float confidence = 0.0f;
};

// One ONNX detector, loaded once and run many times.
//
// NOT THREAD-SAFE, and that is not a caveat to be worked around: cv::dnn::Net
// carries mutable state across forward(), so one detector belongs to one
// thread. A host that detects on a worker owns its detector there.
class MT_YoloDetector
{
public:
	MT_YoloDetector();
	~MT_YoloDetector();

	// Loads an ONNX file. Returns false and fills GetLastError() when the file
	// is missing, when OpenCV cannot parse it, or when its output shape is not
	// one this decoder understands.
	//
	// REFUSING AN UNKNOWN SHAPE IS THE POINT. A decoder that guesses produces
	// boxes that look plausible and are wrong, which is far harder to notice
	// than a load failure.
	bool LoadModel(const char *onnxPath, int inputSize);
	bool IsLoaded() const;
	const char *GetLastError() const;

	// RGBA in (CV_8UC4), boxes in that image's pixel coordinates out.
	// Letterboxes to the model's square input so a non-square frame is not
	// distorted, and maps the boxes back.
	std::vector<MT_YoloDetection> Detect(const cv::Mat &rgba,
										 float confThreshold,
										 float nmsThreshold);

	float GetLastInferenceMs() const;

	// The 80 COCO class names, which is what every model in the shipped
	// catalogue is trained on. Returns "?" out of range.
	static const char *ClassName(int classId);
	static int ClassCount();

private:
	struct Impl;
	Impl *impl;

	MT_YoloDetector(const MT_YoloDetector &) = delete;
	MT_YoloDetector &operator=(const MT_YoloDetector &) = delete;
};

#endif // MT_ENABLE_YOLO
#endif
