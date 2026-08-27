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

check_file_exists "$ROOT_DIR/platform/MacOS/build-image_codecs.sh"
check_file_contains "$ENGINE_PROJECT" "Build image_codecs"
check_file_contains "$ENGINE_PROJECT" '"$(PROJECT_DIR)/build-image_codecs.sh"'
# Declared OUTSIDE the checkout. $(PROJECT_DIR) is platform/MacOS, so the old
# form asserted the archive was written inside the repository.
check_file_contains "$ENGINE_PROJECT" '"$(MT_CAPS_LIBS_DIR)/libmt_image_codecs.a"'
check_file_contains "$ENGINE_PROJECT" '"$(MT_CAPS_LIBS_DIR)/libmt_image_codecs.stamp"'
# install/include, not include: the acquisition script installs the upstream
# headers into other/lib/image-codecs/install/ and the project has pointed there
# for some time. This assertion still named the pre-install path and had been
# failing on its own -- it is not part of the archive relocation.
check_file_contains "$ENGINE_PROJECT" '"$(PROJECT_DIR)/../../other/lib/image-codecs/install/include"'

check_file_not_contains "$ENGINE_PROJECT" '"-ltiff"'
check_file_not_contains "$ENGINE_PROJECT" '"-lwebp"'
check_file_not_contains "$ENGINE_PROJECT" '"-lwebpdemux"'
check_file_not_contains "$ENGINE_PROJECT" '"-lavif"'
check_file_not_contains "$ENGINE_PROJECT" '"-lraw"'

# The app links the bundle through the SEARCH PATH, not a file reference.
#
# It used to carry a PBXFileReference naming an absolute-ish path into the engine
# checkout, plus the matching Frameworks entry -- which is why this test could
# assert a phase ORDER at all. The archives now live outside every checkout at a
# path keyed by the resolved capability set, so there is no fixed path to name:
# the reference became -lmt_image_codecs resolved against $(MT_CAPS_LIBS_DIR),
# and ordering is no longer expressible (nor needed -- ld resolves -l archives
# after the object files regardless of flag order).
check_file_contains "$LIGHTHEROES_PROJECT" '"-lmt_image_codecs"'
check_file_contains "$LIGHTHEROES_PROJECT" '"$(MT_CAPS_LIBS_DIR)"'
check_file_not_contains "$LIGHTHEROES_PROJECT" "MTEngineSDL/platform/MacOS/libs"

git -C "$ROOT_DIR" check-ignore -q "other/lib/image-codecs/src/probe"
git -C "$ROOT_DIR" check-ignore -q "other/lib/image-codecs/build/probe"
