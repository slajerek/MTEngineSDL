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

# libultrahdr -- the ISO 21496-1 gain-map JPEG ("Ultra HDR") codec, for the
# photo app's HDR rendered-edit cache. Licence: Apache-2.0 AND
# MIT, with the bundled image_io under Apache-2.0; it includes Adobe gain-map
# technology whose NOTICE travels in LICENSES.txt (vocabulary.json). Pinned by
# the SHA-256 of GitHub's archive of tag v2.0.2 (commit
# e5f5a022fe96fc4dc2ee35c19f733a50df807abe), downloaded into $DOWNLOAD_DIR --
# the image-codec work root, outside every checkout -- like every tarball here.
# Built against the engine's ONE libjpeg -- libjpeg-turbo, built by
# build-jpeg.sh into $(mt_caps_work_dir jpeg)/install-<arch> before this script
# runs: a second libjpeg in one process would be two jpeg_* ABIs, one of them
# chosen by link order.
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
  rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-* "$OUT_LIB" "$STAMP_FILE"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

# libjpeg-turbo (build-jpeg.sh, the jpeg unit) -- the engine's one libjpeg. Its
# archive is read from the jpeg unit's STORE (MT_STORE_JPEG, emitted for every
# unit), else from the view -- never from this script's own $OUT_LIB_DIR, which
# is the image-codecs store. Its stamp joins both of this bundle's stamps, so a
# new turbo rebuilds the bundle (full and stub alike).
JPEG_LIB_DIR="${MT_STORE_JPEG:-$MT_VIEW_LIB_DIR}"
JPEG_LIB="$JPEG_LIB_DIR/libmt_jpeg.a"
JPEG_STAMP_FILE="$JPEG_LIB_DIR/libmt_jpeg.stamp"
JPEG_WORK_DIR="$(mt_caps_work_dir jpeg)"
if [[ ! -f "$JPEG_LIB" || ! -f "$JPEG_STAMP_FILE" ]]; then
  echo "ERROR: $JPEG_LIB (or its stamp) is missing -- run platform/MacOS/build-jpeg.sh first" >&2
  exit 1
fi
JPEG_STAMP="$(cat "$JPEG_STAMP_FILE")"

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
# One archive serves seven flags, so it is skipped only when EVERY one is off: any
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
  # jpeg_* on every configuration, and the bundle is the archive every app
  # links -- so a codec-off app links turbo through the stub.
  mt_caps_stub_archive "$OUT_LIB.stub.a" "mt_image_codecs"
  rm -f "$OUT_LIB"
  libtool -static -o "$OUT_LIB" "$OUT_LIB.stub.a" "$JPEG_LIB" 2> >(grep -v "has no symbols" >&2)
  rm -f "$OUT_LIB.stub.a"
  if ! nm -g -arch all "$OUT_LIB" 2>/dev/null | grep " T _jpeg_skip_scanlines" >/dev/null; then
    echo "ERROR: the stub bundle $OUT_LIB does not carry libjpeg-turbo (_jpeg_skip_scanlines)" >&2
    exit 1
  fi
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

stamp_value="${script_sha}:jpeg=${JPEG_STAMP}:tiff-${TIFF_VERSION}:webp-${WEBP_VERSION}:avif-${AVIF_VERSION}:libgav1-${LIBGAV1_VERSION}:libraw-${LIBRAW_VERSION}:libjxl-${LIBJXL_VERSION}:libultrahdr-${LIBULTRAHDR_VERSION}-${UHDR_WANTED}-${LIBULTRAHDR_PATCH_REV}:${DEPLOYMENT_TARGET}"
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
  local part="$archive.part"
  local actual_sha

  # A file at $archive is only ever one that passed the hash below (it is
  # renamed into place after the check). A mismatch here therefore means the
  # PIN changed under the same name -- a same-version re-pin -- so the old
  # file is discarded and fetched again instead of blocking every later run.
  if [[ -f "$archive" ]]; then
    actual_sha="$(shasum -a 256 "$archive" | cut -d ' ' -f 1)"
    if [[ "$actual_sha" == "$sha256" ]]; then
      return
    fi
    echo "Cached $archive does not match the pinned SHA-256 (pin changed?) -- downloading again." >&2
    rm -f "$archive"
  fi

  # Into a .part file, verified, THEN renamed: an interrupted or corrupt
  # download must never sit at the final name, where the check above would
  # stop every later run with a checksum error nobody can act on.
  rm -f "$part"
  echo "Downloading $name..."
  curl -L --fail --show-error "$url" -o "$part"
  actual_sha="$(shasum -a 256 "$part" | cut -d ' ' -f 1)"
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
  [[ "$(cat "$dest/.mt-extracted")" == "$(shasum -a 256 "$archive" | cut -d ' ' -f 1)" ]]
}

mark_extracted() {
  local archive="$1"
  local dest="$2"
  shasum -a 256 "$archive" | cut -d ' ' -f 1 | tr -d '\n' > "$dest/.mt-extracted"
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

extract_flat_archive() {
  local name="$1"
  local top_dir="$2"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local dest="$SRC_DIR/$top_dir"

  if extracted_ok "$archive" "$dest"; then
    return
  fi

  rm -rf "$dest"
  echo "Extracting $name..."
  mkdir -p "$dest"
  tar -xzf "$archive" -C "$dest"
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

# RD-A: compile the engine's own vendored zlib into per-arch static archives
# so LibRaw's configure can use it. One copy of the library in the repo
# (roadmap #2.11). The archive is ALSO merged into libmt_image_codecs.a below,
# so the bundle stays self-contained and GNU ld's single-pass archive
# resolution on Linux never needs a back-reference into libMTEngineSDL.a
# (RD-A design #4.2.1 route (a)). jpeg is no longer built here: the engine's
# one libjpeg is libjpeg-turbo, from build-jpeg.sh (JPEG_PREFIX below), and
# its archive is merged into the bundle the same way.
build_engine_codec_deps() {
  local arch="$1"
  local prefix="$2"
  local zlib_src="$ROOT_DIR/src/Engine/Libs/zlib"
  local deps_build="$BUILD_DIR/engine-deps-$arch"

  rm -rf "$deps_build"
  mkdir -p "$deps_build/zlib" "$prefix/lib" "$prefix/include"

  local f base
  for f in "$zlib_src"/*.c; do
    base="$(basename "${f%.c}")"
    clang -c -O2 -arch "$arch" -mmacosx-version-min="$DEPLOYMENT_TARGET"       -I"$zlib_src" -o "$deps_build/zlib/$base.o" "$f"
  done
  libtool -static -o "$prefix/lib/libz.a" "$deps_build"/zlib/*.o

  cp "$zlib_src/zlib.h" "$zlib_src/zconf.h" "$prefix/include/"

  # The jpeg-9a outputs older builds of this script left in the codec prefix.
  # Removed unconditionally, before LibRaw and libultrahdr are configured:
  # LibRaw's include list also carries $prefix/include, and a leftover jpeg-9a
  # jpeglib.h there is exactly the shadowing MT_jpeglib.h exists to stop.
  rm -f "$prefix/include/jpeglib.h" "$prefix/include/jconfig.h" \
        "$prefix/include/jmorecfg.h" "$prefix/include/jerror.h" "$prefix/lib/libjpeg.a"
}

# The jpeg unit's per-arch install prefix (build-jpeg.sh), read-only here.
jpeg_prefix_for() {
  local arch="$1"
  local p="$JPEG_WORK_DIR/install-$arch"
  if [[ ! -f "$p/lib/libjpeg.a" || ! -f "$p/include/jpeglib.h" ]]; then
    echo "ERROR: libjpeg-turbo prefix $p is incomplete -- run platform/MacOS/build-jpeg.sh first" >&2
    exit 1
  fi
  printf '%s' "$p"
}

build_libraw() {
  local arch="$1"
  local prefix="$2"
  local jpeg_prefix="$3"

  pushd "$SRC_DIR/LibRaw-${LIBRAW_VERSION}" >/dev/null
  if [[ ! -x ./configure ]]; then
    autoreconf --force --install --verbose
  fi
  make distclean >/dev/null 2>&1 || true
  # RD-A (#2.11): jpeg + zlib ON -- jpeg from libjpeg-turbo's prefix
  # (build-jpeg.sh), zlib from the engine's vendored copy staged in $prefix by
  # build_engine_codec_deps. Two different mechanisms:
  #   jpeg -- AC_CHECK_LIB link test (needs -ljpeg findable via LDFLAGS) plus
  #           AC_CHECK_HEADERS (needs jpeglib.h on CPPFLAGS); the turbo prefix
  #           comes FIRST on both, so the link test is against turbo's libjpeg.a;
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
    CPPFLAGS="-I$jpeg_prefix/include -I$prefix/include -DUSE_JXL" \
    LDFLAGS="-arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET -L$jpeg_prefix/lib -L$prefix/lib" \
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

# libultrahdr, per architecture, against THIS arch's libjpeg-turbo (the jpeg
# unit's prefix, build-jpeg.sh). Every option that could reach outside our
# own tree is pinned OFF:
#   UHDR_ENABLE_HEIF   defaults TRUE and runs find_package(libheif) -- which on a
#                      developer Mac finds Homebrew's (CMakeLists.txt:130, 643).
#   UHDR_BUILD_DEPS    would git-clone libjpeg-turbo at configure time.
#   JPEG_*             given explicitly, so find_package(JPEG) never searches;
#                      CMAKE_IGNORE_PREFIX_PATH is the belt to those braces.
#   UHDR_MAX_DIMENSION defaults to 8192 (lib/src/jpegdecoderhelper.cpp:48-57); a
#                      45 Mpx sensor is 8256 px wide. 32768 covers every camera.
#   UHDR_WRITE_XMP     OFF: XMP mode cannot signal a multi-channel gain map.
build_libultrahdr() {
  local arch="$1"
  local prefix="$2"
  local jpeg_prefix="$3"
  build_cmake "$SRC_DIR/libultrahdr-${LIBULTRAHDR_VERSION}" "$BUILD_DIR/libultrahdr-$arch" "$prefix" "$arch" \
    -DBUILD_SHARED_LIBS=OFF \
    -DUHDR_BUILD_EXAMPLES=OFF \
    -DUHDR_BUILD_TESTS=OFF \
    -DUHDR_BUILD_BENCHMARK=OFF \
    -DUHDR_BUILD_FUZZERS=OFF \
    -DUHDR_BUILD_DEPS=OFF \
    -DUHDR_BUILD_JAVA=OFF \
    -DUHDR_BUILD_PACKAGING=OFF \
    -DUHDR_ENABLE_LOGS=OFF \
    -DUHDR_ENABLE_INSTALL=ON \
    -DUHDR_ENABLE_INTRINSICS=ON \
    -DUHDR_ENABLE_GLES=OFF \
    -DUHDR_ENABLE_WERROR=OFF \
    -DUHDR_ENABLE_SMPTE2094_50=OFF \
    -DUHDR_ENABLE_HEIF=OFF \
    -DUHDR_WRITE_XMP=OFF \
    -DUHDR_WRITE_ISO=ON \
    -DUHDR_MAX_DIMENSION=32768 \
    -DJPEG_INCLUDE_DIR="$jpeg_prefix/include" \
    -DJPEG_LIBRARY="$jpeg_prefix/lib/libjpeg.a" \
    -DJPEG_LIBRARY_RELEASE="$jpeg_prefix/lib/libjpeg.a" \
    -DCMAKE_FIND_FRAMEWORK=NEVER \
    "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local"
  require_file "$prefix/lib/libuhdr.a"
  require_file "$prefix/include/ultrahdr_api.h"
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
if [[ "$UHDR_WANTED" != "0" ]]; then
  download_archive "libultrahdr-${LIBULTRAHDR_VERSION}" "$LIBULTRAHDR_URL" "$LIBULTRAHDR_SHA256"
fi

extract_archive "tiff-${TIFF_VERSION}" "tiff-${TIFF_VERSION}"
extract_archive "libwebp-${WEBP_VERSION}" "libwebp-${WEBP_VERSION}"
extract_archive "libavif-${AVIF_VERSION}" "libavif-${AVIF_VERSION}"
checkout_git_tag "libgav1-v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_URL" "v${LIBGAV1_VERSION}" "$LIBGAV1_GIT_REV"
extract_archive "LibRaw-${LIBRAW_VERSION}" "LibRaw-${LIBRAW_VERSION}"
checkout_libjxl
if [[ "$UHDR_WANTED" != "0" ]]; then
  extract_archive "libultrahdr-${LIBULTRAHDR_VERSION}" "libultrahdr-${LIBULTRAHDR_VERSION}"
  # libultrahdr assumes libjpeg-turbo's raw mode; on jpeg-9a it overflowed its
  # chroma planes on every 4:2:0 JPEG. Built against libjpeg-turbo now, where
  # the patch is a no-op (see its header); kept so a jpeg-9 build stays safe.
  # Idempotent; fails loudly on a bump.
  python3 "$PATCHES_DIR/apply_libultrahdr_jpeg9_raw.py" "$SRC_DIR/libultrahdr-${LIBULTRAHDR_VERSION}"
fi

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
  JPEG_PREFIX="$(jpeg_prefix_for "$arch")" || exit 1
  patch_libraw_jxl
  build_libraw "$arch" "$arch_prefix" "$JPEG_PREFIX"
  if [[ "$UHDR_WANTED" != "0" ]]; then
    build_libultrahdr "$arch" "$arch_prefix" "$JPEG_PREFIX"
  fi
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
if [[ "$UHDR_WANTED" != "0" ]]; then
  combine_universal_lib "lib/libuhdr.a"
fi

INPUT_LIBS=(
  "$PREFIX_DIR/lib/libtiff.a"
  "$PREFIX_DIR/lib/libwebp.a"
  "$PREFIX_DIR/lib/libwebpdemux.a"
  "$PREFIX_DIR/lib/libsharpyuv.a"
  "$PREFIX_DIR/lib/libavif.a"
  "$PREFIX_DIR/lib/libraw.a"
  "$JPEG_LIB"
  "$PREFIX_DIR/lib/libz.a"
  "$PREFIX_DIR/lib/libgav1.a"
  "$PREFIX_DIR/lib/libjxl.a"
  "$PREFIX_DIR/lib/libjxl_cms.a"
  "$PREFIX_DIR/lib/libhwy.a"
  "$PREFIX_DIR/lib/libbrotlidec.a"
  "$PREFIX_DIR/lib/libbrotlicommon.a"
)
if [[ "$UHDR_WANTED" != "0" ]]; then
  INPUT_LIBS+=("$PREFIX_DIR/lib/libuhdr.a")
fi

for lib in "${INPUT_LIBS[@]}"; do
  require_file "$lib"
  require_universal "$lib"
done

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${INPUT_LIBS[@]}" 2> >(grep -v "has no symbols" >&2)
ranlib "$OUT_LIB" || true
require_universal "$OUT_LIB"

require_symbol "_TIFFOpen"
require_symbol "_WebPDecodeRGBA"
require_symbol "_WebPDemuxGetFrame"
require_symbol "_avifDecoderCreate"
require_symbol "LibRaw::open_file(char const*)"
require_symbol "_jpeg_mem_src"
# libjpeg-turbo, not any other libjpeg (jpeg_skip_scanlines is turbo-only), and
# jpegtran's transupp, which lossless JPEG rotation calls.
require_symbol "_jpeg_skip_scanlines"
require_symbol "_jtransform_execute_transform"
require_symbol "_uncompress"
require_symbol "_JxlDecoderCreate"
# The decoder we patch into LibRaw. Without this the bundle can still contain
# libjxl while every JPEG XL DNG stays unreadable -- the exact failure the
# USE_JXL configure assertion above also guards, checked here on the shipped
# artifact rather than on a Makefile.
require_symbol "LibRaw::jxl_dng_load_raw()"
# The three entry points the engine's MT_UltraHdr wrapper calls -- checked on
# the SHIPPED archive, like the JPEG XL decoder above.
if [[ "$UHDR_WANTED" != "0" ]]; then
  require_symbol "_uhdr_encode"
  require_symbol "_uhdr_decode"
  require_symbol "_is_uhdr_image"
fi

# libultrahdr's licence texts travel with the archive (vocabulary.json carries
# the summary and the Adobe NOTICE into LICENSES.txt; these are the full texts).
# Staged BEFORE the stamp: a run that died after the stamp would leave a
# stamp-hit bundle with no licence texts, and the stamp-hit path never stages
# them again.
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
# Headers travel WITH the archive (Phase 2): every project used to read them
# from the in-checkout install tree this relocation retired.
rm -rf "$OUT_LIB_DIR/image-codecs/include"
mkdir -p "$OUT_LIB_DIR/image-codecs"
cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/image-codecs/include"
echo "Image codec bundle built: $OUT_LIB (headers staged to $OUT_LIB_DIR/image-codecs/include)"
