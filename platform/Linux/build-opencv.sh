#!/usr/bin/env bash
set -euo pipefail

# Build OpenCV 5 static libs via CMake for Linux, then merge them into a single
# libmt_opencv.a. Modules: core, imgproc, geometry, flann, features, plus
# xfeatures2d when MT_ENABLE_OPENCV_NONFREE is on.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). The unit follows BOTH OpenCV keys, because contrib is
# not a separate configure: with NONFREE on, the same CMake run gains a module.
mt_caps_use_store "${MT_STORE_OPENCV:-}" opencv

OUT_LIB="$OUT_LIB_DIR/libmt_opencv.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_opencv.stamp"
mkdir -p "$OUT_LIB_DIR"

OPENCV_VERSION="5.0.0"
OPENCV_URL="https://github.com/opencv/opencv/archive/refs/tags/${OPENCV_VERSION}.tar.gz"
OPENCV_SHA256="b0528f5a1d379d59d4701cb28c36e22214cc51cf64594e5b56f2d3e6c0233095"
CONTRIB_URL="https://github.com/opencv/opencv_contrib/archive/refs/tags/${OPENCV_VERSION}.tar.gz"
CONTRIB_SHA256="c58f6344170c39abf187c56f3843b59cab1fd3e89cf19ba2ce25dc061659b27f"

SCRIPT_SHA="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  SCRIPT_SHA="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
elif command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}

# ---------------------------------------------------------------------------
# THE CAPABILITY GATE, and it is the only one in the tree that defaults OFF.
#
# Every other dependency script spells `${MT_ENABLE_X:-1}`, so a bare engine
# clone with no manifest still builds everything. OpenCV is the exception the
# vocabulary records: turning it on downloads 154 MB and builds a second
# computer-vision library, and three of the five apps never call it. So absent
# means OFF here, and `!= "1"` rather than `== "0"` says that in one line.
#
# It comes BEFORE the download on purpose, and it emits a STUB ARCHIVE rather
# than skipping: a PBXBuildFile cannot be conditioned on a build setting, so
# simply not producing the archive turns the saving into a link error. A stub
# carries no library symbols, so the `nm` proof still tells on from off.
# ---------------------------------------------------------------------------
if [[ "${MT_ENABLE_OPENCV:-0}" != "1" ]]; then
  STUB_STAMP="disabled:${SCRIPT_SHA}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "mt_opencv"
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  # The headers go too. Leaving a previous build's opencv2/ behind would let a
  # translation unit include <opencv2/core.hpp> in a build that cannot link it,
  # and fail at the link rather than at the #include.
  rm -rf "$OUT_LIB_DIR/opencv"
  exit 0
fi

NONFREE="${MT_ENABLE_OPENCV_NONFREE:-0}"

# THE DNN MODE (MT_CAP_YOLO). Object detection is OpenCV's own dnn module, so
# it is this same configure with one more module and the protobuf that ONNX
# parsing needs. WITH_ONNXRUNTIME stays OFF: it is the one dnn option that
# would make the configure download something.
#
# No cross-compilation dance here, unlike macOS: this script builds one native
# architecture, so MLAS reads the right CMAKE_SYSTEM_PROCESSOR by default.
YOLO="${MT_ENABLE_YOLO:-0}"
if [[ "$YOLO" == "1" ]]; then
  DNN_ARGS=(-DWITH_PROTOBUF=ON -DBUILD_PROTOBUF=ON -DWITH_ONNXRUNTIME=OFF)
else
  DNN_ARGS=(-DWITH_PROTOBUF=OFF -DBUILD_PROTOBUF=OFF)
fi

# Stamp-based caching. The nonfree flag is IN the stamp: the two capabilities
# produce different archives from the same script and the same sources, so a
# stamp without it would call an OpenCV-only archive up to date for a build
# that asked for SURF.
STAMP_VALUE="enabled:${OPENCV_SHA256:0:12}:${CONTRIB_SHA256:0:12}:nonfree=${NONFREE}:yolo=${YOLO}:${SCRIPT_SHA}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$STAMP_VALUE" ]]; then
    exit 0
  fi
fi

WORK_DIR="$(mt_caps_work_dir opencv)"
DOWNLOAD_DIR="$WORK_DIR/downloads"
SRC_DIR="$WORK_DIR/src"
EXTRA_DIR="$WORK_DIR/extra-modules"
mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR"

download_archive() {
  local name="$1" url="$2" sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"

  if [[ ! -f "$archive" ]]; then
    echo "Downloading $name..."
    curl -L --fail --show-error "$url" -o "$archive"
  fi

  local actual_sha
  actual_sha="$(sha256_of "$archive")"
  if [[ "$actual_sha" != "$sha256" ]]; then
    echo "ERROR: checksum mismatch for $archive" >&2
    echo "Expected: $sha256" >&2
    echo "Actual:   $actual_sha" >&2
    exit 1
  fi

  if [[ ! -d "$SRC_DIR/$name" ]]; then
    echo "Extracting $name..."
    tar -xzf "$archive" -C "$SRC_DIR"
  fi
}

download_archive "opencv-$OPENCV_VERSION" "$OPENCV_URL" "$OPENCV_SHA256"

EXTRA_ARGS=()
MODULES="core,imgproc,geometry,features"
if [[ "$YOLO" == "1" ]]; then
  MODULES="$MODULES,dnn"
fi
if [[ "$NONFREE" == "1" ]]; then
  download_archive "opencv_contrib-$OPENCV_VERSION" "$CONTRIB_URL" "$CONTRIB_SHA256"

  # ONLY xfeatures2d, copied into a directory of its own.
  #
  # CMake scans EVERY directory under OPENCV_EXTRA_MODULES_PATH and runs each
  # module's own dependency detection, including for modules that BUILD_LIST
  # excludes. Measured: pointing this at the whole contrib modules tree made
  # the configure download ade/archive/v0.1.2e.zip (gapi wants it) and detect
  # system freetype, harfbuzz, HDF5, Eigen, OGRE and VTK -- none of which we
  # build, all of which would have become silent host dependencies.
  rm -rf "$EXTRA_DIR"
  mkdir -p "$EXTRA_DIR"
  cp -R "$SRC_DIR/opencv_contrib-$OPENCV_VERSION/modules/xfeatures2d" "$EXTRA_DIR/"

  MODULES="$MODULES,xfeatures2d"
  EXTRA_ARGS+=(
    -DOPENCV_EXTRA_MODULES_PATH="$EXTRA_DIR"
    -DOPENCV_ENABLE_NONFREE=ON
    # xfeatures2d otherwise fetches the BoostDesc and VGG descriptor data at
    # configure time. Without them those two descriptors report "not
    # available"; SURF, FREAK, BRIEF and DAISY are unaffected.
    -DOPENCV_SKIP_FEATURES2D_DOWNLOADING=ON
  )
fi

# PARALLELISM: every core unless MT_BUILD_JOBS says otherwise -- the same
# variable, with the same meaning, that app-build-linux.sh and the other
# producers read. It matters most HERE: OpenCV is the largest and most
# memory-hungry dependency in the tree, so a box with little RAM per core
# loses a compiler to the OOM killer in this build before any other, and
# make reports that as a plain failure naming no cause. Until 2026-09-07
# this line read $(nproc) alone, so --jobs N on the prod wrappers reached
# the app and the codec bundles and silently missed OpenCV.
JOBS="${MT_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"

# ---------------------------------------------------------------------------
# The engine's OWN vendored zlib, compiled per architecture.
#
# OpenCV core needs zlib. It must not bring a second one: BUILD_ZLIB=OFF plus
# these two paths point its configure at the same sources the engine compiles
# into libMTEngineSDL.a.
#
# And the helper IS merged into the bundle below, which looks like duplication
# and is not. Linux GNU ld is single-pass and every app links libMTEngineSDL.a
# BEFORE the capability archives, so a zlib reference from an OpenCV object
# would never be resolved from the engine archive that came earlier on the
# line. build-image_codecs.sh merges its copy for exactly this reason. The
# duplicate costs 100 KB and no correctness: a linker pulls an archive member
# only for a symbol that is still undefined.
# ---------------------------------------------------------------------------
build_zlib_helper() {
  local prefix="$1"
  local zlib_src="$ROOT_DIR/src/Engine/Libs/zlib"
  local objdir="$WORK_DIR/zobj"

  [[ -f "$prefix/lib/libz.a" ]] && return 0

  mkdir -p "$objdir" "$prefix/lib" "$prefix/include"
  local f base
  for f in "$zlib_src"/*.c; do
    base="$(basename "$f" .c)"
    cc -c -O2 -fPIC -I"$zlib_src" -o "$objdir/$base.o" "$f"
  done
  ar rcs "$prefix/lib/libz.a" "$objdir"/*.o
  cp "$zlib_src/zlib.h" "$zlib_src/zconf.h" "$prefix/include/"
}

PREFIX="$WORK_DIR/helper-linux"
BUILD_DIR="$WORK_DIR/build-linux"
INSTALL_DIR="$WORK_DIR/install-linux"

build_zlib_helper "$PREFIX"

mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$SRC_DIR/opencv-$OPENCV_VERSION"

cmake -S "$SRC_DIR/opencv-$OPENCV_VERSION" -B "$BUILD_DIR" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
  -DBUILD_LIST="$MODULES" \
  "${EXTRA_ARGS[@]+"${EXTRA_ARGS[@]}"}" \
  -DBUILD_TESTS=OFF -DBUILD_PERF_TESTS=OFF -DBUILD_EXAMPLES=OFF \
  -DBUILD_opencv_apps=OFF -DBUILD_JAVA=OFF -DBUILD_OBJC=OFF \
  -DBUILD_opencv_python2=OFF -DBUILD_opencv_python3=OFF -DBUILD_opencv_js=OFF \
  -DOPENCV_GENERATE_PKGCONFIG=OFF -DINSTALL_C_EXAMPLES=OFF \
  -DWITH_FFMPEG=OFF -DWITH_AVFOUNDATION=OFF -DWITH_MSMF=OFF -DWITH_DSHOW=OFF \
  -DWITH_V4L=OFF -DWITH_GTK=OFF -DWITH_QT=OFF -DWITH_1394=OFF -DWITH_GSTREAMER=OFF \
  -DWITH_OPENCL=OFF -DWITH_IPP=OFF -DWITH_TBB=OFF -DWITH_OPENMP=OFF \
  -DWITH_EIGEN=OFF -DWITH_LAPACK=OFF \
  "${DNN_ARGS[@]}" \
  -DWITH_ITT=OFF -DWITH_CUDA=OFF -DWITH_VA=OFF -DWITH_OPENEXR=OFF \
  -DWITH_JPEG=OFF -DWITH_PNG=OFF -DWITH_TIFF=OFF -DWITH_WEBP=OFF \
  -DWITH_OPENJPEG=OFF -DWITH_JASPER=OFF -DWITH_IMGCODEC_HDR=OFF \
  -DWITH_IMGCODEC_SUNRASTER=OFF -DWITH_IMGCODEC_PXM=OFF -DWITH_IMGCODEC_PFM=OFF \
  -DWITH_KLEIDICV=OFF -DWITH_CAROTENE=OFF -DWITH_FLATBUFFERS=OFF \
  -DWITH_UNIFONT=OFF \
  -DBUILD_ZLIB=OFF \
  -DZLIB_INCLUDE_DIR="$PREFIX/include" \
  -DZLIB_LIBRARY="$PREFIX/lib/libz.a" \
  -DOPENCV_DOWNLOAD_PATH="$DOWNLOAD_DIR/cmake" \
  -DCV_ENABLE_INTRINSICS=ON

cmake --build "$BUILD_DIR" --config Release -j "$JOBS"

# WIPE THE INSTALL TREE FIRST: it is not keyed by the capability set, and
# `cmake --install` adds rather than prunes, so a previous run with dnn on
# would leave opencv2/dnn.hpp here for a build that cannot link it.
rm -rf "$INSTALL_DIR"
cmake --install "$BUILD_DIR" --config Release

# flann is in this list although nothing asked for it: modules/features
# declares `OPTIONAL opencv_flann`, and BUILD_LIST resolution enables an
# optional dependency that is present. Leaving it out of the merge is a link
# error in any host that uses a matcher.
LIBS=()
MLAS_OBJS=()
for m in core imgproc geometry flann features; do
  LIBS+=("$INSTALL_DIR/lib/libopencv_$m.a")
done
if [[ "$NONFREE" == "1" ]]; then
  LIBS+=("$INSTALL_DIR/lib/libopencv_xfeatures2d.a")
fi
if [[ "$YOLO" == "1" ]]; then
  LIBS+=("$INSTALL_DIR/lib/libopencv_dnn.a")
  LIBS+=("$INSTALL_DIR/lib/opencv5/3rdparty/liblibprotobuf.a")
fi
LIBS+=("$PREFIX/lib/libz.a")

for f in "${LIBS[@]}"; do
  if [[ ! -f "$f" ]]; then
    echo "ERROR: expected $f after the OpenCV build" >&2
    ls -la "$INSTALL_DIR/lib" >&2 || true
    exit 1
  fi
done

if [[ "$YOLO" == "1" ]]; then
  # MLAS objects, IF they are not already members of libopencv_dnn.a --
  # 3rdparty/mlas exports them into the dnn module's own source list, so on some
  # configurations they are. Adding them twice puts duplicate member NAMES in
  # one archive, which `ar` handles far less gracefully than libtool.
  if [[ "$(ar t "$INSTALL_DIR/lib/libopencv_dnn.a" 2>/dev/null | grep -c -i mlas)" == "0" ]]; then
    mlas_count=0
    while IFS= read -r o; do
      MLAS_OBJS+=("$o")
      mlas_count=$((mlas_count + 1))
    done < <(find "$BUILD_DIR/3rdparty/mlas" -name '*.o')
    if [[ "$mlas_count" -eq 0 ]]; then
      echo "ERROR: no MLAS objects under $BUILD_DIR/3rdparty/mlas" >&2
      exit 1
    fi
  fi

  # ONE SYMBOL OPENCV 5.0.0 DECLARES, CALLS AND NEVER DEFINES. See the macOS
  # script for the full explanation; the short version is that
  # 3rdparty/mlas/lib/compute.cpp calls MlasHGemmSupported and no source in the
  # tree defines it, so a merged static archive fails to link. Answering "not
  # supported" is the conservative truth at the only call site.
  STUB_SRC="$WORK_DIR/mlas_hgemm_stub.cpp"
  cat > "$STUB_SRC" <<'STUBEOF'
enum CBLAS_TRANSPOSE { CblasNoTrans = 111, CblasTrans = 112, CblasConjTrans = 113 };
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE);
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE) { return false; }
STUBEOF
  STUB_OBJ="$WORK_DIR/mlas_hgemm_stub.o"
  c++ -std=c++17 -c -O2 -fPIC -o "$STUB_OBJ" "$STUB_SRC"
  MLAS_OBJS+=("$STUB_OBJ")
fi

# ar MRI script, the same merge build-ftxui.sh does on this platform.
rm -f "$OUT_LIB"
MRI_FILE="$WORK_DIR/opencv_merge.mri"
{
  echo "CREATE $OUT_LIB"
  for lib in "${LIBS[@]}"; do
    echo "ADDLIB $lib"
  done
  # ADDMOD, not ADDLIB: these are loose object files, not archives.
  for obj in "${MLAS_OBJS[@]+"${MLAS_OBJS[@]}"}"; do
    echo "ADDMOD $obj"
  done
  echo "SAVE"
  echo "END"
} > "$MRI_FILE"
ar -M < "$MRI_FILE"
ranlib "$OUT_LIB" || true
rm -f "$MRI_FILE"

# OpenCV 5 installs headers under include/opencv5/, not the include/opencv4/
# that every OpenCV 4 recipe on the internet names.
rm -rf "$OUT_LIB_DIR/opencv/include"
mkdir -p "$OUT_LIB_DIR/opencv/include"
cp -R "$INSTALL_DIR/include/opencv5/opencv2" "$OUT_LIB_DIR/opencv/include/"

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "OpenCV built: $OUT_LIB (nonfree=$NONFREE)"
