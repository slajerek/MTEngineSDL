# macOS Image Codec Bundle

MTEngineSDL builds TIFF, WebP, AVIF, and RAW image codec dependencies into one macOS static archive for app targets that link `libMTEngineSDL.a`.

## Why This Exists

`libMTEngineSDL.a` is a static library, so Xcode app targets do not automatically inherit codec libraries used by engine object files. Apps must link an archive that provides the codec symbols after they link `libMTEngineSDL.a`.

## Build Script

The Xcode project runs `platform/MacOS/build-image_codecs.sh` in the `Build image_codecs` pre-build phase.

The script downloads pinned upstream sources into:

- `other/lib/image-codecs/downloads/`
- `other/lib/image-codecs/src/`

It builds per-architecture static libraries under `other/lib/image-codecs/build/`, stages headers/libs under `other/lib/image-codecs/install/`, and packages the final universal archive at:

- `$MT_CAPS_LIBS_DIR/libmt_image_codecs.a`

The cache stamp is:

- `$MT_CAPS_LIBS_DIR/libmt_image_codecs.stamp`

## Included Codecs

- `tiff-4.7.1`
- `libwebp-1.6.0`
- `libavif-1.4.2`
- `libgav1-v0.20.0`
- `LibRaw-0.22.1`

The script verifies the final archive is universal `arm64` + `x86_64` and contains the engine-used TIFF, WebP, AVIF, and LibRaw symbols.

## Headers

Public headers are staged in:

- `other/lib/image-codecs/include/`

The macOS Xcode target includes this path before Homebrew/MacPorts include directories so engine image loaders compile against the vendored headers.

## Consumer Apps

macOS apps that link MTEngineSDL must link:

- `$MT_CAPS_LIBS_DIR/libmt_image_codecs.a`

The archive must appear after `libMTEngineSDL.a` in the app target's Frameworks/link phase so the linker sees unresolved engine codec references before scanning the codec archive.

Do not add direct app linker flags such as `-ltiff`, `-lwebp`, `-lwebpdemux`, `-lavif`, or `-lraw` for these codecs on macOS.

## Manual Build

```bash
./platform/MacOS/build-image_codecs.sh
```

To clean generated build/install outputs while keeping downloaded archives:

```bash
./platform/MacOS/build-image_codecs.sh clean
```
