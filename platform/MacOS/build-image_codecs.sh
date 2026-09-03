#!/usr/bin/env bash
set -euo pipefail

# Build macOS image codec dependencies as universal static libraries, then
# package them into one archive for apps that link libMTEngineSDL.a.

export PATH="/opt/local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own. A script phase inherits
# ~600 Xcode build settings, and this script's own BUILD_DIR is one of the names
# in that namespace -- so the strip has to happen while those values are still
# purely inherited. (Measured the other way round: called after the assignments,
# it deleted the script's BUILD_DIR and the build died on an unbound variable.)
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env
# The WORK root moved outside the checkout (Phase 2). The PATCHES stay in the
# repo -- they are tracked source, not build state.
CACHE_DIR="$(mt_caps_work_dir image-codecs)"
PATCHES_DIR="$ROOT_DIR/other/lib/image-codecs/patches"
DOWNLOAD_DIR="$CACHE_DIR/downloads"
SRC_DIR="$CACHE_DIR/src"
BUILD_DIR="$CACHE_DIR/build"
PREFIX_DIR="$CACHE_DIR/install"
# OUTSIDE the checkout -- see mt_caps_lib_dir in ../caps-lib.sh for why this
# is a correctness change and not tidiness.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). The image codecs follow the three capabilities their configure reads: photo codecs, RAW and colour management.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_IMAGE_CODECS:-}" image_codecs
OUT_LIB="$OUT_LIB_DIR/libmt_image_codecs.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_image_codecs.stamp"

ARCH_LIST=(arm64 x86_64)
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
JOBS="${MT_BUILD_JOBS:-8}"

TIFF_VERSION="4.7.1"
WEBP_VERSION="1.6.0"
AVIF_VERSION="1.4.2"
LIBGAV1_VERSION="0.20.0"
LIBRAW_VERSION="0.22.1"
# libjxl and the three dependencies a DECODER-ONLY build actually links. Pinned
# by git revision, not tag, so an upstream re-tag cannot change what we ship.
# Licences (all permissive, verified 2026-08-19 -- see the photo app
# specs/superpowers/specs/2026-08-19-jpegxl-dng-licence-spike.md):
#   libjxl  BSD-3-Clause + a royalty-free Google patent grant (PATENTS)
#   highway Apache-2.0 OR BSD-3-Clause  (we take BSD-3)
#   brotli  MIT
#   skcms   BSD-3-Clause
LIBJXL_VERSION="0.11.2"

TIFF_URL="https://download.osgeo.org/libtiff/tiff-${TIFF_VERSION}.tar.gz"
TIFF_SHA256="f698d94f3103da8ca7438d84e0344e453fe0ba3b7486e04c5bf7a9a3fabe9b69"
WEBP_URL="https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WEBP_VERSION}.tar.gz"
WEBP_SHA256="e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
AVIF_URL="https://github.com/AOMediaCodec/libavif/archive/refs/tags/v${AVIF_VERSION}.tar.gz"
AVIF_SHA256="2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d"
LIBGAV1_GIT_URL="https://chromium.googlesource.com/codecs/libgav1"
LIBGAV1_GIT_REV="c05bf9be660cf170d7c26bd06bb42b3322180e58"
LIBRAW_URL="https://www.libraw.org/data/LibRaw-${LIBRAW_VERSION}.tar.gz"
LIBRAW_SHA256="a789dc4e2409e2901d93793a4e0b80c7b49d0d97cf6ad71c850eb7616acfd786"
LIBJXL_GIT_URL="https://github.com/libjxl/libjxl"
LIBJXL_GIT_REV="332feb17d17311c748445f7ee75c4fb55cc38530"
LIBJXL_HIGHWAY_REV="457c891775a7397bdb0376bb1031e6e027af1c48"
LIBJXL_BROTLI_REV="36533a866ed1ca4b75cf049f4521e4ec5fe24727"
LIBJXL_SKCMS_REV="b2e692629c1fb19342517d7fb61f1cf83d075492"

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning image codec build artifacts..."
  rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-* "$OUT_LIB" "$STAMP_FILE"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

script_sha="unknown"
if command -v shasum >/dev/null 2>&1; then
  script_sha="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

# THE CAPABILITY GATE.
#
# This bundle was the one dependency with no gate on EITHER platform, so a C64
# debugger with MT_CAP_RAW=0 and MT_CAP_PHOTO_CODECS=0 still downloaded and
# compiled LibRaw, libtiff, libwebp, libavif, libgav1 and libjxl. FTXUI,
# llama.cpp and mbedTLS have had one here since the programme started.
#
# One archive serves six flags, so it is skipped only when EVERY one is off: any
# single codec still wanted means building the bundle, because there is no
# per-codec archive to stub.
#
# libjxl has no capability of its own -- it decodes JPEG XL DNGs, a RAW concern,
# so it travels with MT_ENABLE_LIBRAW.
#
# Absent means ON (the :-1 default), so a bare engine build still builds
# everything. It comes BEFORE any download, which is the whole point.
codecs_wanted=0
for flag in "${MT_ENABLE_LIBTIFF:-1}" "${MT_ENABLE_LIBWEBP:-1}" "${MT_ENABLE_LIBAVIF:-1}" \
            "${MT_ENABLE_LIBHEIF:-1}" "${MT_ENABLE_LIBRAW:-1}" "${MT_ENABLE_LCMS2:-1}"; do
  if [[ "$flag" != "0" ]]; then
    codecs_wanted=1
    break
  fi
done

if [[ "$codecs_wanted" == "0" ]]; then
  STUB_STAMP="disabled:${script_sha}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "mt_image_codecs"
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

stamp_value="${script_sha}:tiff-${TIFF_VERSION}:webp-${WEBP_VERSION}:avif-${AVIF_VERSION}:libgav1-${LIBGAV1_VERSION}:libraw-${LIBRAW_VERSION}:libjxl-${LIBJXL_VERSION}:${DEPLOYMENT_TARGET}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$stamp_value" ]]; then
    # Stamp hit: backfill the Phase 2 header staging into this keyed bucket.
    if [[ ! -d "$OUT_LIB_DIR/image-codecs/include" && -d "$PREFIX_DIR/include" ]]; then
      mkdir -p "$OUT_LIB_DIR/image-codecs"
      cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/image-codecs/include"
    fi
    exit 0
  fi
fi

download_archive() {
  local name="$1"
  local url="$2"
  local sha256="$3"
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
}

extract_archive() {
  local name="$1"
  local top_dir="$2"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local dest="$SRC_DIR/$top_dir"

  if [[ -d "$dest" ]]; then
    return
  fi

  echo "Extracting $name..."
  tar -xzf "$archive" -C "$SRC_DIR"
}

extract_flat_archive() {
  local name="$1"
  local top_dir="$2"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local dest="$SRC_DIR/$top_dir"

  if [[ -d "$dest" ]]; then
    return
  fi

  echo "Extracting $name..."
  mkdir -p "$dest"
  tar -xzf "$archive" -C "$dest"
}

checkout_git_tag() {
  local name="$1"
  local url="$2"
  local tag="$3"
  local expected_rev="$4"
  local dest="$SRC_DIR/$name"

  if [[ -d "$dest/.git" ]]; then
    local actual_rev
    actual_rev="$(git -C "$dest" rev-parse HEAD)"
    if [[ "$actual_rev" == "$expected_rev" ]]; then
      return
    fi
  fi

  rm -rf "$dest"
  echo "Checking out $name..."
  git clone --depth 1 --branch "$tag" "$url" "$dest"

  local actual_rev
  actual_rev="$(git -C "$dest" rev-parse HEAD)"
  if [[ "$actual_rev" != "$expected_rev" ]]; then
    echo "ERROR: unexpected git revision for $name" >&2
    echo "Expected: $expected_rev" >&2
    echo "Actual:   $actual_rev" >&2
    exit 1
  fi
}

checkout_libjxl() {
  local dest="$SRC_DIR/libjxl-${LIBJXL_VERSION}"

  # Pinned by REVISION, not tag: an upstream re-tag must not silently change
  # what we ship. Only the three submodules a decoder-only build links are
  # fetched -- googletest, libjpeg-turbo, libpng, sjpeg, zlib and testdata are
  # for tools and tests we disable, and fetching them would add a lot of bytes
  # nobody links.
  if [[ -d "$dest/.git" ]]; then
    local actual_rev
    actual_rev="$(git -C "$dest" rev-parse HEAD)"
    if [[ "$actual_rev" == "$LIBJXL_GIT_REV" ]]; then
      return
    fi
  fi

  rm -rf "$dest"
  echo "Checking out libjxl ${LIBJXL_VERSION}..."
  git clone --quiet "$LIBJXL_GIT_URL" "$dest"
  git -C "$dest" checkout --quiet "$LIBJXL_GIT_REV"

  local actual_rev
  actual_rev="$(git -C "$dest" rev-parse HEAD)"
  if [[ "$actual_rev" != "$LIBJXL_GIT_REV" ]]; then
    echo "ERROR: unexpected git revision for libjxl" >&2
    echo "Expected: $LIBJXL_GIT_REV" >&2
    echo "Actual:   $actual_rev" >&2
    exit 1
  fi

  git -C "$dest" submodule update --init --depth 1 \
      third_party/highway third_party/brotli third_party/skcms

  # Assert each submodule landed on the revision we pinned. Left unchecked, a
  # moved submodule pointer changes the shipped code and the licence set with
  # no signal at all.
  local sm rev want
  for sm in highway brotli skcms; do
    case "$sm" in
      highway) want="$LIBJXL_HIGHWAY_REV" ;;
      brotli)  want="$LIBJXL_BROTLI_REV" ;;
      skcms)   want="$LIBJXL_SKCMS_REV" ;;
    esac
    rev="$(git -C "$dest/third_party/$sm" rev-parse HEAD)"
    if [[ "$rev" != "$want" ]]; then
      echo "ERROR: libjxl submodule $sm is at $rev, expected $want" >&2
      exit 1
    fi
  done
}

patch_libraw_jxl() {
  # Adds a real JPEG XL DNG decoder in place of LibRaw's throwing placeholder.
  # Idempotent, and it FAILS LOUDLY if a LibRaw bump moves its anchors -- see
  # the script's own header for why this is not a context diff.
  python3 "$PATCHES_DIR/apply_libraw_jxl.py" "$SRC_DIR/LibRaw-${LIBRAW_VERSION}"
}

build_cmake() {
  local src="$1"
  local build="$2"
  local prefix="$3"
  local arch="$4"
  shift 4

  mt_caps_reset_stale_cmake_cache "$build" "$src"
  cmake -S "$src" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="$arch" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$DEPLOYMENT_TARGET" \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    "$@"
  cmake --build "$build" --config Release -j "$JOBS"
  cmake --install "$build" --config Release
}

# RD-A: compile the engine's own vendored jpeg-9a and zlib into per-arch
# static archives so LibRaw's configure can link-test them (AC_CHECK_LIB is a
# REAL link per arch -- a single-slice archive silently disables the codec for
# the other slice). Sources and the jmorecfg.h boolean patch travel from the
# engine tree itself, so there is exactly one copy of each library in the repo
# (roadmap #2.11). The archives are ALSO merged into libmt_image_codecs.a
# below, so the bundle stays self-contained and GNU ld's single-pass archive
# resolution on Linux never needs a back-reference into libMTEngineSDL.a
# (RD-A design #4.2.1 route (a)).
build_engine_codec_deps() {
  local arch="$1"
  local prefix="$2"
  local jpeg_src="$ROOT_DIR/src/Engine/Libs/jpeg/jpeg-9a"
  local zlib_src="$ROOT_DIR/src/Engine/Libs/zlib"
  local deps_build="$BUILD_DIR/engine-deps-$arch"

  rm -rf "$deps_build"
  mkdir -p "$deps_build/jpeg" "$deps_build/zlib" "$prefix/lib" "$prefix/include"

  local f base
  for f in "$jpeg_src"/*.c; do
    base="$(basename "${f%.c}")"
    clang -c -O2 -arch "$arch" -mmacosx-version-min="$DEPLOYMENT_TARGET"       -I"$jpeg_src" -o "$deps_build/jpeg/$base.o" "$f"
  done
  libtool -static -o "$prefix/lib/libjpeg.a" "$deps_build"/jpeg/*.o

  for f in "$zlib_src"/*.c; do
    base="$(basename "${f%.c}")"
    clang -c -O2 -arch "$arch" -mmacosx-version-min="$DEPLOYMENT_TARGET"       -I"$zlib_src" -o "$deps_build/zlib/$base.o" "$f"
  done
  libtool -static -o "$prefix/lib/libz.a" "$deps_build"/zlib/*.o

  # Headers for LibRaw's compile and for configure's AC_CHECK_HEADERS probe --
  # a library that links but whose headers are missing WARNS AND DISABLES
  # SILENTLY (configure.ac:53-62).
  cp "$jpeg_src/jpeglib.h" "$jpeg_src/jconfig.h" "$jpeg_src/jmorecfg.h"      "$jpeg_src/jerror.h" "$prefix/include/"
  cp "$zlib_src/zlib.h" "$zlib_src/zconf.h" "$prefix/include/"
}

build_libraw() {
  local arch="$1"
  local prefix="$2"

  pushd "$SRC_DIR/LibRaw-${LIBRAW_VERSION}" >/dev/null
  if [[ ! -x ./configure ]]; then
    autoreconf --force --install --verbose
  fi
  make distclean >/dev/null 2>&1 || true
  # RD-A (#2.11): jpeg + zlib ON, from the engine's vendored copies staged in
  # $prefix by build_engine_codec_deps. Two different mechanisms:
  #   jpeg -- AC_CHECK_LIB link test (needs -ljpeg findable via LDFLAGS) plus
  #           AC_CHECK_HEADERS (needs jpeglib.h on CPPFLAGS);
  #   zlib -- PKG_CHECK_MODULES, which is NOT a link test: ZLIB_CFLAGS and
  #           ZLIB_LIBS are honoured directly and pkg-config is never invoked
  #           (no build script on any platform depends on pkg-config).
  ./configure \
    --prefix="$prefix" \
    --disable-shared \
    --enable-static \
    --disable-openmp \
    --disable-lcms \
    --disable-examples \
    CFLAGS="-arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
    CXXFLAGS="-arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
    CPPFLAGS="-I$prefix/include -DUSE_JXL" \
    LDFLAGS="-arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET -L$prefix/lib" \
    ZLIB_CFLAGS="-I$prefix/include" \
    ZLIB_LIBS="-L$prefix/lib -lz"

  # Both failure paths are SILENT (AC_MSG_WARN, then a green build without the
  # capability), so assert the flags actually took. USE_JPEG/USE_ZLIB land in
  # the generated Makefile's CPPFLAGS (configure:19767, :19879).
  if ! grep -q "USE_JPEG" Makefile || ! grep -q "USE_ZLIB" Makefile; then
    echo "FATAL: LibRaw configure did not enable jpeg/zlib for arch $arch:" >&2
    grep -n "CPPFLAGS" Makefile | head -5 >&2
    exit 1
  fi
  # Same failure shape as jpeg/zlib: without USE_JXL the patched decoder
  # compiles to the throwing stub and JPEG XL DNGs stay unreadable, silently.
  if ! grep -q "USE_JXL" Makefile; then
    echo "FATAL: LibRaw configure did not carry USE_JXL for arch $arch:" >&2
    grep -n "CPPFLAGS" Makefile | head -5 >&2
    exit 1
  fi

  make -j "$JOBS"
  make install
  popd >/dev/null
}

combine_universal_lib() {
  local rel_path="$1"
  local out="$PREFIX_DIR/$rel_path"
  local inputs=()

  for arch in "${ARCH_LIST[@]}"; do
    inputs+=("$CACHE_DIR/install-$arch/$rel_path")
  done

  for input in "${inputs[@]}"; do
    require_file "$input"
  done

  mkdir -p "$(dirname "$out")"
  rm -f "$out"
  lipo -create "${inputs[@]}" -output "$out"
  ranlib "$out" || true
  require_universal "$out"
}

combine_found_arch_lib() {
  local name_pattern="$1"
  local out="$2"
  local inputs=()

  for arch in "${ARCH_LIST[@]}"; do
    local found
    found="$(find "$BUILD_DIR/avif-$arch" -name "$name_pattern" -type f | head -n 1 || true)"
    if [[ -z "$found" ]]; then
      echo "ERROR: expected $name_pattern in $BUILD_DIR/avif-$arch" >&2
      exit 1
    fi
    inputs+=("$found")
  done

  mkdir -p "$(dirname "$out")"
  rm -f "$out"
  lipo -create "${inputs[@]}" -output "$out"
  ranlib "$out" || true
  require_universal "$out"
}

require_file() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    echo "ERROR: expected file not found: $file" >&2
    exit 1
  fi
}

require_universal() {
  local file="$1"
  local info
  info="$(lipo -info "$file")"
  if [[ "$info" != *"arm64"* || "$info" != *"x86_64"* ]]; then
    echo "ERROR: expected universal arm64+x86_64 archive: $file" >&2
    echo "$info" >&2
    exit 1
  fi
}

require_symbol() {
  local symbol="$1"
  if ! nm -g "$OUT_LIB" | c++filt | grep -F "$symbol" >/dev/null; then
    echo "ERROR: expected symbol not found in $OUT_LIB: $symbol" >&2
    exit 1
  fi
}

download_archive "tiff-${TIFF_VERSION}" "$TIFF_URL" "$TIFF_SHA256"
download_archive "libwebp-${WEBP_VERSION}" "$WEBP_URL" "$WEBP_SHA256"
download_archive "libavif-${AVIF_VERSION}" "$AVIF_URL" "$AVIF_SHA256"
download_archive "LibRaw-${LIBRAW_VERSION}" "$LIBRAW_URL" "$LIBRAW_SHA256"

extract_archive "tiff-${TIFF_VERSION}" "tiff-${TIFF_VERSION}"
extract_archive "libwebp-${WEBP_VERSION}" "libwebp-${WEBP_VERSION}"
extract_archive "libavif-${AVIF_VERSION}" "libavif-${AVIF_VERSION}"
checkout_git_tag "libgav1-v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_URL" "v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_REV"
extract_archive "LibRaw-${LIBRAW_VERSION}" "LibRaw-${LIBRAW_VERSION}"
checkout_libjxl

rm -rf "$SRC_DIR/libavif-${AVIF_VERSION}/ext/libgav1"
mkdir -p "$SRC_DIR/libavif-${AVIF_VERSION}/ext"
cp -R "$SRC_DIR/libgav1-v${LIBGAV1_VERSION}" "$SRC_DIR/libavif-${AVIF_VERSION}/ext/libgav1"

rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-*
mkdir -p "$BUILD_DIR" "$PREFIX_DIR"

for arch in "${ARCH_LIST[@]}"; do
  arch_prefix="$CACHE_DIR/install-$arch"
  mkdir -p "$arch_prefix"

  build_cmake "$SRC_DIR/tiff-${TIFF_VERSION}" "$BUILD_DIR/tiff-$arch" "$arch_prefix" "$arch" \
    -DBUILD_SHARED_LIBS=OFF \
    -Dtiff-static=ON \
    -Dtiff-tools=OFF \
    -Dtiff-tests=OFF \
    -Dtiff-contrib=OFF \
    -Dtiff-docs=OFF \
    -Dtiff-cxx=OFF \
    -Dzlib=OFF \
    -Dlibdeflate=OFF \
    -Djpeg=OFF \
    -Dold-jpeg=OFF \
    -Djpeg12=OFF \
    -Djbig=OFF \
    -Dlerc=OFF \
    -Dlzma=OFF \
    -Dzstd=OFF \
    -Dwebp=OFF

  build_cmake "$SRC_DIR/libwebp-${WEBP_VERSION}" "$BUILD_DIR/webp-$arch" "$arch_prefix" "$arch" \
    -DBUILD_SHARED_LIBS=OFF \
    -DWEBP_LINK_STATIC=ON \
    -DWEBP_BUILD_ANIM_UTILS=OFF \
    -DWEBP_BUILD_CWEBP=OFF \
    -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF \
    -DWEBP_BUILD_IMG2WEBP=OFF \
    -DWEBP_BUILD_VWEBP=OFF \
    -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_LIBWEBPMUX=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF \
    -DWEBP_BUILD_EXTRAS=OFF \
    -DWEBP_BUILD_WEBP_JS=OFF \
    -DWEBP_BUILD_FUZZTEST=OFF

  build_cmake "$SRC_DIR/libavif-${AVIF_VERSION}" "$BUILD_DIR/avif-$arch" "$arch_prefix" "$arch" \
    -DBUILD_SHARED_LIBS=OFF \
    -DAVIF_BUILD_APPS=OFF \
    -DAVIF_BUILD_TESTS=OFF \
    -DAVIF_BUILD_EXAMPLES=OFF \
    -DAVIF_BUILD_MAN_PAGES=OFF \
    -DAVIF_CODEC_AOM=OFF \
    -DAVIF_CODEC_DAV1D=OFF \
    -DAVIF_CODEC_LIBGAV1=LOCAL \
    -DAVIF_CODEC_RAV1E=OFF \
    -DAVIF_CODEC_SVT=OFF \
    -DAVIF_ZLIBPNG=OFF \
    -DAVIF_JPEG=OFF \
    -DAVIF_LIBYUV=OFF \
    -DAVIF_LIBSHARPYUV=OFF \
    -DAVIF_LIBXML2=OFF \
    -DLIBGAV1_ENABLE_AVX2=OFF \
    -DLIBGAV1_ENABLE_NEON=OFF \
    -DLIBGAV1_ENABLE_SSE4_1=OFF

  # DECODER ONLY. Every tool, test, plugin and encoder-side extra is off: we
  # decode JXL tiles out of DNGs and nothing else. JPEGXL_ENABLE_SKCMS keeps
  # colour handling inside libjxl's own small skcms rather than pulling in
  # lcms2, which we already ship separately and do not want a second copy of.
  build_cmake "$SRC_DIR/libjxl-${LIBJXL_VERSION}" "$BUILD_DIR/libjxl-$arch" "$arch_prefix" "$arch" \
    -DBUILD_SHARED_LIBS=OFF \
    -DBUILD_TESTING=OFF \
    -DJPEGXL_STATIC=OFF \
    -DJPEGXL_ENABLE_TOOLS=OFF \
    -DJPEGXL_ENABLE_VIEWERS=OFF \
    -DJPEGXL_ENABLE_DEVTOOLS=OFF \
    -DJPEGXL_ENABLE_EXAMPLES=OFF \
    -DJPEGXL_ENABLE_BENCHMARK=OFF \
    -DJPEGXL_ENABLE_FUZZERS=OFF \
    -DJPEGXL_ENABLE_MANPAGES=OFF \
    -DJPEGXL_ENABLE_DOXYGEN=OFF \
    -DJPEGXL_ENABLE_JNI=OFF \
    -DJPEGXL_ENABLE_PLUGINS=OFF \
    -DJPEGXL_ENABLE_OPENEXR=OFF \
    -DJPEGXL_ENABLE_SJPEG=OFF \
    -DJPEGXL_ENABLE_JPEGLI=OFF \
    -DJPEGXL_ENABLE_JPEGLI_LIBJPEG=OFF \
    -DJPEGXL_ENABLE_TRANSCODE_JPEG=OFF \
    -DJPEGXL_ENABLE_SKCMS=ON \
    -DJPEGXL_BUNDLE_SKCMS=ON \
    -DJPEGXL_ENABLE_TCMALLOC=OFF \
    -DJPEGXL_FORCE_SYSTEM_BROTLI=OFF \
    -DJPEGXL_FORCE_SYSTEM_HWY=OFF \
    -DJPEGXL_BUNDLE_LIBPNG=OFF \
    -DJPEGXL_ENABLE_COVERAGE=OFF \
    -DJPEGXL_WARNINGS_AS_ERRORS=OFF

  build_engine_codec_deps "$arch" "$arch_prefix"
  patch_libraw_jxl
  build_libraw "$arch" "$arch_prefix"
done

rm -rf "$PREFIX_DIR/include" "$PREFIX_DIR/lib"
mkdir -p "$PREFIX_DIR/lib"
cp -R "$CACHE_DIR/install-arm64/include" "$PREFIX_DIR/include"

combine_universal_lib "lib/libtiff.a"
combine_universal_lib "lib/libwebp.a"
combine_universal_lib "lib/libwebpdemux.a"
combine_universal_lib "lib/libsharpyuv.a"
combine_universal_lib "lib/libavif.a"
combine_universal_lib "lib/libraw.a"
combine_universal_lib "lib/libjpeg.a"
combine_universal_lib "lib/libz.a"
# libjxl and the pieces a DECODE path actually pulls. libjxl_threads is left
# out on purpose -- the DNG decoder passes a NULL parallel runner, so nothing
# references it -- and so is brotlienc, which only the encoder needs.
combine_universal_lib "lib/libjxl.a"
combine_universal_lib "lib/libjxl_cms.a"
combine_universal_lib "lib/libhwy.a"
combine_universal_lib "lib/libbrotlidec.a"
combine_universal_lib "lib/libbrotlicommon.a"
combine_found_arch_lib "libgav1*.a" "$PREFIX_DIR/lib/libgav1.a"

INPUT_LIBS=(
  "$PREFIX_DIR/lib/libtiff.a"
  "$PREFIX_DIR/lib/libwebp.a"
  "$PREFIX_DIR/lib/libwebpdemux.a"
  "$PREFIX_DIR/lib/libsharpyuv.a"
  "$PREFIX_DIR/lib/libavif.a"
  "$PREFIX_DIR/lib/libraw.a"
  "$PREFIX_DIR/lib/libjpeg.a"
  "$PREFIX_DIR/lib/libz.a"
  "$PREFIX_DIR/lib/libgav1.a"
  "$PREFIX_DIR/lib/libjxl.a"
  "$PREFIX_DIR/lib/libjxl_cms.a"
  "$PREFIX_DIR/lib/libhwy.a"
  "$PREFIX_DIR/lib/libbrotlidec.a"
  "$PREFIX_DIR/lib/libbrotlicommon.a"
)

for lib in "${INPUT_LIBS[@]}"; do
  require_file "$lib"
  require_universal "$lib"
done

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${INPUT_LIBS[@]}"
ranlib "$OUT_LIB" || true
require_universal "$OUT_LIB"

require_symbol "_TIFFOpen"
require_symbol "_WebPDecodeRGBA"
require_symbol "_WebPDemuxGetFrame"
require_symbol "_avifDecoderCreate"
require_symbol "LibRaw::open_file(char const*)"
require_symbol "_jpeg_mem_src"
require_symbol "_uncompress"
require_symbol "_JxlDecoderCreate"
# The decoder we patch into LibRaw. Without this the bundle can still contain
# libjxl while every JPEG XL DNG stays unreadable -- the exact failure the
# USE_JXL configure assertion above also guards, checked here on the shipped
# artifact rather than on a Makefile.
require_symbol "LibRaw::jxl_dng_load_raw()"

echo -n "$stamp_value" > "$STAMP_FILE"
# Headers travel WITH the archive (Phase 2): every project used to read them
# from the in-checkout install tree this relocation retired.
rm -rf "$OUT_LIB_DIR/image-codecs/include"
mkdir -p "$OUT_LIB_DIR/image-codecs"
cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/image-codecs/include"
echo "Image codec bundle built: $OUT_LIB (headers staged to $OUT_LIB_DIR/image-codecs/include)"
