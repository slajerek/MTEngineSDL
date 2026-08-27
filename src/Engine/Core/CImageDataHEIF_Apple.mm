// ImageIO decode, for any format the platform understands.
//
// Written for HEIC and named for it, but nothing here is HEIF-specific:
// CGImageSourceCreateWithURL sniffs the container, so this decodes JPEG, PNG,
// TIFF and the rest just as well. It is now used for two things -- the HEIF
// path, and as the LAST RESORT when stb_image refuses a file the platform can
// still read (CImageData::LoadWithPlatformDecoder). Renamed to say so.
#if defined(MACOS) || defined(__APPLE__)
#import "CImageData.h"
#import "DBG_Log.h"
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Foundation/Foundation.h>

// The HDR arm: decode a gain-map HEIC to half-float extended-linear sRGB.
//
// FAILS CLOSED. Returns true ONLY when the OS is new enough, the file really
// carries a gain map, and the float decode succeeded -- any other outcome
// returns false with the image untouched, and the caller falls through to the
// ordinary 8-bit path. Same discipline as PC_HeicDecodeProbe.
//
// macOS ONLY, and macOS 14+ at that. Windows has no HEIC decode at all, and
// libheif gain-map reconstruction is out of scope, so this degrades to today's
// 8-bit decode everywhere else -- by returning false, not by #error.
bool CImageData::LoadWithImageIO_AppleHDR(const char *fileName)
{
    if (@available(macOS 14.0, *))
    {
        // nothing -- the real work is below, guarded once
    }
    else
    {
        return false;
    }

    if (@available(macOS 14.0, *))
    {
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:fileName]];
    NSDictionary *srcOpts = @{(__bridge NSString *)kCGImageSourceShouldCache: @NO};
    CGImageSourceRef src = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFDictionaryRef)srcOpts);
    if (!src)
        return false;

    // STAGE 1, and it is the cheap one: is there a gain map at all?
    //
    // BOTH aux types. kCGImageAuxiliaryDataTypeHDRGainMap is Apple's legacy
    // flavour; since iOS 18 / macOS 15 Apple writes ISO 21496-1 ("Adaptive
    // HDR") exposed as kCGImageAuxiliaryDataTypeISOGainMap instead. Checking
    // only the legacy type would silently decode a CURRENT iPhone's HDR photo
    // as SDR -- fail-closed pointing the wrong way.
    //
    // This reads the auxiliary payload only, never the full image, so the cost
    // on an ordinary SDR HEIC is a metadata read and nothing more.
    CFDictionaryRef legacyGain = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
        src, 0, kCGImageAuxiliaryDataTypeHDRGainMap);
    CFDictionaryRef isoGain = NULL;
    if (@available(macOS 15.0, *))
    {
        isoGain = CGImageSourceCopyAuxiliaryDataInfoAtIndex(
            src, 0, kCGImageAuxiliaryDataTypeISOGainMap);
    }
    const bool hasGainMap = (legacyGain != NULL) || (isoGain != NULL);
    if (legacyGain) CFRelease(legacyGain);
    if (isoGain)    CFRelease(isoGain);

    if (!hasGainMap)
    {
        CFRelease(src);
        return false;   // an ordinary SDR HEIC: the 8-bit path owns it
    }

    // STAGE 2: decode WITH the gain map applied, into HDR.
    NSDictionary *opts = @{
        (__bridge NSString *)kCGImageSourceShouldCache : @NO,
        (__bridge NSString *)kCGImageSourceDecodeRequest :
            (__bridge NSString *)kCGImageSourceDecodeToHDR,
    };
    CGImageRef img = CGImageSourceCreateImageAtIndex(
        src, 0, (__bridge CFDictionaryRef)opts);
    CFRelease(src);
    if (!img)
        return false;

    const size_t w = CGImageGetWidth(img);
    const size_t h = CGImageGetHeight(img);
    if (w == 0 || h == 0)
    {
        CGImageRelease(img);
        return false;
    }

    // Draw into half-float EXTENDED LINEAR sRGB. This is where "linear,
    // 1.0 = SDR reference white" is established -- by Core Graphics, from the
    // file's own gain map, not by arithmetic of ours.
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearSRGB);
    if (space == NULL)
    {
        CGImageRelease(img);
        return false;
    }

    u16 *halfBuf = new u16[w * h * 4];
    memset(halfBuf, 0, w * h * 4 * sizeof(u16));

    CGContextRef ctx = CGBitmapContextCreate(
        halfBuf, w, h, 16, w * 8, space,
        kCGImageAlphaPremultipliedLast | kCGBitmapFloatComponents |
        kCGBitmapByteOrder16Little);
    CGColorSpaceRelease(space);

    if (ctx == NULL)
    {
        delete [] halfBuf;
        CGImageRelease(img);
        return false;
    }

    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);

    // Un-premultiply IN FLOAT. The 8-bit path's loop divides bytes, which
    // cannot serve here -- and doing it in half rather than not at all matters
    // because a premultiplied above-white pixel is wrong in a way that only
    // shows on the display that can render it.
    {
        const size_t n = w * h;
        for (size_t i = 0; i < n; i++)
        {
            const float a = HalfToFloat(halfBuf[i * 4 + 3]);
            if (a > 0.0f && a < 1.0f)
            {
                const float inv = 1.0f / a;
                for (int c = 0; c < 3; c++)
                    halfBuf[i * 4 + c] = FloatToHalf(HalfToFloat(halfBuf[i * 4 + c]) * inv);
            }
        }
    }

    this->DeallocImage();
    this->width      = (int)w;
    this->height     = (int)h;
    this->type       = IMG_TYPE_RGBA_16F;
    this->resultData = (u8 *)halfBuf;
    // The engine's product is LINEAR (the S-5 Phase 2 contract); the app
    // applies the surface's primaries and encoding afterwards.
    this->floatIsSurfaceEncoded = false;

    // No ICC profile is recorded on this path, deliberately: the pixels are in
    // a KNOWN space by construction (extended linear sRGB, established by the
    // draw above), and the assumed-profile machinery describes the 8-bit lanes.
    // Same reasoning as the sRGB-fallback comment below -- profile and pixels
    // must agree, and here the agreement is definitional.

    float peak = 0.0f;
    {
        const size_t n = w * h;
        for (size_t i = 0; i < n; i++)
            for (int c = 0; c < 3; c++)
            {
                const float v = HalfToFloat(halfBuf[i * 4 + c]);
                if (v > peak) peak = v;
            }
    }
    this->contentMaxComponent = peak;

    LOGD("CImageData::LoadWithImageIO_AppleHDR: '%s' -> %zux%zu float, peak %.3f x SDR white",
         fileName, w, h, peak);
    return true;
    }   // @available(macOS 14.0)

    return false;
}

bool CImageData::LoadWithImageIO_Apple(const char *fileName)
{
    NSURL *url = [NSURL fileURLWithPath:
        [NSString stringWithUTF8String:fileName]];
    NSDictionary *opts = @{(__bridge NSString *)kCGImageSourceShouldCache: @NO};
    CGImageSourceRef src = CGImageSourceCreateWithURL(
        (__bridge CFURLRef)url, (__bridge CFDictionaryRef)opts);
    if (!src)
    {
        LOGError("CImageData::LoadWithImageIO_Apple: cannot open '%s'", fileName);
        return false;
    }

    CGImageRef img = CGImageSourceCreateImageAtIndex(src, 0, nil);
    CFRelease(src);
    if (!img)
    {
        LOGError("CImageData::LoadWithImageIO_Apple: decode failed '%s'", fileName);
        return false;
    }

    size_t w = CGImageGetWidth(img);
    size_t h = CGImageGetHeight(img);
    if (w == 0 || h == 0)
    {
        LOGError("CImageData::LoadWithImageIO_Apple: zero dimensions '%s'", fileName);
        CGImageRelease(img);
        return false;
    }

    this->width      = (int)w;
    this->height     = (int)h;
    this->type       = IMG_TYPE_RGBA;
    this->resultData = new u8[w * h * 4];
    memset(this->resultData, 0, w * h * 4);

    // Capture the source colour space BEFORE drawing, and draw into that same
    // space so the draw is a straight copy.
    //
    // This used to draw into CGColorSpaceCreateDeviceRGB(). That is NOT a
    // no-op: measured against real iPhone captures, a Display P3 HEIC comes
    // out with different pixels (Core Graphics converts into the device
    // space) and the profile that described them is gone -- the file's own
    // colour is destroyed on the platform where HEIC is most common. sRGB
    // sources happen to survive byte-identically, which is why the bug went
    // unnoticed.
    CGColorSpaceRef srcSpace = CGImageGetColorSpace(img);
    CFDataRef       iccData  = NULL;
    CGColorSpaceRef drawSpace = NULL;

    if (srcSpace != NULL && CGColorSpaceGetModel(srcSpace) == kCGColorSpaceModelRGB)
    {
        iccData = CGColorSpaceCopyICCData(srcSpace);
        if (iccData != NULL)
            drawSpace = CGColorSpaceRetain(srcSpace);
    }
    if (drawSpace == NULL)
        drawSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);

    CGContextRef ctx = CGBitmapContextCreate(
        this->resultData, w, h, 8, w * 4, drawSpace,
        kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);

    if (ctx == NULL && iccData != NULL)
    {
        // Core Graphics refuses some source spaces (monochrome, certain HDR /
        // HLG captures). Fall back to sRGB -- and drop the captured profile
        // WITH it. Profile and pixels must switch together: a recorded profile
        // that does not describe the pixels would be worse than the DeviceRGB
        // bug this replaces.
        CFRelease(iccData);
        iccData = NULL;
        CGColorSpaceRelease(drawSpace);
        drawSpace = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        ctx = CGBitmapContextCreate(
            this->resultData, w, h, 8, w * 4, drawSpace,
            kCGImageAlphaPremultipliedLast | kCGBitmapByteOrder32Big);
    }
    CGColorSpaceRelease(drawSpace);

    if (!ctx)
    {
        LOGError("CImageData::LoadWithImageIO_Apple: CGBitmapContext failed '%s'", fileName);
        if (iccData) CFRelease(iccData);
        CGImageRelease(img);
        delete[] this->resultData;
        this->resultData = nullptr;
        return false;
    }

    CGContextDrawImage(ctx, CGRectMake(0, 0, (CGFloat)w, (CGFloat)h), img);
    CGContextRelease(ctx);
    CGImageRelease(img);

    if (iccData != NULL)
    {
        SetIccProfile((const u8 *)CFDataGetBytePtr(iccData), (u32)CFDataGetLength(iccData));
        CFRelease(iccData);
    }
    // On the sRGB fallback path we record NO profile, which leaves the image
    // untagged -- the same state every other loader leaves an untagged file
    // in, and which CM-B resolves with the assumed-profile setting.

    // Un-premultiply alpha in-place.
    for (size_t i = 0; i < w * h; i++)
    {
        u8 a = this->resultData[i * 4 + 3];
        if (a > 0 && a < 255)
        {
            this->resultData[i * 4 + 0] = (u8)((this->resultData[i * 4 + 0] * 255u) / a);
            this->resultData[i * 4 + 1] = (u8)((this->resultData[i * 4 + 1] * 255u) / a);
            this->resultData[i * 4 + 2] = (u8)((this->resultData[i * 4 + 2] * 255u) / a);
        }
    }
    return true;
}
#endif // MACOS / __APPLE__
