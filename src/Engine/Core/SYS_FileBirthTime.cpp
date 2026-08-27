// _GNU_SOURCE MUST precede every include: glibc gates statx()/struct statx/
// STATX_BTIME behind it, and the engine CMake does not define it globally
// (../MTEngineSDL/CMakeLists.txt:66-71). No effect on macOS/Windows.
#if defined(__linux__) && !defined(_GNU_SOURCE)
  #define _GNU_SOURCE 1
#endif
#include "SYS_FileBirthTime.h"

#if defined(__APPLE__)
  #include <sys/stat.h>
#elif defined(_WIN32)
  #define WIN32_LEAN_AND_MEAN
  #include <windows.h>
  #include <vector>
#elif defined(__linux__)
  #include <fcntl.h>
  #include <sys/stat.h>   // statx, STATX_BTIME (Linux 4.11+, glibc 2.28+)
#endif

bool SYS_GetFileBirthTime(const std::string &path, int64_t *outUnixSeconds)
{
#if defined(__APPLE__)
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;   // macOS native paths are UTF-8
    if (outUnixSeconds) *outUnixSeconds = (int64_t)st.st_birthtimespec.tv_sec;
    return true;
#elif defined(_WIN32)
    // path is UTF-8; convert to UTF-16 with the Win32 API (engine is C++17 --
    // no std::u8string). GetFileAttributesExW needs no handle and no access
    // rights (a plain CreateFileA / fs::path(std::string) would mangle non-ASCII
    // via the ANSI code page).
    const int wlen = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    if (wlen <= 0) return false;
    std::vector<wchar_t> wpath((size_t)wlen);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wpath.data(), wlen);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(wpath.data(), GetFileExInfoStandard, &fad)) return false;
    // FILETIME is 100ns ticks since 1601-01-01; convert to Unix seconds.
    ULARGE_INTEGER u; u.LowPart = fad.ftCreationTime.dwLowDateTime; u.HighPart = fad.ftCreationTime.dwHighDateTime;
    if (u.QuadPart == 0) return false;                // no creation time recorded
    if (outUnixSeconds) *outUnixSeconds = (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
    return true;
#elif defined(__linux__) && defined(STATX_BTIME)
    struct statx stx;                                 // Linux native paths are UTF-8
    if (statx(AT_FDCWD, path.c_str(), AT_STATX_SYNC_AS_STAT, STATX_BTIME, &stx) != 0)
        return false;
    if (!(stx.stx_mask & STATX_BTIME)) return false;  // kernel/FS does not record it
    if (outUnixSeconds) *outUnixSeconds = (int64_t)stx.stx_btime.tv_sec;
    return true;
#else
    (void)path; (void)outUnixSeconds;                 // no birth-time facility on this target
    return false;
#endif
}
