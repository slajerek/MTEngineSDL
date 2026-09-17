#include "MT_YoloDetector.h"
#include "DBG_Log.h"

#if MT_ENABLE_YOLO

#include <opencv2/imgproc.hpp>
#include <opencv2/dnn.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>

bool MT_Yolo_IsAvailable() { return true; }

// The 80 COCO classes, in the order every model in the shipped catalogue was
// trained on. Kept here rather than in a data file because a model whose class
// order differs is a DIFFERENT model, not a configuration of this one.
static const char *kCocoClasses[] = {
	"person", "bicycle", "car", "motorcycle", "airplane", "bus", "train", "truck",
	"boat", "traffic light", "fire hydrant", "stop sign", "parking meter", "bench",
	"bird", "cat", "dog", "horse", "sheep", "cow", "elephant", "bear", "zebra",
	"giraffe", "backpack", "umbrella", "handbag", "tie", "suitcase", "frisbee",
	"skis", "snowboard", "sports ball", "kite", "baseball bat", "baseball glove",
	"skateboard", "surfboard", "tennis racket", "bottle", "wine glass", "cup",
	"fork", "knife", "spoon", "bowl", "banana", "apple", "sandwich", "orange",
	"broccoli", "carrot", "hot dog", "pizza", "donut", "cake", "chair", "couch",
	"potted plant", "bed", "dining table", "toilet", "tv", "laptop", "mouse",
	"remote", "keyboard", "cell phone", "microwave", "oven", "toaster", "sink",
	"refrigerator", "book", "clock", "vase", "scissors", "teddy bear",
	"hair drier", "toothbrush"
};
static const int kCocoClassCount = (int)(sizeof(kCocoClasses) / sizeof(kCocoClasses[0]));

struct MT_YoloDetector::Impl
{
	cv::dnn::Net net;
	bool loaded = false;
	int inputSize = 640;
	int numClasses = 0;
	float lastMs = 0.0f;
	std::string lastError;
};

MT_YoloDetector::MT_YoloDetector() : impl(new Impl()) {}
MT_YoloDetector::~MT_YoloDetector() { delete impl; }

bool MT_YoloDetector::IsLoaded() const { return impl->loaded; }
const char *MT_YoloDetector::GetLastError() const { return impl->lastError.c_str(); }
float MT_YoloDetector::GetLastInferenceMs() const { return impl->lastMs; }
int MT_YoloDetector::ClassCount() { return kCocoClassCount; }

const char *MT_YoloDetector::ClassName(int classId)
{
	if (classId < 0 || classId >= kCocoClassCount)
		return "?";
	return kCocoClasses[classId];
}

bool MT_YoloDetector::LoadModel(const char *onnxPath, int inputSize)
{
	impl->loaded = false;
	impl->lastError.clear();

	if (onnxPath == NULL || onnxPath[0] == '\0')
	{
		impl->lastError = "no model path";
		return false;
	}
	if (inputSize <= 0 || (inputSize % 32) != 0)
	{
		impl->lastError = "input size must be a positive multiple of 32";
		return false;
	}

	try
	{
		impl->net = cv::dnn::readNetFromONNX(onnxPath);
	}
	catch (const cv::Exception &e)
	{
		impl->lastError = std::string("OpenCV could not read the model: ") + e.what();
		LOGError("MT_YoloDetector::LoadModel: %s", impl->lastError.c_str());
		return false;
	}

	if (impl->net.empty())
	{
		impl->lastError = "the model loaded as an empty network";
		return false;
	}

	// PROBE THE OUTPUT SHAPE NOW, not at the first detection. A model whose
	// head this decoder does not understand must fail HERE, where the host can
	// say so, rather than silently returning nonsense boxes later.
	try
	{
		cv::Mat probe(inputSize, inputSize, CV_8UC3, cv::Scalar(0, 0, 0));
		cv::Mat blob = cv::dnn::blobFromImage(probe, 1.0, cv::Size(inputSize, inputSize),
											  cv::Scalar(), true, false);
		impl->net.setInput(blob);
		cv::Mat out = impl->net.forward();

		// Expected: 1 x N x (5 + numClasses), with N the anchor count of a
		// three-level pyramid at strides 8/16/32 -- 80^2 + 40^2 + 20^2 = 8400
		// at a 640 input.
		//
		// THE ANCHOR COUNT IS CHECKED, not just the rank, and that check is the
		// whole value of this probe. A yolov5s.onnx is 1 x 25200 x 85: it passes
		// every test that looks only at dims and size[2], reports a plausible 80
		// classes, and then decodes into nonsense, because its boxes are already
		// absolute pixels and exp() of a pixel width overflows to inf. A
		// yolov8n.onnx is 1 x 84 x 8400 -- transposed -- and would report 8395
		// classes. Both are one Browse click away, so both must fail HERE with a
		// message rather than in Detect with garbage.
		const int expectedAnchors = (inputSize / 8) * (inputSize / 8)
								  + (inputSize / 16) * (inputSize / 16)
								  + (inputSize / 32) * (inputSize / 32);
		
		if (out.dims != 3 || out.size[0] != 1 || out.size[2] < 6)
		{
			char buf[160];
			snprintf(buf, sizeof(buf),
					 "unsupported output shape (dims=%d), this decoder handles 1 x N x (5 + classes)",
					 out.dims);
			impl->lastError = buf;
			LOGError("MT_YoloDetector::LoadModel: %s", buf);
			return false;
		}
		if (out.size[1] != expectedAnchors)
		{
			char buf[256];
			snprintf(buf, sizeof(buf),
					 "this model has %d anchors; a YOLOX head at %d has %d (strides 8/16/32). "
					 "This decoder handles a raw YOLOX head only -- a YOLOv5 or YOLOv8 export "
					 "needs a different decoder, not a different threshold.",
					 out.size[1], inputSize, expectedAnchors);
			impl->lastError = buf;
			LOGError("MT_YoloDetector::LoadModel: %s", buf);
			return false;
		}
		if (out.type() != CV_32F)
		{
			// Detect() reads out.data as float. An int8 or half blob read that
			// way is not a wrong answer, it is a different number entirely.
			char buf[160];
			snprintf(buf, sizeof(buf),
					 "the model returned a type-%d blob; this decoder reads CV_32F",
					 out.type());
			impl->lastError = buf;
			LOGError("MT_YoloDetector::LoadModel: %s", buf);
			return false;
		}
		impl->numClasses = out.size[2] - 5;
	}
	catch (const cv::Exception &e)
	{
		impl->lastError = std::string("the model failed a probe inference: ") + e.what();
		LOGError("MT_YoloDetector::LoadModel: %s", impl->lastError.c_str());
		return false;
	}

	impl->inputSize = inputSize;
	impl->loaded = true;
	LOGD("MT_YoloDetector: loaded %s (%dx%d, %d classes)",
		 onnxPath, inputSize, inputSize, impl->numClasses);
	return true;
}

std::vector<MT_YoloDetection> MT_YoloDetector::Detect(const cv::Mat &rgba,
													  float confThreshold,
													  float nmsThreshold)
{
	std::vector<MT_YoloDetection> result;

	if (!impl->loaded)
	{
		LOGError("MT_YoloDetector::Detect: no model loaded");
		return result;
	}
	if (rgba.empty() || rgba.type() != CV_8UC4)
	{
		LOGError("MT_YoloDetector::Detect: expected a non-empty CV_8UC4 image");
		return result;
	}

	const int side = impl->inputSize;

	// ---- letterbox, so a non-square frame is not distorted ----
	const float scale = std::min((float)side / (float)rgba.cols,
								 (float)side / (float)rgba.rows);
	
	// CLAMPED TO 1, not merely rounded. A 4000x3 image scales to a height of
	// round(0.48) = 0, and cv::resize THROWS on a zero-sized destination -- from
	// a worker thread, where an escaping exception is std::terminate rather than
	// a failed detection.
	const int scaledW = std::max(1, (int)std::round(rgba.cols * scale));
	const int scaledH = std::max(1, (int)std::round(rgba.rows * scale));
	const int padX = (side - scaledW) / 2;
	const int padY = (side - scaledH) / 2;
	
	cv::Mat out;
	try
	{
		// THE PRE-PROCESSING IS INSIDE THE TRY, not only the forward pass.
		// cvtColor, resize and copyTo all throw cv::Exception, and this runs on
		// a worker whose caller has no handler.
		cv::Mat rgb;
		cv::cvtColor(rgba, rgb, cv::COLOR_RGBA2RGB);
		cv::Mat resized;
		cv::resize(rgb, resized, cv::Size(scaledW, scaledH));
		cv::Mat letterboxed(side, side, CV_8UC3, cv::Scalar(114, 114, 114));
		resized.copyTo(letterboxed(cv::Rect(padX, padY, scaledW, scaledH)));
	
		// swapRB=true, and the DOUBLE SWAP IS DELIBERATE. The engine hands us
		// RGBA, the cvtColor above makes it RGB, and this flips it to BGR --
		// which is what the catalogue's YOLOX export expects, because OpenCV's
		// own reference pipeline feeds it a cv::imread result and imread returns
		// BGR. Written down because NO TEST CAN CATCH THIS BEING WRONG: the
		// decode test runs at a zero threshold precisely so it does not depend
		// on the model's opinion, and a swapped channel order degrades every
		// real detection silently rather than failing.
		cv::Mat blob = cv::dnn::blobFromImage(letterboxed, 1.0, cv::Size(side, side),
											  cv::Scalar(), true, false);
		impl->net.setInput(blob);
	
		auto t0 = std::chrono::steady_clock::now();
		out = impl->net.forward();
		auto t1 = std::chrono::steady_clock::now();
		impl->lastMs = (float)std::chrono::duration<double, std::milli>(t1 - t0).count();
	}
	catch (const cv::Exception &e)
	{
		LOGError("MT_YoloDetector::Detect: %s", e.what());
		return result;
	}
	
	if (out.dims != 3 || out.type() != CV_32F || out.size[1] <= 0)
	{
		LOGError("MT_YoloDetector::Detect: the forward pass returned an unexpected blob");
		return result;
	}

	const int numAnchors = out.size[1];
	const int stride = out.size[2];
	const float *data = (const float *)out.data;

	// ---- THE GRID AND STRIDE DECODE ----
	//
	// This head is RAW, and forgetting that is the single easiest way to ship a
	// detector that looks like it works. The evidence, from the model itself:
	// its graph has 80 Sigmoid nodes and NO Exp node, and the anchor count is
	// 80^2 + 40^2 + 20^2 -- a three-level feature pyramid at strides 8, 16 and
	// 32. So objectness and class scores arrive already activated, and the box
	// values do not: they are a per-cell offset and a log-space size.
	//
	//   cx = (raw_x + gx) * s      cy = (raw_y + gy) * s
	//   w  = exp(raw_w) * s        h  = exp(raw_h) * s
	//
	// Without it every box lands near the top-left corner at roughly one pixel
	// across, which reads as "the model found nothing" rather than as a bug.
	std::vector<cv::Rect> nmsBoxes;      // NMSBoxes has no Rect2f overload
	std::vector<float> nmsScores;
	std::vector<int> nmsClasses;         // parallel; NMS is per class, see below
	std::vector<MT_YoloDetection> candidates;

	const int levels[3] = { side / 8, side / 16, side / 32 };
	const int strides[3] = { 8, 16, 32 };

	int anchor = 0;
	for (int level = 0; level < 3 && anchor < numAnchors; level++)
	{
		const int grid = levels[level];
		const int s = strides[level];

		for (int gy = 0; gy < grid && anchor < numAnchors; gy++)
		{
			for (int gx = 0; gx < grid && anchor < numAnchors; gx++, anchor++)
			{
				const float *row = data + (size_t)anchor * stride;
				const float objectness = row[4];
				if (objectness < confThreshold)
					continue;

				int bestClass = -1;
				float bestScore = 0.0f;
				for (int c = 0; c < impl->numClasses; c++)
				{
					const float sc = row[5 + c];
					if (sc > bestScore)
					{
						bestScore = sc;
						bestClass = c;
					}
				}

				const float confidence = objectness * bestScore;
				if (confidence < confThreshold || bestClass < 0)
					continue;

				const float cx = (row[0] + (float)gx) * (float)s;
				const float cy = (row[1] + (float)gy) * (float)s;
				const float w = std::exp(row[2]) * (float)s;
				const float h = std::exp(row[3]) * (float)s;

				// Undo the letterbox: subtract the pad, divide by the scale.
				const float x = (cx - w * 0.5f - (float)padX) / scale;
				const float y = (cy - h * 0.5f - (float)padY) / scale;
				const float bw = w / scale;
				const float bh = h / scale;

				MT_YoloDetection d;
				d.box = cv::Rect2f(x, y, bw, bh);
				d.classId = bestClass;
				d.confidence = confidence;
				candidates.push_back(d);

				nmsBoxes.push_back(cv::Rect((int)std::round(x), (int)std::round(y),
											(int)std::round(bw), (int)std::round(bh)));
				nmsScores.push_back(confidence);
				nmsClasses.push_back(bestClass);
			}
		}
	}

	// ---- NMS, PER CLASS ----
	//
	// Class-agnostic NMS is the easy mistake here and it loses real detections:
	// a person on a bicycle overlaps at well over the 0.45 default, so one of
	// the two boxes is suppressed by the other even though they are different
	// objects. Every YOLOX reference implementation batches by class, and
	// NMSBoxesBatched is OpenCV's name for exactly that.
	std::vector<int> keep;
	cv::dnn::NMSBoxesBatched(nmsBoxes, nmsScores, nmsClasses,
							 confThreshold, nmsThreshold, keep);
	result.reserve(keep.size());
	for (size_t i = 0; i < keep.size(); i++)
		result.push_back(candidates[keep[i]]);

	return result;
}

#else

bool MT_Yolo_IsAvailable() { return false; }

#endif // MT_ENABLE_YOLO
