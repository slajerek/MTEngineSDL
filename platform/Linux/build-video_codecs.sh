#!/usr/bin/env bash
set -euo pipefail

# Build Linux video codec dependencies as native (single-arch) libraries, then
# package them into one archive for apps that compile/link engine video code.
#
# This is the Linux port of platform/MacOS/build-video_codecs.sh -- read that
# script first. The licensing-load-bearing FFmpeg configure block below is
# copied verbatim except for the documented platform tail: no
# --enable-videotoolbox (that's an Apple VideoToolbox hwaccel), no darwin
# cross-compile/universal-binary flags. This is a NATIVE single-arch build
# (uname -m), so there is no lipo/universal-merge step, no install_name_tool
# @rpath normalization, and no per-arch header merge -- FFmpeg's own
# `make install` already produces one lib/ + include/ tree, unlike macOS
# where two arch-specific installs have to be glued together afterwards.
#
# VAAPI hardware-accel decode is intentionally NOT enabled (out of scope for
# this build plan; --disable-everything already excludes all hwaccels by
# default, so simply not passing --enable-vaapi is sufficient -- no explicit
# --disable-vaapi flag is needed).
#
# rpath: this script does NOT set rpath/RUNPATH in the FFmpeg .so files. The
# app's own link step provides $ORIGIN so the bundled dylibs are found next
# to the executable; FFmpeg's default SONAME behavior (libavcodec.so ->
# libavcodec.so.61 -> libavcodec.so.61.19.101) is left untouched.

export PATH="/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin:$PATH"

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
# OUTSIDE the checkout, and keyed by the resolved capability set -- see
# mt_caps_lib_dir in ../caps-lib.sh and resolve.deps_dir for why this is a
# correctness change and not tidiness. One flat platform/Linux/libs served all
# four apps, and a capability being off writes a STUB over the real archive.
# Sourced BEFORE any use of mt_caps_work_dir/mt_caps_lib_dir below -- this
# script runs as its own `bash` child process (app-build-linux.sh -> the
# engine's build-linux.sh -> `bash build-video_codecs.sh`), so nothing a
# caller sourced is inherited here.
if ! declare -f mt_caps_lib_dir >/dev/null 2>&1; then
  # shellcheck source=../caps-lib.sh
  . "$ROOT_DIR/platform/caps-lib.sh"
fi
CACHE_DIR="$(mt_caps_work_dir video-codecs)"
DOWNLOAD_DIR="$CACHE_DIR/downloads"
SRC_DIR="$CACHE_DIR/src"

ARCH="$(uname -m)"
BUILD_DIR="$CACHE_DIR/build-linux-$ARCH"
OUT_LIB_DIR="$(mt_caps_lib_dir)"

# Store and view (L16). The video codecs follow MT_CAP_VIDEO_PLAYBACK and the two licence keys.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_VIDEO_CODECS:-}" video_codecs

# Single native-arch install prefix. FFmpeg's shared libs/headers AND vpx's/
# opus's static libs all install here side by side (no per-arch merge is needed
# on Linux, so there is only ever one "$PREFIX_DIR", unlike macOS where
# install-arm64/install-x86_64 first exist separately and only get glued into one
# $PREFIX_DIR by the lipo/merge steps).
#
# IT LIVES WITH THE ARCHIVE, keyed identically, and that is a CORRECTNESS
# requirement rather than tidiness. It used to be
# other/lib/video-codecs/install-linux-$(uname -m) -- one shared directory inside
# the checkout, which every app's CMake read the FFmpeg .so files out of
# directly. The skip check below tests `-f "$PREFIX_DIR/lib/libavcodec.so"`, so a
# keyed stamp over a SHARED prefix does this: app A (full) fills the prefix; app
# B (commercial) has no stamp in its own keyed directory, rebuilds, and
# overwrites the prefix with the store-safe FFmpeg; app A builds again, finds its
# own stamp valid and libavcodec.so present, SKIPS -- and links the commercial
# decoder set. No error, wrong binary. Today that cannot happen only because the
# stamp is shared too, so a mode flip invalidates it. Keying one without the
# other is strictly worse than keying neither.
PREFIX_DIR="$OUT_LIB_DIR/ffmpeg"

# The guard, because the paragraph above is a comment and comments do not fail
# builds. Both halves of this script's output belong to ONE capability set.
case "$PREFIX_DIR" in
  "$OUT_LIB_DIR"/*) ;;
  *) echo "ERROR: FFmpeg prefix $PREFIX_DIR is not inside the keyed dependency directory $OUT_LIB_DIR" >&2
     exit 2 ;;
esac
OUT_LIB="$OUT_LIB_DIR/libmt_video_codecs.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_video_codecs.stamp"

JOBS="${MT_BUILD_JOBS:-$(nproc)}"

# Build mode: COMMERCIAL=1 produces the store-safe FFmpeg (no patent-encumbered
# software decoders: no WMV/VC-1, no WMA, no HEVC/AAC/EAC3). COMMERCIAL=0
# ("full") additionally enables WMV/WMA/HEVC/AAC/EAC3 software
# decode for non-store builds.
# COMMERCIAL is a PARAMETER now, not a file read. The engine used to track a
# platform/BUILD_MODE_DEFAULT and the photo app tracked a root-level file of the
# same name -- two files, two owners, one name, and neither reconciled with
# MT_COMMERCIAL_BUILD. All three are retired: the licence mode lives in the app's
# mtengine.caps and reaches acquisition, compilation and LICENSES.txt through one
# channel. The caller passes COMMERCIAL=0|1; a direct run defaults to 0
# (non-commercial), which is the safer default because it only ever includes
# more.
COMMERCIAL="${COMMERCIAL:-0}"
case "$COMMERCIAL" in
  0|1) ;;
  *) echo "ERROR: COMMERCIAL must be 0 or 1 (got '$COMMERCIAL')" >&2; exit 1 ;;
esac
# The RESOLVED mode wins when present (unification plan Phase 2, 2026-08-31):
# mtcaps emits MT_FFMPEG_BUILD_MODE into every fragment, derived as `full` only
# for MT_PRIVATE_BUILD=1 -- the withheld decoders are patent-encumbered and
# patents attach to DISTRIBUTION, so the public/free tier gets the restricted
# set exactly like the store tier. The COMMERCIAL env var stays as the legacy
# channel for direct/standalone runs (engine dev = private use, so its default
# maps to `full`).
if [[ -n "${MT_FFMPEG_BUILD_MODE:-}" ]]; then
  case "$MT_FFMPEG_BUILD_MODE" in
    full|commercial) FFMPEG_BUILD_MODE="$MT_FFMPEG_BUILD_MODE" ;;
    *) echo "ERROR: MT_FFMPEG_BUILD_MODE must be full or commercial (got '$MT_FFMPEG_BUILD_MODE')" >&2; exit 1 ;;
  esac
else
  if [[ "$COMMERCIAL" == "1" ]]; then
    FFMPEG_BUILD_MODE="commercial"
  else
    FFMPEG_BUILD_MODE="full"
  fi
fi

VPX_VERSION="1.15.2"
OPUS_VERSION="1.5.2"
FFMPEG_VERSION="7.1.2"

# ---- Pinned URLs/hashes: copied verbatim from platform/MacOS/build-video_codecs.sh.
#      Same upstream tarballs are used on every platform; never re-derive
#      these from memory. ----

VPX_URL="https://github.com/webmproject/libvpx/archive/refs/tags/v${VPX_VERSION}.tar.gz"
VPX_SHA256="26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa"
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
# ffmpeg.org does not publish a .sha256 sidecar for release tarballs (only a
# detached .asc PGP signature). This hash was computed locally from a tarball
# downloaded over TLS from https://ffmpeg.org/releases/ and independently
# verified against the "FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>"
# (fingerprint FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8) via the
# accompanying ffmpeg-7.1.2.tar.xz.asc before being pinned here (same value as
# the macOS script -- it is the same upstream tarball on both platforms).
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_SHA256="089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304"

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning video codec build artifacts..."
  rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$OUT_LIB" "$STAMP_FILE"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

# ---- toolchain detection: never assume clang. Prefer the generic `cc`
#      alias; fall back to `gcc`. Passed explicitly to vpx/opus/ffmpeg below
#      instead of relying on each build system's own default guess, so the
#      script's compiler choice is deterministic across distros. ----
if command -v cc >/dev/null 2>&1; then
  HOST_CC="cc"
elif command -v gcc >/dev/null 2>&1; then
  HOST_CC="gcc"
else
  echo "ERROR: no C compiler found on PATH (checked cc, gcc)." >&2
  echo "Install a C toolchain (Debian/Ubuntu: sudo apt install build-essential) and rerun." >&2
  exit 1
fi

script_sha="unknown"
if command -v sha256sum >/dev/null 2>&1; then
  script_sha="$(sha256sum "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
elif command -v shasum >/dev/null 2>&1; then
  script_sha="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

# THE CAPABILITY GATE. build-video_codecs had none on any platform, so
# MT_CAP_VIDEO_PLAYBACK=0 still downloaded and built FFmpeg, libvpx and opus.
#
# Absent means ON, so a bare engine build still builds everything, and this comes
# before any download.
# WEBM_VPX is the archive's own flag (the vpx/opus/nestegg lane); FFMPEG kept
# as fallback for a fragment older than 2026-08-31.
if [[ "${MT_ENABLE_WEBM_VPX:-${MT_ENABLE_FFMPEG:-1}}" == "0" ]]; then
  STUB_STAMP="disabled:${script_sha}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "mt_video_codecs"
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

stamp_value="${script_sha}:libvpx-${VPX_VERSION}:opus-${OPUS_VERSION}:ffmpeg-${FFMPEG_VERSION}:linux-${ARCH}:mode-${FFMPEG_BUILD_MODE}"
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && -f "$PREFIX_DIR/lib/libavcodec.so" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$stamp_value" ]]; then
    echo "Video codec bundle is up to date: $OUT_LIB"
    exit 0
  fi
fi

download_archive() {
  local name="$1"
  local url="$2"
  local sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.gz"

  if [[ ! -f "$archive" ]]; then
    if ! command -v curl >/dev/null 2>&1; then
      echo "ERROR: curl is required to download video codec sources." >&2
      echo "Install curl and rerun this script (Debian/Ubuntu: sudo apt install curl)." >&2
      exit 1
    fi
    echo "Downloading $name..."
    curl -L --fail --show-error "$url" -o "$archive"
  fi

  local actual_sha
  actual_sha="$(sha256sum "$archive" | cut -d ' ' -f 1)"
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

download_archive_xz() {
  local name="$1"
  local url="$2"
  local sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.xz"

  if [[ ! -f "$archive" ]]; then
    if ! command -v curl >/dev/null 2>&1; then
      echo "ERROR: curl is required to download video codec sources." >&2
      echo "Install curl and rerun this script (Debian/Ubuntu: sudo apt install curl)." >&2
      exit 1
    fi
    echo "Downloading $name..."
    curl -L --fail --show-error "$url" -o "$archive"
  fi

  local actual_sha
  actual_sha="$(sha256sum "$archive" | cut -d ' ' -f 1)"
  if [[ "$actual_sha" != "$sha256" ]]; then
    echo "ERROR: checksum mismatch for $archive" >&2
    echo "Expected: $sha256" >&2
    echo "Actual:   $actual_sha" >&2
    exit 1
  fi
}

extract_archive_xz() {
  local name="$1"
  local top_dir="$2"
  local archive="$DOWNLOAD_DIR/$name.tar.xz"
  local dest="$SRC_DIR/$top_dir"

  if [[ -d "$dest" ]]; then
    return
  fi

  echo "Extracting $name..."
  tar -xJf "$archive" -C "$SRC_DIR"
}

require_file() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    echo "ERROR: expected file not found: $file" >&2
    exit 1
  fi
}

# Maps uname -m to libvpx's own target-triple naming (accepts both "arm64"
# and "aarch64" spellings of the same architecture on the input side, since
# `uname -m` reports "aarch64" on essentially all mainstream Linux distros).
vpx_target_for_arch() {
  local arch="$1"
  case "$arch" in
    x86_64) echo "x86_64-linux-gcc" ;;
    aarch64|arm64) echo "arm64-linux-gcc" ;;
    *)
      echo "ERROR: unsupported architecture for libvpx: $arch" >&2
      exit 1
      ;;
  esac
}

build_libvpx() {
  local prefix="$1"
  local target
  target="$(vpx_target_for_arch "$ARCH")"
  local build="$BUILD_DIR/libvpx"

  mkdir -p "$build"
  pushd "$build" >/dev/null
  CC="$HOST_CC" \
    AR="ar" \
    LD="$HOST_CC" \
    "$SRC_DIR/libvpx-${VPX_VERSION}/configure" \
      --target="$target" \
      --prefix="$prefix" \
      --disable-examples \
      --disable-tools \
      --disable-docs \
      --disable-unit-tests \
      --disable-install-bins \
      --enable-pic \
      --enable-vp9 \
      --enable-vp9-highbitdepth \
      --disable-vp8-encoder \
      --disable-vp8-decoder \
      --disable-vp9-encoder
  make -j "$JOBS"
  make install
  popd >/dev/null
}

build_opus() {
  local prefix="$1"
  local build="$BUILD_DIR/opus"

  mkdir -p "$build"
  pushd "$build" >/dev/null
  # NOTE: same configure flags as the macOS script (--disable-shared
  # --enable-static --disable-doc --disable-extra-programs); only the
  # arch/deployment-target CFLAGS/CXXFLAGS/LDFLAGS are dropped (meaningless
  # on a native single-arch Linux build) and CC is pinned to the detected
  # cc/gcc instead of clang. --with-pic is added (Linux-only, mirroring
  # vpx's --enable-pic): a static-only libtool build does not default to
  # -fPIC, and these objects are linked into the PIE the photo app executable.
  "$SRC_DIR/opus-${OPUS_VERSION}/configure" \
    --prefix="$prefix" \
    --disable-shared \
    --enable-static \
    --with-pic \
    --disable-doc \
    --disable-extra-programs \
    CC="$HOST_CC" \
    CFLAGS="-O2" \
    CXXFLAGS="-O2"
  make -j "$JOBS"
  make install
  popd >/dev/null
}

build_ffmpeg() {
  local prefix="$1"
  local build="$BUILD_DIR/ffmpeg"

  mkdir -p "$build"
  pushd "$build" >/dev/null

  # WMV/ASF (2026-07-18 spec): the asf demuxer is enabled in BOTH modes --
  # demuxing ASF is licensing-safe (Microsoft Open Specification Promise).
  # The WMV/WMA software DECODERS (patent-encumbered: VC-1 pool, WMA) are
  # full-mode only. Identical to the macOS script.
  # msmpeg4v1/v2/v3: early-era .wmv/.avi coverage, patents expired -- same
  # rationale/deps note as the macOS script.
  # hevc/aac/eac3 (2026-07-19 codec-superset spec): software fallbacks for
  # internal builds -- on Linux there is no native decoder at all, so full
  # builds gain HEVC video and AAC/E-AC-3 audio outright. eac3's ac3-core
  # dependency is already satisfied (ac3 in the base list, both modes).
  # THE MODE, not the tier. $FFMPEG_BUILD_MODE comes from resolved
  # MT_FFMPEG_BUILD_MODE, which is `full` only for MT_PRIVATE_BUILD=1.
  # $COMMERCIAL is the LICENCE TIER: the public/free tier is COMMERCIAL=0
  # with mode `commercial`, because patents attach to distribution, not to
  # payment. Reading the tier here handed that tier the withheld decoders.
  # No mode branch: MT_FFMPEG_DECODERS and MT_FFMPEG_PARSERS already carry the
  # withheld names when the resolved mode is `full`. (Identical note to the
  # macOS script.)
  local mode_flags=()

  # NOTE: FFmpeg's mpeg4 decoder select-depends on the h263 decoder core
  # (configure: mpeg4_decoder_select="h263_decoder"); h263 is therefore
  # force-enabled. Passing --disable-decoder=h263 disables mpeg4 along with
  # it, so it cannot be removed while mpeg4 stays on the enabled list.
  # Approved deviation (H.263 patents expired; documented in spec codec
  # table). The exact resulting decoder set is asserted after configure via
  # require_ffmpeg_exact_set. (Identical note to the macOS script.)
  "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}/configure" --prefix="$prefix" \
    --arch="$ARCH" --cc="$HOST_CC" \
    --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-network --disable-everything \
    --enable-protocol=file \
    --enable-demuxer="$(policy_csv "$MT_FFMPEG_DEMUXERS")" \
    --enable-decoder="$(policy_csv "$MT_FFMPEG_DECODERS")" \
    --enable-parser="$(policy_csv "$MT_FFMPEG_PARSERS")" \
    --disable-encoders --disable-muxers --disable-filters --disable-bsfs \
    --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,aac_adtstoasc \
    --disable-devices \
    --disable-autodetect \
    "${mode_flags[@]}" \
    2>&1 | tee "$build/configure.log"
  # (No --enable-videotoolbox: that's an Apple-only hwaccel, dropped entirely
  # rather than replaced -- there is no VAAPI equivalent enabled in this
  # plan. No darwin cross-compile/universal-binary flags: this is always a
  # native single-arch build, so --arch/--cc above are the only toolchain
  # flags needed, and no --extra-cflags/--extra-ldflags deployment-target
  # pinning applies on Linux.)

  make -j "$JOBS" 2>&1 | tee "$build/make.log"
  make install 2>&1 | tee "$build/install.log"
  popd >/dev/null
}

# The exact component sets expected in the built libraries, per build mode.
# Anything outside these lists appearing enabled fails the build -- this is
# the licensing guard for the LGPL decode-only bundle, and it is the
# AUTHORITATIVE check (the commercial symbol trace scan below is a
# best-effort secondary). Identical lists and identical parsing mechanism to
# the macOS script (grep/sed/tr/sort config_components.h -- all standard
# POSIX text tools).
# Policy comes from the vocabulary through MT_FFMPEG_*; what FFmpeg
# force-enables on top of it is a fact about ffmpeg-$FFMPEG_VERSION and lives
# here beside that pin. (Identical note to the macOS script.)
FFMPEG_IMPLICIT_DECODERS="h263"
FFMPEG_IMPLICIT_PARSERS="h263"

FFMPEG_EXPECTED_DECODERS="$(policy_sorted "$MT_FFMPEG_DECODERS $FFMPEG_IMPLICIT_DECODERS")"
FFMPEG_EXPECTED_DEMUXERS="$(policy_sorted "$MT_FFMPEG_DEMUXERS")"
FFMPEG_EXPECTED_PARSERS="$(policy_sorted "$MT_FFMPEG_PARSERS $FFMPEG_IMPLICIT_PARSERS")"
# HEVC/AAC/EAC3 moved from a former always-forbidden list on 2026-07-19
# (codec-superset spec): full builds carry them as software fallbacks;
# commercial exclusion unchanged.
# Derived from the same withheld list the licence scanner reads, so the two
# cannot drift; 13 names now, where the hand-written copy had 10.
FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL="$(printf '%s' "$MT_FFMPEG_DECODERS_WITHHELD" | tr '[:lower:] ' '[:upper:]\n' | tr '\n' ' ')"

# require_ffmpeg_exact_set <build-dir> <KIND> <expected-space-separated>
# KIND is the config_components.h suffix: DECODER, DEMUXER, or PARSER.
require_ffmpeg_exact_set() {
  local build="$1"
  local kind="$2"
  local expected_list="$3"
  local config_components_h="$build/config_components.h"
  require_file "$config_components_h"
  local actual expected
  actual="$(grep -E "^#define CONFIG_[A-Z0-9_]+_${kind} 1$" "$config_components_h" \
    | sed -E "s/^#define CONFIG_(.+)_${kind} 1$/\1/" \
    | tr '[:upper:]' '[:lower:]' | sort)"
  expected="$(printf '%s\n' "$expected_list" | tr ' ' '\n' | sort)"
  if [[ "$actual" != "$expected" ]]; then
    echo "ERROR: ffmpeg enabled-${kind} set mismatch in $config_components_h (mode: $FFMPEG_BUILD_MODE)" >&2
    echo "Diff (expected vs actual):" >&2
    diff <(printf '%s\n' "$expected") <(printf '%s\n' "$actual") >&2 || true
    exit 1
  fi
}

require_ffmpeg_decoder_disabled() {
  local build="$1"
  local decoder="$2"
  # Per-component CONFIG_*_DECODER defines live in config_components.h, not
  # config.h (which only has the aggregate CONFIG_DECODERS 1/0 and the
  # verbatim configure invocation string).
  local config_components_h="$build/config_components.h"
  require_file "$config_components_h"
  if ! grep -Eq "define[[:space:]]+CONFIG_${decoder}_DECODER[[:space:]]+0" "$config_components_h"; then
    echo "ERROR: forbidden decoder '$decoder' is not confirmed disabled in $config_components_h" >&2
    exit 1
  fi
}

# Locate the SONAME-level symlink for an ffmpeg .so, e.g. libavcodec.so.61
# (exactly one numeric version component), as distinct from the unversioned
# dev symlink (libavcodec.so) and the fully-versioned real file
# (libavcodec.so.61.19.101). Linux equivalent of the macOS script's
# ffmpeg_soname_link (which does the same thing for the .N.dylib pattern).
ffmpeg_soname_link() {
  local dir="$1"
  local lib="$2"
  (cd "$dir" && ls | grep -E "^${lib}\.so\.[0-9]+\$")
}

# Best-effort dependency sanity check: every ELF NEEDED entry on each FFmpeg
# .so must be either one of our own libav*/libsw* SONAMEs or a base-system
# library. This is the Linux equivalent of the macOS script's
# require_ffmpeg_clean_deps (which checks otool -L output for stray
# absolute/Homebrew paths); here it uses readelf -d instead of otool, since
# there is no rpath/install-name rewriting step to verify on Linux (per the
# brief, rpath is intentionally left alone -- the app's own $ORIGIN handles
# it). The system-library allowlist below is a reasonable default but is
# UNVERIFIED against a real Linux ldd/readelf run -- see task-7-report.md
# open questions; extend it if the first real build trips over a legitimate
# libc/toolchain dependency not listed here.
require_ffmpeg_clean_deps() {
  local file="$1"
  local dep
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    case "$dep" in
      libavutil.so.*|libavcodec.so.*|libavformat.so.*|libswscale.so.*|libswresample.so.*) continue ;;
      libc.so.*|libm.so.*|libpthread.so.*|libdl.so.*|librt.so.*|libgcc_s.so.*|ld-linux*.so.*|linux-vdso.so.*) continue ;;
      *)
        echo "ERROR: unexpected dependency in $file: $dep" >&2
        exit 1
        ;;
    esac
  done < <(readelf -d "$file" | awk -F'[][]' '/\(NEEDED\)/ {print $2}')
}

require_symbol() {
  local symbol="$1"
  if ! nm -g "$OUT_LIB" 2>/dev/null | grep -F "$symbol" >/dev/null; then
    echo "ERROR: expected symbol not found in $OUT_LIB: $symbol" >&2
    exit 1
  fi
}

download_archive "libvpx-${VPX_VERSION}" "$VPX_URL" "$VPX_SHA256"
download_archive "opus-${OPUS_VERSION}" "$OPUS_URL" "$OPUS_SHA256"
download_archive_xz "ffmpeg-${FFMPEG_VERSION}" "$FFMPEG_URL" "$FFMPEG_SHA256"

extract_archive "libvpx-${VPX_VERSION}" "libvpx-${VPX_VERSION}"
extract_archive "opus-${OPUS_VERSION}" "opus-${OPUS_VERSION}"
extract_archive_xz "ffmpeg-${FFMPEG_VERSION}" "ffmpeg-${FFMPEG_VERSION}"

rm -rf "$BUILD_DIR" "$PREFIX_DIR"
mkdir -p "$BUILD_DIR" "$PREFIX_DIR"

build_libvpx "$PREFIX_DIR"
build_opus "$PREFIX_DIR"
build_ffmpeg "$PREFIX_DIR"

# The guard picks its expected set on the same value the configure line used,
# or it asserts one mode's library against the other's set.
# ONE expected set per kind, whatever the mode. (Identical note to the macOS
# script.) Only the absence check is mode-specific.
require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg" "DECODER" "$FFMPEG_EXPECTED_DECODERS"
require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg" "PARSER" "$FFMPEG_EXPECTED_PARSERS"
if [[ "$FFMPEG_BUILD_MODE" == "commercial" ]]; then
  for dec in $FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL; do
    require_ffmpeg_decoder_disabled "$BUILD_DIR/ffmpeg" "$dec"
  done
fi
require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg" "DEMUXER" "$FFMPEG_EXPECTED_DEMUXERS"

for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
  ff_soname="$(ffmpeg_soname_link "$PREFIX_DIR/lib" "$ff_lib")"
  require_ffmpeg_clean_deps "$PREFIX_DIR/lib/$ff_soname"
done

# Commercial-only binary trace scan (secondary guard; require_ffmpeg_exact_set
# on config_components.h is the authoritative one). Scans the .so symbol
# tables for the forbidden decoders' FFCodec entry symbols
# (ff_<name>_decoder -- no leading underscore on ELF). Uses the full symtab
# (plain nm), NOT nm -D: FFmpeg's ELF version script hides ff_* from the
# dynamic table, so a dynamic-only scan would vacuously pass. Deliberately
# does NOT `strings`-scan for bare codec names like "wmv3": libavcodec's
# codec_desc.c embeds the name of EVERY known codec ID regardless of which
# decoders are compiled in. Symbol tables can be stripped -- best-effort by
# design.
require_ffmpeg_no_forbidden_symbols() {
  local libdir="$1"
  local forbidden="hevc aac eac3 wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro"
  local lib name sym hits
  for lib in libavcodec libavformat; do
    local soname
    soname="$(ffmpeg_soname_link "$libdir" "$lib")"
    require_file "$libdir/$soname"
    for name in $forbidden; do
      sym="ff_${name}_decoder"
      hits="$(nm "$libdir/$soname" 2>/dev/null | grep -c "[[:space:]]${sym}\$" || true)"
      if [[ "$hits" != "0" ]]; then
        echo "ERROR: commercial trace scan found forbidden decoder symbol $sym in $libdir/$soname" >&2
        exit 1
      fi
    done
  done
  echo "Commercial trace scan clean: no forbidden decoder symbols in $libdir"
}

if [[ "$FFMPEG_BUILD_MODE" == "commercial" ]]; then
  require_ffmpeg_no_forbidden_symbols "$PREFIX_DIR/lib"
fi

# Mode marker: consumed by app build scripts (guard #3 in the 2026-07-18 WMV
# spec). A commercial app build against a "full" install is a fatal error
# there; this file is how it knows what it is linking.
echo -n "$FFMPEG_BUILD_MODE" > "$PREFIX_DIR/.ffmpeg-build-mode"

INPUT_LIBS=(
  "$PREFIX_DIR/lib/libvpx.a"
  "$PREFIX_DIR/lib/libopus.a"
)

for lib in "${INPUT_LIBS[@]}"; do
  require_file "$lib"
done

# Combine libvpx + opus into one static archive. macOS uses `libtool -static`
# for this; Linux has no libtool-static equivalent for plain archives, so
# this uses a GNU ar MRI script instead (CREATE/ADDLIB/SAVE/END), extracting
# and re-adding each input archive's member object files into one new
# archive. This is the same mechanism (and the same reasoning) already
# established in this repo's build-image_codecs.sh Linux port for combining
# libtiff/libwebp/libavif/libraw/libgav1 into libmt_image_codecs.a.
rm -f "$OUT_LIB"
MRI_SCRIPT="$(mktemp)"
{
  echo "CREATE $OUT_LIB"
  for lib in "${INPUT_LIBS[@]}"; do
    echo "ADDLIB $lib"
  done
  echo "SAVE"
  echo "END"
} > "$MRI_SCRIPT"
ar -M < "$MRI_SCRIPT"
rm -f "$MRI_SCRIPT"
ranlib "$OUT_LIB" || true

require_file "$OUT_LIB"
require_symbol "vpx_codec_dec_init_ver"
require_symbol "vpx_codec_decode"
require_symbol "vpx_codec_destroy"
require_symbol "vpx_codec_err_to_string"
require_symbol "vpx_codec_get_frame"
require_symbol "vpx_codec_vp9_dx"
require_symbol "opus_decoder_create"
require_symbol "opus_decode_float"
require_symbol "opus_decoder_ctl"
require_symbol "opus_decoder_destroy"
require_symbol "opus_strerror"

echo -n "$stamp_value" > "$STAMP_FILE"
echo "Video codec bundle built: $OUT_LIB"
echo "FFmpeg LGPL decode shared libs built: $PREFIX_DIR/lib (avutil/avcodec/avformat/swscale/swresample)"
