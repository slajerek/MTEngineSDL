#!/bin/bash
# Regenerates compressed C font arrays from source TTFs.
# Requires: clang++ (to build the conversion tool), curl, unzip
#
# Font sources (OFL 1.1 license - free for commercial use and binary embedding):
#   Inter v4.1:          https://github.com/rsms/inter/releases/tag/v4.1
#   JetBrains Mono v2.304: https://github.com/JetBrains/JetBrainsMono/releases/tag/v2.304
#
# Usage: ./generate_fonts.sh

set -e
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TOOL_SRC="$(cd "$SCRIPT_DIR/../../other/tools/DeployMaker" && pwd)/binary_to_compressed_c.cpp"
TOOL_BIN="/tmp/binary_to_compressed_c_gen"

echo "Building conversion tool..."
clang++ "$TOOL_SRC" -o "$TOOL_BIN"

TMP=/tmp/fonts_gen
mkdir -p "$TMP"

echo "Downloading Inter 4.1..."
curl -fsSL "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip" -o "$TMP/Inter-4.1.zip"
unzip -o "$TMP/Inter-4.1.zip" "extras/ttf/Inter-Regular.ttf" "extras/ttf/Inter-Bold.ttf" "extras/ttf/Inter-Italic.ttf" "extras/ttf/Inter-BoldItalic.ttf" -d "$TMP"

echo "Downloading JetBrains Mono 2.304..."
curl -fsSL "https://github.com/JetBrains/JetBrainsMono/releases/download/v2.304/JetBrainsMono-2.304.zip" -o "$TMP/JBM.zip"
unzip -o "$TMP/JBM.zip" "fonts/ttf/JetBrainsMono-Regular.ttf" -d "$TMP"

echo "Converting..."
"$TOOL_BIN" -nostatic "$TMP/extras/ttf/Inter-Regular.ttf"    font_inter_regular      > "$SCRIPT_DIR/font_inter_regular.cpp"
"$TOOL_BIN" -nostatic "$TMP/extras/ttf/Inter-Bold.ttf"       font_inter_bold         > "$SCRIPT_DIR/font_inter_bold.cpp"
"$TOOL_BIN" -nostatic "$TMP/extras/ttf/Inter-Italic.ttf"     font_inter_italic       > "$SCRIPT_DIR/font_inter_italic.cpp"
"$TOOL_BIN" -nostatic "$TMP/extras/ttf/Inter-BoldItalic.ttf" font_inter_bold_italic  > "$SCRIPT_DIR/font_inter_bold_italic.cpp"
"$TOOL_BIN" -nostatic "$TMP/fonts/ttf/JetBrainsMono-Regular.ttf" font_jetbrains_mono > "$SCRIPT_DIR/font_jetbrains_mono.cpp"

echo "Done. Files written to $SCRIPT_DIR"
