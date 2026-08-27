#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LIGHTHEROES_DIR="$(cd "$ROOT_DIR/../LightHeroes" && pwd)"

ENGINE_PROJECT="$ROOT_DIR/platform/MacOS/MTEngineSDL.xcodeproj/project.pbxproj"
LIGHTHEROES_PROJECT="$LIGHTHEROES_DIR/platform/MacOS/LightHeroes.xcodeproj/project.pbxproj"

check_file_exists() {
  local file="$1"
  if [[ ! -f "$file" ]]; then
    echo "Expected file to exist: $file" >&2
    return 1
  fi
}

check_file_contains() {
  local file="$1"
  local expected="$2"
  if ! grep -Fq "$expected" "$file"; then
    echo "Expected $file to contain: $expected" >&2
    return 1
  fi
}

check_file_not_contains() {
  local file="$1"
  local forbidden="$2"
  if grep -Fq "$forbidden" "$file"; then
    echo "Expected $file not to contain: $forbidden" >&2
    return 1
  fi
}

check_frameworks_phase_order() {
  local file="$1"
  local first="$2"
  local second="$3"

  if ! awk -v first="$first" -v second="$second" '
    /Begin PBXFrameworksBuildPhase section/ { in_section = 1 }
    /End PBXFrameworksBuildPhase section/ { in_section = 0 }
    in_section && /files = \(/ { in_files = 1; block = ""; next }
    in_files {
      block = block $0 "\n"
      if ($0 ~ /^[[:space:]]*\);/) {
        first_pos = index(block, first)
        second_pos = index(block, second)
        if (first_pos > 0 && second_pos > 0) {
          if (first_pos < second_pos) {
            found_ordered = 1
          } else {
            found_unordered = 1
          }
        }
        in_files = 0
      }
    }
    END {
      if (found_ordered) exit 0
      if (found_unordered) exit 2
      exit 1
    }
  ' "$file"; then
    echo "Expected '$first' to appear before '$second' in the PBXFrameworksBuildPhase files list of $file" >&2
    return 1
  fi
}

check_file_exists "$ROOT_DIR/platform/MacOS/build-video_codecs.sh"
check_file_contains "$ENGINE_PROJECT" "Build video_codecs"
check_file_contains "$ENGINE_PROJECT" '"$(PROJECT_DIR)/build-video_codecs.sh"'
# The acquisition phase declares its outputs OUTSIDE the checkout. $(PROJECT_DIR)
# was platform/MacOS, so these two used to assert the archive was written inside
# the repository -- exactly what THE RULE forbids.
check_file_contains "$ENGINE_PROJECT" '"$(MT_CAPS_LIBS_DIR)/libmt_video_codecs.a"'
check_file_contains "$ENGINE_PROJECT" '"$(MT_CAPS_LIBS_DIR)/libmt_video_codecs.stamp"'

# Linked through the search path rather than a file reference -- see the same
# note in test-macos-image-codec-build-config.sh.
check_file_contains "$LIGHTHEROES_PROJECT" '"-lmt_video_codecs"'
check_file_contains "$LIGHTHEROES_PROJECT" '"$(MT_CAPS_LIBS_DIR)"'
check_file_not_contains "$LIGHTHEROES_PROJECT" "MTEngineSDL/platform/MacOS/libs"

check_file_not_contains "$LIGHTHEROES_PROJECT" "../../../MTEngineSDL/other/lib/libvpx/macos-arm64/libvpx.a"
check_file_not_contains "$LIGHTHEROES_PROJECT" "../../../MTEngineSDL/other/lib/libopus/macos-arm64/libopus.a"
check_file_not_contains "$LIGHTHEROES_PROJECT" "MTEngineSDL/other/lib/libvpx/macos-arm64"
check_file_not_contains "$LIGHTHEROES_PROJECT" "MTEngineSDL/other/lib/libopus/macos-arm64"

git -C "$ROOT_DIR" check-ignore -q "other/lib/video-codecs/src/probe"
git -C "$ROOT_DIR" check-ignore -q "other/lib/video-codecs/build/probe"
