#!/usr/bin/env bash
set -euo pipefail

# Kept for anyone with this path in muscle memory. The real script is at the
# repository root: ../../build-macos.sh
#
# This file used to be a bare `xcodebuild -project ... -configuration Release`
# with no -scheme, which skips scheme pre-actions and loses ONLY_ACTIVE_ARCH,
# so it built every architecture in ARCHS. Delegating means there is one macOS
# build path to keep correct instead of two.

exec "$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/build-macos.sh" "$@"
