#include "MT_OpenCV.h"
#include "CImageData.h"
#include "DBG_Log.h"

#if MT_ENABLE_OPENCV

#include <cstring>

bool MT_OpenCV_IsAvailable() { return true; }
const char *MT_OpenCV_Version() { return CV_VERSION; }

// ONE table both directions read, so a new engine image type cannot be added to
// one direction and forgotten in the other.
static bool MTOpenCVTypeForImage(u8 imageType, int *matType, int *bytesPerPixel)
{
	switch (imageType)
	{
		case IMG_TYPE_RGBA:            *matType = CV_8UC4;  *bytesPerPixel = 4; return true;
		case IMG_TYPE_RGB:             *matType = CV_8UC3;  *bytesPerPixel = 3; return true;
		case IMG_TYPE_GRAYSCALE:       *matType = CV_8UC1;  *bytesPerPixel = 1; return true;
		case IMG_TYPE_GRAYSCALE_16BIT: *matType = CV_16UC1; *bytesPerPixel = 2; return true;
		case IMG_TYPE_RGBA_16BIT:      *matType = CV_16UC4; *bytesPerPixel = 8; return true;
		case IMG_TYPE_RGBA_16F:        *matType = CV_16FC4; *bytesPerPixel = 8; return true;
		default:                       return false;
	}
}

cv::Mat MT_OpenCV_MatFromImageData(const CImageData *image)
{
	if (image == NULL || image->resultData == NULL || image->width <= 0 || image->height <= 0)
	{
		LOGError("MT_OpenCV_MatFromImageData: no image data");
		return cv::Mat();
	}

	int matType = 0;
	int bytesPerPixel = 0;
	if (!MTOpenCVTypeForImage(image->type, &matType, &bytesPerPixel))
	{
		LOGError("MT_OpenCV_MatFromImageData: unsupported image type %d", (int)image->type);
		return cv::Mat();
	}

	// clone(), not the wrapping constructor alone: the Mat must OWN its pixels.
	// A view into resultData would dangle the moment the caller frees the image,
	// and the whole point of handing a Mat to a worker thread is that it
	// outlives the frame that produced it.
	cv::Mat wrapped(image->height, image->width, matType,
					(void *)image->resultData,
					(size_t)image->width * (size_t)bytesPerPixel);
	return wrapped.clone();
}

CImageData *MT_OpenCV_ImageDataFromMat(const cv::Mat &mat)
{
	if (mat.empty())
	{
		LOGError("MT_OpenCV_ImageDataFromMat: empty Mat");
		return NULL;
	}

	u8 imageType = IMG_TYPE_UNKNOWN;
	switch (mat.type())
	{
		case CV_8UC4:  imageType = IMG_TYPE_RGBA;            break;
		case CV_8UC3:  imageType = IMG_TYPE_RGBA;            break; // widened below
		case CV_8UC1:  imageType = IMG_TYPE_GRAYSCALE;       break;
		case CV_16UC1: imageType = IMG_TYPE_GRAYSCALE_16BIT; break;
		case CV_16UC4: imageType = IMG_TYPE_RGBA_16BIT;      break;
		case CV_16FC4: imageType = IMG_TYPE_RGBA_16F;        break;
		default:
			LOGError("MT_OpenCV_ImageDataFromMat: unsupported Mat type %d", mat.type());
			return NULL;
	}

	CImageData *image = new CImageData(mat.cols, mat.rows, imageType);
	image->AllocImage(false, true);

	if (mat.type() == CV_8UC3)
	{
		for (int y = 0; y < mat.rows; y++)
		{
			const u8 *src = mat.ptr<u8>(y);
			u8 *dst = image->resultData + (size_t)y * (size_t)mat.cols * 4;
			for (int x = 0; x < mat.cols; x++)
			{
				dst[x * 4 + 0] = src[x * 3 + 0];
				dst[x * 4 + 1] = src[x * 3 + 1];
				dst[x * 4 + 2] = src[x * 3 + 2];
				dst[x * 4 + 3] = 255;
			}
		}
		return image;
	}

	// ROW BY ROW AGAINST mat.step, never one memcpy of the whole buffer. A ROI
	// or an aligned allocation has step > cols * elemSize, and a single copy
	// shears the image by exactly that padding -- which is the bug the 2021
	// version of this bridge shipped with.
	const size_t rowBytes = (size_t)mat.cols * mat.elemSize();
	for (int y = 0; y < mat.rows; y++)
	{
		memcpy(image->resultData + (size_t)y * rowBytes, mat.ptr(y), rowBytes);
	}
	return image;
}

bool MT_OpenCV_BlitMatToImageData(const cv::Mat &rgba, CImageData *dst)
{
	if (rgba.empty() || rgba.type() != CV_8UC4)
	{
		LOGError("MT_OpenCV_BlitMatToImageData: source must be CV_8UC4, got %d",
				 rgba.empty() ? -1 : rgba.type());
		return false;
	}
	if (dst == NULL || dst->resultData == NULL || dst->type != IMG_TYPE_RGBA)
	{
		LOGError("MT_OpenCV_BlitMatToImageData: destination must be an allocated IMG_TYPE_RGBA image");
		return false;
	}
	if (dst->width < rgba.cols || dst->height < rgba.rows)
	{
		LOGError("MT_OpenCV_BlitMatToImageData: destination %dx%d is smaller than the source %dx%d",
				 dst->width, dst->height, rgba.cols, rgba.rows);
		return false;
	}

	const size_t rowBytes = (size_t)rgba.cols * 4;
	const size_t dstStride = (size_t)dst->width * 4;
	for (int y = 0; y < rgba.rows; y++)
	{
		memcpy(dst->resultData + (size_t)y * dstStride, rgba.ptr(y), rowBytes);
	}
	return true;
}

#else

// The OFF branch. Not an #error and not an absent symbol: a host asks
// MT_OpenCV_IsAvailable() at runtime and gets an honest no, exactly as
// MT_HAS_CAP(MT_CAP_OPENCV) says at compile time.
bool MT_OpenCV_IsAvailable() { return false; }
const char *MT_OpenCV_Version() { return ""; }

#endif // MT_ENABLE_OPENCV
