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
CACHE_DIR="$ROOT_DIR/other/lib/video-codecs"
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
OUT_LIB="$OUT_LIB_DIR/libmt_video_codecs.a"
STAMP_FILE="$OUT_LIB_DIR/libmt_video_codecs.stamp"

ARCH_LIST=(arm64 x86_64)
DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-10.15}"
JOBS="${MT_BUILD_JOBS:-8}"

# Build mode: COMMERCIAL=1 produces the store-safe FFmpeg (no patent-encumbered
# software decoders: no WMV/VC-1, no WMA, no HEVC/AAC/EAC3). COMMERCIAL=0
# ("full") additionally enables WMV/WMA software decode plus the
# HEVC/AAC/EAC3 software fallbacks (2026-07-19 codec-superset spec) for
# non-store internal builds.
# COMMERCIAL is a PARAMETER now, not a file read. The engine used to track a
# platform/BUILD_MODE_DEFAULT and PhotoCruise tracked a root-level file of the
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
if [[ "$COMMERCIAL" == "1" ]]; then
  FFMPEG_BUILD_MODE="commercial"
else
  FFMPEG_BUILD_MODE="full"
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
if [[ "${MT_ENABLE_FFMPEG:-1}" == "0" ]]; then
  STUB_STAMP="disabled:${script_sha}"
  if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" && "$(cat "$STAMP_FILE" 2>/dev/null)" == "$STUB_STAMP" ]]; then
    exit 0
  fi
  mt_caps_stub_archive "$OUT_LIB" "mt_video_codecs"
  echo -n "$STUB_STAMP" > "$STAMP_FILE"
  exit 0
fi

stamp_value="${script_sha}:libvpx-${VPX_VERSION}:opus-${OPUS_VERSION}:ffmpeg-${FFMPEG_VERSION}:${DEPLOYMENT_TARGET}:mode-${FFMPEG_BUILD_MODE}"
# The staged dylib is part of the check, not just the archive. $OUT_LIB_DIR is
# keyed by capability set, so a fresh key has a valid stamp only if it also has
# the dylibs -- and an app that links FFmpeg needs both halves of this script's
# output, not one.
if [[ -f "$OUT_LIB" && -f "$STAMP_FILE" \
      && -f "$PREFIX_DIR/lib/libavcodec.dylib" \
      && -f "$OUT_LIB_DIR/libavcodec.dylib" ]]; then
  if [[ "$(cat "$STAMP_FILE")" == "$stamp_value" ]]; then
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
      --disable-vp8-decoder \
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
  local mode_flags=()
  if [[ "$COMMERCIAL" == "0" ]]; then
    mode_flags+=(--enable-decoder=wmv1,wmv2,wmv3,vc1,wmav1,wmav2,wmapro)
    mode_flags+=(--enable-decoder=msmpeg4v1,msmpeg4v2,msmpeg4v3)
    mode_flags+=(--enable-decoder=hevc,aac,eac3)
    mode_flags+=(--enable-parser=vc1)
  fi

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
    --enable-demuxer=mov,matroska,avi,mpegts,mpegps,mpegvideo,asf \
    --enable-decoder=h264,prores,mjpeg,mpeg2video,mpeg4,mpeg1video,vp8,vp9,dvvideo \
    --enable-decoder=pcm_s16le,pcm_s16be,pcm_s24le,pcm_u8,mp3,mp2,ac3,opus,vorbis,flac \
    --enable-parser=h264,hevc,mpeg4video,mpegvideo,mjpeg,vp8,vp9,aac,ac3,mpegaudio,opus,vorbis,flac \
    --enable-videotoolbox \
    --disable-encoders --disable-muxers --disable-filters --disable-bsfs \
    --enable-bsf=hevc_mp4toannexb,h264_mp4toannexb,aac_adtstoasc \
    --disable-devices \
    --disable-autodetect \
    "${mode_flags[@]}" \
    "${extra_flags[@]}" \
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
FFMPEG_EXPECTED_DECODERS_COMMERCIAL="ac3 dvvideo flac h263 h264 mjpeg mp2 mp3 mpeg1video mpeg2video mpeg4 opus pcm_s16be pcm_s16le pcm_s24le pcm_u8 prores vorbis vp8 vp9"
FFMPEG_EXPECTED_DECODERS_FULL="$FFMPEG_EXPECTED_DECODERS_COMMERCIAL aac eac3 hevc msmpeg4v1 msmpeg4v2 msmpeg4v3 vc1 wmapro wmav1 wmav2 wmv1 wmv2 wmv3"

# Demuxers: identical in both modes (asf demuxing is licensing-safe under
# Microsoft's Open Specification Promise). None of the requested demuxers
# select-depends on another demuxer.
FFMPEG_EXPECTED_DEMUXERS="asf avi matroska mov mpegps mpegts mpegvideo"

# Parsers: the 13 requested plus h263 (force-enabled as a select-dependency
# of the mpeg4video parser). Full mode adds vc1 for the software VC-1
# decode path.
FFMPEG_EXPECTED_PARSERS_COMMERCIAL="aac ac3 flac h263 h264 hevc mjpeg mpeg4video mpegaudio mpegvideo opus vorbis vp8 vp9"
FFMPEG_EXPECTED_PARSERS_FULL="$FFMPEG_EXPECTED_PARSERS_COMMERCIAL vc1"

# Decoders that must be confirmed absent (CONFIG_*_DECODER 0) in COMMERCIAL
# builds (store-safety guard, 2026-07-18 spec). HEVC/AAC/EAC3 moved here from
# a former always-forbidden list on 2026-07-19 (codec-superset spec): full
# builds now carry them as software fallbacks; commercial exclusion unchanged.
FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL="HEVC AAC EAC3 WMV1 WMV2 WMV3 VC1 WMAV1 WMAV2 WMAPRO"

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
  local dir
  for dir in libavutil libavcodec libavformat libswscale libswresample; do
    cp -R "$CACHE_DIR/install-${ARCH_LIST[0]}/include/$dir" "$PREFIX_DIR/include/"
  done

  # libavutil/avconfig.h is generated per-arch; both current arches are
  # little-endian macOS with the same compiler so it should be identical.
  # Fail loudly instead of silently shipping a wrong header if that ever
  # stops being true.
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
download_archive_xz "ffmpeg-${FFMPEG_VERSION}" "$FFMPEG_URL" "$FFMPEG_SHA256"

extract_archive "libvpx-${VPX_VERSION}" "libvpx-${VPX_VERSION}"
extract_archive "opus-${OPUS_VERSION}" "opus-${OPUS_VERSION}"
extract_archive_xz "ffmpeg-${FFMPEG_VERSION}" "ffmpeg-${FFMPEG_VERSION}"

rm -rf "$BUILD_DIR" "$PREFIX_DIR" "$CACHE_DIR"/install-*
mkdir -p "$BUILD_DIR" "$PREFIX_DIR"

for arch in "${ARCH_LIST[@]}"; do
  arch_prefix="$CACHE_DIR/install-$arch"
  mkdir -p "$arch_prefix"
  build_libvpx "$arch" "$arch_prefix"
  build_opus "$arch" "$arch_prefix"
  build_ffmpeg "$arch" "$arch_prefix"
  if [[ "$COMMERCIAL" == "1" ]]; then
    for dec in $FFMPEG_FORBIDDEN_DECODERS_COMMERCIAL; do
      require_ffmpeg_decoder_disabled "$BUILD_DIR/ffmpeg-$arch" "$dec"
    done
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "DECODER" "$FFMPEG_EXPECTED_DECODERS_COMMERCIAL"
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "PARSER" "$FFMPEG_EXPECTED_PARSERS_COMMERCIAL"
  else
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "DECODER" "$FFMPEG_EXPECTED_DECODERS_FULL"
    require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "PARSER" "$FFMPEG_EXPECTED_PARSERS_FULL"
  fi
  require_ffmpeg_exact_set "$BUILD_DIR/ffmpeg-$arch" "DEMUXER" "$FFMPEG_EXPECTED_DEMUXERS"
  normalize_ffmpeg_install_names "$arch"
done

rm -rf "$PREFIX_DIR/lib"
mkdir -p "$PREFIX_DIR/lib"

combine_universal_lib "lib/libvpx.a"
combine_universal_lib "lib/libopus.a"

# FFmpeg dylibs are a separate output (install/lib/*.dylib) -- they are NOT
# merged into libmt_video_codecs.a below. Each arch's dylibs were already
# normalized to @rpath install names above, before this lipo merge.
for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
  combine_universal_ffmpeg_dylib "$ff_lib"
done
merge_ffmpeg_headers

for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
  ff_soname="$(ffmpeg_soname_link "$PREFIX_DIR/lib" "$ff_lib")"
  require_ffmpeg_clean_deps "$PREFIX_DIR/lib/$ff_soname"
done

if [[ "$COMMERCIAL" == "1" ]]; then
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
# install/lib keeps its copies -- PhotoCruise still reads them from there.
echo "Staging FFmpeg dylibs to $OUT_LIB_DIR"
for ff_lib in libavutil libavcodec libavformat libswscale libswresample; do
  for f in "$PREFIX_DIR/lib/$ff_lib".*.dylib "$PREFIX_DIR/lib/$ff_lib.dylib"; do
    [[ -e "$f" ]] || continue
    cp -R -f "$f" "$OUT_LIB_DIR/"
  done
done

echo -n "$stamp_value" > "$STAMP_FILE"
echo "Video codec bundle built: $OUT_LIB"
echo "FFmpeg LGPL decode dylibs built: $PREFIX_DIR/lib, staged to $OUT_LIB_DIR (avutil/avcodec/avformat/swscale/swresample)"
