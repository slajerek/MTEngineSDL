#!/usr/bin/env bash
set -euo pipefail

# Master build script for MTEngineSDL on Linux.
# Builds all required libraries (mbedtls, uSockets, llama.cpp, FTXUI)
# and then compiles MTEngineSDL static library via CMake.
#
# Usage:
#   ./build-linux.sh              # Build everything
#   ./build-linux.sh --deps-only  # Build dependencies only (no CMake)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_ONLY=false

for arg in "$@"; do
  case "$arg" in
    --deps-only) DEPS_ONLY=true ;;
    *) echo "Unknown argument: $arg" >&2; exit 2 ;;
  esac
done

# Ensure git submodules are initialized
if [[ ! -f "$SCRIPT_DIR/other/lib/ftxui/CMakeLists.txt" ]] || \
   [[ ! -f "$SCRIPT_DIR/other/lib/mbedtls/CMakeLists.txt" ]] || \
   [[ ! -f "$SCRIPT_DIR/other/lib/llama.cpp/CMakeLists.txt" ]]; then
  echo -e "\e[94mInitializing git submodules\e[0m"
  cd "$SCRIPT_DIR"
  git submodule update --init --recursive
fi

echo -e "\e[94m=== Building MTEngineSDL dependencies ===\e[0m"

# 1. mbedTLS
echo -e "\n\e[94mBuilding \e[31mmbedTLS\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-mbedtls.sh"

# 2. uSockets
echo -e "\n\e[94mBuilding \e[31muSockets\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-usockets.sh"

# 3. FTXUI (built standalone; also built by CMake if MT_ENABLE_FTXUI=ON,
#    but pre-building ensures the lib is available for non-CMake workflows)
echo -e "\n\e[94mBuilding \e[31mFTXUI\e[0m"
bash "$SCRIPT_DIR/platform/Linux/build-ftxui.sh"

# 4. llama.cpp is built by CMake add_subdirectory, no separate script needed

if [[ "$DEPS_ONLY" == "true" ]]; then
  echo -e "\n\e[1;92mAll dependencies built. Skipping CMake (--deps-only).\e[0m"
  exit 0
fi

# 5. Build MTEngineSDL via CMake
echo -e "\n\e[94mBuilding \e[31mMTEngineSDL\e[0m"
mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"
cmake ../
make -j"$(nproc)" MTEngineSDL

echo -e "\n\e[1;92mMTEngineSDL built successfully.\e[0m"
