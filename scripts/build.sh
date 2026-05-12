#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Build script for dvledtx
# Usage: bash scripts/build.sh [meson options]
#   e.g. bash scripts/build.sh -Denable_mtl_tx=true

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

echo "Building dvledtx..."

echo "Cleaning previous build..."
rm -rf build

echo "Setting up build directory..."
meson setup build "$@"

echo "Compiling dvledtx..."
ninja -C build

echo "Build completed successfully!"
echo ""
echo "To run the application:"
echo "  ./build/dvledtx --help"
echo ""
