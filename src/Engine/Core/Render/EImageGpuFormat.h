#ifndef _EImageGpuFormat_h_
#define _EImageGpuFormat_h_

// GPU-compressed texture format a render backend can upload.
enum EImageGpuFormat
{
	IMG_GPU_UNCOMPRESSED = 0,  // engine cannot use compressed textures
	IMG_GPU_BC7,               // BC7 / BPTC
	IMG_GPU_ASTC_4x4,          // ASTC 4x4 LDR
};

#endif
