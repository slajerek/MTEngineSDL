#ifndef _MT_OpenCV_h_
#define _MT_OpenCV_h_

// The engine's ONLY OpenCV seam: CImageData <-> cv::Mat.
//
// Two capabilities decide what exists here. MT_ENABLE_OPENCV compiles the body
// of MT_OpenCV.cpp and the cv::Mat declarations below; MT_ENABLE_OPENCV_NONFREE
// adds xfeatures2d to the archive but changes nothing in this header -- a host
// that wants SURF includes <opencv2/xfeatures2d/nonfree.hpp> itself.
//
// CHANNEL ORDER IS RGBA END TO END. An engine RGBA image becomes a CV_8UC4 Mat
// holding R, G, B, A in that order, never BGR, so a host passes the RGBA colour
// codes (cv::COLOR_RGBA2GRAY and friends). Nothing here swaps channels. OpenCV's
// BGR convention is an artefact of the file formats its imgcodecs module reads,
// and this engine does not link imgcodecs: every image arrives through
// CImageData, which is RGBA. A silent swap in the middle would be invisible in
// grayscale, obvious only in colour, and wrong everywhere.

#include "MT_Capabilities.h"

class CImageData;

// Declared in BOTH branches, so a host can ask without an #if.
bool MT_OpenCV_IsAvailable();
const char *MT_OpenCV_Version();   // CV_VERSION, or "" when unavailable

#if MT_ENABLE_OPENCV

#include <opencv2/core.hpp>

// A DEEP COPY, always -- the returned Mat owns its pixels.
//
// Supported: IMG_TYPE_RGBA -> CV_8UC4, IMG_TYPE_RGB -> CV_8UC3,
// IMG_TYPE_GRAYSCALE -> CV_8UC1, IMG_TYPE_GRAYSCALE_16BIT -> CV_16UC1,
// IMG_TYPE_RGBA_16BIT -> CV_16UC4, IMG_TYPE_RGBA_16F -> CV_16FC4 (the half
// bits verbatim: OpenCV's CV_16F is IEEE binary16, the same layout CImageData
// stores).
//
// IMG_TYPE_GRAYSCALE_32BIT, IMG_TYPE_CIELAB and IMG_TYPE_GPU_COMPRESSED are NOT
// supported and return an EMPTY Mat after LOGError -- the first two because no
// OpenCV depth matches them usefully, the third because it holds GPU block data
// rather than pixels.
cv::Mat MT_OpenCV_MatFromImageData(const CImageData *image);

// The reverse. CV_8UC3 widens to IMG_TYPE_RGBA with alpha 255; there is no
// three-channel engine type worth carrying. Anything else returns NULL after
// LOGError. Copies row by row against mat.step, so a ROI or a padded
// allocation converts correctly rather than shearing.
CImageData *MT_OpenCV_ImageDataFromMat(const cv::Mat &mat);

// The live-loop primitive, shaped for CGuiViewMovingPaneImage's storage.
//
// That pane keeps a POWER-OF-TWO RGBA raster and displays its top-left
// paneWidth x paneHeight corner, so a result is never the same size as the
// buffer it goes into. This writes rgba.rows rows of rgba.cols * 4 bytes into
// the top-left of dst and touches nothing else -- the same thing
// CViewCamera::UpdateImageData does by hand for camera frames.
//
// Preconditions, all checked: rgba.type() == CV_8UC4, dst != NULL,
// dst->type == IMG_TYPE_RGBA, dst->resultData != NULL, dst->width >= rgba.cols,
// dst->height >= rgba.rows. Returns false and logs when any fails, so a caller
// cannot half-write a frame without knowing.
bool MT_OpenCV_BlitMatToImageData(const cv::Mat &rgba, CImageData *dst);

#endif // MT_ENABLE_OPENCV

#endif
