#pragma once
#include <cstdint>
#include <string>

// File creation ("birth") time in Unix seconds. Returns true and fills
// *outUnixSeconds ONLY when a birth time is obtainable; returns false and
// leaves *outUnixSeconds untouched when it is NOT -- whether because the file
// is missing, the query failed, or the platform/filesystem does not record a
// birth time (macOS/Windows always do; Linux ext4 4.11+ does, several FSs
// never do). The single caller (file.created) needs only "did we get a value",
// so this is a plain bool, not a distinct "unavailable" vs "failed" status.
// `path` is UTF-8. macOS: st_birthtimespec. Windows: GetFileAttributesExW's
// CreationTime. Linux: statx(STATX_BTIME). (A4 #2.3.1; approved 2026-07-22.)
bool SYS_GetFileBirthTime(const std::string &path, int64_t *outUnixSeconds);
