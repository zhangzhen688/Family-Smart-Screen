#!/bin/bash
# Build script for Smart Screen project.
# Usage: ./build.sh [clean]

set -e

BUILD_DIR="build"

if [ "${1:-}" = "clean" ]; then
    echo "==> Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

echo "==> Configuring CMake..."
cmake .. -DSIMULATOR_LINUX=ON -DCMAKE_BUILD_TYPE=Release

echo "==> Building ($(nproc) parallel jobs)..."
make -j"$(nproc)"

echo ""
echo "========================================="
echo " Build complete!"
echo "========================================="
echo " Executables:"
echo "   GUI:            build/gui/smart_gui"
echo "   Device Server:  build/backend/device_server  (port 1234)"
echo "   Camera Server:  build/backend/camera_server  (port 1235)"
echo "   Voice Server:   build/backend/voice_server   (port 1236)"
