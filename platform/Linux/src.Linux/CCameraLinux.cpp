#include "CCameraLinux.h"
#include "DBG_Log.h"
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <linux/videodev2.h>

CCameraLinux::CCameraLinux()
: fd(-1)
, numMappedBuffers(0)
, captureWidth(0)
, captureHeight(0)
, internalBuffer(NULL)
, newFrame(false)
, capturing(false)
, threadRunning(false)
{
	memset(mappedBuffers, 0, sizeof(mappedBuffers));
}

CCameraLinux::~CCameraLinux()
{
	StopCapture();
	if (internalBuffer)
	{
		delete[] internalBuffer;
		internalBuffer = NULL;
	}
}

std::vector<CCameraDevice> CCameraLinux::EnumerateDevices()
{
	std::vector<CCameraDevice> result;
	int idx = 0;

	for (int n = 0; n <= 15; n++)
	{
		char devPath[32];
		snprintf(devPath, sizeof(devPath), "/dev/video%d", n);

		int devFd = open(devPath, O_RDWR | O_NONBLOCK);
		if (devFd < 0)
			continue;

		struct v4l2_capability cap;
		memset(&cap, 0, sizeof(cap));
		if (ioctl(devFd, VIDIOC_QUERYCAP, &cap) < 0)
		{
			close(devFd);
			continue;
		}

		if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE))
		{
			close(devFd);
			continue;
		}

		CCameraDevice d;
		d.index = idx++;
		strncpy(d.name, (const char *)cap.card, sizeof(d.name) - 1);
		d.name[sizeof(d.name) - 1] = '\0';

		// uniqueId: "v4l2:{bus_info}:{device_number}"
		char uniqueId[256];
		snprintf(uniqueId, sizeof(uniqueId), "v4l2:%s:%d", (const char *)cap.bus_info, n);
		strncpy(d.uniqueId, uniqueId, sizeof(d.uniqueId) - 1);
		d.uniqueId[sizeof(d.uniqueId) - 1] = '\0';

		close(devFd);
		result.push_back(d);
	}

	return result;
}

bool CCameraLinux::StartCapture(int deviceIndex, int requestedWidth, int requestedHeight)
{
	StopCapture();

	// Find the device path matching deviceIndex
	int idx = 0;
	int devNumber = -1;
	for (int n = 0; n <= 15; n++)
	{
		char devPath[32];
		snprintf(devPath, sizeof(devPath), "/dev/video%d", n);

		int devFd = open(devPath, O_RDWR | O_NONBLOCK);
		if (devFd < 0)
			continue;

		struct v4l2_capability cap;
		memset(&cap, 0, sizeof(cap));
		bool ok = (ioctl(devFd, VIDIOC_QUERYCAP, &cap) == 0);
		bool isCapture = ok && (cap.capabilities & V4L2_CAP_VIDEO_CAPTURE);
		close(devFd);

		if (!isCapture)
			continue;

		if (idx == deviceIndex)
		{
			devNumber = n;
			break;
		}
		idx++;
	}

	if (devNumber < 0)
	{
		LOGD("CCameraLinux: device index %d not found", deviceIndex);
		return false;
	}

	char devPath[32];
	snprintf(devPath, sizeof(devPath), "/dev/video%d", devNumber);
	fd = open(devPath, O_RDWR | O_NONBLOCK);
	if (fd < 0)
	{
		LOGD("CCameraLinux: failed to open %s: %s", devPath, strerror(errno));
		return false;
	}

	// Set format: YUYV
	struct v4l2_format fmt;
	memset(&fmt, 0, sizeof(fmt));
	fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	fmt.fmt.pix.width       = (unsigned int)requestedWidth;
	fmt.fmt.pix.height      = (unsigned int)requestedHeight;
	fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
	fmt.fmt.pix.field       = V4L2_FIELD_NONE;

	if (ioctl(fd, VIDIOC_S_FMT, &fmt) < 0)
	{
		LOGD("CCameraLinux: VIDIOC_S_FMT failed: %s", strerror(errno));
		close(fd);
		fd = -1;
		return false;
	}

	captureWidth  = (int)fmt.fmt.pix.width;
	captureHeight = (int)fmt.fmt.pix.height;

	// Allocate mmap buffers
	struct v4l2_requestbuffers req;
	memset(&req, 0, sizeof(req));
	req.count  = NUM_BUFFERS;
	req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	req.memory = V4L2_MEMORY_MMAP;

	if (ioctl(fd, VIDIOC_REQBUFS, &req) < 0)
	{
		LOGD("CCameraLinux: VIDIOC_REQBUFS failed: %s", strerror(errno));
		close(fd);
		fd = -1;
		return false;
	}

	numMappedBuffers = (int)req.count;
	for (int i = 0; i < numMappedBuffers; i++)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = (unsigned int)i;

		if (ioctl(fd, VIDIOC_QUERYBUF, &buf) < 0)
		{
			LOGD("CCameraLinux: VIDIOC_QUERYBUF failed for buffer %d: %s", i, strerror(errno));
			UnmapBuffers();
			close(fd);
			fd = -1;
			return false;
		}

		mappedBuffers[i].length = buf.length;
		mappedBuffers[i].start  = mmap(NULL, buf.length,
		                               PROT_READ | PROT_WRITE,
		                               MAP_SHARED,
		                               fd, (off_t)buf.m.offset);

		if (mappedBuffers[i].start == MAP_FAILED)
		{
			LOGD("CCameraLinux: mmap failed for buffer %d: %s", i, strerror(errno));
			mappedBuffers[i].start = NULL;
			mappedBuffers[i].length = 0;
			UnmapBuffers();
			close(fd);
			fd = -1;
			return false;
		}
	}

	// Enqueue all buffers
	for (int i = 0; i < numMappedBuffers; i++)
	{
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;
		buf.index  = (unsigned int)i;

		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
		{
			LOGD("CCameraLinux: VIDIOC_QBUF failed for buffer %d: %s", i, strerror(errno));
			UnmapBuffers();
			close(fd);
			fd = -1;
			return false;
		}
	}

	// Allocate internal RGBA buffer
	{
		std::lock_guard<std::mutex> lock(frameMutex);
		if (internalBuffer)
			delete[] internalBuffer;
		internalBuffer = new u8[captureWidth * captureHeight * 4];
	}

	// Start streaming
	enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	if (ioctl(fd, VIDIOC_STREAMON, &type) < 0)
	{
		LOGD("CCameraLinux: VIDIOC_STREAMON failed: %s", strerror(errno));
		UnmapBuffers();
		close(fd);
		fd = -1;
		return false;
	}

	newFrame      = false;
	capturing     = true;
	threadRunning = true;
	captureThread = std::thread(&CCameraLinux::CaptureThreadFunc, this);

	LOGD("CCameraLinux: started capture on /dev/video%d at %dx%d", devNumber, captureWidth, captureHeight);
	return true;
}

void CCameraLinux::StopCapture()
{
	if (!capturing.load())
		return;

	capturing     = false;
	threadRunning = false;

	if (captureThread.joinable())
		captureThread.join();

	if (fd >= 0)
	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		ioctl(fd, VIDIOC_STREAMOFF, &type);

		UnmapBuffers();
		close(fd);
		fd = -1;
	}

	newFrame = false;
}

bool CCameraLinux::IsCapturing()
{
	return capturing.load();
}

bool CCameraLinux::IsNewFrameReady()
{
	return newFrame.load();
}

bool CCameraLinux::GetFrameRGBA(u8 *destBuffer, int bufferWidth, int bufferHeight)
{
	std::lock_guard<std::mutex> lock(frameMutex);
	if (!internalBuffer || captureWidth != bufferWidth || captureHeight != bufferHeight)
		return false;

	memcpy(destBuffer, internalBuffer, bufferWidth * bufferHeight * 4);
	newFrame = false;
	return true;
}

int CCameraLinux::GetFrameWidth()
{
	return captureWidth;
}

int CCameraLinux::GetFrameHeight()
{
	return captureHeight;
}

void CCameraLinux::UnmapBuffers()
{
	for (int i = 0; i < numMappedBuffers; i++)
	{
		if (mappedBuffers[i].start && mappedBuffers[i].start != MAP_FAILED)
		{
			munmap(mappedBuffers[i].start, mappedBuffers[i].length);
			mappedBuffers[i].start  = NULL;
			mappedBuffers[i].length = 0;
		}
	}
	numMappedBuffers = 0;
}

bool CCameraLinux::ConvertYUYVToRGBA(const u8 *src, int width, int height)
{
	// YUYV packs two pixels per 4 bytes: [Y0 U0 Y1 V0]
	u8 *dst = internalBuffer;
	int numPixelPairs = (width * height) / 2;

	for (int i = 0; i < numPixelPairs; i++)
	{
		int y0 = src[0];
		int u  = src[1] - 128;
		int y1 = src[2];
		int v  = src[3] - 128;
		src += 4;

		// Pixel 0
		int r0 = y0 + (int)(1.402f   * v);
		int g0 = y0 - (int)(0.344f   * u) - (int)(0.714f * v);
		int b0 = y0 + (int)(1.772f   * u);
		dst[0] = (u8)(r0 < 0 ? 0 : r0 > 255 ? 255 : r0);
		dst[1] = (u8)(g0 < 0 ? 0 : g0 > 255 ? 255 : g0);
		dst[2] = (u8)(b0 < 0 ? 0 : b0 > 255 ? 255 : b0);
		dst[3] = 255;
		dst += 4;

		// Pixel 1
		int r1 = y1 + (int)(1.402f   * v);
		int g1 = y1 - (int)(0.344f   * u) - (int)(0.714f * v);
		int b1 = y1 + (int)(1.772f   * u);
		dst[0] = (u8)(r1 < 0 ? 0 : r1 > 255 ? 255 : r1);
		dst[1] = (u8)(g1 < 0 ? 0 : g1 > 255 ? 255 : g1);
		dst[2] = (u8)(b1 < 0 ? 0 : b1 > 255 ? 255 : b1);
		dst[3] = 255;
		dst += 4;
	}

	return true;
}

void CCameraLinux::CaptureThreadFunc()
{
	while (threadRunning.load())
	{
		// Wait for the fd to become readable (1-second timeout for clean exit)
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(fd, &fds);

		struct timeval tv;
		tv.tv_sec  = 1;
		tv.tv_usec = 0;

		int ret = select(fd + 1, &fds, NULL, NULL, &tv);
		if (ret < 0)
		{
			if (errno == EINTR)
				continue;
			LOGD("CCameraLinux: select() error: %s", strerror(errno));
			break;
		}
		if (ret == 0)
			continue; // timeout — loop back to check threadRunning

		// Dequeue a filled buffer
		struct v4l2_buffer buf;
		memset(&buf, 0, sizeof(buf));
		buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
		buf.memory = V4L2_MEMORY_MMAP;

		if (ioctl(fd, VIDIOC_DQBUF, &buf) < 0)
		{
			if (errno == EAGAIN)
				continue;
			LOGD("CCameraLinux: VIDIOC_DQBUF failed: %s", strerror(errno));
			break;
		}

		int bufIdx = (int)buf.index;
		if (bufIdx < 0 || bufIdx >= numMappedBuffers)
		{
			// Re-enqueue and continue
			ioctl(fd, VIDIOC_QBUF, &buf);
			continue;
		}

		const u8 *src = (const u8 *)mappedBuffers[bufIdx].start;

		{
			std::lock_guard<std::mutex> lock(frameMutex);
			if (internalBuffer)
			{
				ConvertYUYVToRGBA(src, captureWidth, captureHeight);
				newFrame = true;
			}
		}

		// Re-enqueue the buffer
		if (ioctl(fd, VIDIOC_QBUF, &buf) < 0)
		{
			LOGD("CCameraLinux: VIDIOC_QBUF (re-enqueue) failed: %s", strerror(errno));
			break;
		}
	}
}

CCameraCapture *SYS_CreatePlatformCameraCapture()
{
	return new CCameraLinux();
}
