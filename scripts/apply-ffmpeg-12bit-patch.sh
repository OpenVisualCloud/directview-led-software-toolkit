#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation
#
# apply-ffmpeg-12bit-patch.sh — Apply the 12-bit pixel format patch to
# the FFmpeg mtl_st20p TX plugin, then rebuild and install FFmpeg.
#
# Usage:
#   ./scripts/apply-ffmpeg-12bit-patch.sh [FFMPEG_SRC_DIR]
#
# FFMPEG_SRC_DIR defaults to ~/ffmpeg_build/FFmpeg if not specified.
# The MTL plugin source (mtl_st20p_tx.c) must already be copied into
# FFmpeg's libavdevice/ directory before running this script.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PATCH_FILE="$SCRIPT_DIR/../patches/ffmpeg-mtl-st20p-12bit-formats.patch"
FFMPEG_DIR="${1:-$HOME/ffmpeg_build/FFmpeg}"

if [[ ! -f "$PATCH_FILE" ]]; then
    echo "ERROR: Patch file not found: $PATCH_FILE"
    exit 1
fi

if [[ ! -d "$FFMPEG_DIR/libavdevice" ]]; then
    echo "ERROR: FFmpeg source not found at: $FFMPEG_DIR"
    echo "Usage: $0 [FFMPEG_SRC_DIR]"
    exit 1
fi

TARGET="$FFMPEG_DIR/libavdevice/mtl_st20p_tx.c"
if [[ ! -f "$TARGET" ]]; then
    echo "ERROR: mtl_st20p_tx.c not found in $FFMPEG_DIR/libavdevice/"
    echo "Ensure the MTL FFmpeg plugin has been copied into the FFmpeg source tree."
    exit 1
fi

# Check if patch is already applied
if grep -q "AV_PIX_FMT_YUV444P12LE" "$TARGET"; then
    echo "Patch already applied — skipping."
    exit 0
fi

echo "Applying 12-bit format patch to: $TARGET"
cd "$FFMPEG_DIR"
patch -p1 < "$PATCH_FILE"

echo ""
echo "Patch applied successfully."
echo "Rebuild FFmpeg with:"
echo "  cd $FFMPEG_DIR && make -j\$(nproc) && sudo make install && sudo ldconfig"
