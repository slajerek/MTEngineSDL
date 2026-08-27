#include "CImageData.h"
#include "DBG_Log.h"

// THE LAST RESORT: hand a file the engine's own decoder refused to the
// operating system's decoder.
//
// stb_image is strict by design -- it aborts on the first bad symbol and has
// no error recovery at all. The platform decoders do: ImageIO and WIC decode
// up to the corruption and keep what they got, which is why a photo that
// PhotoCruise called "unsupported or corrupt" opens in Preview or in Explorer's
// preview pane without complaint. To a photographer that reads as OUR bug,
// because from where they are standing it is one.
//
// This runs ONLY after stb_image has failed AND the missing-EOI repair has
// failed (CImageData::Load). It is not a shortcut around the normal path: the
// normal path is faster, portable, and already handles every healthy file.
//
// PLATFORM PARITY IS DELIBERATELY IMPERFECT HERE, and that is a decision
// rather than an oversight. macOS gets ImageIO, Windows gets WIC, and Linux
// gets nothing -- there is no equivalent system decoder to call. So a
// pathologically damaged file may open on two of the three platforms. The
// alternative was to refuse it everywhere for the sake of symmetry, which
// helps nobody: the Linux user is no better off for the Mac user also being
// unable to see their photo.
bool CImageData::LoadWithPlatformDecoder(const char *fileName)
{
#if defined(MACOS) || defined(__APPLE__)
	return LoadWithImageIO_Apple(fileName);
#elif defined(WIN32) || defined(_WIN32)
	return LoadWithWIC_Windows(fileName);
#else
	// Linux: no system image decoder to fall back on. The file stays refused,
	// which is the same behaviour as before this path existed.
	(void)fileName;
	return false;
#endif
}

// NON-MACOS STUB. The real body lives in CImageDataHEIF_Apple.mm, which is
// only ever compiled on macOS -- so on Windows and Linux the declaration in
// CImageData.h had no definition at all, an S-5 regression neither of those
// platforms had actually linked until the S-6 baseline pass caught it
// (CTestHeicHdrDecode's "off macOS the HDR arm declines" arm calls this
// unconditionally, precisely to prove the decline). Windows has no HEIC HDR
// gain-map decode and Linux has no HEIC decode at all, so declining
// unconditionally is correct on both, not a placeholder for one to be filled
// in later.
#if !defined(MACOS) && !defined(__APPLE__)
bool CImageData::LoadWithImageIO_AppleHDR(const char *fileName)
{
	(void)fileName;
	return false;
}
#endif
