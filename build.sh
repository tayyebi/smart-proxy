#!/bin/bash
# Build script for Smart Proxy Service

set -e

echo "Building Smart Proxy Service..."

# Check if Rust is installed
if ! command -v cargo &> /dev/null; then
    echo "Error: Rust/Cargo is not installed. Please install Rust from https://rustup.rs/"
    exit 1
fi

# Build release binaries
echo "Building release binaries..."
cargo build --release

# Check if build was successful
if [ $? -eq 0 ]; then
    echo ""
    echo "✓ Build complete!"
    echo ""
    echo "Binaries are located in:"
    echo "  - target/release/service"
    echo "  - target/release/cli"
    echo ""
    echo "To run the service:"
    echo "  ./target/release/service"
    echo ""
    echo "To use the CLI:"
    echo "  ./target/release/cli status"
else
    echo "Build failed!"
    exit 1
fi
