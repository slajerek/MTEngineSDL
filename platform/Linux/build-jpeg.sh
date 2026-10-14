#!/usr/bin/env bash
set -euo pipefail

# Build libjpeg-turbo for Linux as ONE static PIC archive, libmt_jpeg.a
# (turbo's libjpeg + jpegtran's transupp), plus its headers and the install
# prefix the image-codec script builds libultrahdr and LibRaw against.
#
# libjpeg-turbo: the engine's ONE jpeg_* library (plan 2026-10-07-libjpeg-turbo).
# Core, not a codec: JPEGWriter needs it on EVERY configuration, so this runs
# before build-image_codecs.sh, which builds libultrahdr and LibRaw against
# the prefix below and merges libmt_jpeg.a into its bundle (stub included).
#
# HIDDEN VISIBILITY (-fvisibility=hidden), the Linux exception stated rather
# than hidden: libheif is a SYSTEM package here and may load the distro's own
# libjpeg.so (jpeg-8 API). GNU ld exports an executable's symbol into .dynsym
# whenever a shared library linked at link time references it, so libheif's
# jpeg_* calls could bind to OUR jpeg-6b-API turbo and corrupt memory. A hidden
# symbol in a static archive still links into the same executable (the engine,
# libultrahdr and LibRaw resolve it at static link time) but is never exported.
# The same hiding protects GTK3's dlopened gdk-pixbuf JPEG loader.
#
# Outputs (all into this unit's STORE; the exit trap copies them to the view):
#   libmt_jpeg.a, libmt_jpeg.stamp, jpeg/include/{jpeglib,jconfig,jmorecfg,jerror,transupp}.h
# Work prefix (read by build-image_codecs.sh, outside every checkout):
#   $(mt_caps_work_dir jpeg)/install-$(uname -m)/{lib,include}
#
# Environment:
#   MT_PROD_BUILD=1      a final build (exported by tools/appbuild/app-build-*
#                        for --prod): a build whose SIMD is off -- x86_64
#                        without nasm, or any slice whose requested SIMD turbo's
#                        configure turned off -- is an ERROR instead of a
#                        warning. (On aarch64 an older GCC only draws turbo's
#                        "suboptimal Neon" warning; SIMD stays on.)
#   MT_JPEG_X86_SIMD=0   force the scalar x86_64 path (the test override).
#   MT_JPEG_TEST_TOOL_DIR  TEST ONLY: a directory put first on PATH (the stamp
#                        regression test's fake tools,
#                        tools/mtcaps/tests/test_build_jpeg_stamp.py).
#
# BUILD TOOL: nasm, for x86_64 SIMD (Debian/Ubuntu: sudo apt install nasm;
# Fedora: sudo dnf install nasm). PATH order is the codec script's.
#
# UNVERIFIED on a Linux machine as of 2026-10-09 (written on macOS beside the
# verified macOS twin); the Linux pass is Task 7 of the plan.
export PATH="${MT_JPEG_TEST_TOOL_DIR:+$MT_JPEG_TEST_TOOL_DIR:}/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
CACHE_DIR="$(mt_caps_work_dir jpeg)"
DOWNLOAD_DIR="$CACHE_DIR/downloads"
SRC_DIR="$CACHE_DIR/src"
BUILD_DIR="$CACHE_DIR/build-linux"
PREFIX_DIR="$CACHE_DIR/install"
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). libjpeg-turbo reads no capability: one bucket per machine.
mt_caps_use_store "${MT_STORE_JPEG:-}" jpeg
OUT_LIB="$OUT_LIB_DIR/libmt_jpeg.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_jpeg.stamp"
JOBS="${MT_BUILD_JOBS:-$(nproc 2>/dev/null || echo 4)}"
ARCH="$(uname -m)"
ARCH_PREFIX="$PREFIX_DIR-$ARCH"   # = $(mt_caps_work_dir jpeg)/install-$(uname -m), the path the codec script reads

# Same pin as platform/MacOS/build-jpeg.sh (newest stable on 2026-10-09).
LIBJPEG_TURBO_VERSION="3.2.0"
LIBJPEG_TURBO_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${LIBJPEG_TURBO_VERSION}/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}.tar.gz"
LIBJPEG_TURBO_SHA256="6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e"

JPEG_HEADERS=(jpeglib.h jconfig.h jmorecfg.h jerror.h transupp.h)

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning libjpeg-turbo build artifacts..."
  rm -rf "$BUILD_DIR" "$CACHE_DIR"/install-* "$OUT_LIB" "$STAMP_FILE" "$OUT_LIB_DIR/jpeg/include"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

sha256_of() {
  if command -v sha256sum >/dev/null 2>&1; then
    sha256sum "$1" | cut -d ' ' -f 1
  else
    shasum -a 256 "$1" | cut -d ' ' -f 1
  fi
}
script_sha="$(sha256_of "${BASH_SOURCE[0]}" 2>/dev/null || echo unknown)"

# STAMP RULE 1 -- the SIMD decision and the prod refusal come FIRST, before any
# stamp comparison (see the macOS twin). aarch64's decision exists only after
# turbo's configure, so it is recorded in the stamp instead and a final build
# refuses any stamp holding simd=0 (stamp_hit below).
turbo_simd_for_arch() {
  local arch="$1"
  if [[ "$arch" == "x86_64" ]]; then
    if [[ "${MT_JPEG_X86_SIMD:-1}" == "0" ]] || ! command -v nasm >/dev/null 2>&1; then
      if [[ "${MT_PROD_BUILD:-0}" == "1" ]]; then
        echo "error: libjpeg-turbo x86_64 needs nasm for SIMD in a final build (install it: sudo apt install nasm)" >&2; exit 1
      fi
      echo "warning: libjpeg-turbo x86_64 slice built WITHOUT SIMD (nasm not found or MT_JPEG_X86_SIMD=0; install it: sudo apt install nasm)" >&2
      echo 0; return
    fi
  fi
  echo 1
}
# (the subshell's exit does not stop the script by itself)
SIMD_REQUESTED="$(turbo_simd_for_arch "$ARCH")" || exit 1

stamp_value_for() {
  printf '%s:libjpeg-turbo-%s:linux:%s:simd=%s' "$script_sha" "$LIBJPEG_TURBO_VERSION" "$ARCH" "$1"
}

# STAMP RULE 2 -- a hit requires every artefact the CONSUMERS read: the store's
# archive and staged headers AND the work prefix build-image_codecs.sh reads.
stamp_hit() {
  [[ -f "$STAMP_FILE" && -f "$OUT_LIB" && -f "$OUT_LIB_DIR/jpeg/include/jpeglib.h" ]] || return 1
  local stamp; stamp="$(cat "$STAMP_FILE")"
  if [[ "$ARCH" == "x86_64" ]]; then
    [[ "$stamp" == "$(stamp_value_for "$SIMD_REQUESTED")" ]] || return 1
  else
    # Decided by configure: either recorded value matches...
    [[ "$stamp" == "$(stamp_value_for 1)" || "$stamp" == "$(stamp_value_for 0)" ]] || return 1
  fi
  # ...but a final build never accepts a scalar slice from the cache.
  if [[ "${MT_PROD_BUILD:-0}" == "1" && "$stamp" == *"simd=0"* ]]; then
    return 1
  fi
  [[ -f "$ARCH_PREFIX/lib/libjpeg.a" && -f "$ARCH_PREFIX/lib/libjpeg_transupp.a" ]] || return 1
  local h
  for h in "${JPEG_HEADERS[@]}"; do
    [[ -f "$ARCH_PREFIX/include/$h" ]] || return 1
  done
  return 0
}

if stamp_hit; then
  echo "libjpeg-turbo: up to date (stamp match), skipping build"
  exit 0
fi

download_archive() {
  local name="$1"
  local url="$2"
  local sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"
  local part="$archive.part"
  local actual_sha

  # See the codec script: a file at $archive has passed the hash; a mismatch
  # means the PIN changed, so it is fetched again.
  if [[ -f "$archive" ]]; then
    actual_sha="$(sha256_of "$archive")"
    if [[ "$actual_sha" == "$sha256" ]]; then
      return
    fi
    echo "Cached $archive does not match the pinned SHA-256 (pin changed?) -- downloading again." >&2
    rm -f "$archive"
  fi
  if ! command -v curl >/dev/null 2>&1; then
    echo "ERROR: curl is required to download libjpeg-turbo." >&2
    exit 1
  fi
  rm -f "$part"
  echo "Downloading $name..."
  curl -L --fail --show-error "$url" -o "$part"
  actual_sha="$(sha256_of "$part")"
  if [[ "$actual_sha" != "$sha256" ]]; then
    rm -f "$part"
    echo "ERROR: checksum mismatch for $name (downloaded from $url); the partial file was deleted" >&2
    echo "Expected: $sha256" >&2
    echo "Actual:   $actual_sha" >&2
    exit 1
  fi
  mv -f "$part" "$archive"
}

extracted_ok() {
  local archive="$1"
  local dest="$2"
  [[ -d "$dest" && -f "$dest/.mt-extracted" ]] || return 1
  [[ "$(cat "$dest/.mt-extracted")" == "$(sha256_of "$archive")" ]]
}

mark_extracted() {
  local archive="$1"
  local dest="$2"
  sha256_of "$archive" | tr -d '\n' > "$dest/.mt-extracted"
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

# The codec script's Linux build_cmake: NO arch argument (shift 3).
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

require_file() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    echo "ERROR: expected file not found: $file" >&2
    exit 1
  fi
}

download_archive "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}" "$LIBJPEG_TURBO_URL" "$LIBJPEG_TURBO_SHA256"
extract_archive "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}" "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}"

SRC="$SRC_DIR/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}"
BUILD="$BUILD_DIR/libjpeg-turbo"
LOG="$BUILD_DIR/libjpeg-turbo.log"
rm -rf "$BUILD_DIR" "$CACHE_DIR"/install-*
mkdir -p "$BUILD_DIR" "$ARCH_PREFIX"

build_cmake "$SRC" "$BUILD" "$ARCH_PREFIX" \
  -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
  -DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
  -DWITH_SIMD="$SIMD_REQUESTED" -DREQUIRE_SIMD=OFF \
  -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_INCLUDEDIR=include \
  -DCMAKE_C_FLAGS="-fexceptions -fvisibility=hidden" \
  "-DCMAKE_IGNORE_PREFIX_PATH=/usr/local" >"$LOG" 2>&1 \
  || { cat "$LOG" >&2; echo "ERROR: libjpeg-turbo build failed (log: $LOG)" >&2; exit 1; }

# What the build ACTUALLY got, from the INSTALLED jconfig.h (`#define
# WITH_SIMD 1`) -- not from configure's output: when requested SIMD fails (a
# NASM configure rejected, an unsupported CPU) turbo's simd_fail prints only
# "WARNING: SIMD extensions disabled: ..." and turns WITH_SIMD off; "SIMD
# extensions: None" appears only when WITH_SIMD=0 was asked for.
SIMD_ACTUAL=0
if grep -q '^#define WITH_SIMD 1' "$ARCH_PREFIX/include/jconfig.h" 2>/dev/null; then
  SIMD_ACTUAL=1
fi
if [[ "$SIMD_REQUESTED" == "1" && "$SIMD_ACTUAL" == "0" ]]; then
  WHY="$(grep -m1 "SIMD extensions disabled" "$LOG" || true)"
  if [[ "${MT_PROD_BUILD:-0}" == "1" ]]; then
    echo "error: libjpeg-turbo $ARCH built WITHOUT SIMD in a final build (${WHY:-jconfig.h has no WITH_SIMD}; see $LOG)" >&2; exit 1
  fi
  echo "warning: libjpeg-turbo $ARCH built WITHOUT SIMD (${WHY:-jconfig.h has no WITH_SIMD}; see $LOG)" >&2
fi

# transupp.c is jpegtran's, not the library's -- compiled against the build
# tree's generated headers for lossless JPEG rotation, hidden like the rest.
cc -c -O2 -fPIC -fexceptions -fvisibility=hidden \
  -I"$BUILD" -I"$SRC/src" -o "$BUILD/transupp.o" "$SRC/src/transupp.c"
rm -f "$ARCH_PREFIX/lib/libjpeg_transupp.a"
ar rcs "$ARCH_PREFIX/lib/libjpeg_transupp.a" "$BUILD/transupp.o"
cp "$SRC/src/transupp.h" "$ARCH_PREFIX/include/"

require_file "$ARCH_PREFIX/lib/libjpeg.a"
require_file "$ARCH_PREFIX/lib/libjpeg_transupp.a"
for h in "${JPEG_HEADERS[@]}"; do
  require_file "$ARCH_PREFIX/include/$h"
done

# One archive from the two, through a GNU ar MRI script (the codec script's
# merge pattern).
rm -f "$OUT_LIB"
MRI_SCRIPT="$(mktemp)"
{
  echo "CREATE $OUT_LIB"
  echo "ADDLIB $ARCH_PREFIX/lib/libjpeg.a"
  echo "ADDLIB $ARCH_PREFIX/lib/libjpeg_transupp.a"
  echo "SAVE"
  echo "END"
} > "$MRI_SCRIPT"
ar -M < "$MRI_SCRIPT"
rm -f "$MRI_SCRIPT"
ranlib "$OUT_LIB" 2>/dev/null || true
require_file "$OUT_LIB"

rm -rf "$OUT_LIB_DIR/jpeg/include"
mkdir -p "$OUT_LIB_DIR/jpeg/include"
for h in "${JPEG_HEADERS[@]}"; do
  cp "$ARCH_PREFIX/include/$h" "$OUT_LIB_DIR/jpeg/include/"
done

# The stamp LAST, recording what the build actually got.
printf '%s' "$(stamp_value_for "$SIMD_ACTUAL")" > "$STAMP_FILE"
echo "libjpeg-turbo ${LIBJPEG_TURBO_VERSION} built: $OUT_LIB (headers staged to $OUT_LIB_DIR/jpeg/include)"
