#!/bin/bash
# Build script for Smart Proxy Service (C++)

set -e

echo "Building Smart Proxy Service..."

# Check if CMake is installed
if ! command -v cmake &> /dev/null; then
    echo "Error: CMake is not installed. Please install CMake from https://cmake.org/"
    exit 1
fi

# Check if C++ compiler is installed
if ! command -v g++ &> /dev/null && ! command -v clang++ &> /dev/null; then
    echo "Error: C++ compiler (g++ or clang++) is not installed."
    exit 1
fi

# Create build directory
mkdir -p build
cd build

# Configure with CMake
echo "Configuring build..."
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build
echo "Building..."
cmake --build . --config Release -j$(nproc 2>/dev/null || echo 4)

# Check if build was successful
if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Build complete!"
    echo ""
    
    # Copy binary to project root for convenience
    if [ -f smartproxy ]; then
        cp smartproxy ../smartproxy
        chmod +x ../smartproxy
        echo "Binary is located in:"
        echo "  build/smartproxy"
        echo "  smartproxy (copied to project root)"
        echo ""
        echo "To run the service:"
        echo "  ./smartproxy"
        echo "  or"
        echo "  ./build/smartproxy"
    else
        echo "Binary is located in: build/smartproxy"
    fi
else
    echo "Build failed!"
    exit 1
fi
