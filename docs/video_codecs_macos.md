# macOS Video Codec Bundle

MTEngineSDL builds VPX and Opus video playback dependencies into one macOS static archive for app targets that compile or link engine video code.

## Why This Exists

LightHeroes currently compiles MTEngineSDL video sources directly from `src/Engine/Video/`. Those sources use libvpx for VP9 decode and libopus for Opus audio decode.

The previous macOS app project linked arm64-only archives from `other/lib/libvpx/macos-arm64/` and `other/lib/libopus/macos-arm64/`. That works for arm64 but fails for `x86_64` because the linker ignores the arm64 objects and leaves VPX/Opus symbols unresolved.

## Build Script

The Xcode project runs `platform/MacOS/build-video_codecs.sh` in the `Build video_codecs` pre-build phase.

The script downloads pinned upstream sources into:

- `other/lib/video-codecs/downloads/`
- `other/lib/video-codecs/src/`

It builds per-architecture static libraries under `other/lib/video-codecs/build/`, stages libraries under `other/lib/video-codecs/install/`, and packages the final universal archive at:

- `$MT_CAPS_LIBS_DIR/libmt_video_codecs.a`

The cache stamp is:

- `$MT_CAPS_LIBS_DIR/libmt_video_codecs.stamp`

### FFmpeg is a second, dynamic output

FFmpeg is deliberately **not** merged into `libmt_video_codecs.a` -- shipping it
as a dylib is what discharges the LGPL obligation. The five libraries are built
into `other/lib/video-codecs/install/lib/` and **also staged next to the
archive**:

- `$MT_CAPS_LIBS_DIR/lib{avutil,avcodec,avformat,swscale,swresample}.dylib`
  (the SONAME symlink is copied as a symlink, since the install names were
  normalised to `@rpath/<soname>`)

An app that compiles `CVideoSourceFFmpeg` therefore needs BOTH halves of this
script's output. The freshness check covers both: a valid stamp with no staged
dylib triggers a rebuild rather than a silent skip.

Consumers link them by explicit path -- `$(MT_CAPS_LIBS_DIR)/libavcodec.dylib`
in `OTHER_LDFLAGS`, not `-lavcodec`. MEASURED, and the reason is not style: with
`$(HOMEBREW_PREFIX)/lib` on the search path, `-lavcodec` resolved to Homebrew's
FFmpeg 63 instead of this build's 61, and the linked binary carried absolute
`/opt/homebrew/...` paths to libraries nobody audited. `ld` records the file's
own `LC_ID_DYLIB`, so linking by path still yields `@rpath/libavcodec.61.dylib`.

The app then copies them into `Contents/Frameworks` and sets
`LD_RUNPATH_SEARCH_PATHS = @executable_path/../Frameworks`. A Copy Files phase
cannot be used for this: it needs `PBXFileReference`s, and a file reference is a
fixed path -- which is exactly what `$MT_CAPS_LIBS_DIR` no longer has.

## Included Codecs

- `libvpx-1.15.2`
- `opus-1.5.2`

The script verifies the final archive is universal `arm64` + `x86_64` and contains the VPX/Opus symbols used by `CVideoPlayer`.

## Headers

LightHeroes and engine video sources continue to include the checked-in headers from:

- `src/Engine/Libs/libvpx/`
- `src/Engine/Libs/libopus/`

The generated `other/lib/video-codecs/` tree is build/cache output, not an app header dependency.

## Consumer Apps

macOS apps that compile or link MTEngineSDL video code must link:

- `$MT_CAPS_LIBS_DIR/libmt_video_codecs.a`

The archive must appear after `libMTEngineSDL.a` in the app target's Frameworks/link phase, matching the other engine-provided static bundles.

Do not link the old arm64-only `other/lib/libvpx/macos-arm64/libvpx.a` or `other/lib/libopus/macos-arm64/libopus.a` archives directly in app targets.

## Manual Build

```bash
./platform/MacOS/build-video_codecs.sh
```

To clean generated build/install outputs while keeping downloaded archives:

```bash
./platform/MacOS/build-video_codecs.sh clean
```
