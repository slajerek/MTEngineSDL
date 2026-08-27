#pragma once

// UTF-8-safe file opening on every platform.
//
// The engine's convention is that a path in a `char *` / `std::string` is
// UTF-8. That is native on macOS and Linux, but on Windows every *narrow*
// CRT/Win32 entry point -- fopen, fopen_s, std::ifstream, CreateFileA, and
// std::filesystem's std::string constructors -- decodes that char* using the
// process ANSI code page, historically CP1252/CP1250/CP932. UTF-8 bytes then
// name a file that does not exist, and the open fails with ENOENT for a file
// that is plainly on disk: a photo under a folder such as
// "Chrzaszczozewoszyce", or with CJK, emoji or macOS NFD-normalized accents in
// its name, could be listed through the wide APIs and then not opened.
//
// Two independent layers fix this, and a Windows host wants both:
//
//   1. platform/Windows/MTEngineSDL.manifest sets activeCodePage=UTF-8, which
//      makes GetACP() return 65001 so EVERY narrow path API in the process --
//      the engine's, the app's and the vendored libraries' -- speaks UTF-8.
//      Host apps get it by importing platform/Windows/MTEngineSDLHost.props.
//      This is the systemic fix, but it needs Windows 10 1903+ and needs the
//      host to actually import the props.
//   2. These helpers, used by the engine's own media-loading code, which work
//      regardless of the process code page and regardless of the Windows
//      version.
//
// SYS_FopenUtf8() is a drop-in for fopen(): identical for an ASCII path,
// correct for everything else. It deliberately does NOT apply the "\\?\"
// extended-length prefix (see SYS_WindowsPathUtils.h) -- that prefix also
// disables path normalization, so a caller passing a relative path or one
// containing ".." would break. Encoding is the bug being fixed here; MAX_PATH
// is a separate concern with a separate helper.

#include <cstdio>
#include <filesystem>
#include <string>

#if defined(_WIN32)

// Declared rather than #include <windows.h>. This header is included by
// libjpeg's translation units, and windows.h's basetsd.h typedefs INT32 as
// `int` where libjpeg's jmorecfg.h already typedef'd it as `long` -- a hard
// "typedef redefinition with different types" error. Declaring the two
// functions we need keeps windows.h (and its min/max/near/far/small/boolean
// macros) out of every consumer. The signatures match windows.h exactly, so a
// TU that includes both is a legal redeclaration. Same approach as
// stb_image.h's UTF-8 fopen path.
extern "C" __declspec(dllimport) int __stdcall MultiByteToWideChar(
	unsigned int codePage, unsigned long flags, const char *multiByteStr,
	int cbMultiByte, wchar_t *wideCharStr, int cchWideChar);

extern "C" __declspec(dllimport) int __stdcall WideCharToMultiByte(
	unsigned int codePage, unsigned long flags, const wchar_t *wideCharStr,
	int cchWideChar, char *multiByteStr, int cbMultiByte,
	const char *defaultChar, int *usedDefaultChar);

#define SYS_FILEUTF8_CP_UTF8 65001u

// UTF-8 -> UTF-16. Returns an empty string for an empty input or on a
// conversion failure; callers treat that as "cannot open".
inline std::wstring SYS_Utf8ToWide(const char *utf8)
{
	if (utf8 == NULL || utf8[0] == '\0')
		return std::wstring();

	const int len = MultiByteToWideChar(SYS_FILEUTF8_CP_UTF8, 0, utf8, -1, NULL, 0);
	if (len <= 1)
		return std::wstring();

	std::wstring wide((size_t)(len - 1), L'\0');   // len counts the NUL
	if (MultiByteToWideChar(SYS_FILEUTF8_CP_UTF8, 0, utf8, -1, &wide[0], len) == 0)
		return std::wstring();
	return wide;
}

// UTF-16 -> UTF-8.
inline std::string SYS_WideToUtf8(const wchar_t *wide)
{
	if (wide == NULL || wide[0] == L'\0')
		return std::string();

	const int len = WideCharToMultiByte(SYS_FILEUTF8_CP_UTF8, 0, wide, -1, NULL, 0, NULL, NULL);
	if (len <= 1)
		return std::string();

	std::string utf8((size_t)(len - 1), '\0');
	if (WideCharToMultiByte(SYS_FILEUTF8_CP_UTF8, 0, wide, -1, &utf8[0], len, NULL, NULL) == 0)
		return std::string();
	return utf8;
}

#endif

// fopen() that always interprets `path` as UTF-8.
inline FILE *SYS_FopenUtf8(const char *path, const char *mode)
{
	if (path == NULL || mode == NULL)
		return NULL;

#if defined(_WIN32)
	const std::wstring widePath = SYS_Utf8ToWide(path);
	const std::wstring wideMode = SYS_Utf8ToWide(mode);
	if (widePath.empty() || wideMode.empty())
		return NULL;
	return _wfopen(widePath.c_str(), wideMode.c_str());
#else
	return fopen(path, mode);
#endif
}

inline FILE *SYS_FopenUtf8(const std::string &path, const char *mode)
{
	return SYS_FopenUtf8(path.c_str(), mode);
}

// A UTF-8 string -> std::filesystem::path, and back. The implicit narrow
// conversions go through the process code page in both directions on Windows:
// path(std::string) silently mangles non-representable bytes, and .string()
// THROWS std::system_error ("No mapping for the Unicode character exists in the
// target multi-byte code page"). Neither of these does either. Use them
// anywhere a path crosses the fs::path <-> std::string boundary, and to open an
// std::ifstream/std::ofstream on a UTF-8 path.
inline std::filesystem::path SYS_Utf8ToFsPath(const std::string &utf8)
{
#if defined(_WIN32)
	return std::filesystem::path(SYS_Utf8ToWide(utf8.c_str()));
#else
	return std::filesystem::path(utf8);
#endif
}

inline std::string SYS_FsPathToUtf8(const std::filesystem::path &path)
{
	// u8string() is WideCharToMultiByte(CP_UTF8, ...) on Windows and a
	// pass-through elsewhere; it can represent every code point and never
	// throws. The iterator-pair copy handles both the C++17 (std::string) and
	// C++20 (std::u8string) return types.
	auto u8 = path.u8string();
	return std::string(u8.begin(), u8.end());
}
