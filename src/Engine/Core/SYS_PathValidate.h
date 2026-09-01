#pragma once
#include <string>
#include <string_view>

// One PATH COMPONENT (never a whole path) contains a character illegal on ANY
// supported platform: < > : " | ? * or a control char (< 0x20). Cross-platform
// by design (spec #5.2) so a rule authored on macOS still runs on Windows.
// The drive-colon exception (#5.3) is the CALLER's job -- this answers the
// component question only.
bool SYS_PathComponentHasIllegalChar(std::string_view component);

// Component equals a Windows reserved device name (CON PRN AUX NUL COM1-9
// LPT1-9), case-insensitive, matched against the text before the first '.'
// (Windows reserves CON.txt too). Spec #5.4.
bool SYS_PathIsReservedDeviceName(std::string_view component);

// Component ends in a dot or a space (illegal as a directory name on Windows).
bool SYS_PathComponentEndsWithDotOrSpace(std::string_view component);

// Strip zero-width (U+200B-200D, U+FEFF) and bidi override (U+202A-202E,
// U+2066-2069) code points from a UTF-8 string. NUL is left to the illegal-char
// check. Spec #5.2 -- these are real EXIF injection vectors.
std::string SYS_PathStripInvisibleUnicode(std::string_view s);

// The per-component byte cap (255, the floor across NTFS/APFS/ext4).
constexpr size_t SYS_kPathComponentByteCap = 255;

// Host total-path byte limit: macOS 1024, Linux 4096, Windows 32767.
size_t SYS_PathMaxBytes();

// WEAK canonicalisation (spec #5.4, load-bearing): a macro destination does
// not exist until the executor creates it, so strict canonical (which requires
// every component to exist) fails for this feature's headline case. Physically
// canonicalise the nearest EXISTING ancestor (symlinks resolved), then append
// the not-yet-existing remainder lexically normalised. `fs::weakly_canonical`
// semantics. Never fix a strict-canonical failure by skipping resolution.
std::string SYS_WeakCanonical(const std::string &path);

// Is `candidate` within `root` (or equal to it)? Both sides are WEAK-
// canonicalised first (SYS_WeakCanonical -- see above; a macro destination
// need not exist yet), then compared as a lexical prefix on COMPONENT
// boundaries: "/a" is within "/a/b/c" (true), but NOT within "/a/bc" (false --
// "bc" is not a path-separator-delimited continuation of "a"). Canonicalising
// first means a "/a/../etc" candidate resolves to "/etc" before the prefix
// check, so it correctly reports as escaping a root of "/a" (false). This is
// the sandboxed-destination scope-escape decision (spec #5.5): a security-
// scoped bookmark resolves to a root, and every planned destination must stay
// within it.
bool SYS_PathIsWithin(const std::string &root, const std::string &candidate);

// Are two paths on the same filesystem volume? Walks EACH to its nearest
// existing ancestor before the syscall, so a not-yet-created destination
// reports its parent's volume (spec #9.0) -- exact, not a heuristic. POSIX:
// stat().st_dev of the ancestors. Windows: GetVolumePathNameW of the ancestors.
// Relocated from the photo app CActionLibrary.cpp:124 (was file-local, misreported
// every macro destination as cross-volume).
bool SYS_SameVolume(const std::string &pathA, const std::string &pathB);
