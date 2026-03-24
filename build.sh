#!/bin/bash

# Build script for TxApp

set -e

echo "Building TxApp..."

# Clean previous build
echo "Cleaning previous build..."
rm -rf build

# Setup build directory
echo "Setting up build directory..."
meson setup build

# Compile
echo "Compiling TxApp..."
ninja -C build

echo "Build completed successfully!"
echo ""
echo "To run the application:"
echo "  ./build/TxApp --help"
echo ""
echo "Example usage:"
echo "  ./build/TxApp --port 0000:af:01.0 --dip 239.168.85.20 --tx_url video.yuv"
echo ""