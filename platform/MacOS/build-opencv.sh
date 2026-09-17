#!/usr/bin/env bash
set -euo pipefail

# Build OpenCV 5 static libs via CMake, then package them into a single
# libmt_opencv.a. Modules: core, imgproc, geometry, flann, features, plus
# xfeatures2d when MT_ENABLE_OPENCV_NONFREE is on.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own. A script phase inherits
# ~600 Xcode build settings, and this script's own BUILD_DIR is one of the names
# in that namespace -- so the strip has to happen while those values are still
# purely inherited. (Same reasoning, and the same measured failure, as
# build-ftxui.sh.)
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env

# OUTSIDE the checkout -- see mt_caps_lib_dir in ../caps-lib.sh for why this is
# a correctness change and not tidiness.
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
if command -v shasum >/dev/null 2>&1; then
  SCRIPT_SHA="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

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

# ---------------------------------------------------------------------------
# THE DNN MODE (MT_CAP_YOLO).
#
# Object detection is OpenCV's own dnn module, so it is not a separate
# dependency -- it is this same configure with one more module and the protobuf
# that ONNX parsing needs. Measured: 39s -> 60s and 16.6 MB -> 45.4 MB per
# architecture, which is why it is a capability and not simply always on.
#
# WITH_ONNXRUNTIME stays OFF deliberately: it is the one dnn option that would
# make the configure download something, and the two pinned tarballs are meant
# to be the only downloads this script performs.
# ---------------------------------------------------------------------------
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
  actual_sha="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
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

DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
JOBS="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"

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
  local arch="$1" prefix="$2"
  local zlib_src="$ROOT_DIR/src/Engine/Libs/zlib"
  local objdir="$WORK_DIR/zobj-$arch"

  [[ -f "$prefix/lib/libz.a" ]] && return 0

  mkdir -p "$objdir" "$prefix/lib" "$prefix/include"
  local f base
  for f in "$zlib_src"/*.c; do
    base="$(basename "$f" .c)"
    clang -c -O2 -arch "$arch" -mmacosx-version-min="$DEPLOYMENT_TARGET" \
      -I"$zlib_src" -o "$objdir/$base.o" "$f"
  done
  libtool -static -o "$prefix/lib/libz.a" "$objdir"/*.o
  cp "$zlib_src/zlib.h" "$zlib_src/zconf.h" "$prefix/include/"
}

# CMAKE_SYSTEM_PROCESSOR IS SET EXPLICITLY, and only the dnn mode needs it.
# On macOS, CMAKE_OSX_ARCHITECTURES selects the target but leaves
# CMAKE_SYSTEM_PROCESSOR at the HOST's value, and OpenCV's vendored MLAS keys
# its assembly kernels off that variable (3rdparty/mlas/CMakeLists.txt:44-55).
# So the x86_64 pass on an Apple-silicon host tried to assemble the aarch64
# SgemvKernelNeon.S and died on "invalid instruction mnemonic 'blo'". Setting
# it per iteration is what makes the universal dnn build possible at all.
#
# ONE ARCHITECTURE AT A TIME, then lipo -- not one configure with two values in
# CMAKE_OSX_ARCHITECTURES. OpenCV picks its SIMD baseline and dispatch set per
# architecture at CONFIGURE time (CPU_BASELINE/CPU_DISPATCH), so a single
# configure cannot describe both, and OpenCV's own framework build does the
# same thing.
ARCH_LIBS=()
for ARCH in arm64 x86_64; do
  PREFIX="$WORK_DIR/helper-$ARCH"
  BUILD_DIR="$WORK_DIR/build-macos-$ARCH"
  INSTALL_DIR="$WORK_DIR/install-macos-$ARCH"

  build_zlib_helper "$ARCH" "$PREFIX"

  # CROSS-COMPILING MODE FOR THE NON-NATIVE PASS, and only the dnn mode needs
  # it. CMAKE_OSX_ARCHITECTURES selects the target but leaves
  # CMAKE_SYSTEM_PROCESSOR at the HOST's value, and OpenCV's vendored MLAS
  # picks its assembly kernels from that variable
  # (3rdparty/mlas/CMakeLists.txt:43-55) with no option to override it -- the
  # flags are INTERNAL and FORCEd. So on an Apple-silicon host the x86_64 pass
  # tried to assemble aarch64 SgemvKernelNeon.S and died on "invalid
  # instruction mnemonic 'blo'".
  #
  # Setting CMAKE_SYSTEM_NAME as well is what actually takes effect: a bare
  # -DCMAKE_SYSTEM_PROCESSOR is overwritten during project(), and only naming
  # the system too puts CMake in cross-compiling mode where the value sticks.
  CROSS_ARGS=()
  if [[ "$ARCH" != "$(uname -m)" ]]; then
    CROSS_ARGS=(-DCMAKE_SYSTEM_NAME=Darwin -DCMAKE_SYSTEM_PROCESSOR="$ARCH")
  fi

  mt_caps_reset_stale_cmake_cache "$BUILD_DIR" "$SRC_DIR/opencv-$OPENCV_VERSION"

  cmake -S "$SRC_DIR/opencv-$OPENCV_VERSION" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$ARCH" \
    "${CROSS_ARGS[@]+"${CROSS_ARGS[@]}"}" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
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

  # WIPE THE INSTALL TREE FIRST. It is keyed by architecture only, so a
  # previous run with a different capability set left its headers and archives
  # here -- and `cmake --install` adds, it does not prune. Measured: a YOLO=0
  # build after a YOLO=1 build still staged opencv2/dnn.hpp, which would let a
  # translation unit include a header this archive cannot link, and fail at the
  # link rather than at the #include. That is the exact failure the capability
  # gate's own comment warns about a few lines above.
  rm -rf "$INSTALL_DIR"
  cmake --install "$BUILD_DIR" --config Release

  # flann is in this list although nothing asked for it: modules/features
  # declares `OPTIONAL opencv_flann`, and BUILD_LIST resolution enables an
  # optional dependency that is present. Leaving it out of the merge is a link
  # error in any host that uses a matcher.
  LIBS=()
  for m in core imgproc geometry flann features; do
    LIBS+=("$INSTALL_DIR/lib/libopencv_$m.a")
  done
  if [[ "$NONFREE" == "1" ]]; then
    LIBS+=("$INSTALL_DIR/lib/libopencv_xfeatures2d.a")
  fi
  if [[ "$YOLO" == "1" ]]; then
    LIBS+=("$INSTALL_DIR/lib/libopencv_dnn.a")
  fi
  LIBS+=("$PREFIX/lib/libz.a")

  if [[ "$YOLO" == "1" ]]; then
    # ONNX parsing needs protobuf, and OpenCV installs its bundled copy under
    # the 3rdparty prefix rather than beside the modules.
    LIBS+=("$INSTALL_DIR/lib/opencv5/3rdparty/liblibprotobuf.a")

    # MLAS objects, IF they are not already members of libopencv_dnn.a.
    # 3rdparty/mlas/CMakeLists.txt exports MLAS_OBJECTS into the dnn module's
    # own source list, so on some configurations they already are -- and adding
    # them twice duplicates members with identical names, which libtool
    # tolerates and the Linux `ar` MRI merge does not. Checked rather than
    # assumed.
    if [[ "$(ar t "$INSTALL_DIR/lib/libopencv_dnn.a" 2>/dev/null | grep -c -i mlas)" == "0" ]]; then
      mlas_count=0
      while IFS= read -r o; do
        LIBS+=("$o")
        mlas_count=$((mlas_count + 1))
      done < <(find "$BUILD_DIR/3rdparty/mlas" -name '*.o')
      # A process substitution's status is never checked, so `set -e` cannot see
      # a find that matched nothing; the miss would surface as undefined Mlas*
      # symbols at the APP's link, a long way from the cause.
      if [[ "$mlas_count" -eq 0 ]]; then
        echo "ERROR: no MLAS objects under $BUILD_DIR/3rdparty/mlas" >&2
        exit 1
      fi
    fi

    # ONE SYMBOL OPENCV 5.0.0 DECLARES, CALLS AND NEVER DEFINES.
    # 3rdparty/mlas/inc/mlas.h declares MlasHGemmSupported and
    # 3rdparty/mlas/lib/compute.cpp calls it; no source in the whole tree
    # defines it -- there is no hgemm file in the vendored MLAS at all.
    # OpenCV's own builds never notice because nothing pulls compute.cpp.o out
    # of the object library; merging everything into one static archive does,
    # and the link fails on aarch64.
    #
    # Answering "not supported" is the conservative truth at the only call
    # site, which asks whether an fp16 grouped-query-attention fast path
    # exists. Object detection never reaches it. GENERATED here rather than
    # tracked, so it cannot outlive the upstream defect silently: delete this
    # block when a release defines the symbol.
    STUB_SRC="$WORK_DIR/mlas_hgemm_stub.cpp"
    cat > "$STUB_SRC" <<'STUBEOF'
enum CBLAS_TRANSPOSE { CblasNoTrans = 111, CblasTrans = 112, CblasConjTrans = 113 };
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE);
bool MlasHGemmSupported(CBLAS_TRANSPOSE, CBLAS_TRANSPOSE) { return false; }
STUBEOF
    STUB_OBJ="$WORK_DIR/mlas_hgemm_stub-$ARCH.o"
    clang++ -std=c++17 -c -O2 -arch "$ARCH" \
      -mmacosx-version-min="$DEPLOYMENT_TARGET" -o "$STUB_OBJ" "$STUB_SRC"
    LIBS+=("$STUB_OBJ")
  fi

  for f in "${LIBS[@]}"; do
    if [[ ! -f "$f" ]]; then
      echo "ERROR: expected $f after the OpenCV build" >&2
      ls -la "$INSTALL_DIR/lib" >&2 || true
      exit 1
    fi
  done

  ARCH_LIB="$WORK_DIR/libmt_opencv-$ARCH.a"
  rm -f "$ARCH_LIB"
  libtool -static -o "$ARCH_LIB" "${LIBS[@]}"
  ARCH_LIBS+=("$ARCH_LIB")
done

rm -f "$OUT_LIB"
lipo -create -output "$OUT_LIB" "${ARCH_LIBS[@]}"
ranlib "$OUT_LIB" || true

# Headers from either architecture's install -- they are identical. OpenCV 5
# installs them under include/opencv5/, not the include/opencv4/ that every
# OpenCV 4 recipe on the internet names.
rm -rf "$OUT_LIB_DIR/opencv/include"
mkdir -p "$OUT_LIB_DIR/opencv/include"
cp -R "$WORK_DIR/install-macos-arm64/include/opencv5/opencv2" "$OUT_LIB_DIR/opencv/include/"

echo -n "$STAMP_VALUE" > "$STAMP_FILE"

echo "OpenCV built: $OUT_LIB (nonfree=$NONFREE, $(lipo -archs "$OUT_LIB"))"
