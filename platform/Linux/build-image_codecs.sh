#!/usr/bin/env bash
set -euo pipefail
export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
# Sourced BEFORE any use of mt_caps_work_dir/mt_caps_lib_dir below -- this
# script runs as its own `bash` child process (app-build-linux.sh -> the
# engine's build-linux.sh -> `bash build-image_codecs.sh`), so nothing a
# caller sourced is inherited here.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
CACHE_DIR="$(mt_caps_work_dir image-codecs)"
PATCHES_DIR="$ROOT_DIR/other/lib/image-codecs/patches"
DOWNLOAD_DIR="$CACHE_DIR/downloads"
SRC_DIR="$CACHE_DIR/src"
BUILD_DIR="$CACHE_DIR/build-linux"
PREFIX_DIR="$CACHE_DIR/install-linux"
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). The image codecs follow the three capabilities their configure reads.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_IMAGE_CODECS:-}" image_codecs
OUT_LIB="$OUT_LIB_DIR/libmt_image_codecs.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_image_codecs.stamp"
JOBS="${MT_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
TIFF_VERSION="4.7.1"
WEBP_VERSION="1.6.0"
AVIF_VERSION="1.4.2"
LIBGAV1_VERSION="0.20.0"
LIBRAW_VERSION="0.22.1"
# libjxl decodes JPEG XL DNGs (Compression 52546), which current Adobe DNG
# Converter writes from its plain "lossy compression" option. Pinned by
# REVISION with each submodule asserted -- see the macOS script for the full
# rationale and the licence survey.
LIBJXL_VERSION="0.11.2"
# lcms2 is the Linux-only CMS backend (ColorSync on macOS, ICM/WCS on Windows).
# 2.19 is the first release with a root CMakeLists.txt -- earlier versions ship
# only autotools/Meson, which would need a second builder in this script.
LCMS2_VERSION="2.19.1"
TIFF_URL="https://download.osgeo.org/libtiff/tiff-${TIFF_VERSION}.tar.gz"
TIFF_SHA256="f698d94f3103da8ca7438d84e0344e453fe0ba3b7486e04c5bf7a9a3fabe9b69"
WEBP_URL="https://storage.googleapis.com/downloads.webmproject.org/releases/webp/libwebp-${WEBP_VERSION}.tar.gz"
WEBP_SHA256="e4ab7009bf0629fd11982d4c2aa83964cf244cffba7347ecd39019a9e38c4564"
AVIF_URL="https://github.com/AOMediaCodec/libavif/archive/refs/tags/v${AVIF_VERSION}.tar.gz"
AVIF_SHA256="2b645287340ba5a631d268b551dc2d72bd73ac33335962dd36dcdb6d8366921d"
LIBGAV1_GIT_URL="https://chromium.googlesource.com/codecs/libgav1"
LIBGAV1_GIT_REV="c05bf9be660cf170d7c26bd06bb42b3322180e58"
LIBRAW_URL="https://www.libraw.org/data/LibRaw-${LIBRAW_VERSION}.tar.gz"
LIBJXL_GIT_URL="https://github.com/libjxl/libjxl"
LIBJXL_GIT_REV="332feb17d17311c748445f7ee75c4fb55cc38530"
LIBJXL_HIGHWAY_REV="457c891775a7397bdb0376bb1031e6e027af1c48"
LIBJXL_BROTLI_REV="36533a866ed1ca4b75cf049f4521e4ec5fe24727"
LIBJXL_SKCMS_REV="b2e692629c1fb19342517d7fb61f1cf83d075492"
LIBRAW_SHA256="a789dc4e2409e2901d93793a4e0b80c7b49d0d97cf6ad71c850eb7616acfd786"
LCMS2_URL="https://github.com/mm2/Little-CMS/archive/refs/tags/lcms${LCMS2_VERSION}.tar.gz"
# Filled in 2026-08-18 on the first real Linux run: sha256sum of the
# lcms2.19.1 tag tarball, cross-checked against the value already pinned in
# platform/Windows/build-image_codecs.ps1 (same GitHub tag tarball) -- both
# match.
LCMS2_SHA256="267705e278e2f7c2fb886c259dadcbaeb2be52748bcbc71c79f08aacacb7a709"

# libultrahdr -- the ISO 21496-1 gain-map JPEG ("Ultra HDR") codec, for the
# photo app's HDR rendered-edit cache. Licence: Apache-2.0 AND
# MIT, with the bundled image_io under Apache-2.0; it includes Adobe gain-map
# technology whose NOTICE travels in LICENSES.txt (vocabulary.json). Pinned by
# the SHA-256 of GitHub's archive of tag v2.0.2 (commit
# e5f5a022fe96fc4dc2ee35c19f733a50df807abe), downloaded into $DOWNLOAD_DIR --
# the image-codec work root, outside every checkout -- like every tarball here.
# Built against the engine's ONE libjpeg -- libjpeg-turbo, built by
# build-jpeg.sh into $(mt_caps_work_dir jpeg)/install-$(uname -m) before this
# script runs: a second libjpeg (the distro's) in one process would be two
# jpeg_* ABIs, one of them chosen by link order.
LIBULTRAHDR_VERSION="2.0.2"
LIBULTRAHDR_URL="https://github.com/google/libultrahdr/archive/refs/tags/v${LIBULTRAHDR_VERSION}.tar.gz"
LIBULTRAHDR_SHA256="aa8d193bb887c348c419780511dd03b374f4e07af8812b6d3f80c8537cf1ef2c"
UHDR_WANTED="${MT_ENABLE_LIBULTRAHDR:-1}"
# The revision of our own libultrahdr source patches (patches/apply_libultrahdr_
# jpeg9_raw.py). It is part of the stamp, so a new patch rebuilds the archive.
LIBULTRAHDR_PATCH_REV="jpeg9-raw-1"

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning image codec build artifacts..."
  rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-linux* "$OUT_LIB" "$STAMP_FILE"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

# libjpeg-turbo (build-jpeg.sh, the jpeg unit) -- the engine's one libjpeg. Its
# archive is read from the jpeg unit's STORE (MT_STORE_JPEG), else from the
# view -- never from this script's own $OUT_LIB_DIR (the image-codecs store).
# Its stamp joins both of this bundle's stamps, so a new turbo rebuilds it.
JPEG_LIB_DIR="${MT_STORE_JPEG:-$MT_VIEW_LIB_DIR}"
JPEG_LIB="$JPEG_LIB_DIR/libmt_jpeg.a"
JPEG_STAMP_FILE="$JPEG_LIB_DIR/libmt_jpeg.stamp"
JPEG_PREFIX="$(mt_caps_work_dir jpeg)/install-$(uname -m)"
if [[ ! -f "$JPEG_LIB" || ! -f "$JPEG_STAMP_FILE" ]]; then
  echo "ERROR: $JPEG_LIB (or its stamp) is missing -- run platform/Linux/build-jpeg.sh first" >&2
  exit 1
fi
JPEG_STAMP="$(cat "$JPEG_STAMP_FILE")"

script_sha="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  script_sha="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
elif command -v shasum >/dev/null 2>&1; then
  script_sha="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi
# THE CAPABILITY GATE. This bundle had none on any platform, so a C64 debugger
# with MT_CAP_RAW=0 and MT_CAP_PHOTO_CODECS=0 still built LibRaw, libtiff,
# libwebp, libavif, libgav1 and libjxl.
#
# One archive serves seven flags, so it is skipped only when EVERY one is off: any
# single codec still wanted means building the bundle, as there is no per-codec
# archive to stub. libjxl has no capability of its own -- it decodes JPEG XL
# DNGs, a RAW concern -- so it travels with MT_ENABLE_LIBRAW.
#
# Absent means ON, and this comes before any download.
codecs_wanted=0
for flag in "${MT_ENABLE_LIBTIFF:-1}" "${MT_ENABLE_LIBWEBP:-1}" "${MT_ENABLE_LIBAVIF:-1}" \
            "${MT_ENABLE_LIBHEIF:-1}" "${MT_ENABLE_LIBRAW:-1}" "${MT_ENABLE_LCMS2:-1}" \
            "${MT_ENABLE_LIBULTRAHDR:-1}"; do
  if [[ "$flag" != "0" ]]; then
    codecs_wanted=1
    break
  fi
done

if [[ "$codecs_wanted" == "0" ]]; then
  STUB_STAMP="disabled:${script_sha}:jpeg=${JPEG_STAMP}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  # The stub STILL carries libjpeg-turbo: JPEGWriter (libMTEngineSDL.a) needs
  # jpeg_* on every configuration, and this is the archive every app links.
  mt_caps_stub_archive "$OUT_LIB.stub.a" "mt_image_codecs"
  rm -f "$OUT_LIB"
  MRI_SCRIPT="$(mktemp)"
  printf 'CREATE %s\nADDLIB %s\nADDLIB %s\nSAVE\nEND\n' "$OUT_LIB" "$OUT_LIB.stub.a" "$JPEG_LIB" > "$MRI_SCRIPT"
  ar -M < "$MRI_SCRIPT"
  rm -f "$MRI_SCRIPT" "$OUT_LIB.stub.a"
  ranlib "$OUT_LIB" 2>/dev/null || true
  if ! nm -g "$OUT_LIB" 2>/dev/null | grep "jpeg_skip_scanlines" >/dev/null; then
    echo "ERROR: the stub bundle $OUT_LIB does not carry libjpeg-turbo (jpeg_skip_scanlines)" >&2
    exit 1
  fi
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

stamp_value="${script_sha}:jpeg=${JPEG_STAMP}:tiff-${TIFF_VERSION}:webp-${WEBP_VERSION}:avif-${AVIF_VERSION}:libgav1-${LIBGAV1_VERSION}:libraw-${LIBRAW_VERSION}:libjxl-${LIBJXL_VERSION}:lcms2-${LCMS2_VERSION}:libultrahdr-${LIBULTRAHDR_VERSION}-${UHDR_WANTED}-${LIBULTRAHDR_PATCH_REV}:linux"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$stamp_value" ]]; then
    # Stamp hit: backfill the Phase 2 header staging into this keyed bucket.
    if [[ ! -d "$OUT_LIB_DIR/image-codecs/include" && -d "$PREFIX_DIR/include" ]]; then
      mkdir -p "$OUT_LIB_DIR/image-codecs"
      cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/image-codecs/include"
    fi
    echo "Image codec bundle is up to date: $OUT_LIB"
    exit 0
  fi
fi

download_archive() {
  local name="$1"
  local url="$2"
  local sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local part="$archive.part"
  local actual_sha

  # A file at $archive is only ever one that passed the hash below (it is
  # renamed into place after the check). A mismatch here therefore means the
  # PIN changed under the same name -- a same-version re-pin -- so the old
  # file is discarded and fetched again instead of blocking every later run.
  if [[ -f "$archive" ]]; then
    actual_sha="$(sha256sum "$archive" | cut -d ' ' -f 1)"
    if [[ "$actual_sha" == "$sha256" ]]; then
      return
    fi
    echo "Cached $archive does not match the pinned SHA-256 (pin changed?) -- downloading again." >&2
    rm -f "$archive"
  fi
  if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl is required to download image codec sources." >&2
    echo "Install curl and rerun this script (Debian/Ubuntu: sudo apt install curl)." >&2
    exit 1
  fi

  # Into a .part file, verified, THEN renamed: an interrupted or corrupt
  # download must never sit at the final name, where the check above would
  # stop every later run with a checksum error nobody can act on.
  rm -f "$part"
  echo "Downloading $name..."
  curl -L --fail --show-error "$url" -o "$part"
  actual_sha="$(sha256sum "$part" | cut -d ' ' -f 1)"
  if [[ "$actual_sha" != "$sha256" ]]; then
    rm -f "$part"
    echo "ERROR: checksum mismatch for $name (downloaded from $url); the partial file was deleted" >&2
    echo "Expected: $sha256" >&2
    echo "Actual:   $actual_sha" >&2
    exit 1
  fi
  mv -f "$part" "$archive"
}

# The extraction guard: a marker written INSIDE the tree after tar finished,
# holding the archive's (verified, so pinned) SHA-256. A tree without it -- an
# interrupted extraction -- or with another hash -- a same-version re-pin --
# is removed and extracted again. The Windows script's Assert-Extracted is the
# same idea. "The directory exists" alone made a half-extracted tree permanent.
extracted_ok() {
  local archive="$1"
  local dest="$2"
  [[ -d "$dest" && -f "$dest/.mt-extracted" ]] || return 1
  [[ "$(cat "$dest/.mt-extracted")" == "$(sha256sum "$archive" | cut -d ' ' -f 1)" ]]
}

mark_extracted() {
  local archive="$1"
  local dest="$2"
  sha256sum "$archive" | cut -d ' ' -f 1 | tr -d '\n' > "$dest/.mt-extracted"
}

extract_archive() {
  local name="$1"
  local top_dir="$2"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local dest="$SRC_DIR/$top_dir"

  if extracted_ok "$archive" "$dest"; then
    return
  fi

  rm -rf "$dest"
  echo "Extracting $name..."
  tar -xzf "$archive" -C "$SRC_DIR"
  if [[ ! -d "$dest" ]]; then
    echo "ERROR: $archive did not extract to $dest" >&2
    exit 1
  fi
  mark_extracted "$archive" "$dest"
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

build_cmake() {
  local src="$1"
  local build="$2"
  local prefix="$3"
  shift 3
  mt_caps_reset_stale_cmake_cache "$build" "$src"
  cmake -S "$src" -B "$build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$prefix" \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    "$@"
  cmake --build "$build" --config Release -j "$JOBS"
  cmake --install "$build" --config Release
}

# RD-A: compile the engine's own vendored zlib into a static archive for
# LibRaw. One copy of the library in the repo (roadmap #2.11); the archive is
# ALSO merged into libmt_image_codecs.a below, which keeps the bundle
# self-contained -- Linux is the single-pass-GNU-ld platform the RD-A design
# #4.2.1 route (a) exists for. jpeg is no longer built here: the engine's one
# libjpeg is libjpeg-turbo, from build-jpeg.sh ($JPEG_PREFIX), merged the same way.
build_engine_codec_deps() {
  local prefix="$1"
  local zlib_src="$ROOT_DIR/src/Engine/Libs/zlib"
  local deps_build="$BUILD_DIR/engine-deps"

  rm -rf "$deps_build"
  mkdir -p "$deps_build/zlib" "$prefix/lib" "$prefix/include"

  local f base
  for f in "$zlib_src"/*.c; do
    base="$(basename "${f%.c}")"
    cc -c -O2 -fPIC -I"$zlib_src" -o "$deps_build/zlib/$base.o" "$f"
  done
  ar rcs "$prefix/lib/libz.a" "$deps_build"/zlib/*.o

  cp "$zlib_src/zlib.h" "$zlib_src/zconf.h" "$prefix/include/"

  # The jpeg-9a outputs older builds of this script left in the codec prefix,
  # removed unconditionally before LibRaw and libultrahdr are configured:
  # LibRaw's include list also carries $prefix/include, and a leftover jpeg-9a
  # jpeglib.h there is exactly the shadowing MT_jpeglib.h exists to stop.
  rm -f "$prefix/include/jpeglib.h" "$prefix/include/jconfig.h" \
        "$prefix/include/jmorecfg.h" "$prefix/include/jerror.h" "$prefix/lib/libjpeg.a"
}

checkout_libjxl() {
  local dest="$SRC_DIR/libjxl-${LIBJXL_VERSION}"
  if [[ -d "$dest/.git" ]] && [[ "$(git -C "$dest" rev-parse HEAD)" == "$LIBJXL_GIT_REV" ]]; then
    return
  fi
  rm -rf "$dest"
  echo "Checking out libjxl ${LIBJXL_VERSION}..."
  git clone --quiet "$LIBJXL_GIT_URL" "$dest"
  git -C "$dest" checkout --quiet "$LIBJXL_GIT_REV"
  if [[ "$(git -C "$dest" rev-parse HEAD)" != "$LIBJXL_GIT_REV" ]]; then
    echo "ERROR: unexpected git revision for libjxl" >&2
    exit 1
  fi
  # Only the three submodules a decoder-only build links.
  git -C "$dest" submodule update --init --depth 1 \
      third_party/highway third_party/brotli third_party/skcms
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
  # Idempotent, and fails loudly if a LibRaw bump moves its anchors.
  python3 "$PATCHES_DIR/apply_libraw_jxl.py" "$SRC_DIR/LibRaw-${LIBRAW_VERSION}"
}

build_libraw() {
  local prefix="$1"
  pushd "$SRC_DIR/LibRaw-${LIBRAW_VERSION}" >/dev/null
  if [[ ! -x ./configure ]]; then
    autoreconf --force --install --verbose
  fi
  make distclean >/dev/null 2>&1 || true
  # RD-A (#2.11): jpeg + zlib ON -- jpeg from libjpeg-turbo's prefix
  # (build-jpeg.sh, FIRST on both flags so the link test is against turbo),
  # zlib from the engine's vendored copy staged in $prefix by
  # build_engine_codec_deps. jpeg is a real AC_CHECK_LIB link test
  # (LDFLAGS) plus AC_CHECK_HEADERS (CPPFLAGS); zlib is PKG_CHECK_MODULES,
  # which honours ZLIB_CFLAGS/ZLIB_LIBS directly without pkg-config.
  ./configure \
    --prefix="$prefix" \
    --disable-shared \
    --enable-static \
    --disable-openmp \
    --disable-lcms \
    --disable-examples \
    CFLAGS="-fPIC" \
    CXXFLAGS="-fPIC" \
    CPPFLAGS="-I$JPEG_PREFIX/include -I$prefix/include -DUSE_JXL" \
    LDFLAGS="-L$JPEG_PREFIX/lib -L$prefix/lib" \
    ZLIB_CFLAGS="-I$prefix/include" \
    ZLIB_LIBS="-L$prefix/lib -lz"

  # Both failure paths are SILENT (AC_MSG_WARN, then a green build without
  # the capability), so assert the flags actually took (they land in the
  # generated Makefile's CPPFLAGS).
  if ! grep -q "USE_JPEG" Makefile || ! grep -q "USE_ZLIB" Makefile; then
    echo "FATAL: LibRaw configure did not enable jpeg/zlib:" >&2
    grep -n "CPPFLAGS" Makefile | head -5 >&2
    exit 1
  fi
  # Same silent-failure shape: without USE_JXL the patched decoder compiles to
  # the throwing stub and JPEG XL DNGs stay unreadable.
  if ! grep -q "USE_JXL" Makefile; then
    echo "FATAL: LibRaw configure did not carry USE_JXL:" >&2
    grep -n "CPPFLAGS" Makefile | head -5 >&2
    exit 1
  fi

  make -j "$JOBS"
  make install
  popd >/dev/null
}

require_file() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    echo "ERROR: expected file not found: $file" >&2
    exit 1
  fi
}

require_symbol() {
  local symbol="$1"
  if ! nm -g "$OUT_LIB" 2>/dev/null | c++filt | grep -F "$symbol" >/dev/null; then
    echo "ERROR: expected symbol not found in $OUT_LIB: $symbol" >&2
    exit 1
  fi
}

# Download and extract sources
download_archive "tiff-${TIFF_VERSION}" "$TIFF_URL" "$TIFF_SHA256"
download_archive "libwebp-${WEBP_VERSION}" "$WEBP_URL" "$WEBP_SHA256"
download_archive "libavif-${AVIF_VERSION}" "$AVIF_URL" "$AVIF_SHA256"
download_archive "LibRaw-${LIBRAW_VERSION}" "$LIBRAW_URL" "$LIBRAW_SHA256"
download_archive "lcms${LCMS2_VERSION}" "$LCMS2_URL" "$LCMS2_SHA256"
if [[ "$UHDR_WANTED" != "0" ]]; then
  download_archive "libultrahdr-${LIBULTRAHDR_VERSION}" "$LIBULTRAHDR_URL" "$LIBULTRAHDR_SHA256"
fi

extract_archive "tiff-${TIFF_VERSION}" "tiff-${TIFF_VERSION}"
extract_archive "libwebp-${WEBP_VERSION}" "libwebp-${WEBP_VERSION}"
extract_archive "libavif-${AVIF_VERSION}" "libavif-${AVIF_VERSION}"
checkout_git_tag "libgav1-v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_URL" "v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_REV"
extract_archive "LibRaw-${LIBRAW_VERSION}" "LibRaw-${LIBRAW_VERSION}"
extract_archive "lcms${LCMS2_VERSION}" "Little-CMS-lcms${LCMS2_VERSION}"
if [[ "$UHDR_WANTED" != "0" ]]; then
  extract_archive "libultrahdr-${LIBULTRAHDR_VERSION}" "libultrahdr-${LIBULTRAHDR_VERSION}"
  # libultrahdr assumes libjpeg-turbo's raw mode; on jpeg-9a it overflowed its
  # chroma planes on every 4:2:0 JPEG. Built against libjpeg-turbo now, where
  # the patch is a no-op (see its header); kept so a jpeg-9 build stays safe.
  # Idempotent; fails loudly on a bump.
  python3 "$PATCHES_DIR/apply_libultrahdr_jpeg9_raw.py" "$SRC_DIR/libultrahdr-${LIBULTRAHDR_VERSION}"
fi
checkout_libjxl

# Link libgav1 into libavif ext dir
rm -rf "$SRC_DIR/libavif-${AVIF_VERSION}/ext/libgav1"
mkdir -p "$SRC_DIR/libavif-${AVIF_VERSION}/ext"
cp -R "$SRC_DIR/libgav1-v${LIBGAV1_VERSION}" "$SRC_DIR/libavif-${AVIF_VERSION}/ext/libgav1"

# Clean previous builds
rm -rf "$BUILD_DIR" "$PREFIX_DIR"
mkdir -p "$BUILD_DIR" "$PREFIX_DIR"

# Build TIFF
build_cmake "$SRC_DIR/tiff-${TIFF_VERSION}" "$BUILD_DIR/tiff" "$PREFIX_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
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

# Build WebP
build_cmake "$SRC_DIR/libwebp-${WEBP_VERSION}" "$BUILD_DIR/webp" "$PREFIX_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
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

# Build AVIF (with embedded libgav1)
build_cmake "$SRC_DIR/libavif-${AVIF_VERSION}" "$BUILD_DIR/avif" "$PREFIX_DIR" \
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
  -DAVIF_LIBXML2=OFF

# libjxl, DECODER ONLY -- every tool, test, plugin and encoder extra is off.
build_cmake "$SRC_DIR/libjxl-${LIBJXL_VERSION}" "$BUILD_DIR/libjxl" "$PREFIX_DIR" \
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

# Build the engine zlib archive, then LibRaw (patched for JPEG XL) against it
# and libjpeg-turbo's prefix.
build_engine_codec_deps "$PREFIX_DIR"
if [[ ! -f "$JPEG_PREFIX/lib/libjpeg.a" || ! -f "$JPEG_PREFIX/include/jpeglib.h" ]]; then
  echo "ERROR: libjpeg-turbo prefix $JPEG_PREFIX is incomplete -- run platform/Linux/build-jpeg.sh first" >&2
  exit 1
fi
patch_libraw_jxl
build_libraw "$PREFIX_DIR"

# Build lcms2 (Linux-only CMS backend). MIT-licensed and store-safe.
#
# TOOLS AND TESTS OFF, and the tools are not merely waste. LCMS2_BUILD_TOOLS
# defaults to ON and builds tificc/tifdiff/jpgicc, which `find_package(TIFF)`
# -- and that finds the libtiff THIS SCRIPT just installed into the prefix,
# whose exported target names CMath::CMath, a target its config does not teach
# the consumer to define. The generate step then fails with
#
#   The link interface of target "TIFF::tiff" contains: CMath::CMath
#   but the target was not found.
#
# A machine with libtiff-dev installed never sees it, because find_package
# picks the system copy first; a clean CI runner has only ours. Measured on
# GitHub's ubuntu image 2026-09-03. Nothing here consumes the command-line
# tools, so the dependency should never have been taken. The Windows script has
# carried these four options since it was written, with a note that they are
# independent of BUILD_SHARED_LIBS; Linux is the copy that never got them.
build_cmake "$SRC_DIR/Little-CMS-lcms${LCMS2_VERSION}" "$BUILD_DIR/lcms2" "$PREFIX_DIR" \
  -DBUILD_SHARED_LIBS=OFF \
  -DLCMS2_BUILD_SHARED=OFF \
  -DLCMS2_BUILD_STATIC=ON \
  -DLCMS2_BUILD_TOOLS=OFF \
  -DLCMS2_BUILD_TESTS=OFF \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON

# libultrahdr (P3b): against libjpeg-turbo's prefix (build-jpeg.sh). Same pins
# as the macOS script, for the same reasons
# -- here the thing find_package(JPEG) would otherwise find is the distro's
# libjpeg-turbo, a second jpeg_* ABI beside the engine's. CMAKE_INSTALL_LIBDIR
# is pinned to lib: libultrahdr installs through GNUInstallDirs, which says
# lib64 on Fedora-family hosts, and everything below reads $PREFIX_DIR/lib.
if [[ "$UHDR_WANTED" != "0" ]]; then
  build_cmake "$SRC_DIR/libultrahdr-${LIBULTRAHDR_VERSION}" "$BUILD_DIR/libultrahdr" "$PREFIX_DIR" \
    -DBUILD_SHARED_LIBS=OFF \
    -DUHDR_BUILD_EXAMPLES=OFF -DUHDR_BUILD_TESTS=OFF -DUHDR_BUILD_BENCHMARK=OFF \
    -DUHDR_BUILD_FUZZERS=OFF -DUHDR_BUILD_DEPS=OFF -DUHDR_BUILD_JAVA=OFF -DUHDR_BUILD_PACKAGING=OFF \
    -DUHDR_ENABLE_LOGS=OFF -DUHDR_ENABLE_INSTALL=ON -DUHDR_ENABLE_INTRINSICS=ON \
    -DUHDR_ENABLE_GLES=OFF -DUHDR_ENABLE_WERROR=OFF -DUHDR_ENABLE_SMPTE2094_50=OFF \
    -DUHDR_ENABLE_HEIF=OFF -DUHDR_WRITE_XMP=OFF -DUHDR_WRITE_ISO=ON \
    -DUHDR_MAX_DIMENSION=32768 \
    -DCMAKE_INSTALL_LIBDIR=lib \
    -DJPEG_INCLUDE_DIR="$JPEG_PREFIX/include" \
    -DJPEG_LIBRARY="$JPEG_PREFIX/lib/libjpeg.a" \
    -DJPEG_LIBRARY_RELEASE="$JPEG_PREFIX/lib/libjpeg.a" \
    "-DCMAKE_IGNORE_PREFIX_PATH=/usr/local"
  require_file "$PREFIX_DIR/lib/libuhdr.a"
  require_file "$PREFIX_DIR/include/ultrahdr_api.h"
fi

# Collect all static libs
INPUT_LIBS=(
  "$PREFIX_DIR/lib/libtiff.a"
  "$PREFIX_DIR/lib/libwebp.a"
  "$PREFIX_DIR/lib/libwebpdemux.a"
  "$PREFIX_DIR/lib/libsharpyuv.a"
  "$PREFIX_DIR/lib/libavif.a"
  "$PREFIX_DIR/lib/libraw.a"
  "$JPEG_LIB"
  "$PREFIX_DIR/lib/libz.a"
  "$PREFIX_DIR/lib/liblcms2.a"
  "$PREFIX_DIR/lib/libjxl.a"
  "$PREFIX_DIR/lib/libjxl_cms.a"
  "$PREFIX_DIR/lib/libhwy.a"
  "$PREFIX_DIR/lib/libbrotlidec.a"
  "$PREFIX_DIR/lib/libbrotlicommon.a"
)

if [[ "$UHDR_WANTED" != "0" ]]; then
  INPUT_LIBS+=("$PREFIX_DIR/lib/libuhdr.a")
fi

# Find libgav1 (built inside avif build tree)
GAV1_LIB="$(find "$BUILD_DIR/avif" -name "libgav1.a" -type f | head -n 1 || true)"
if [[ -n "$GAV1_LIB" ]]; then
  INPUT_LIBS+=("$GAV1_LIB")
fi

# Verify all input libs exist
for lib in "${INPUT_LIBS[@]}"; do
  require_file "$lib"
done

# Merge into single static library using MRI script
rm -f "$OUT_LIB"
# Create MRI script for GNU ar
MRI_SCRIPT="$(mktemp)"
echo "CREATE $OUT_LIB" > "$MRI_SCRIPT"
for lib in "${INPUT_LIBS[@]}"; do
  echo "ADDLIB $lib" >> "$MRI_SCRIPT"
done
echo "SAVE" >> "$MRI_SCRIPT"
echo "END" >> "$MRI_SCRIPT"
ar -M < "$MRI_SCRIPT"
rm -f "$MRI_SCRIPT"

ranlib "$OUT_LIB" 2>/dev/null || true

# Verify critical symbols
require_file "$OUT_LIB"
require_symbol "TIFFOpen"
require_symbol "WebPDecodeRGBA"
require_symbol "WebPDemuxGetFrame"
require_symbol "avifDecoderCreate"
require_symbol "LibRaw::open_file(char const*)"
require_symbol "jpeg_mem_src"
# libjpeg-turbo, not any other libjpeg (jpeg_skip_scanlines is turbo-only), and
# jpegtran's transupp, which lossless JPEG rotation calls.
require_symbol "jpeg_skip_scanlines"
require_symbol "jtransform_execute_transform"
require_symbol "uncompress"
require_symbol "cmsCreateTransform"
require_symbol "JxlDecoderCreate"
# The decoder we patch into LibRaw. Without this the bundle can still contain
# libjxl while every JPEG XL DNG stays unreadable -- checked on the shipped
# artifact, not on a Makefile.
require_symbol "LibRaw::jxl_dng_load_raw()"
# The three entry points the engine's MT_UltraHdr wrapper calls.
if [[ "$UHDR_WANTED" != "0" ]]; then
  require_symbol "uhdr_encode"
  require_symbol "uhdr_decode"
  require_symbol "is_uhdr_image"
fi

# Headers travel WITH the archive (Phase 2).
rm -rf "$OUT_LIB_DIR/image-codecs/include"
mkdir -p "$OUT_LIB_DIR/image-codecs"
cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/image-codecs/include"
# libultrahdr's licence texts travel with the archive -- before the stamp, as
# on macOS: a stamp-hit run never stages them again.
rm -rf "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr"
if [[ "$UHDR_WANTED" != "0" ]]; then
  UHDR_SRC="$SRC_DIR/libultrahdr-${LIBULTRAHDR_VERSION}"
  mkdir -p "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr"
  cp "$UHDR_SRC/LICENSE" "$UHDR_SRC/LICENSE-APACHE" "$UHDR_SRC/LICENSE-MIT" \
     "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr/"
  cp "$UHDR_SRC/adobe-hdr-gain-map-license/NOTICE" \
     "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr/NOTICE-adobe-gain-map"
  cp "$UHDR_SRC/third_party/image_io/LICENSE" \
     "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr/LICENSE-image_io"
  # modp_b64.cc is compiled into image_io (BSD-3-Clause): its licence ships too.
  cp "$UHDR_SRC/third_party/image_io/src/modp_b64/LICENSE" \
     "$OUT_LIB_DIR/image-codecs/licenses/libultrahdr/LICENSE-modp_b64"
fi
echo -n "$stamp_value" > "$STAMP_FILE"
echo "Image codec bundle built: $OUT_LIB (headers staged to $OUT_LIB_DIR/image-codecs/include)"
