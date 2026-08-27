#if defined(MACOS) || defined(__APPLE__)
#import "SYS_SecurityScopedAccess.h"
#import "DBG_Log.h"
#import <Foundation/Foundation.h>

// The opaque handle is the resolved NSURL itself, ARC-retained via
// CFBridgingRetain across the C boundary -- SYS_EndScopedAccess bridges it
// back and lets ARC release it after the matching stop call, so the URL that
// started access is EXACTLY the URL that stops it.
struct SYS_ScopedAccess
{
    NSURL *url;
    bool scoped;   // startAccessing succeeded; stop only what started
};

SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string &bookmarkBlob,
                                        std::string *resolvedRootOut)
{
    return SYS_BeginScopedAccess(bookmarkBlob, resolvedRootOut, nullptr);
}

SYS_ScopedAccess *SYS_BeginScopedAccess(const std::string &bookmarkBlob,
                                        std::string *resolvedRootOut,
                                        bool *outStale)
{
    if (resolvedRootOut)
        resolvedRootOut->clear();
    if (outStale)
        *outStale = false;

    if (bookmarkBlob.empty())
        return nullptr;

    @autoreleasepool
    {
        NSData *bookmarkData = [NSData dataWithBytes:bookmarkBlob.data()
                                               length:bookmarkBlob.size()];
        if (!bookmarkData)
        {
            LOGError("SYS_BeginScopedAccess: empty bookmark data");
            return nullptr;
        }

        BOOL isStale = NO;
        NSError *error = nil;
        NSURL *url = [NSURL URLByResolvingBookmarkData:bookmarkData
                                                options:NSURLBookmarkResolutionWithSecurityScope
                                          relativeToURL:nil
                                    bookmarkDataIsStale:&isStale
                                                  error:&error];
        bool scoped = false;
        if (url != nil)
        {
            scoped = [url startAccessingSecurityScopedResource] == YES;
        }
        if (url == nil || !scoped)
        {
            // RD-D #6: an unsandboxed process (the app today) may hold a
            // PLAIN bookmark from the fallback producer arm, or a scoped
            // one it cannot start access on. Resolve without the scope --
            // in an unsandboxed process plain file access works anyway.
            isStale = NO;
            error = nil;
            NSURL *plain = [NSURL URLByResolvingBookmarkData:bookmarkData
                                                     options:0
                                               relativeToURL:nil
                                         bookmarkDataIsStale:&isStale
                                                       error:&error];
            if (plain == nil)
            {
                LOGError("SYS_BeginScopedAccess: bookmark resolution failed: %s",
                         error ? [[error localizedDescription] UTF8String] : "(unknown)");
                return nullptr;
            }
            url = plain;
            scoped = false;
        }

        if (outStale)
            *outStale = (isStale == YES);

        SYS_ScopedAccess *access = new SYS_ScopedAccess();
        access->url = url;         // ARC keeps this alive for the struct's lifetime
        access->scoped = scoped;

        if (resolvedRootOut)
            *resolvedRootOut = std::string([[url path] UTF8String]);

        return access;
    }
}

void SYS_EndScopedAccess(SYS_ScopedAccess *access)
{
    if (!access)
        return;

    @autoreleasepool
    {
        if (access->scoped)
            [access->url stopAccessingSecurityScopedResource];
    }
    delete access;
}

bool SYS_CreateScopedBookmark(const std::string &utf8Path,
                              std::string *blobOut)
{
    if (blobOut)
        blobOut->clear();
    if (utf8Path.empty() || blobOut == nullptr)
        return false;

    @autoreleasepool
    {
        NSString *path = [NSString stringWithUTF8String:utf8Path.c_str()];
        if (path == nil)
            return false;
        NSURL *url = [NSURL fileURLWithPath:path];
        if (url == nil)
            return false;

        // Scoped first (live under the Store build's sandbox), plain as
        // the unsandboxed fallback -- the consumer resolves either.
        NSError *error = nil;
        NSData *data = [url bookmarkDataWithOptions:NSURLBookmarkCreationWithSecurityScope
                     includingResourceValuesForKeys:nil
                                      relativeToURL:nil
                                              error:&error];
        if (data == nil)
        {
            error = nil;
            data = [url bookmarkDataWithOptions:0
                 includingResourceValuesForKeys:nil
                                  relativeToURL:nil
                                          error:&error];
        }
        if (data == nil)
        {
            LOGError("SYS_CreateScopedBookmark: creation failed for '%s': %s",
                     utf8Path.c_str(),
                     error ? [[error localizedDescription] UTF8String] : "(unknown)");
            return false;
        }

        blobOut->assign((const char *)[data bytes], (size_t)[data length]);
        return true;
    }
}

#endif // defined(MACOS) || defined(__APPLE__)
