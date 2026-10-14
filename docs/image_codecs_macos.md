# macOS Image Codec Bundle

MTEngineSDL builds TIFF, WebP, AVIF, and RAW image codec dependencies into one macOS static archive for app targets that link `libMTEngineSDL.a`.

## Why This Exists

`libMTEngineSDL.a` is a static library, so Xcode app targets do not automatically inherit codec libraries used by engine object files. Apps must link an archive that provides the codec symbols after they link `libMTEngineSDL.a`.

## Build Script

The Xcode project runs `platform/MacOS/build-image_codecs.sh` in the `Build image_codecs` pre-build phase, directly after the `Build jpeg` phase (`platform/MacOS/build-jpeg.sh`, libjpeg-turbo), whose outputs are among its inputs. The codec script refuses to run without libjpeg-turbo's archive and prefixes ("run build-jpeg first").

The script downloads pinned upstream sources into:

- `other/lib/image-codecs/downloads/`
- `other/lib/image-codecs/src/`

It builds per-architecture static libraries under `other/lib/image-codecs/build/`, stages headers/libs under `other/lib/image-codecs/install/`, and packages the final universal archive at:

- `$MT_CAPS_LIBS_DIR/libmt_image_codecs.a`

The cache stamp is:

- `$MT_CAPS_LIBS_DIR/libmt_image_codecs.stamp`

## Build tool: NASM (libjpeg-turbo's x86 SIMD)

`build-jpeg.sh` assembles libjpeg-turbo's x86_64 SIMD with NASM -- a build
tool, never linked. Install it from MacPorts: `sudo port install nasm` (it lands
in `/opt/local/bin`, which both `build-jpeg.sh` and `build-image_codecs.sh`
search first). Without it a development build warns once and builds the x86_64
slice without SIMD; a final build (`./build-macos.sh --prod`, which exports
`MT_PROD_BUILD=1`) fails instead, as does any build whose requested SIMD turbo's
configure turned off (read from the installed `jconfig.h`). The other
platforms: Linux `sudo apt install nasm` (Fedora `sudo dnf install nasm`) for
x86_64, nothing for aarch64; Windows x64 NASM on PATH or in MSYS2
(`pacman -S nasm`, `C:\msys64\usr\bin\nasm.exe` or `$env:MSYS2_ROOT`), nothing
for ARM64.

## Included Codecs

- `tiff-4.7.1`
- `libwebp-1.6.0`
- `libavif-1.4.2`
- `libgav1-v0.20.0`
- `LibRaw-0.22.1`
- `libultrahdr-2.0.2` (the ISO 21496-1 gain-map JPEG; against the engine's libjpeg-turbo; licences staged in `image-codecs/licenses/libultrahdr/`)
- `libjpeg-turbo-3.2.0` -- NOT built here: `platform/MacOS/build-jpeg.sh` (its own phase, which runs first) builds it into `libmt_jpeg.a` plus `$(MT_CAPS_LIBS_DIR)/jpeg/include`, and per-arch prefixes under `~/.cache/mtengine/_deps/work/jpeg/install-<arch>` that LibRaw and libultrahdr build against. This script merges `libmt_jpeg.a` into the bundle -- the stub bundle too, because the engine's `JPEGWriter` needs `jpeg_*` on every configuration -- and its stamp joins the bundle's, so a new turbo rebuilds the bundle

The script verifies the final archive is universal `arm64` + `x86_64` and contains the engine-used TIFF, WebP, AVIF, and LibRaw symbols, the libultrahdr entry points (`uhdr_encode`, `uhdr_decode`, `is_uhdr_image`), and libjpeg-turbo's `jpeg_skip_scanlines` (turbo-only) and `jtransform_execute_transform` (transupp).

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
