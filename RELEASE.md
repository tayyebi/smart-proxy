# Release Process

## Creating a Release

To create a new release that triggers builds for all platforms:

1. **Create and push a version tag:**
   ```bash
   git tag v1.0.0
   git push origin v1.0.0
   ```

2. **Or use GitHub Actions workflow dispatch:**
   - Go to Actions → Build and Release → Run workflow
   - Enter version tag (e.g., `v1.0.0`)
   - Click "Run workflow"

## Supported Platforms

The release workflow builds binaries for:

### Linux Variants
- **x86_64** (64-bit Intel/AMD)
- **i686** (32-bit Intel/AMD)
- **aarch64** (ARM64)
- **armv7** (ARMv7, Raspberry Pi 2/3)
- **armv6** (ARMv6, Raspberry Pi Zero/1)
- **mips64** (MIPS64)
- **ppc64le** (PowerPC64 Little Endian)
- **s390x** (IBM Z/LinuxONE mainframes)
- **riscv64** (RISC-V 64-bit)
- **loongarch64** (LoongArch 64-bit)

### Windows
- **x86_64** (64-bit, MSVC and MinGW)
- **i686** (32-bit, MSVC and MinGW)

### macOS
- **x86_64** (Intel Macs)
- **arm64** (Apple Silicon)

### FreeBSD
- **x86_64** (64-bit)
- **aarch64** (ARM64)
- **i686** (32-bit)

### Other BSD
- **OpenBSD**: x86_64, aarch64
- **NetBSD**: x86_64, aarch64
- **DragonFly BSD**: x86_64

### Android
- **arm64** (ARM64)
- **armv7** (ARMv7)
- **x86_64** (64-bit Intel)
- **i686** (32-bit Intel)

### Other Platforms
- **Solaris**: x86_64
- **Haiku**: x86_64

## Build Artifacts

Each release includes:
- Binaries for all supported platforms
- `checksums.txt` file with SHA256 checksums for verification

## Verification

To verify a downloaded binary:

```bash
# Download checksums.txt from release
sha256sum -c checksums.txt
```

Or manually:
```bash
sha256sum smartproxy-linux-x86_64
# Compare with value in checksums.txt
```

## Notes

- Some cross-compilation toolchains may not be available on GitHub Actions runners
- Failed builds will be skipped (empty files are filtered out)
- Android builds require NDK setup (currently skipped)
- Some exotic platforms may need manual toolchain installation

## Manual Builds

For platforms not covered by CI/CD, you can build manually:

```bash
# Set up cross-compilation toolchain
export CC=target-gcc
export CXX=target-g++
export CFLAGS="-march=target-arch"
export CXXFLAGS="-march=target-arch"

# Build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER=$CC \
  -DCMAKE_CXX_COMPILER=$CXX \
  -DCMAKE_SYSTEM_NAME=TargetOS \
  -DCMAKE_SYSTEM_PROCESSOR=target-arch
cmake --build . --config Release
```
