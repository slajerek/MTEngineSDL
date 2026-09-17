#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

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

check_file_contains "$ROOT_DIR/platform/MacOS/build-mbedtls.sh" 'BUILD_DIR="$(mt_caps_work_dir mbedtls)/build-macos"'
check_file_contains "$ROOT_DIR/platform/Linux/build-mbedtls.sh" 'BUILD_DIR="$(mt_caps_work_dir mbedtls)/build-linux"'
check_file_contains "$ROOT_DIR/platform/Windows/build-mbedtls.ps1" 'Join-Path (Get-MTCapsWorkDir '\''mbedtls'\'') "build-windows-$Platform"'

check_file_not_contains "$ROOT_DIR/platform/MacOS/build-mbedtls.sh" 'BUILD_DIR="$MBEDTLS_SRC_DIR/build-macos"'
check_file_not_contains "$ROOT_DIR/platform/Linux/build-mbedtls.sh" 'BUILD_DIR="$MBEDTLS_SRC_DIR/build-linux"'
check_file_not_contains "$ROOT_DIR/platform/Windows/build-mbedtls.ps1" "Join-Path \$mbedSrc 'build-windows'"

check_file_not_contains "$ROOT_DIR/platform/MacOS/build-mbedtls.sh" 'other/lib/mbedtls.macos'
check_file_not_contains "$ROOT_DIR/platform/Linux/build-mbedtls.sh" 'other/lib/mbedtls.linux'
check_file_not_contains "$ROOT_DIR/platform/Windows/build-mbedtls.ps1" 'other\lib\mbedtls.windows'

if git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.macos/probe"; then
  echo "Expected other/lib/mbedtls.macos not to be ignored" >&2
  exit 1
fi
if git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.linux/probe"; then
  echo "Expected other/lib/mbedtls.linux not to be ignored" >&2
  exit 1
fi
if git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.windows-x64/probe"; then
  echo "Expected other/lib/mbedtls.windows-x64 not to be ignored" >&2
  exit 1
fi
