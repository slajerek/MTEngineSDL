#include "MT_AudioFrames.h"

namespace
{
	// MPEG audio bitrate table, kbps, indexed [versionIsMpeg1][layerIndex][bitrateIndex].
	// layerIndex: 0 = Layer I, 1 = Layer II, 2 = Layer III.
	const int kBitrate[2][3][16] = {
		{ // MPEG-2 / 2.5
			{ 0,32,48,56,64,80,96,112,128,144,160,176,192,224,256,0 },
			{ 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },
			{ 0, 8,16,24,32,40,48, 56, 64, 80, 96,112,128,144,160,0 },
		},
		{ // MPEG-1
			{ 0,32,64,96,128,160,192,224,256,288,320,352,384,416,448,0 },
			{ 0,32,48,56, 64, 80, 96,112,128,160,192,224,256,320,384,0 },
			{ 0,32,40,48, 56, 64, 80, 96,112,128,160,192,224,256,320,0 },
		}
	};
	// Sample rate, indexed [versionIndex][rateIndex]; versionIndex 0 = MPEG-2.5,
	// 2 = MPEG-2, 3 = MPEG-1 (1 is the reserved value and never reaches here).
	const int kRate[4][4] = {
		{ 11025, 12000,  8000, 0 },
		{     0,     0,     0, 0 },
		{ 22050, 24000, 16000, 0 },
		{ 44100, 48000, 32000, 0 },
	};

	inline bool MpegHeaderLooksValid(const u8 *h)
	{
		if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
		if (((h[1] >> 3) & 0x03) == 1) return false;   // reserved version
		if (((h[1] >> 1) & 0x03) == 0) return false;   // reserved layer
		if (((h[2] >> 4) & 0x0F) == 0x0F) return false; // bad bitrate index
		if (((h[2] >> 2) & 0x03) == 0x03) return false; // reserved sample rate
		return true;
	}
}

size_t MT_AudioFrames::MpegFrameSize(const u8 *data, size_t avail)
{
	if (data == NULL || avail < 4 || !MpegHeaderLooksValid(data))
		return 0;

	const int versionIdx = (data[1] >> 3) & 0x03;          // 0=2.5, 2=2, 3=1
	const int layerBits  = (data[1] >> 1) & 0x03;          // 3=I, 2=II, 1=III
	const int layerIdx   = 3 - layerBits;                  // 0=I, 1=II, 2=III
	const int bitrateIdx = (data[2] >> 4) & 0x0F;
	const int rateIdx    = (data[2] >> 2) & 0x03;
	const int padding    = (data[2] >> 1) & 0x01;

	const int isMpeg1 = (versionIdx == 3) ? 1 : 0;
	const int kbps = kBitrate[isMpeg1][layerIdx][bitrateIdx];
	const int rate = kRate[versionIdx][rateIdx];
	if (kbps == 0 || rate == 0)
		return 0;   // "free format" bitrate: the size is not in the header

	size_t size;
	if (layerIdx == 0)
	{
		// Layer I is 384 samples and counts in 4-byte slots.
		size = (size_t)(((12 * kbps * 1000) / rate) + padding) * 4;
	}
	else
	{
		// Layer II is always 1152 samples; Layer III is 1152 on MPEG-1 and
		// 576 on MPEG-2/2.5, which is the halving below.
		const int samples = (layerIdx == 2 && !isMpeg1) ? 576 : 1152;
		size = (size_t)((samples / 8 * kbps * 1000) / rate) + (size_t)padding;
	}
	if (size < 4 || size > avail)
		return 0;
	return size;
}

size_t MT_AudioFrames::Ac3FrameSize(const u8 *data, size_t avail)
{
	if (data == NULL || avail < 5 || data[0] != 0x0B || data[1] != 0x77)
		return 0;

	const int bsid = (avail >= 6) ? (data[5] >> 3) : 8;
	if (bsid > 10)
	{
		// E-AC-3: frmsiz is an explicit word count, which is the whole reason
		// it needs no table.
		const size_t words = (size_t)(((data[2] & 0x07) << 8) | data[3]) + 1;
		const size_t size = words * 2;
		return (size >= 6 && size <= avail) ? size : 0;
	}

	// AC-3: frame size in 16-bit words, by sample rate and frmsizecod.
	static const unsigned short kFrmSize[3][38] = {
		{ 64,64,80,80,96,96,112,112,128,128,160,160,192,192,224,224,256,256,320,320,
		  384,384,448,448,512,512,640,640,768,768,896,896,1024,1024,1152,1152,1280,1280 },
		{ 69,70,87,88,104,105,121,122,139,140,174,175,208,209,243,244,278,279,348,349,
		  417,418,487,488,557,558,696,697,835,836,975,976,1114,1115,1253,1254,1393,1394 },
		{ 96,96,120,120,144,144,168,168,192,192,240,240,288,288,336,336,384,384,480,480,
		  576,576,672,672,768,768,960,960,1152,1152,1344,1344,1536,1536,1728,1728,1920,1920 },
	};
	const int fscod = (data[4] >> 6) & 0x03;
	const int frmsizecod = data[4] & 0x3F;
	if (fscod > 2 || frmsizecod > 37)
		return 0;
	const size_t size = (size_t)kFrmSize[fscod][frmsizecod] * 2;
	return (size <= avail) ? size : 0;
}

size_t MT_AudioFrames::FindMpegSync(const u8 *data, size_t avail)
{
	if (data == NULL) return avail;
	for (size_t i = 0; i + 4 <= avail; i++)
		if (MpegHeaderLooksValid(data + i))
			return i;
	return avail;
}

size_t MT_AudioFrames::FindAc3Sync(const u8 *data, size_t avail)
{
	if (data == NULL) return avail;
	for (size_t i = 0; i + 2 <= avail; i++)
		if (data[i] == 0x0B && data[i + 1] == 0x77)
			return i;
	return avail;
}
