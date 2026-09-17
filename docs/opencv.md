# OpenCV

Three capabilities, all **off by default**, and they are the only ones in the
vocabulary that are: turning them on downloads 154 MB of source tarballs and
builds a second computer-vision library, which most hosts never call.

| capability | what it adds |
|---|---|
| `MT_CAP_OPENCV` | OpenCV 5.0.0 `core`, `imgproc`, `geometry`, `flann`, `features`, plus the engine's `CImageData` <-> `cv::Mat` bridge |
| `MT_CAP_OPENCV_NONFREE` | `opencv_contrib`'s `xfeatures2d` built with `OPENCV_ENABLE_NONFREE`: SURF, FREAK, BRIEF, DAISY. Implies `MT_CAP_OPENCV` |
| `MT_CAP_YOLO` | OpenCV's `dnn` module, an ONNX object detector, a model catalogue and its download URLs. Implies `MT_CAP_OPENCV`, `MT_CAP_HTTP` and `MT_CAP_HTTPS` |

**SIFT is not in the non-free half.** Its patent expired in 2020, it moved
into the main repository in 4.4, and in 5.0 it lives in `features`. So the
base capability already gives SIFT, ORB, BRISK, FAST, MSER and
`SimpleBlobDetector`. What the second capability adds is SURF, which is still
patented, and the descriptors that moved to contrib in 5.0.

A commercial build (`--tier commercial`) resolves `MT_CAP_OPENCV_NONFREE` to
0 whatever the manifest says, because a distributed binary must not carry a
patented algorithm. `tools/appbuild/scan-capability-symbols.sh` proves the
result has no `xfeatures2d` symbol.

## Using it

An app turns the capability on in its `mtengine.caps`:

```
MT_CAP_OPENCV=1
MT_CAP_OPENCV_NONFREE=0
```

and then includes OpenCV headers directly. Gate app code on `MT_CAP_OPENCV`
(compile-time removal) and `MT_HAS_CAP(MT_CAP_OPENCV)` (the runtime query a
menu uses to grey an entry rather than hide it).

### Images cross the boundary through `MT_OpenCV.h`, and nothing else

`src/Engine/OpenCV/MT_OpenCV.h` is the whole engine-side surface:

```cpp
bool        MT_OpenCV_IsAvailable();                  // both branches
const char *MT_OpenCV_Version();                      // both branches

cv::Mat     MT_OpenCV_MatFromImageData(const CImageData *image);
CImageData *MT_OpenCV_ImageDataFromMat(const cv::Mat &mat);
bool        MT_OpenCV_BlitMatToImageData(const cv::Mat &rgba, CImageData *dst);
```

Three things about it are load-bearing.

**Channel order is RGBA end to end.** An engine RGBA image becomes a
`CV_8UC4` Mat holding R, G, B, A in that order, never BGR, so callers pass
the RGBA colour codes (`cv::COLOR_RGBA2GRAY`). OpenCV's BGR convention is an
artefact of the `imgcodecs` module, which this build does not contain.

**There is no `imgcodecs`, so there is no `cv::imread`.** Images arrive
through `CImageData`, which already decodes every format the build supports
and honours the engine's colour management. An app that wants OpenCV to see a
JPEG turns `MT_CAP_PHOTO_CODECS` on as well; that is a separate decision, not
an implication, so a vision capability never drags 40 MB of codecs in behind
it.

**Copies honour `mat.step`.** A ROI or a padded allocation has a stride
larger than `cols * elemSize`, and a single `memcpy` of the whole buffer
shears the image by exactly that difference. `MT_OpenCV_ImageDataFromMat`
copies row by row for that reason.

`MT_OpenCV_BlitMatToImageData` exists for the live case: `CGuiViewMovingPaneImage`
keeps a power-of-two RGBA raster and displays its top-left corner, so a result
is never the size of the buffer it goes into. The blit writes the corner and
refuses a source larger than the destination.

Supported conversions:

| `CImageData` | `cv::Mat` |
|---|---|
| `IMG_TYPE_RGBA` | `CV_8UC4` |
| `IMG_TYPE_RGB` | `CV_8UC3` (widened to RGBA on the way back) |
| `IMG_TYPE_GRAYSCALE` | `CV_8UC1` |
| `IMG_TYPE_GRAYSCALE_16BIT` | `CV_16UC1` |
| `IMG_TYPE_RGBA_16BIT` | `CV_16UC4` |
| `IMG_TYPE_RGBA_16F` | `CV_16FC4` |

`IMG_TYPE_GRAYSCALE_32BIT`, `IMG_TYPE_CIELAB` and `IMG_TYPE_GPU_COMPRESSED`
are refused with a logged error rather than mangled.

### Consuming it in a build

| platform | headers | archive |
|---|---|---|
| macOS | `$(MT_CAPS_LIBS_DIR)/opencv/include` on `HEADER_SEARCH_PATHS` | `-lmt_opencv` on `OTHER_LDFLAGS` |
| Linux | `${MT_CAPS_LIBS_DIR}/opencv/include` | `${MT_CAPS_LIBS_DIR}/libmt_opencv.a`, added under a plain `if(MT_ENABLE_OPENCV)` |
| Windows | `$(MTCapsLibsDir)\opencv\include` in the project AND in `MTEngineApp.targets` (the IDE path re-adds it once the property has a value) | `mt_opencv.lib` in `AdditionalDependencies` |

On Linux, use `if(MT_ENABLE_OPENCV)`, not the `if(NOT DEFINED X OR X)` idiom
the default-1 capabilities use. That idiom spells "absent means on", which is
the opposite of what this capability promises.

## What the acquisition scripts do

`platform/{MacOS,Linux}/build-opencv.sh`, `platform/Windows/build-opencv.ps1`.
One build unit, keyed by both capabilities, because contrib is not a separate
configure: with the non-free capability on, the same CMake run gains a module.

Two configure traps are encoded there, and both were measured rather than
anticipated:

**`OPENCV_EXTRA_MODULES_PATH` names a staged directory holding only
`xfeatures2d`.** CMake scans every directory under that path and runs each
module's own dependency detection, even for modules `BUILD_LIST` excludes.
Pointing it at the whole contrib tree made the configure download
`ade/archive/v0.1.2e.zip` and detect system freetype, harfbuzz, HDF5, Eigen,
OGRE and VTK.

**`WITH_UNIFONT=OFF`.** OpenCV 5's imgproc has a new TrueType text engine
whose built-in font is downloaded at configure time. The option defaults on.

With both, plus `OPENCV_SKIP_FEATURES2D_DOWNLOADING=ON` for the contrib
descriptors, the configure downloads nothing but the two pinned tarballs.

`flann` is in the merge list although nothing asks for it: `features`
declares it as an optional dependency and `BUILD_LIST` resolution enables it.

zlib comes from the engine's own vendored copy, compiled into a helper
archive for OpenCV's configure and merged into the bundle. Merged, not left
to the engine archive, because Linux GNU ld is single-pass and every app
links the engine archive before the capability archives; `build-image_codecs.sh`
does the same for the same reason.

macOS builds each architecture separately and `lipo`s them, because OpenCV
chooses its SIMD baseline and dispatch set per architecture at configure time.

Measured on a 12-core Apple silicon machine: 39 s per architecture, 16.6 MB
per architecture, 33 MB universal. With `MT_CAP_YOLO` on, the same unit gains
`dnn` and `protobuf`: 97 MB universal.

Three more traps live in the dnn mode, and the first two were found by building
the SECOND architecture rather than the first:

**Cross-compilation has to be told.** MLAS, the matrix library dnn brings in,
picks its assembly kernels from `CMAKE_SYSTEM_PROCESSOR`, which defaults to
the host. The x86_64 pass on an Apple-silicon machine tried to assemble the
aarch64 kernels. The script passes `-DCMAKE_SYSTEM_NAME` and
`-DCMAKE_SYSTEM_PROCESSOR` whenever the target differs from `uname -m`.

On Windows the same variable is wrong for a second reason, and it costs more
than kernels. With a Visual Studio generator CMake does **not** derive
`CMAKE_SYSTEM_PROCESSOR` from `-A <platform>`; it copies the host value -- and
the host value is the architecture of the **cmake binary**, not of the machine.
Measured on a Windows-on-ARM box, the x64 CMake in `C:\Program Files\CMake`
runs emulated and reports `CMAKE_HOST_SYSTEM_PROCESSOR = AMD64`, so an
`-A ARM64` configure produced `X86_64=1`, `CPU_BASELINE=SSE3` and a
`CPU_DISPATCH` of `SSE4_1..AVX512_SKX`. NEON was in neither, so
`conv_block.neon.cpp` was generated and never compiled -- while `clang-cl`,
actually targeting ARM64, defines `__ARM_NEON`, so OpenCV's own `CV_NEON` is 1
and `convolution.cpp` emits the calls regardless. It surfaced at the *app's*
link, two units downstream: `undefined symbol: cv::dnn::opt_NEON::convBlock_F32`
and `convBlockMR1_F32`, from `mt_opencv.lib(convolution.obj)`.

The mismatch cuts both ways -- on that same host an `-A x64` build is right only
*because* the emulated cmake says AMD64, and would be wrong under a native ARM64
cmake -- so `build-opencv.ps1` does not consult the host at all. It passes
`-DOPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION=ON` (upstream's documented hand-off
for exactly this) plus `AARCH64`/`X86_64`/`X86` taken from `$Platform`.
`CPU_BASELINE` and `CPU_DISPATCH` are `CACHE STRING` defaults that OpenCV sets
only when unset, and the generator, platform and toolset all still match, so
`Reset-MTStaleCMakeCache` has no reason to fire: the script therefore wipes the
build directory itself when the cached `CPU_BASELINE` disagrees with the target.

**The install tree is wiped before each install.** One install directory
serves every configuration of this unit, so a previous YOLO-on build left
`dnn.hpp` where a YOLO-off build then staged it -- headers for a module that
was not in the archive.

**MLAS IS NOT BUILT AT ALL ON WINDOWS/x86, and `build-opencv.ps1` patches
upstream to say so.** OpenCV 5.0.0's vendored MLAS assumes a GCC driver twice
over, and neither assumption survives `clang-cl`:

- It force-includes `<cstring>` as the two tokens `-include` + `cstring`, behind
  `CMAKE_CXX_COMPILER_ID MATCHES "Clang"`. clang-cl *reports* Clang and *drives*
  like MSVC, so it takes that branch and then ignores `-include` as an unknown
  argument, leaving `cstring` as a positional input:
  `no such file or directory: 'cstring'`. (`/FIcstring` is the spelling that
  works.) Upstream guards its other force-include, `platform.cpp`/`unistd.h`,
  with `AND NOT WIN32`; not this one.
- Past that, the x86_64 SGEMM kernels are GNU-syntax `.S` files with GCC flags
  (`-msse2`/`-mavx`). The MSVC driver cannot assemble them, and the vendored
  tree ships **no MASM equivalent for x86 or x86_64** -- only `arm64` and
  `arm64ec` have `.asm`. MSBuild produced no object and it surfaced two targets
  later, at `opencv_dnn`'s lib step:
  `SgemmKernelSse2.obj: no such file or directory`.

The second has no fix short of writing the MASM kernels, so the script applies
one idempotent patch to the extracted tree routing MSVC/x86 into **the skip
branch upstream already provides** for "no ASM compiler available". The summary
then reads `DNN MLAS: NO (GNU-syntax .S kernels unsupported by the MSVC driver
on AMD64)` and dnn falls back to its built-in SGEMM -- slower GEMM, same
results.

**On Windows/ARM64 that guard fires too, and the message is misleading.**
MLAS reads `CMAKE_SYSTEM_PROCESSOR` directly, so it is not covered by the
`OPENCV_SKIP_SYSTEM_PROCESSOR_DETECTION` hand-off above: on a Windows-on-ARM
host it still sets `MLAS_X86_64` and the patch skips MLAS on a target whose
`.asm` kernels *are* present. That costs an optimisation, not a link, and
dnn's built-in SGEMM carries it -- so it is left alone deliberately rather
than fixed together with the NEON defect, since enabling it would build the
`arm64` MASM kernels for the first time. An earlier note here said ARM64 was
untouched by the guard; that was inferred, not measured, and it is wrong.

The patch is anchored on `check_language(ASM)` / `if(CMAKE_ASM_COMPILER)` and
**throws if that anchor is gone**, so a version bump surfaces as a build error
rather than a silently unpatched tree. `$scriptSha` is in the stamp, so editing
it re-runs the build. Found 2026-09-07, the first time the dnn mode was ever
built on Windows.

A report for OpenCV's tracker, with the pristine line numbers, the isolated
`clang-cl` measurement and a minimal repro, is written up in
`MTEngineSDLDummyApp/claude/upstream/opencv-5.0.0-mlas-windows-msvc.md`. **If it
is accepted upstream, this patch is what comes out** -- check it against the
version bump rather than carrying it forever.

## Object detection (`MT_CAP_YOLO`)

Turning it on adds `dnn` and `protobuf` to the same build unit. The archive
goes from 35 MB to 97 MB, which is why it is a capability rather than part of
`MT_CAP_OPENCV`.

**No model weights ship**, here or in any host. `CYoloModelManager` holds a
catalogue of https URLs and a host downloads what the user chooses, exactly as
`MT_CAP_LLM` does with GGUF files.

### The catalogue is permissive-only, and a test enforces it

Every Ultralytics YOLO -- v5, v8, 11 and YOLO26 -- ships under **AGPL-3.0**,
which obliges the whole application that links it to be open-sourced. A row in
the shipped catalogue would put that obligation on every host that copies the
template, silently, at the moment a user clicked Download.

So the catalogue carries Apache-2.0 models only, each row carries a `licence`
field, and `CTestYoloDetector` fails the build's test suite if a row's licence
is outside `{Apache-2.0, MIT, BSD-3-Clause, BSD-2-Clause}` or its URL is not
`https://huggingface.co/...`. A user who holds an Ultralytics licence, or
whose own application is AGPL, reaches those models through the host's Browse
button: the choice and the obligation are theirs, and the file never came from
here.

### The head is decoded, not read

`MT_YoloDetector` handles a **raw** YOLOX head, shaped `1 x N x (5 + classes)`.
The evidence from the model itself: 80 `Sigmoid` nodes and no `Exp` node, and
`N = 80^2 + 40^2 + 20^2 = 8400` at a 640 input, which is a three-level pyramid
at strides 8, 16 and 32. So objectness and class scores arrive activated and
the box values do not:

```
cx = (raw_x + gx) * s      cy = (raw_y + gy) * s
w  = exp(raw_w) * s        h  = exp(raw_h) * s
```

Skip that and every box lands near the top-left corner about a pixel across,
which reads as "the model found nothing" rather than as a bug. That is the one
failure this capability is most likely to ship, so `CTestYoloDetector` runs the
head at a **zero** confidence threshold -- all 8400 anchors become candidates
-- and asserts the geometry: centres spread across the frame, sides tens of
pixels. Measured on YOLOX-S: 8392 candidates, furthest centre (636, 557),
largest side 620 px.

`LoadModel` probes the output shape once at load and refuses a shape this
decoder does not understand, rather than returning plausible nonsense later.

### Threading

`cv::dnn::Net` keeps mutable state across `forward()`, so **one detector
belongs to one thread**. A host that detects on a worker constructs its
detector there and lets nothing else hold a pointer to it. The template's
`CViewYoloDemo` is the worked example.

## Changing the version

Four values and one word:

1. `OPENCV_VERSION` in all three scripts.
2. `OPENCV_SHA256` and `OPENCV_CONTRIB_SHA256` in all three scripts.
3. The `version` and `provenance` fields of both dependency rows in
   `tools/mtcaps/vocabulary.json`, then regenerate `docs/CAPABILITIES.md`.
4. `BUILD_LIST`, if the module set changes. OpenCV 5 renamed `features2d` to
   `features` and split `calib3d` into `calib`, `stereo`, `geometry` and
   `ptcloud`, so a version bump is the moment to check the module names still
   exist.

The stamp carries both hashes and the non-free flag, so changing any of them
rebuilds rather than reusing a stale archive.
