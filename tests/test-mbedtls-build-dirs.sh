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

check_file_contains "$ROOT_DIR/platform/MacOS/build-mbedtls.sh" 'BUILD_DIR="$ROOT_DIR/other/lib/mbedtls.macos"'
check_file_contains "$ROOT_DIR/platform/Linux/build-mbedtls.sh" 'BUILD_DIR="$ROOT_DIR/other/lib/mbedtls.linux"'
check_file_contains "$ROOT_DIR/platform/Windows/build-mbedtls.ps1" 'Join-Path $repoRoot "other\lib\mbedtls.windows-$Platform"'

check_file_not_contains "$ROOT_DIR/platform/MacOS/build-mbedtls.sh" 'BUILD_DIR="$MBEDTLS_SRC_DIR/build-macos"'
check_file_not_contains "$ROOT_DIR/platform/Linux/build-mbedtls.sh" 'BUILD_DIR="$MBEDTLS_SRC_DIR/build-linux"'
check_file_not_contains "$ROOT_DIR/platform/Windows/build-mbedtls.ps1" "Join-Path \$mbedSrc 'build-windows'"

git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.macos/probe"
git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.linux/probe"
git -C "$ROOT_DIR" check-ignore -q "other/lib/mbedtls.windows-x64/probe"
