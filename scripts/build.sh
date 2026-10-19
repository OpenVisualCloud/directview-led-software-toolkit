#!/bin/bash
# SPDX-License-Identifier: BSD-3-Clause
# Copyright 2026 Intel Corporation

# Build script for dvledtx and dvledrx
# Usage: bash scripts/build.sh [meson options]
#   e.g. bash scripts/build.sh -Denable_mtl_tx=true

set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"

echo "Building dvledtx and dvledrx..."

echo "Cleaning previous build..."
rm -rf build

echo "Setting up build directory..."
meson setup build "$@"

echo "Compiling applications..."
ninja -C build

test -x ./build/dvledtx/dvledtx
test -x ./build/dvledrx/dvledrx

echo "Build completed successfully!"
echo ""
echo "To run the applications:"
echo "  ./build/dvledtx/dvledtx --help"
echo "  ./build/dvledrx/dvledrx --help"
echo ""
