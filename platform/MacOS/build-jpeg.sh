#!/usr/bin/env bash
set -euo pipefail

# Build libjpeg-turbo for macOS as ONE universal static archive, libmt_jpeg.a
# (turbo's libjpeg + jpegtran's transupp), plus its headers and a per-arch
# install prefix the image-codec script builds libultrahdr and LibRaw against.
#
# libjpeg-turbo: the engine's ONE jpeg_* library (plan 2026-10-07-libjpeg-turbo).
# Core, not a codec: JPEGWriter needs it on EVERY configuration, so this runs
# before build-image_codecs.sh, which builds libultrahdr and LibRaw against
# the prefix below and merges libmt_jpeg.a into its bundle (stub included).
#
# Outputs (all into this unit's STORE; the exit trap copies them to the view):
#   libmt_jpeg.a            universal arm64 + x86_64
#   libmt_jpeg.stamp        script sha, turbo version, deployment target, and
#                           the SIMD state of every slice (arm64:simd=1,...)
#   jpeg/include/           jpeglib.h jconfig.h jmorecfg.h jerror.h transupp.h
# Work prefixes (read by build-image_codecs.sh, outside every checkout):
#   $(mt_caps_work_dir jpeg)/install-<arch>/{lib,include}
#
# Environment:
#   MT_PROD_BUILD=1      a final build (exported by tools/appbuild/app-build-*
#                        for --prod). An x86_64 slice without SIMD (no nasm) is
#                        then an ERROR instead of a warning: a shipped Intel
#                        slice must not silently lose SIMD.
#   MT_JPEG_X86_SIMD=0   force the scalar x86_64 path (the test override).
#   MT_JPEG_TEST_TOOL_DIR  TEST ONLY: a directory put first on PATH, ahead of
#                        everything below -- the stamp regression test
#                        (tools/mtcaps/tests/test_build_jpeg_stamp.py) puts its
#                        fake cmake/clang/libtool/lipo/nasm there.
#
# BUILD TOOL: nasm, for the x86_64 slice's SIMD. MacPorts: `sudo port install
# nasm` (lands in /opt/local/bin, searched first below). Without it a
# development build warns and builds that slice scalar; a final build fails.
#
# PATH order is the codec script's (build-image_codecs.sh): MacPorts first, so
# both scripts pick the same cmake, and /opt/homebrew is never preferred to it.
export PATH="${MT_JPEG_TEST_TOOL_DIR:+$MT_JPEG_TEST_TOOL_DIR:}/opt/local/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

# FIRST, before this script defines anything of its own -- see the same call in
# build-image_codecs.sh for why (BUILD_DIR is also an Xcode build setting).
# shellcheck source=../caps-lib.sh
. "$ROOT_DIR/platform/caps-lib.sh"
mt_caps_strip_host_build_env
CACHE_DIR="$(mt_caps_work_dir jpeg)"
DOWNLOAD_DIR="$CACHE_DIR/downloads"
SRC_DIR="$CACHE_DIR/src"
BUILD_DIR="$CACHE_DIR/build"
PREFIX_DIR="$CACHE_DIR/install"
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). libjpeg-turbo reads no capability, so its store key is
# empty and it has ONE bucket per machine, shared by every app. With no store
# in the environment (a standalone run) the store IS the view.
mt_caps_use_store "${MT_STORE_JPEG:-}" jpeg
OUT_LIB="$OUT_LIB_DIR/libmt_jpeg.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_jpeg.stamp"

ARCH_LIST=(arm64 x86_64)
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
JOBS="${MT_BUILD_JOBS:-8}"

# The newest stable release on 2026-10-09 (3.2.0, 2026-06-30). The official
# source tarball -- NOT GitHub's auto-generated "Source code" archive, which
# upstream does not support. SHA-256 cross-checked against the digest GitHub
# publishes for this release asset.
LIBJPEG_TURBO_VERSION="3.2.0"
LIBJPEG_TURBO_URL="https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/${LIBJPEG_TURBO_VERSION}/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}.tar.gz"
LIBJPEG_TURBO_SHA256="6f30092cef9fb839779646608f4ee14ae3cbac989c47fa05e841b0841f09878e"

# The five headers every consumer reads (the staged jpeg/include and each prefix).
JPEG_HEADERS=(jpeglib.h jconfig.h jmorecfg.h jerror.h transupp.h)

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning libjpeg-turbo build artifacts..."
  rm -rf "$BUILD_DIR" "$CACHE_DIR"/install-* "$OUT_LIB" "$STAMP_FILE" "$OUT_LIB_DIR/jpeg/include"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

script_sha="unknown"
if command -v shasum >/dev/null 2>&1; then
  script_sha="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

# STAMP RULE 1 -- the SIMD decision and the prod refusal come FIRST, before any
# stamp comparison. Otherwise a development scalar build leaves a stamp that
# says x86_64:simd=0, a final build computes the same expected value, hits the
# stamp and ships a scalar Intel slice: the prod check bypassed by a cache hit.
turbo_simd_for_arch() {
  local arch="$1"
  if [[ "$arch" == "x86_64" ]]; then
    if [[ "${MT_JPEG_X86_SIMD:-1}" == "0" ]] || ! command -v nasm >/dev/null 2>&1; then
      if [[ "${MT_PROD_BUILD:-0}" == "1" ]]; then
        echo "error: libjpeg-turbo x86_64 needs nasm for SIMD in a final build (install it: sudo port install nasm)" >&2; exit 1
      fi
      echo "warning: libjpeg-turbo x86_64 slice built WITHOUT SIMD (nasm not found or MT_JPEG_X86_SIMD=0; install it: sudo port install nasm)" >&2
      echo 0; return
    fi
  fi
  echo 1
}

SIMD_REQUESTED=()
for arch in "${ARCH_LIST[@]}"; do
  # (the subshell's exit does not stop the script by itself)
  simd="$(turbo_simd_for_arch "$arch")" || exit 1
  SIMD_REQUESTED+=("$simd")
done

stamp_value_for() {
  # $@ = one simd value per arch, in ARCH_LIST order
  local parts="" arch
  local values=("$@")
  local i=0
  for arch in "${ARCH_LIST[@]}"; do
    parts+="${parts:+,}${arch}:simd=${values[$i]}"
    i=$((i+1))
  done
  printf '%s:libjpeg-turbo-%s:%s:%s' "$script_sha" "$LIBJPEG_TURBO_VERSION" "$DEPLOYMENT_TARGET" "$parts"
}
expected_stamp="$(stamp_value_for "${SIMD_REQUESTED[@]}")"

# STAMP RULE 2 -- a hit requires every artefact the CONSUMERS read, not only
# the store's: build-image_codecs.sh reads the work prefixes, and the store and
# the work directory live and die separately (a cleaned work root beside a
# surviving _depstore). A store hit with a missing prefix is a MISS: exiting 0
# there would leave a prefix the codec script refuses ("run build-jpeg first"),
# and rerunning build-jpeg would hit the same stamp forever.
#
# A stamp records the SIMD state the build ACTUALLY got (requested SIMD can
# still fail inside turbo's configure), so a final build also refuses any
# stamp holding simd=0.
stamp_hit() {
  [[ -f "$STAMP_FILE" && -f "$OUT_LIB" && -f "$OUT_LIB_DIR/jpeg/include/jpeglib.h" ]] || return 1
  [[ "$(cat "$STAMP_FILE")" == "$expected_stamp" ]] || return 1
  if [[ "${MT_PROD_BUILD:-0}" == "1" && "$(cat "$STAMP_FILE")" == *"simd=0"* ]]; then
    return 1
  fi
  local arch h
  for arch in "${ARCH_LIST[@]}"; do
    [[ -f "$PREFIX_DIR-$arch/lib/libjpeg.a" && -f "$PREFIX_DIR-$arch/lib/libjpeg_transupp.a" ]] || return 1
    for h in "${JPEG_HEADERS[@]}"; do
      [[ -f "$PREFIX_DIR-$arch/include/$h" ]] || return 1
    done
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
  # download must never sit at the final name.
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
# holding the archive's (verified, so pinned) SHA-256 -- see the codec script.
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

# Prints arch:simd=<0|1> -- the SIMD state the build ACTUALLY got, which is
# what the stamp records. Read from the INSTALLED jconfig.h (`#define
# WITH_SIMD 1`), not from configure's output: when requested SIMD fails
# (no usable NASM, an unsupported CPU), turbo's simd_fail prints only
# "WARNING: SIMD extensions disabled: ..." and turns WITH_SIMD off; the
# "SIMD extensions: None" line appears only when WITH_SIMD=0 was asked for.
build_turbo_arch() {
  local arch="$1"
  local src="$SRC_DIR/libjpeg-turbo-${LIBJPEG_TURBO_VERSION}"
  local build="$BUILD_DIR/libjpeg-turbo-$arch"
  local prefix="$PREFIX_DIR-$arch"   # = $(mt_caps_work_dir jpeg)/install-$arch, the path the codec script reads
  local simd="$2"                    # from turbo_simd_for_arch, decided BEFORE the stamp check
  local log="$BUILD_DIR/libjpeg-turbo-$arch.log"
  # WITH_TOOLS/WITH_TESTS off: cjpeg/djpeg/jpegtran and the test programs are
  # not shipped, and the tools would look for zlib/libspng. CMAKE_IGNORE_PREFIX_PATH
  # keeps any find_* away from package-manager prefixes regardless.
  build_cmake "$src" "$build" "$prefix" "$arch" \
    -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=OFF \
    -DWITH_TOOLS=OFF -DWITH_TESTS=OFF \
    -DWITH_SIMD="$simd" -DREQUIRE_SIMD=OFF \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_INSTALL_INCLUDEDIR=include \
    -DCMAKE_C_FLAGS="-fexceptions" \
    "-DCMAKE_IGNORE_PREFIX_PATH=/opt/homebrew;/usr/local;/opt/local" >"$log" 2>&1 \
    || { cat "$log" >&2; echo "ERROR: libjpeg-turbo build failed for $arch (log: $log)" >&2; exit 1; }
  local actual=0
  if grep -q '^#define WITH_SIMD 1' "$prefix/include/jconfig.h" 2>/dev/null; then
    actual=1
  fi
  if [[ "$simd" == "1" && "$actual" == "0" ]]; then
    local why; why="$(grep -m1 "SIMD extensions disabled" "$log" || true)"
    if [[ "${MT_PROD_BUILD:-0}" == "1" ]]; then
      echo "error: libjpeg-turbo $arch built WITHOUT SIMD in a final build (${why:-jconfig.h has no WITH_SIMD}; see $log)" >&2; exit 1
    fi
    echo "warning: libjpeg-turbo $arch built WITHOUT SIMD (${why:-jconfig.h has no WITH_SIMD}; see $log)" >&2
  fi
  # transupp.c is jpegtran's, not the library's: compiled against the build
  # tree's generated headers (jconfigint.h) for lossless JPEG rotation.
  # turbo's jpegtran target adds no define for it (its only COMPILE_FLAGS is
  # USE_SETMODE, a Windows setmode() switch for jpegtran.c).
  clang -c -O2 -fexceptions -arch "$arch" -mmacosx-version-min="$DEPLOYMENT_TARGET" \
    -I"$build" -I"$src/src" -o "$build/transupp.o" "$src/src/transupp.c"
  libtool -static -o "$prefix/lib/libjpeg_transupp.a" "$build/transupp.o"
  cp "$src/src/transupp.h" "$prefix/include/"
  echo "$arch:simd=$actual"
}

download_archive "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}" "$LIBJPEG_TURBO_URL" "$LIBJPEG_TURBO_SHA256"
extract_archive "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}" "libjpeg-turbo-${LIBJPEG_TURBO_VERSION}"

rm -rf "$BUILD_DIR" "$CACHE_DIR"/install-*
mkdir -p "$BUILD_DIR"

SIMD_ACTUAL=()
i=0
for arch in "${ARCH_LIST[@]}"; do
  rm -rf "$PREFIX_DIR-$arch"
  mkdir -p "$PREFIX_DIR-$arch"
  line="$(build_turbo_arch "$arch" "${SIMD_REQUESTED[$i]}")" || exit 1
  SIMD_ACTUAL+=("${line##*=}")
  require_file "$PREFIX_DIR-$arch/lib/libjpeg.a"
  require_file "$PREFIX_DIR-$arch/lib/libjpeg_transupp.a"
  for h in "${JPEG_HEADERS[@]}"; do
    require_file "$PREFIX_DIR-$arch/include/$h"
  done
  # (jpeg_nbits.c.o is empty on x86_64 by design: libtool's "has no symbols"
  #  note about it is filtered, every other diagnostic still reaches stderr)
  libtool -static -o "$BUILD_DIR/libmt_jpeg-$arch.a" \
    "$PREFIX_DIR-$arch/lib/libjpeg.a" "$PREFIX_DIR-$arch/lib/libjpeg_transupp.a" \
    2> >(grep -v "has no symbols" >&2)
  i=$((i+1))
done

# ONE header set serves both slices. The two prefixes' jconfig.h can differ in
# exactly one line, `#define WITH_SIMD 1` (`/* #undef WITH_SIMD */` on the
# other side), when the x86_64 slice is scalar; it affects no public struct or
# API (diffed on 3.2.0, 2026-10-09: with SIMD on both sides the two files are
# byte-identical, with a scalar x86_64 slice that line is the whole diff).
# Asserted here, so a turbo bump that makes
# jconfig.h arch-dependent in any other way stops the build instead of shipping
# one slice the other's configuration.
if ! diff <(grep -v 'WITH_SIMD' "$PREFIX_DIR-arm64/include/jconfig.h") \
          <(grep -v 'WITH_SIMD' "$PREFIX_DIR-x86_64/include/jconfig.h") >/dev/null; then
  echo "ERROR: jconfig.h differs between arm64 and x86_64 beyond the WITH_SIMD line:" >&2
  diff "$PREFIX_DIR-arm64/include/jconfig.h" "$PREFIX_DIR-x86_64/include/jconfig.h" >&2 || true
  exit 1
fi

rm -f "$OUT_LIB"
lipo -create "$BUILD_DIR/libmt_jpeg-arm64.a" "$BUILD_DIR/libmt_jpeg-x86_64.a" -output "$OUT_LIB"
require_universal "$OUT_LIB"

rm -rf "$OUT_LIB_DIR/jpeg/include"
mkdir -p "$OUT_LIB_DIR/jpeg/include"
for h in "${JPEG_HEADERS[@]}"; do
  cp "$PREFIX_DIR-arm64/include/$h" "$OUT_LIB_DIR/jpeg/include/"
done

# The stamp LAST, and recording what the build actually got.
printf '%s' "$(stamp_value_for "${SIMD_ACTUAL[@]}")" > "$STAMP_FILE"
echo "libjpeg-turbo ${LIBJPEG_TURBO_VERSION} built: $OUT_LIB (headers staged to $OUT_LIB_DIR/jpeg/include)"
