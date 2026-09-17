#!/usr/bin/env bash
set -euo pipefail

# Build macOS video codec dependencies as universal static libraries, then
# package them into one archive for apps that compile/link engine video code.

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
# THE FFmpeg LANE IS SEPARATE FROM THE WEBM LANE, since ef700aab.
#
# MT_ENABLE_WEBM_VPX (vpx/opus/nestegg) and MT_ENABLE_FFMPEG used to derive
# from MT_CAP_VIDEO_PLAYBACK together, so one switch served the whole script --
# the note at the capability gate below still said "both derive from
# MT_CAP_VIDEO_PLAYBACK today, so the values agree". An app can now keep video
# and drop FFmpeg, and a store build does exactly that: it must not pay for a
# multi-minute FFmpeg build and a 40 MB download it never links.
FFMPEG_ON="${MT_ENABLE_FFMPEG:-1}"

# A SEPARATE WORK TREE WHEN FFmpeg IS OFF, and this is not tidiness.
#
# mt_caps_work_dir returns ONE global directory per unit, shared across every
# app and every caps set, and the build below does `rm -rf "$BUILD_DIR"
# "$PREFIX_DIR"` unconditionally. Sharing it between the two configurations
# means one FFmpeg-off run deletes install/lib/libavcodec.dylib for every app
# on the machine, and every FFmpeg-ON build after it fails its stamp
# precondition and does a full cold rebuild of libvpx, opus AND FFmpeg --
# in both directions, forever, on every alternation.
#
# The suffix appears ONLY when the flag is explicitly off, so the default work
# tree keeps the path (and the warm state) it has today.
if [[ "$FFMPEG_ON" == "0" ]]; then
  CACHE_DIR="$(mt_caps_work_dir video-codecs-noffmpeg)"
else
  CACHE_DIR="$(mt_caps_work_dir video-codecs)"
fi
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

# Store and view (L16). The video codecs follow MT_CAP_VIDEO_PLAYBACK and the two licence keys, because the FFmpeg decoder set depends on the resolved mode -- a full and a commercial build must never share a store.
# With no store in the environment (a standalone run) the store IS the view
# and the sync is a no-op.
mt_caps_use_store "${MT_STORE_VIDEO_CODECS:-}" video_codecs
OUT_LIB="$OUT_LIB_DIR/libmt_video_codecs.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_video_codecs.stamp"

ARCH_LIST=(arm64 x86_64)

# AN x86 TARGET NEEDS AN x86 ASSEMBLER, and libvpx has no way to go without one.
#
# FFmpeg degrades: --disable-x86asm costs decode speed and builds. libvpx's
# configure simply stops, deep inside this script, with a message that names
# neither the package to install nor the build that wanted it -- and on macOS it
# does so inside an Xcode script phase, where almost nobody finds it. Twice in
# one day of CI (2026-09-03), on two platforms, for the same missing tool.
#
# So it is asked here, once, before anything is downloaded or built.
mt_require_x86_assembler() {
    command -v nasm >/dev/null 2>&1 && return 0
    command -v yasm >/dev/null 2>&1 && return 0
    echo "ERROR: building x86_64 video codecs needs an x86 assembler (nasm), and neither nasm nor yasm is on PATH." >&2
    echo "       libvpx cannot be built without one; FFmpeg would only lose its x86 assembly." >&2
    echo "       Install it:  brew install nasm   (or: port install nasm)" >&2
    return 1
}

# macOS always builds universal, so the x86_64 half is never optional.
mt_require_x86_assembler || exit 1
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
JOBS="${MT_BUILD_JOBS:-8}"

# Build mode: COMMERCIAL=1 produces the store-safe FFmpeg (no patent-encumbered
# software decoders: no WMV/VC-1, no WMA, no HEVC/AAC/EAC3). COMMERCIAL=0
# ("full") additionally enables WMV/WMA software decode plus the
# HEVC/AAC/EAC3 software fallbacks (2026-07-19 codec-superset spec) for
# non-store internal builds.
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

VPX_URL="https://github.com/webmproject/libvpx/archive/refs/tags/v${VPX_VERSION}.tar.gz"
VPX_SHA256="26fcd3db88045dee380e581862a6ef106f49b74b6396ee95c2993a260b4636aa"
OPUS_URL="https://downloads.xiph.org/releases/opus/opus-${OPUS_VERSION}.tar.gz"
OPUS_SHA256="65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1"
# ffmpeg.org does not publish a .sha256 sidecar for release tarballs (only a
# detached .asc PGP signature). This hash was computed locally from a tarball
# downloaded over TLS from https://ffmpeg.org/releases/ and independently
# verified against the "FFmpeg release signing key <ffmpeg-devel@ffmpeg.org>"
# (fingerprint FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8) via the
# accompanying ffmpeg-7.1.2.tar.xz.asc before being pinned here.
FFMPEG_URL="https://ffmpeg.org/releases/ffmpeg-${FFMPEG_VERSION}.tar.xz"
FFMPEG_SHA256="089bc60fb59d6aecc5d994ff530fd0dcb3ee39aa55867849a2bbc4e555f9c304"

mkdir -p "$DOWNLOAD_DIR" "$SRC_DIR" "$OUT_LIB_DIR"

if [[ "${1:-}" == "clean" ]]; then
  echo "Cleaning video codec build artifacts..."
  rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-* "$OUT_LIB" "$STAMP_FILE"
  echo "Done. Source downloads remain cached in $DOWNLOAD_DIR."
  exit 0
fi

script_sha="unknown"
if command -v shasum >/dev/null 2>&1; then
  script_sha="$(shasum -a 256 "${BASH_SOURCE[0]}" | cut -d ' ' -f 1)"
fi

# THE CAPABILITY GATE. build-video_codecs had none on any platform, so
# MT_CAP_VIDEO_PLAYBACK=0 still downloaded and built FFmpeg, libvpx and opus.
#
# Absent means ON, so a bare engine build still builds everything, and this comes
# before any download.
# WEBM_VPX is the archive's own flag (the vpx/opus/nestegg lane); FFMPEG kept
# as fallback for a fragment older than 2026-08-31. Both derive from
# MT_CAP_VIDEO_PLAYBACK today, so the values agree.
if [[ "${MT_ENABLE_WEBM_VPX:-${MT_ENABLE_FFMPEG:-1}}" == "0" ]]; then
  STUB_STAMP="disabled:${script_sha}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "mt_video_codecs"
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

# THE CODEC POLICY IS PART OF THE IDENTITY OF THIS BUILD.
#
# vocabulary.json reaches this script only through these four variables, and
# not one of them was in any cache key. Adding a decoder to decoders_base
# therefore changed nothing any layer could see: the stamp below still matched,
# the build was skipped, and require_ffmpeg_exact_set never ran -- reporting
# success because it did not execute. Measured 2026-09-09: with `tta` added to
# decoders_base and MT_FFMPEG_DECODERS carrying it into the environment,
# config_components.h still said CONFIG_TTA_DECODER 0 and the dylib's mtime was
# an hour old.
#
# The :- defaults are load-bearing. This script runs under `set -euo pipefail`
# and none of the four is defaulted before the early exit below, so a bare
# expansion would abort the script on any run that does not set them.
ffmpeg_policy_sha="$(printf '%s|%s|%s|%s' \
  "${MT_FFMPEG_DECODERS:-}" "${MT_FFMPEG_DEMUXERS:-}" \
  "${MT_FFMPEG_PARSERS:-}"  "${MT_FFMPEG_DECODERS_WITHHELD:-}" \
  | shasum -a 256 | cut -d ' ' -f 1)"

stamp_value="${script_sha}:libvpx-${VPX_VERSION}:opus-${OPUS_VERSION}:ffmpeg-${FFMPEG_VERSION}:${DEPLOYMENT_TARGET}:mode-${FFMPEG_BUILD_MODE}:policy-${ffmpeg_policy_sha}:ffmpeg-${FFMPEG_ON}"
# The staged dylib is part of the check, not just the archive. $OUT_LIB_DIR is
# keyed by capability set, so a fresh key has a valid stamp only if it also has
# the dylibs -- and an app that links FFmpeg needs both halves of this script's
# output, not one.
# With FFmpeg off there is no libavcodec.dylib to find, and demanding one
# would mean the stamp NEVER hits: libvpx and opus would rebuild from source on
# every single invocation, forever. The archive and the stamp are the whole
# check in that configuration.
if [[ "$FFMPEG_ON" == "0" ]]; then
  ffmpeg_outputs_present=1
elif [[ -f "$PREFIX_DIR/lib/libavcodec.dylib" && -f "$OUT_LIB_DIR/libavcodec.dylib" ]]; then
  ffmpeg_outputs_present=1
else
  ffmpeg_outputs_present=0
fi
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$ffmpeg_outputs_present" == "1" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$stamp_value" ]]; then
    # Stamp hit: make sure the header/marker staging (Phase 2) is present in
    # this keyed bucket too -- a bucket stamped before the relocation has the
    # dylibs but not the staged include tree.
    if [[ ! -d "$OUT_LIB_DIR/ffmpeg/include" && -d "$PREFIX_DIR/include" ]]; then
      mkdir -p "$OUT_LIB_DIR/ffmpeg"
      cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/ffmpeg/include"
    fi
    [[ -f "$OUT_LIB_DIR/.ffmpeg-build-mode" ]] || echo -n "$FFMPEG_BUILD_MODE" > "$OUT_LIB_DIR/.ffmpeg-build-mode"
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

download_archive_xz() {
  local name="$1"
  local url="$2"
  local sha256="$3"
  local archive="$DOWNLOAD_DIR/$name.tar.xz"

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

vpx_target_for_arch() {
  local arch="$1"
  case "$arch" in
    arm64) echo "arm64-darwin24-gcc" ;;
    x86_64) echo "x86_64-darwin24-gcc" ;;
    *)
      echo "ERROR: unsupported architecture for libvpx: $arch" >&2
      exit 1
      ;;
  esac
}

build_libvpx() {
  local arch="$1"
  local prefix="$2"
  local target
  target="$(vpx_target_for_arch "$arch")"
  local build="$BUILD_DIR/libvpx-$arch"

  # VP8 DECODE IS BUILT, and it was not before 2026-09-10. The configure line
  # below used to pass `--disable-vp8-decoder` explicitly, so the archive
  # carried zero VP8 symbols and a VP8 WebM could only play through FFmpeg --
  # i.e. not at all in a store build. That was a licensing decision applied to
  # the wrong library: libvpx is BSD-3 and VP8 carries Google's irrevocable
  # royalty-free patent grant, so it is store-safe by the same standard as the
  # VP9 beside it. The ENCODERS stay off; nothing in this engine encodes.
  #
  # NOTE FOR ANYONE EDITING THE LINE BELOW: it is one backslash-continued
  # command. A `#` comment cannot go INSIDE it -- the continuation swallows the
  # rest of the command into the comment, which is how `--disable-vp9-encoder`
  # briefly stopped being passed while this very change was being written.
  mkdir -p "$build"
  pushd "$build" >/dev/null
  CC="clang -arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
    AR="ar" \
    LD="clang -arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
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
      --disable-vp9-encoder
  make -j "$JOBS"
  make install
  popd >/dev/null
}

build_opus() {
  local arch="$1"
  local prefix="$2"
  local build="$BUILD_DIR/opus-$arch"

  mkdir -p "$build"
  pushd "$build" >/dev/null
  "$SRC_DIR/opus-${OPUS_VERSION}/configure" \
    --prefix="$prefix" \
    --disable-shared \
    --enable-static \
    --disable-doc \
    --disable-extra-programs \
    CFLAGS="-O2 -arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
    CXXFLAGS="-O2 -arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET" \
    LDFLAGS="-arch $arch -mmacosx-version-min=$DEPLOYMENT_TARGET"
  make -j "$JOBS"
  make install
  popd >/dev/null
}

build_ffmpeg() {
  local arch="$1"
  local prefix="$2"
  local build="$BUILD_DIR/ffmpeg-$arch"

  mkdir -p "$build"
  pushd "$build" >/dev/null

  local PREFIX_ARCH="$prefix"
  local ARCH="$arch"
  local extra_flags=()

  if [[ "$arch" == "x86_64" ]]; then
    # arm64 is the native host toolchain; x86_64 is a cross build.
    extra_flags+=(--enable-cross-compile --target-os=darwin)
    if ! command -v nasm >/dev/null 2>&1 && ! command -v yasm >/dev/null 2>&1; then
      echo "WARNING: nasm/yasm not found; disabling x86 assembly for the x86_64 ffmpeg build (--disable-x86asm). Decode performance on Intel Macs will be reduced." >&2
      extra_flags+=(--disable-x86asm)
    fi
  fi

  # WMV/ASF (2026-07-18 spec): the asf demuxer is enabled in BOTH modes --
  # demuxing ASF is licensing-safe (Microsoft Open Specification Promise)
  # and the commercial Windows build needs it to feed WMV packets to Media
  # Foundation. The WMV/WMA software DECODERS (patent-encumbered: VC-1 pool,
  # WMA) are full-mode only. NOTE: if the Windows MF spike shows the MFT
  # needs parser-conditioned VC-1 input, --enable-parser=vc1 (licensing-safe)
  # may be promoted to both modes here and in the expected-parser sets.
  # msmpeg4v1/v2/v3 (MS MPEG-4 1996-1999, patents expired): the codecs real
  # early-era .wmv/.asf (and DivX3-era .avi) files actually carry -- the
  # first field sample checked was msmpeg4v3-in-ASF. Same select-dependency
  # (msmpeg4dec) the wmv1/wmv2 decoders already pull in.
  # hevc/aac/eac3 (2026-07-19 codec-superset spec): software fallbacks for
  # internal builds only (Windows N editions / pre-HEVC GPUs, Linux). Native
  # OS decoders always win where present -- these are used only when no
  # native decoder resolves. eac3's configure dependency on the ac3 core is
  # already satisfied (ac3 is in the base enabled-decoder list, both modes).
  # THE MODE, not the tier. $FFMPEG_BUILD_MODE is derived at the top of this
  # script from resolved MT_FFMPEG_BUILD_MODE, which is `full` only for
  # MT_PRIVATE_BUILD=1. $COMMERCIAL is the LICENCE TIER and answers a
  # different question: the public/free tier is COMMERCIAL=0 with mode
  # `commercial`, because patents attach to distribution, not to payment.
  # Reading the tier here handed that tier the withheld decoders.
  # No mode branch here any more: MT_FFMPEG_DECODERS and MT_FFMPEG_PARSERS
  # already carry the withheld names when the resolved mode is `full`, and
  # mtcaps decided that from MT_PRIVATE_BUILD. The branch this replaces read
  # the licence TIER and handed the public/free tier the withheld set.
  # NOTE: FFmpeg's mpeg4 decoder select-depends on the h263 decoder core
  # (configure: mpeg4_decoder_select="h263_decoder"); h263 is therefore
  # force-enabled. Passing --disable-decoder=h263 disables mpeg4 along with
  # it, so it cannot be removed while mpeg4 stays on the enabled list.
  # Approved deviation (H.263 patents expired; documented in spec codec
  # table). The exact resulting decoder set is asserted after configure via
  # require_ffmpeg_exact_set (decoders, demuxers, and parsers, per mode).
  "$SRC_DIR/ffmpeg-${FFMPEG_VERSION}/configure" --prefix="$PREFIX_ARCH" \
    --arch=$ARCH --cc="clang -arch $ARCH" \
    --extra-cflags="-mmacosx-version-min=$DEPLOYMENT_TARGET" \
    --extra-ldflags="-mmacosx-version-min=$DEPLOYMENT_TARGET" \
    --enable-shared --disable-static --disable-programs --disable-doc \
    --disable-network --disable-everything \
    --enable-protocol=file \
    --enable-demuxer="$(policy_csv "$MT_FFMPEG_DEMUXERS")" \
    --enable-decoder="$(policy_csv "$MT_FFMPEG_DECODERS")" \
    --enable-parser="$(policy_csv "$MT_FFMPEG_PARSERS")" \
    --enable-videotoolbox \
    --disable-encoders --disable-muxers --disable-filters --disable-bsfs \
    --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,aac_adtstoasc \
    --disable-devices \
    --disable-autodetect \
    ${extra_flags[@]+"${extra_flags[@]}"} \
    2>&1 | tee "$build/configure.log"

  make -j "$JOBS" 2>&1 | tee "$build/make.log"
  make install 2>&1 | tee "$build/install.log"
  popd >/dev/null
}

# The exact component sets expected in the built libraries, per build mode.
# Anything outside these lists appearing enabled fails the build -- this is
# the licensing guard for the LGPL decode-only bundle, and it is the
# AUTHORITATIVE check (config_components.h is generated from the configure
# result that compiled the libraries; the commercial symbol trace scan below
# is a best-effort secondary).
#
# Decoders (commercial): the 19 decoders explicitly requested via
# --enable-decoder above, plus h263, which FFmpeg's configure force-enables
# as a select-dependency of mpeg4 (see NOTE above the configure invocation).
# Full mode adds the 7 WMV/WMA software decoders (wmv3 select-depends on
# vc1, which is already on the list), the 3 msmpeg4 decoders, and the
# hevc/aac/eac3 software fallbacks (2026-07-19 codec-superset spec).
# WHAT IS POLICY AND WHAT IS THIS FFMPEG. The requested sets come from the
# vocabulary through MT_FFMPEG_*; what FFmpeg force-enables on top of them is
# a fact about ffmpeg-$FFMPEG_VERSION and lives here, next to that pin:
#   h263 decoder  <- mpeg4_decoder_select     (configure:3026)
#   h263 parser   <- mpeg4video parser select
# Putting these in the vocabulary would make it a second source of truth for
# someone else's configure, version-coupled to a pin it does not own.
FFMPEG_IMPLICIT_DECODERS="h263"
FFMPEG_IMPLICIT_PARSERS="h263"

# These four derive the expected codec sets from the vocabulary's policy lists.
# They are FFmpeg-only -- every consumer is inside an FFMPEG_ON block -- and
# under `set -u` an unset MT_FFMPEG_DECODERS is a hard exit, which is what an
# FFmpeg-off build legitimately has: no FFmpeg means no decoder policy to
# state. Defaulted rather than gated so the variables always exist for any
# later reader, and empty is the truthful value.
if [[ "$FFMPEG_ON" == "1" ]]; then
FFMPEG_EXPECTED_DECODERS="$(policy_sorted "$MT_FFMPEG_DECODERS $FFMPEG_IMPLICIT_DECODERS")"

# Demuxers: identical in both modes (asf demuxing is licensing-safe under
# Microsoft's Open Specification Promise). None of the requested demuxers
# select-depends on another demuxer.
FFMPEG_EXPECTED_DEMUXERS="$(policy_sorted "$MT_FFMPEG_DEMUXERS")"

# Parsers: the 13 requested plus h263 (force-enabled as a select-dependency
# of the mpeg4video parser). Full mode adds vc1 for the software VC-1
# decode path.
FFMPEG_EXPECTED_PARSERS="$(policy_sorted "$MT_FFMPEG_PARSERS $FFMPEG_IMPLICIT_PARSERS")"

# Decoders that must be confirmed absent (CONFIG_*_DECODER 0) in COMMERCIAL
# builds (store-safety guard, 2026-07-18 spec). HEVC/AAC/EAC3 moved here from
# a former always-forbidden list on 2026-07-19 (codec-superset spec): full
# builds now carry them as software fallbacks; commercial exclusion unchanged.
# Upper-cased for the CONFIG_<NAME>_DECODER symbols this checks. Derived from
# the same withheld list the licence scanner reads, so the two cannot drift --
# and it grows from 10 names to 13, because the scripts' hand-written copy had
# been missing msmpeg4v1/v2/v3 while the vocabulary listed them.
FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL="$(printf '%s' "$MT_FFMPEG_DECODERS_WITHHELD" | tr '[:lower:] ' '[:upper:]\n' | tr '\n' ' ')"
else
  FFMPEG_EXPECTED_DECODERS=""
  FFMPEG_EXPECTED_DEMUXERS=""
  FFMPEG_EXPECTED_PARSERS=""
  FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL=""
fi

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

# Commercial-only binary trace scan (secondary guard; require_ffmpeg_exact_set
# on config_components.h is the authoritative one). Scans the merged universal
# dylibs' symbol tables for the forbidden decoders' FFCodec entry symbols
# (_ff_<name>_decoder -- non-static globals in libavcodec). Deliberately does
# NOT `strings`-scan for bare codec names like "wmv3": libavcodec's
# codec_desc.c embeds the name of EVERY known codec ID regardless of which
# decoders are compiled in, so name strings would false-positive on a clean
# build. Symbol tables can in principle be stripped -- best-effort by design.
require_ffmpeg_no_forbidden_symbols() {
  local libdir="$1"
  local forbidden="hevc aac eac3 wmv1 wmv2 wmv3 vc1 wmav1 wmav2 wmapro"
  local lib name sym hits
  for lib in libavcodec libavformat; do
    local soname
    soname="$(ffmpeg_soname_link "$libdir" "$lib")"
    require_file "$libdir/$soname"
    for name in $forbidden; do
      sym="_ff_${name}_decoder"
      hits="$(nm -a "$libdir/$soname" 2>/dev/null | grep -c "[[:space:]]${sym}\$" || true)"
      if [[ "$hits" != "0" ]]; then
        echo "ERROR: commercial trace scan found forbidden decoder symbol $sym in $libdir/$soname" >&2
        exit 1
      fi
    done
  done
  echo "Commercial trace scan clean: no forbidden decoder symbols in $libdir"
}

# Locate the SONAME-level symlink for an ffmpeg dylib, e.g. libavcodec.61.dylib
# (exactly one numeric version component), as distinct from the unversioned
# dev symlink (libavcodec.dylib) and the fully-versioned real file
# (libavcodec.61.19.101.dylib).
ffmpeg_soname_link() {
  local dir="$1"
  local lib="$2"
  (cd "$dir" && ls | grep -E "^${lib}\.[0-9]+\.dylib\$")
}

combine_universal_ffmpeg_dylib() {
  local lib="$1"
  local ref_arch="${ARCH_LIST[0]}"
  local ref_dir="$CACHE_DIR/install-$ref_arch/lib"

  local real_name
  real_name="$(cd "$ref_dir" && find . -maxdepth 1 -name "$lib.*.dylib" ! -type l -exec basename {} \;)"
  if [[ -z "$real_name" ]]; then
    echo "ERROR: could not locate real (non-symlink) dylib for $lib in $ref_dir" >&2
    exit 1
  fi

  local inputs=()
  local arch
  for arch in "${ARCH_LIST[@]}"; do
    local input="$CACHE_DIR/install-$arch/lib/$real_name"
    require_file "$input"
    inputs+=("$input")
  done

  local out="$PREFIX_DIR/lib/$real_name"
  rm -f "$out"
  lipo -create "${inputs[@]}" -output "$out"
  require_universal "$out"

  # Recreate the soname + dev symlinks `make install` produced (same topology
  # in every arch install since both build the same ffmpeg version/config),
  # now pointing at the merged universal file.
  local link
  for link in "$ref_dir/$lib".*.dylib "$ref_dir/$lib".dylib; do
    [[ -e "$link" ]] || continue
    if [[ -L "$link" ]]; then
      local link_base target
      link_base="$(basename "$link")"
      target="$(readlink "$link")"
      ln -sf "$target" "$PREFIX_DIR/lib/$link_base"
    fi
  done
}

normalize_ffmpeg_install_names() {
  # IMPORTANT: this must run per-arch, on each arch's own install-$arch/lib
  # dylibs, BEFORE lipo -create merges them. Each arch was configured with a
  # different --prefix (install-arm64 vs install-x86_64), so the *absolute*
  # dependency path baked into e.g. libavcodec's reference to libswresample
  # differs between the two arch slices. If normalization instead ran once
  # on the already-lipo'd universal binary, a single `install_name_tool
  # -change <old> <new>` only rewrites the slice whose recorded path
  # happens to equal the one `old` value we extracted, silently leaving the
  # other architecture's slice pointing at a build-machine-only absolute
  # path. Normalizing before the merge means both slices already agree on
  # the same "@rpath/..." string by the time they're glued together.
  local arch="$1"
  local dir="$CACHE_DIR/install-$arch/lib"
  local lib
  for lib in libavutil libavcodec libavformat libswscale libswresample; do
    local soname real
    soname="$(ffmpeg_soname_link "$dir" "$lib")"
    if [[ -z "$soname" ]]; then
      echo "ERROR: could not locate SONAME symlink for $lib in $dir" >&2
      exit 1
    fi
    real="$dir/$soname"
    install_name_tool -id "@rpath/$soname" "$real"
    local dep
    for dep in libavutil libavcodec libavformat libswscale libswresample; do
      local old
      old=$(otool -L "$real" | awk "/$dep/ {print \$1; exit}")
      [[ -n "$old" && "$old" != @rpath/* ]] && install_name_tool -change "$old" "@rpath/$(basename "$old")" "$real"
    done
  done
}

require_ffmpeg_clean_deps() {
  local file="$1"
  local dep
  # otool -L on a universal (fat) dylib repeats an unindented
  # "<path> (architecture <arch>):" header line before each slice's
  # dependency list. Only indented lines are actual dependencies (the
  # first of which is always the dylib's own LC_ID_DYLIB entry) --
  # filter on leading whitespace so per-arch header lines (which start
  # at column 0 and would otherwise look like a self-referential
  # absolute-path "dependency") are never mistaken for one.
  while IFS= read -r dep; do
    [[ -z "$dep" ]] && continue
    case "$dep" in
      @rpath/libav*.dylib|@rpath/libsw*.dylib) continue ;;
      /usr/lib/*|/System/Library/Frameworks/*) continue ;;
      *)
        echo "ERROR: unexpected dependency in $file: $dep" >&2
        exit 1
        ;;
    esac
  done < <(otool -L "$file" | grep -E '^[[:space:]]' | awk '{print $1}')
}

merge_ffmpeg_headers() {
  rm -rf "$PREFIX_DIR/include"
  mkdir -p "$PREFIX_DIR/include"
  # vpx and opus BELONG HERE, and their absence was invisible for as long as
  # every machine that built this had them from Homebrew or MacPorts.
  #
  # The engine includes <vpx/vpx_codec.h> and <opus/opus.h> directly, not
  # through FFmpeg. Staging only the libav* directories left those headers to
  # be found wherever the machine happened to keep a copy: this one compiled
  # against Homebrew's libvpx 1.16.0 while linking the 1.15.2 this script
  # builds, and nobody noticed because the two happen to agree. A runner with
  # no system libvpx simply cannot compile the engine -- measured 2026-09-03,
  # "fatal error: 'vpx/vpx_codec.h' file not found", after every dependency
  # had built successfully.
  #
  # Linux never had this: it stages the whole install prefix, so vpx's and
  # opus's headers travel with FFmpeg's. This makes macOS match.
  local dir
  local dirs=(vpx opus)
  if [[ "$FFMPEG_ON" == "1" ]]; then
    dirs=(libavutil libavcodec libavformat libswscale libswresample vpx opus)
  fi
  for dir in "${dirs[@]}"; do
    cp -R "$CACHE_DIR/install-${ARCH_LIST[0]}/include/$dir" "$PREFIX_DIR/include/"
  done

  # libavutil/avconfig.h is generated per-arch; both current arches are
  # little-endian macOS with the same compiler so it should be identical.
  # Fail loudly instead of silently shipping a wrong header if that ever
  # stops being true.
  # Only when FFmpeg was built -- there is no libavutil/avconfig.h otherwise,
  # and diff'ing a file that does not exist would fail this whole function,
  # which now also stages vpx and opus.
  [[ "$FFMPEG_ON" == "1" ]] || return 0
  local avconfig_ref="$CACHE_DIR/install-${ARCH_LIST[0]}/include/libavutil/avconfig.h"
  local arch
  for arch in "${ARCH_LIST[@]}"; do
    local avconfig_other="$CACHE_DIR/install-$arch/include/libavutil/avconfig.h"
    if ! diff -q "$avconfig_ref" "$avconfig_other" >/dev/null 2>&1; then
      echo "ERROR: libavutil/avconfig.h differs between architectures ($arch vs ${ARCH_LIST[0]});" >&2
      echo "       universal header merge needs manual handling." >&2
      diff "$avconfig_ref" "$avconfig_other" >&2 || true
      exit 1
    fi
  done
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

require_symbol() {
  local symbol="$1"
  if ! nm -g "$OUT_LIB" | grep -F "$symbol" >/dev/null; then
    echo "ERROR: expected symbol not found in $OUT_LIB: $symbol" >&2
    exit 1
  fi
}

download_archive "libvpx-${VPX_VERSION}" "$VPX_URL" "$VPX_SHA256"
download_archive "opus-${OPUS_VERSION}" "$OPUS_URL" "$OPUS_SHA256"
if [[ "$FFMPEG_ON" == "1" ]]; then
  download_archive_xz "ffmpeg-${FFMPEG_VERSION}" "$FFMPEG_URL" "$FFMPEG_SHA256"
fi

extract_archive "libvpx-${VPX_VERSION}" "libvpx-${VPX_VERSION}"
extract_archive "opus-${OPUS_VERSION}" "opus-${OPUS_VERSION}"
if [[ "$FFMPEG_ON" == "1" ]]; then
  extract_archive_xz "ffmpeg-${FFMPEG_VERSION}" "ffmpeg-${FFMPEG_VERSION}"
fi

rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-*
mkdir -p "$BUILD_DIR" "$PREFIX_DIR"

for arch in "${ARCH_LIST[@]}"; do
  arch_prefix="$CACHE_DIR/install-$arch"
  mkdir -p "$arch_prefix"
  build_libvpx "$arch" "$arch_prefix"
  build_opus "$arch" "$arch_prefix"
  if [[ "$FFMPEG_ON" == "1" ]]; then
    build_ffmpeg "$arch" "$arch_prefix"
    # ONE expected set per kind, whatever the mode: the policy lists already
    # carry the withheld names when the mode is `full`, so the guard no longer
    # picks between two hand-written sets -- which is what it got wrong by
    # picking on the licence tier. Only the absence check is mode-specific,
    # because "these must NOT be here" has no meaning in full mode.
    #
    # Every one of these reads $BUILD_DIR/ffmpeg-$arch and hard-exits when it is
    # missing, so they belong INSIDE the gate with the build that creates it.
    # Gating only the build_ffmpeg call leaves six guards to fail on a directory
    # that was never made.
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "DECODER" "$FFMPEG_EXPECTED_DECODERS"
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "PARSER" "$FFMPEG_EXPECTED_PARSERS"
    if [[ "$FFMPEG_BUILD_MODE" == "commercial" ]]; then
      for dec in $FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL; do
        require_ffmpeg_decoder_disabled "$BUILD_DIR/ffmpeg-$arch" "$dec"
      done
    fi
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "DEMUXER" "$FFMPEG_EXPECTED_DEMUXERS"
    normalize_ffmpeg_install_names "$arch"
  fi
done

rm -rf "$PREFIX_DIR/lib"
mkdir -p "$PREFIX_DIR/lib"

combine_universal_lib "lib/libvpx.a"
combine_universal_lib "lib/libopus.a"

# FFmpeg dylibs are a separate output (install/lib/*.dylib) -- they are NOT
# merged into libmt_video_codecs.a below. Each arch's dylibs were already
# normalized to @rpath install names above, before this lipo merge.
if [[ "$FFMPEG_ON" == "1" ]]; then
  for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
    combine_universal_ffmpeg_dylib "$ff_lib"
  done
fi
# ALWAYS, in both configurations. merge_ffmpeg_headers stages vpx and opus as
# well as the libav* trees -- the engine includes <vpx/vpx_codec.h> and
# <opus/opus.h> directly, and this function is the only thing that puts them
# where the compiler looks. Skipping it wholesale with FFmpeg reproduces the
# 2026-09-03 failure its own comment records: "fatal error: 'vpx/vpx_codec.h'
# file not found", after every dependency had built successfully.
merge_ffmpeg_headers

if [[ "$FFMPEG_ON" == "1" ]]; then
  for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
    ff_soname="$(ffmpeg_soname_link "$PREFIX_DIR/lib" "$ff_lib")"
    require_ffmpeg_clean_deps "$PREFIX_DIR/lib/$ff_soname"
  done

  if [[ "$FFMPEG_BUILD_MODE" == "commercial" ]]; then
    require_ffmpeg_no_forbidden_symbols "$PREFIX_DIR/lib"
  fi
fi

# Mode marker: consumed by app build scripts (guard #3 in the 2026-07-18 WMV
# spec). A commercial app build against a "full" install is a fatal error
# there; this file is how it knows what it is linking.
# NOT written when FFmpeg is off. The marker answers "what kind of FFmpeg is
# installed here", and the honest answer to that in a build with none is the
# file's absence -- app-build-*.sh treats a missing marker as a hard error for
# a commercial build, which is exactly right for a bucket that claims FFmpeg
# and has not got it.
if [[ "$FFMPEG_ON" == "1" ]]; then
  echo -n "$FFMPEG_BUILD_MODE" > "$PREFIX_DIR/.ffmpeg-build-mode"
fi

INPUT_LIBS=(
  "$PREFIX_DIR/lib/libvpx.a"
  "$PREFIX_DIR/lib/libopus.a"
)

for lib in "${INPUT_LIBS[@]}"; do
  require_file "$lib"
  require_universal "$lib"
done

rm -f "$OUT_LIB"
libtool -static -o "$OUT_LIB" "${INPUT_LIBS[@]}"
ranlib "$OUT_LIB" || true
require_universal "$OUT_LIB"

require_symbol "_vpx_codec_dec_init_ver"
require_symbol "_vpx_codec_decode"
require_symbol "_vpx_codec_destroy"
require_symbol "_vpx_codec_err_to_string"
require_symbol "_vpx_codec_get_frame"
require_symbol "_vpx_codec_vp9_dx"
require_symbol "_opus_decoder_create"
require_symbol "_opus_decode_float"
require_symbol "_opus_decoder_ctl"
require_symbol "_opus_decoder_destroy"
require_symbol "_opus_strerror"

# STAGE THE DYLIBS BESIDE THE ARCHIVES, outside every checkout.
#
# FFmpeg is the one dependency this script does not fold into a static archive
# (LGPL dynamic linking is what keeps the licence obligation discharged by
# shipping the dylib), so an app that uses CVideoSourceFFmpeg needs these five
# files as well as libmt_video_codecs.a. They were only ever produced inside the
# checkout, at other/lib/video-codecs/install/lib, which is a fixed shared path
# of exactly the kind the archive relocation removed.
#
# -R preserves the SONAME symlink (libavcodec.61.dylib -> libavcodec.61.19.101
# .dylib) as a symlink. That matters: the install names are already normalised
# to @rpath/<soname>, so a consumer resolves @rpath/libavcodec.61.dylib and the
# link has to still be there to reach the real file. Copying it as a second
# regular file would work but would double what an app embeds.
#
# install/lib keeps its copies -- the photo app still reads them from there.
if [[ "$FFMPEG_ON" == "1" ]]; then
  echo "Staging FFmpeg dylibs to $OUT_LIB_DIR"
  for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
    for f in "$PREFIX_DIR/lib/$ff_lib".*.dylib "$PREFIX_DIR/lib/$ff_lib.dylib"; do
      [[ -e "$f" ]] || continue
      cp -R -f "$f" "$OUT_LIB_DIR/"
    done
  done
fi

echo -n "$stamp_value" > "$STAMP_FILE"
echo "Video codec bundle built: $OUT_LIB"
# THE VIEW IS SHARED BETWEEN THE TWO CONFIGURATIONS AND IS NEVER CLEANED.
#
# $OUT_LIB_DIR is keyed by the capability set, and MT_ENABLE_FFMPEG is not part
# of that key -- flipping the flag does not change this path. The staging below
# only ever COPIES, so without the removal an FFmpeg-off build inherits the
# previous FFmpeg-on build's dylibs, its libav* headers and its mode marker.
#
# Measured 2026-09-10, from a wiped _deps: an FFmpeg-off build reported the
# skip correctly and the view still held 9 libav dylibs, a resolvable
# <libavcodec/avcodec.h> and a marker reading "full". The test app then compiled
# green at MT_ENABLE_FFMPEG=0 purely because its test header's unconditional
# libavcodec include resolved against those stale headers -- a FALSE GREEN for
# the exact configuration this programme exists to produce.
if [[ "$FFMPEG_ON" == "0" ]]; then
  # BOTH the store and the view. mt_caps_sync_store_to_view is `tar c | tar x`
  # -- purely additive, and deliberately so: the view is SHARED by every unit
  # (sdl3, opencv, image_codecs, ...), so a sync that mirrored its store would
  # delete the other units' archives. The consequence is that only the
  # producing script can retire its own outputs, because only it knows which
  # files are its own. Removing from the store alone leaves the view holding
  # the previous FFmpeg-on build's dylibs and headers forever.
  for dir in "$OUT_LIB_DIR" "${MT_VIEW_LIB_DIR:-}"; do
    [[ -n "$dir" && -d "$dir" ]] || continue
    for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
      rm -f "$dir/$ff_lib".*.dylib "$dir/$ff_lib.dylib"
    done
    rm -f "$dir/.ffmpeg-build-mode"
    rm -rf "$dir/ffmpeg/include/libavutil" "$dir/ffmpeg/include/libavcodec" \
           "$dir/ffmpeg/include/libavformat" "$dir/ffmpeg/include/libswscale" \
           "$dir/ffmpeg/include/libswresample"
  done
else
  # The mode marker travels WITH the staged dylibs (Phase 2, 2026-08-31): a
  # guard reading the in-checkout prefix reads a path the relocation will
  # delete, and a stale tree there could answer for the wrong mode.
  echo -n "$FFMPEG_BUILD_MODE" > "$OUT_LIB_DIR/.ffmpeg-build-mode"
fi
# FFmpeg headers travel with the dylibs too (Phase 2) -- the projects used to
# read other/lib/video-codecs/install/include from inside the checkout. The
# rm -rf is what makes this correct in both directions: the include tree is
# REPLACED by whatever this configuration built, so an FFmpeg-off build leaves
# vpx and opus and no libav* at all.
rm -rf "$OUT_LIB_DIR/ffmpeg/include"
mkdir -p "$OUT_LIB_DIR/ffmpeg"
cp -R "$PREFIX_DIR/include" "$OUT_LIB_DIR/ffmpeg/include"
if [[ "$FFMPEG_ON" == "1" ]]; then
  echo "FFmpeg LGPL decode dylibs built: $PREFIX_DIR/lib, staged to $OUT_LIB_DIR (avutil/avcodec/avformat/swscale/swresample)"
else
  echo "MT_ENABLE_FFMPEG=0: no FFmpeg downloaded, built or staged. libvpx + opus only."
fi
