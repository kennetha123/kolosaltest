#!/bin/bash
set -e  # exit if anything fails

# Change to script directory
cd "$(dirname "$0")"

# Build folder
BUILD_DIR=build

# Ensure vcpkg is cloned if not already
if [ ! -d "vcpkg" ]; then
  echo "Cloning vcpkg..."
  git clone https://github.com/microsoft/vcpkg.git
  ./vcpkg/bootstrap-vcpkg.sh
fi

# Toolchain file path
VCPKG_TOOLCHAIN_FILE="$(pwd)/vcpkg/scripts/buildsystems/vcpkg.cmake"

# Create build folder if not exists
mkdir -p "$BUILD_DIR"

# Run CMake (default generator = Unix Makefiles or Ninja if installed)
cmake -B "$BUILD_DIR" -S . -DCMAKE_TOOLCHAIN_FILE="$VCPKG_TOOLCHAIN_FILE"

# Build worker and server (use all CPU cores)
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo "Build complete. Executables should be in $BUILD_DIR/"
