#include "SYS_PathValidate.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <set>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#include <wchar.h>   // _wcsicmp
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;

namespace
{
    // Windows reserved device names. Reserved with OR without an extension, so
    // the check is on the basename alone. Enforced on every platform: a name
    // that is legal on macOS but unopenable on Windows would make the library
    // non-portable, and these files travel between machines.
    //
    // Ported verbatim from the photo app/src/FileOps/CRenameOperator.cpp (the
    // kReserved set and IsReservedDeviceName()) so the photo app's later
    // delegation to this engine primitive is behaviour-preserving.
    bool IsReservedDeviceName(const std::string &basename)
    {
        std::string upper = basename;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return (char)std::toupper(c); });

        static const std::set<std::string> kReserved = {
            "CON", "PRN", "AUX", "NUL",
            "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
        };
        return kReserved.count(upper) > 0;
    }
} // namespace

bool SYS_PathComponentHasIllegalChar(std::string_view component)
{
    for (char c : component)
    {
        const unsigned char u = (unsigned char)c;
        if (u < 0x20)
            return true;                                  // control characters
        if (c == '<' || c == '>' || c == ':' || c == '"' ||
            c == '|' || c == '?' || c == '*')
            return true;
    }
    return false;
}

bool SYS_PathIsReservedDeviceName(std::string_view component)
{
    // Windows reserves CON.txt too -- match against the text before the
    // first '.', not the whole component.
    const size_t dot = component.find('.');
    const std::string basename = (dot == std::string_view::npos)
        ? std::string(component)
        : std::string(component.substr(0, dot));
    return IsReservedDeviceName(basename);
}

bool SYS_PathComponentEndsWithDotOrSpace(std::string_view component)
{
    if (component.empty())
        return false;
    const char last = component.back();
    return last == '.' || last == ' ';
}

std::string SYS_PathStripInvisibleUnicode(std::string_view s)
{
    // Strip zero-width (U+200B-200D, U+FEFF) and bidi override (U+202A-202E,
    // U+2066-2069) code points from a UTF-8 string. NUL is left to the
    // illegal-char check. Spec #5.2 -- these are real EXIF injection vectors.
    //
    // All target ranges encode as 3-byte UTF-8 sequences:
    //   U+200B-200D, U+202A-202E, U+2066-2069 -> E2 80/81/82 xx
    //   U+FEFF                                -> EF BB BF
    std::string out;
    out.reserve(s.size());

    size_t i = 0;
    while (i < s.size())
    {
        const unsigned char b0 = (unsigned char)s[i];

        if (b0 == 0xE2 && i + 2 < s.size())
        {
            const unsigned char b1 = (unsigned char)s[i + 1];
            const unsigned char b2 = (unsigned char)s[i + 2];

            // U+200B ZERO WIDTH SPACE, U+200C ZWNJ, U+200D ZWJ: E2 80 8B/8C/8D
            const bool isZeroWidth = (b1 == 0x80) && (b2 == 0x8B || b2 == 0x8C || b2 == 0x8D);
            // U+202A-202E (LRE/RLE/PDF/LRO/RLO): E2 80 AA-AE
            const bool isBidiA = (b1 == 0x80) && (b2 >= 0xAA && b2 <= 0xAE);
            // U+2066-2069 (LRI/RLI/FSI/PDI): E2 81 A6-A9
            const bool isBidiB = (b1 == 0x81) && (b2 >= 0xA6 && b2 <= 0xA9);

            if (isZeroWidth || isBidiA || isBidiB)
            {
                i += 3;
                continue;
            }
        }
        else if (b0 == 0xEF && i + 2 < s.size())
        {
            const unsigned char b1 = (unsigned char)s[i + 1];
            const unsigned char b2 = (unsigned char)s[i + 2];

            // U+FEFF ZERO WIDTH NO-BREAK SPACE / BOM: EF BB BF
            if (b1 == 0xBB && b2 == 0xBF)
            {
                i += 3;
                continue;
            }
        }

        out.push_back(s[i]);
        i += 1;
    }

    return out;
}

size_t SYS_PathMaxBytes()
{
#if defined(__APPLE__)
    return 1024;
#elif defined(__linux__)
    return 4096;
#elif defined(_WIN32)
    return 32767;
#else
    return 1024;
#endif
}

std::string SYS_WeakCanonical(const std::string &path)
{
    std::error_code ec;
    fs::path result = fs::weakly_canonical(fs::path(path), ec);
    if (ec)
        result = fs::path(path).lexically_normal();
    return result.string();
}

bool SYS_PathIsWithin(const std::string &root, const std::string &candidate)
{
    // Weak-canonicalise both sides first: "/a/../etc" must resolve to "/etc"
    // BEFORE the prefix check, or the ".." would slip through as a literal
    // path component.
    const fs::path rootPath = fs::path(SYS_WeakCanonical(root)).lexically_normal();
    const fs::path candidatePath = fs::path(SYS_WeakCanonical(candidate)).lexically_normal();

    // Component-boundary prefix: iterate rootPath's components against
    // candidatePath's, matching a mismatch/short-candidate escape (false),
    // and requiring the root to be fully consumed. This naturally lands on
    // component boundaries -- "/a" vs "/a/bc" mismatches at the "a" vs "bc"
    // component (not a partial-string match), so "/a" is NOT within "/a/bc".
    auto rootIt = rootPath.begin();
    auto candIt = candidatePath.begin();
    for (; rootIt != rootPath.end(); ++rootIt, ++candIt)
    {
        if (candIt == candidatePath.end())
            return false;               // candidate is shorter than root
        if (*rootIt != *candIt)
            return false;               // component mismatch
    }
    return true;                        // root fully consumed as a prefix
}

bool SYS_SameVolume(const std::string &pathA, const std::string &pathB)
{
    // Walk each argument to its nearest EXISTING ancestor first -- a
    // not-yet-created macro destination must report its parent's volume
    // (spec #9.0), not "different" by default.
    fs::path a(pathA);
    while (!fs::exists(a) && a.has_parent_path())
        a = a.parent_path();
    fs::path b(pathB);
    while (!fs::exists(b) && b.has_parent_path())
        b = b.parent_path();

    const std::string srcFolder = a.string();
    const std::string destFolder = b.string();
    if (srcFolder.empty() || destFolder.empty()) return false;

#if defined(_WIN32)
    wchar_t volSrc[MAX_PATH] = {0}, volDst[MAX_PATH] = {0};
    const std::wstring ws = fs::path(srcFolder).wstring();
    const std::wstring wd = fs::path(destFolder).wstring();
    if (!GetVolumePathNameW(ws.c_str(), volSrc, MAX_PATH)) return false;
    if (!GetVolumePathNameW(wd.c_str(), volDst, MAX_PATH)) return false;
    return _wcsicmp(volSrc, volDst) == 0;
#else
    struct stat ss{}, ds{};
    if (::stat(srcFolder.c_str(), &ss) != 0)  return false;
    if (::stat(destFolder.c_str(), &ds) != 0) return false;
    return ss.st_dev == ds.st_dev;
#endif
}
