#include "CImageData.h"
#include "DBG_Log.h"

// HEIC/HEIF decode, dispatched to whichever backend this platform actually has.
//
// THE RULE: prefer the OPERATING SYSTEM's own decoder, and vendor a library
// only where there is no OS decoder to call. That is the same rule
// CImageDataPlatformDecode.cpp states for the last-resort decode path
// ("macOS gets ImageIO, Windows gets WIC, and Linux gets nothing"), applied
// here to a format rather than to a repair attempt.
//
// It matters more for HEIF than for anything else in this file's neighbourhood,
// because HEIF's payload is HEVC and HEVC is PATENT-encumbered. Calling the
// platform decoder means the HEVC licence is Apple's or Microsoft's (or the
// device manufacturer's), already paid for on the machine the code runs on.
// Vendoring libheif+libde265 instead would put a decoder in OUR binary and
// make that licensing our problem -- which is why the vendored path is gated
// behind MT_PRIVATE_BUILD (see PRIVATE_ONLY_FLAGS in tools/mtcaps/resolve.py)
// and why neither platform below needs that gate.
//
// WINDOWS CAVEAT, and the reason a decode can still fail on a healthy file:
// WIC decodes HEIF only when the machine has Microsoft's HEVC Video
// Extensions. It is not present by default on every install. There is nothing
// to fix in this file when it is missing -- the correct response is to tell
// the user what is missing, which is an APP-level concern served by the
// availability API below.
bool CImageData::LoadHEIF(const char *fileName)
{
#if defined(MACOS) || defined(__APPLE__)
	return LoadWithImageIO_Apple(fileName);
#elif defined(WIN32) || defined(_WIN32)
	// WIC dispatches on the file's CONTENT, not its extension, so the same
	// entry point that serves the last-resort path decodes HEIF here. Ahead of
	// MT_ENABLE_LIBHEIF deliberately: if a private build ever vendors libheif
	// on Windows too, the platform decoder still wins, exactly as ImageIO wins
	// on macOS.
	return LoadWithWIC_Windows(fileName);
#elif MT_ENABLE_LIBHEIF
	// Linux: no system image decoder exists to call, so this is the one
	// platform where the vendored library is the only way to read a HEIC.
	return LoadHEIF_libheif(fileName);
#else
	LOGError("CImageData::LoadHEIF: HEIC/HEIF not supported on this build (%s)", fileName);
	return false;
#endif
}

// ---------------------------------------------------------------------------
// Availability: what the ENGINE tells a host about HEIF, and in what shape.
//
// THREE DELIBERATE CHOICES, because the obvious version of each is wrong here.
//
// 1. AN ENUM AND A KEY, NEVER A MESSAGE. This engine ships no localized
//    strings -- there is no assets/locale in this repository -- and its
//    established seam is a host-installed translate function taking a KEY
//    (CGuiView::SetImGuiTitleTranslateFunc, and CGuiViewVideoPlayer::
//    SetTranslateLabelFunc for the same reason). An engine returning an
//    English sentence forces every localized host to pattern-match on it.
//    the photo app already does that to one engine marker string; it is not a
//    habit worth spreading.
//
// 2. A SYSTEM QUESTION, ASKED ONCE -- NOT A PER-FILE ONE. "This machine cannot
//    decode HEIF" is true of every HEIC forever, until the user installs
//    something. "This file failed" is about one file. A host that only ever
//    learns the second cannot state the first without repeating it once per
//    file, and a photo browser opening a folder of HEICs would repeat it
//    hundreds of times. Ask this once and keep the answer.
//
//    Rule of thumb worth stating, because it is the trap: IF A DESIGN NEEDS A
//    "SHOW ONCE" FLAG TO STAY TOLERABLE, THE DESIGN IS WRONG. Ask the
//    system-level question instead of suppressing a per-file one.
//
// 3. NO UI FROM THE DECODE PATH, EVER. Decode runs on worker threads that have
//    no business raising windows. Everything here is a VALUE. Presentation
//    belongs to the host, which is also the only layer that knows whether a
//    user asked for this file explicitly or a thumbnailer did -- and those two
//    deserve different treatment.
// ---------------------------------------------------------------------------

#if !defined(WIN32) && !defined(_WIN32)
// The Windows arm lives in CImageDataWIC_Windows.cpp, beside the WIC code it
// queries -- the same split that puts the Apple decode in
// CImageDataHEIF_Apple.mm. It asks WIC rather than the video stack even though
// a missing HEVC Video Extensions breaks both; the comment there says why (the
// short version is that the video probe does not exist in a build with
// MT_ENABLE_FFMPEG off, and reading a photo must not require the video stack).
CImageData::EHeifAvailability CImageData::GetHeifAvailability()
{
#if defined(MACOS) || defined(__APPLE__)
	// ImageIO has decoded HEIF since macOS 10.13, which predates the minimum
	// this engine targets: nothing to probe, and nothing a user could install
	// that would change the answer.
	return HEIF_AVAILABLE;
#elif MT_ENABLE_LIBHEIF
	// Compiled in means available: libheif is linked, not discovered.
	return HEIF_AVAILABLE;
#else
	// Linux without the vendored library, or any build with HEIF gated off.
	// A DIFFERENT value from the Windows case on purpose: nothing the user
	// installs fixes this one, because it is a property of the build.
	return HEIF_UNAVAILABLE_NOT_BUILT;
#endif
}
#endif

bool CImageData::IsHEIFDecodeAvailable()
{
	return GetHeifAvailability() == HEIF_AVAILABLE;
}

const char *CImageData::GetHeifAvailabilityI18nKey()
{
	switch (GetHeifAvailability())
	{
		case HEIF_AVAILABLE:
			// NULL rather than a "success" key, so the caller's test keeps the
			// same shape as the availability test itself.
			return NULL;

		case HEIF_UNAVAILABLE_SYSTEM_CODEC_MISSING:
			// The one case the USER can fix -- which is why it is worth
			// surfacing at all, and why GetHeifCodecInstallUrl() is non-NULL
			// beside it.
			return "engine.heif.unavailable.system_codec_missing";

		case HEIF_UNAVAILABLE_NOT_BUILT:
		default:
			// The user can do nothing about this one. A host may reasonably
			// choose to stay silent rather than explain its own build.
			return "engine.heif.unavailable.not_built";
	}
}

const char *CImageData::GetHeifCodecInstallUrl()
{
#if defined(WIN32) || defined(_WIN32)
	// PFN, not the numeric Store ProductId. The package family name is what the
	// machine itself reports (Get-AppxPackage Microsoft.HEVCVideoExtension), so
	// this was READ rather than guessed -- and a mistyped ProductId fails as a
	// silently dead link, which is the worst way for this to be wrong.
	//
	// HEVCVideoExtension is the paid one and the one actually missing on a
	// stock install. HEIFImageExtension is free and usually already present, so
	// pointing at it first would send a user who already has it on a pointless
	// errand.
	//
	// NULL, not "", when there is nothing to install: `if (url)` is the whole
	// test a caller needs.
	return (GetHeifAvailability() == HEIF_UNAVAILABLE_SYSTEM_CODEC_MISSING)
	       ? "ms-windows-store://pdp/?PFN=Microsoft.HEVCVideoExtension_8wekyb3d8bbwe"
	       : NULL;
#else
	// Nothing a user could install changes the answer elsewhere: macOS already
	// has it, Linux decides at build time.
	return NULL;
#endif
}
