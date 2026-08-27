#pragma once
#include <string>

// Brackets access to a macOS security-scoped bookmark (App Store sandboxed
// destinations, spec #5.5): resolve the bookmark blob to a filesystem root
// and hold [NSURL startAccessingSecurityScopedResource] for the lifetime of
// the returned handle. The matching stop call happens in
// SYS_EndScopedAccess, keyed off the SAME handle -- callers never touch the
// underlying NSURL.
//
// Non-Apple (Windows/Linux): there is no sandbox and no bookmark concept, so
// this is an inline no-op pair, right here in the header -- no .mm on those
// platforms at all. SYS_BeginScopedAccess returns a non-null sentinel
// (never null -- null would read as "begin failed" to a caller) and reports
// an EMPTY resolvedRoot, which is the "no scope" signal the app-side seam
// (PCDestinationAccess::DecideGranted) treats as "writability alone
// governs" -- see SYS_PathValidate.h's SYS_PathIsWithin for the containment
// check a non-empty root feeds.
struct SYS_ScopedAccess;

SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string &bookmarkBlob,
                                        std::string *resolvedRootOut);

// RD-D #6: the stale-aware overload. The real stale case -- the target
// moved/renamed, the OS re-issuing the bookmark -- resolves SUCCESSFULLY
// with isStale set, so the caller must re-create the bookmark when
// *outStale comes back true; the 2-arg overload above discards it and the
// bookmark quietly rots.
SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string &bookmarkBlob,
                                        std::string *resolvedRootOut,
                                        bool *outStale);
void              SYS_EndScopedAccess(SYS_ScopedAccess *access);

// RD-D #6: the PRODUCER the engine never had (the folder picker returns a
// path string; nothing anywhere created a bookmark). macOS: tries a
// security-scoped bookmark first and falls back to a plain one -- an
// UNSANDBOXED process (the app today) cannot always mint scoped bookmarks,
// and the Store build's sandbox makes the scoped arm the live one. The
// consumer above resolves either kind. Non-Apple: inline false (no
// bookmark concept).
bool SYS_CreateScopedBookmark(const std::string &utf8Path,
                              std::string *blobOut);

#if !defined(__APPLE__)

// Inline no-op implementations. An empty bookmark blob is the common case on
// EVERY platform (no sandbox in play), so the same "" == no scope contract
// holds macOS-empty-blob and non-Apple alike -- callers never branch on
// platform, only on whether *resolvedRootOut came back empty.
namespace SYS_SecurityScopedAccess_Detail
{
    // A fixed, valid, never-dereferenced sentinel address -- distinct from
    // nullptr so callers can rely on "non-null == handle to release", with
    // nothing behind it that could ever be touched (SYS_EndScopedAccess on
    // non-Apple does not dereference it either).
    inline SYS_ScopedAccess *NonNullSentinel()
    {
        static int sentinel = 0;
        return reinterpret_cast<SYS_ScopedAccess *>(&sentinel);
    }
}

inline SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string & /*bookmarkBlob*/,
                                               std::string *resolvedRootOut)
{
    if (resolvedRootOut)
        resolvedRootOut->clear();
    return SYS_SecurityScopedAccess_Detail::NonNullSentinel();
}

inline SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string &bookmarkBlob,
                                               std::string *resolvedRootOut,
                                               bool *outStale)
{
    if (outStale)
        *outStale = false;
    return SYS_BeginScopedAccess(bookmarkBlob, resolvedRootOut);
}

inline void SYS_EndScopedAccess(SYS_ScopedAccess * /*access*/)
{
    // No-op: nothing was started.
}

inline bool SYS_CreateScopedBookmark(const std::string & /*utf8Path*/,
                                     std::string *blobOut)
{
    if (blobOut)
        blobOut->clear();
    return false;
}

#endif // !defined(__APPLE__)
