// Windows Imaging Component decode, the Windows half of
// CImageData::LoadWithPlatformDecoder. Compiles to nothing elsewhere.
#if defined(WIN32) || defined(_WIN32)

#include "CImageData.h"
#include "DBG_Log.h"
#include "SYS_FileUtf8.h"

#include <windows.h>
#include <wincodec.h>
#include <string>
#include <vector>    // the IWICColorContext array WIC fills
#include <new>       // std::nothrow

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

// RAII for the COM apartment. the photo app's decode workers are ordinary
// threads that never called CoInitialize, but the main thread may have -- and
// a second initialise on an already-initialised thread returns S_FALSE (or
// RPC_E_CHANGED_MODE for a different model). Both are survivable: only
// uninitialise if WE were the one who initialised.
namespace
{
	struct WicComScope
	{
		bool owns = false;
		WicComScope()
		{
			HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
			owns = SUCCEEDED(hr) && hr != S_FALSE;
		}
		~WicComScope() { if (owns) CoUninitialize(); }
	};

	template <typename T> struct WicPtr
	{
		T *p = nullptr;
		~WicPtr() { if (p) p->Release(); }
		T **operator&() { return &p; }
		T *operator->() const { return p; }
		explicit operator bool() const { return p != nullptr; }
	};
}

bool CImageData::LoadWithWIC_Windows(const char *fileName)
{
	WicComScope com;

	WicPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
	                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory)
	{
		LOGError("CImageData::LoadWithWIC_Windows: no WIC factory (hr=0x%08lX)", (unsigned long)hr);
		return false;
	}

	// UTF-8 -> UTF-16. Windows file paths are the one place this engine has
	// been bitten repeatedly; SYS_FileUtf8 owns that conversion.
	std::wstring wide = SYS_Utf8ToWide(fileName);
	if (wide.empty())
		return false;

	WicPtr<IWICBitmapDecoder> decoder;
	// ON DEMAND, not ON LOAD: these files are damaged by definition, and
	// asking WIC to validate all metadata up front is exactly the strictness
	// that got us here.
	hr = factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
	                                        WICDecodeMetadataCacheOnDemand, &decoder);
	if (FAILED(hr) || !decoder)
		return false;                 // quiet: this is a fallback, failing is normal

	WicPtr<IWICBitmapFrameDecode> frame;
	hr = decoder->GetFrame(0, &frame);
	if (FAILED(hr) || !frame)
		return false;

	UINT w = 0, h = 0;
	hr = frame->GetSize(&w, &h);
	if (FAILED(hr) || w == 0 || h == 0)
		return false;

	// Straight (NON-premultiplied) RGBA8, which is what resultData is and what
	// every other loader in this class produces. Asking WIC to convert means
	// we never have to un-premultiply by hand, unlike the ImageIO path.
	WicPtr<IWICFormatConverter> converter;
	hr = factory->CreateFormatConverter(&converter);
	if (FAILED(hr) || !converter)
		return false;
	hr = converter->Initialize(frame.p, GUID_WICPixelFormat32bppRGBA,
	                           WICBitmapDitherTypeNone, nullptr, 0.0,
	                           WICBitmapPaletteTypeCustom);
	if (FAILED(hr))
		return false;

	const size_t stride = (size_t)w * 4;
	const size_t bytes  = stride * (size_t)h;
	u8 *buf = new (std::nothrow) u8[bytes];
	if (buf == nullptr)
		return false;
	memset(buf, 0, bytes);            // a partial decode leaves the rest black

	hr = converter->CopyPixels(nullptr, (UINT)stride, (UINT)bytes, buf);
	if (FAILED(hr))
	{
		delete[] buf;
		return false;
	}

	this->width      = (int)w;
	this->height     = (int)h;
	this->type       = IMG_TYPE_RGBA;
	this->resultData = buf;

	// ---- embedded ICC profile -------------------------------------------
	//
	// WHY THIS IS SAFE TO ATTACH TO THE PIXELS ABOVE, which is the only
	// question that matters here. The Apple path states the invariant:
	// "profile and pixels must switch together -- a recorded profile that
	// does not describe the pixels is worse than none."
	//
	// IWICFormatConverter converts the PIXEL FORMAT and nothing else: it does
	// not colour-manage, and it does not resample primaries. The conversion
	// above (whatever the frame's native format -> 32bppRGBA) therefore leaves
	// the pixels in the FILE's colour space, which is exactly the space this
	// profile describes. A 10-bit HDR HEIC loses range to the 8-bit target,
	// but range is not primaries: the profile still describes the result
	// correctly.
	//
	// This used to be skipped entirely, and the reasoning was sound while this
	// function had ONE caller -- the last-resort path, where every file is
	// damaged by definition and its metadata is the least trustworthy thing
	// about it. CImageData::LoadHEIF() made it a primary decoder for healthy
	// files, and those routinely carry Display P3 straight out of an iPhone.
	// Dropping that is a visible, wrong-colour bug, not a cautious omission.
	//
	// Still tolerant of the damaged-file caller: everything below fails soft.
	// No contexts, an unreadable context, a non-ICC context or a malformed
	// profile all leave the image UNTAGGED, which is the state the
	// assumed-profile setting (CM-B) already handles.
	UINT contextCount = 0;
	// A frame with no colour contexts returns count 0; some decoders return an
	// error instead of 0, so a failure here is "untagged", never fatal.
	if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &contextCount)) && contextCount > 0)
	{
		// WIC does not allocate these: it fills objects the caller creates.
		std::vector<IWICColorContext *> contexts(contextCount, nullptr);
		bool allCreated = true;
		for (UINT i = 0; i < contextCount; i++)
		{
			if (FAILED(factory->CreateColorContext(&contexts[i])) || contexts[i] == nullptr)
			{
				allCreated = false;
				break;
			}
		}

		UINT actualContexts = 0;
		if (allCreated &&
		    SUCCEEDED(frame->GetColorContexts(contextCount, contexts.data(), &actualContexts)))
		{
			for (UINT i = 0; i < actualContexts && this->iccProfile == NULL; i++)
			{
				WICColorContextType contextType = WICColorContextUninitialized;
				if (FAILED(contexts[i]->GetType(&contextType)))
					continue;

				// ONLY a real embedded profile is recorded.
				//
				// The other kind WIC reports, WICColorContextExifColorSpace, is
				// an EXIF enum (1 = sRGB, 2 = Adobe RGB) rather than a profile,
				// and is deliberately NOT translated into a synthesised profile
				// here. The engine's own container-colour-space channel exists
				// for exactly that sort of signal and is documented as being
				// written by ONE loader (LoadRAWPreview); quietly adding a
				// second writer would break the lifecycle reasoning stated on
				// that field. An sRGB-tagged file left untagged lands on the
				// assumed-sRGB default anyway, so the practical loss is nil.
				if (contextType != WICColorContextProfile)
					continue;

				UINT profileSize = 0;
				if (FAILED(contexts[i]->GetProfileBytes(0, nullptr, &profileSize)) ||
				    profileSize == 0)
					continue;

				u8 *profileBytes = new (std::nothrow) u8[profileSize];
				if (profileBytes == nullptr)
					continue;

				UINT actualSize = 0;
				if (SUCCEEDED(contexts[i]->GetProfileBytes(profileSize, profileBytes, &actualSize)) &&
				    actualSize > 0)
				{
					// SetIccProfile deep-copies and structurally validates,
					// leaving the image untagged if these bytes are not a
					// profile -- so a damaged file cannot hand a broken
					// profile to the CMM, where it is a crash and not merely a
					// colour bug.
					this->SetIccProfile(profileBytes, (u32)actualSize);
				}
				delete[] profileBytes;
			}
		}

		for (UINT i = 0; i < contextCount; i++)
		{
			if (contexts[i] != nullptr)
				contexts[i]->Release();
		}
	}

	return true;
}

// "Can this machine decode HEIF at all?" -- the Windows arm; the others are in
// CImageDataHEIF.cpp, which explains why this asks WIC rather than the video
// stack (short version: the video probe does not exist in a build with
// MT_ENABLE_FFMPEG off, and reading a photo must not require the video stack).
//
// Enumerates WIC's registered decoders and looks for one claiming the HEIF
// container. That is a question about the SYSTEM, so the answer cannot change
// while the process runs -- except by the user installing the extension
// mid-session, which is exactly the case worth re-checking for, so the cache
// below is deliberately only populated on SUCCESS.
CImageData::EHeifAvailability CImageData::GetHeifAvailability()
{
	// -1 unknown, 1 yes. A "no" is NOT cached: the user may install HEVC Video
	// Extensions from the Store while the app is open, and the whole point of
	// telling them it is missing is that they might go and fix it. Re-probing
	// after a negative costs a COM enumeration on a path that is already
	// rare.
	//
	// Benign race only: concurrent decode workers may each probe and each
	// write, but every writer writes the same 1.
	static int cachedAvailable = -1;
	if (cachedAvailable == 1)
		return HEIF_AVAILABLE;

	WicComScope com;

	WicPtr<IWICImagingFactory> factory;
	HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
	                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
	if (FAILED(hr) || !factory)
	{
		LOGError("CImageData::GetHeifAvailability: no WIC factory (hr=0x%08lX)", (unsigned long)hr);
		// No WIC at all is not "the codec is missing" -- nothing the user
		// installs would help -- so this is the build-level value, which
		// carries no install URL.
		return HEIF_UNAVAILABLE_NOT_BUILT;
	}

	WicPtr<IEnumUnknown> components;
	hr = factory->CreateComponentEnumerator(WICDecoder, WICComponentEnumerateDefault,
	                                        &components);
	if (FAILED(hr) || !components)
		return HEIF_UNAVAILABLE_NOT_BUILT;

	bool found = false;
	IUnknown *unknown = nullptr;
	ULONG fetched = 0;
	while (!found && components->Next(1, &unknown, &fetched) == S_OK && fetched == 1)
	{
		IWICBitmapDecoderInfo *info = nullptr;
		if (SUCCEEDED(unknown->QueryInterface(IID_PPV_ARGS(&info))) && info)
		{
			// GetContainerFormat is SINGULAR: a WIC codec declares exactly one
			// container, so this is a plain comparison rather than a list
			// search. (GetPixelFormats, right beside it, IS plural -- one
			// codec supports many pixel formats but only one container.)
			GUID container = GUID_NULL;
			if (SUCCEEDED(info->GetContainerFormat(&container)) &&
			    IsEqualGUID(container, GUID_ContainerFormatHeif))
			{
				found = true;
			}
			info->Release();
		}
		unknown->Release();
		unknown = nullptr;
	}

	if (found)
		cachedAvailable = 1;

	// WIC is present but no decoder claims the HEIF container: that IS the
	// missing-package case, and the only state carrying an install URL.
	return found ? HEIF_AVAILABLE : HEIF_UNAVAILABLE_SYSTEM_CODEC_MISSING;
}

#endif // WIN32
