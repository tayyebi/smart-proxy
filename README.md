# Smart Proxy Service

A smart proxy service that routes traffic through optimal runways (interface + optional upstream proxy + DNS server + resolved IP) based on target accessibility, response times, and configured routing modes.

**Written in C++17 with zero external dependencies - only standard library!**

## Features

- **Multi-Protocol Support**: HTTP/HTTPS (CONNECT partially supported)
- **Intelligent Routing**: Latency-based, first-accessible, or round-robin modes
- **Learning System**: Tracks accessibility and performance per runway per target
- **User-Level Success Validation**: Measures actual usability, not just network connectivity
- **Automatic Health Checks**: Detects runway accessibility changes automatically
- **Edge Case Handling**: Comprehensive handling of network failures, DNS issues, and more
- **Zero Dependencies**: Pure C++17 standard library implementation
- **Cross-Platform**: Supports Linux/Unix (POSIX) and Windows
- **Defensive Terminal Handling**: Safe output that won't break terminals

## Requirements

- C++17 compatible compiler (GCC 7+, Clang 5+, MSVC 2017+)
- CMake 3.10 or higher
- Linux/Unix: Standard POSIX environment
- Windows: Visual Studio 2017+ or MinGW-w64

## Installation

### Linux/Unix

```bash
./build.sh
```

The binary will be in `build/smartproxy`.

### Windows

```batch
build.bat
```

The binary will be in `build\Release\smartproxy.exe`.

### Manual Build

```bash
mkdir build
cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Configuration

Edit `config.json` to configure:
- DNS servers
- Upstream proxies
- Network interfaces
- Routing mode
- Timeouts and limits

Example `config.json`:
```json
{
  "routing_mode": "latency",
  "dns_servers": [
    {"host": "8.8.8.8", "port": 53, "name": "Google DNS"},
    {"host": "1.1.1.1", "port": 53, "name": "Cloudflare DNS"}
  ],
  "upstream_proxies": [
    {"type": "http", "host": "proxy.example.com", "port": 8080}
  ],
  "interfaces": ["auto"],
  "proxy_listen_host": "127.0.0.1",
  "proxy_listen_port": 2123
}
```

## Usage

### Start the proxy service

Run without arguments to start the proxy server:

```bash
./smartproxy
```

Or on Windows:
```batch
smartproxy.exe
```

The service will:
- Discover available network interfaces
- Create runways (combinations of interfaces, proxies, and DNS servers)
- Start listening on the configured host/port (default: 127.0.0.1:2123)
- Begin health monitoring and routing optimization

### Use the CLI

The same binary provides a CLI interface when run with commands:

```bash
# Show help
./smartproxy

# Show current status
./smartproxy status

# List all runways
./smartproxy runways

# Show target accessibility matrix
./smartproxy targets

# Show performance statistics
./smartproxy stats

# Change routing mode
./smartproxy mode latency
./smartproxy mode first_accessible
./smartproxy mode round_robin

# Test target accessibility
./smartproxy test example.com
./smartproxy test example.com runway_id

# Reload configuration
./smartproxy reload

# JSON output
./smartproxy --json status
./smartproxy --json runways
./smartproxy --json targets
./smartproxy --json stats
```

**Note**: The CLI commands work independently - you don't need the service running to use them. They initialize the components to read configuration and display information.

### Configure your application

Set your application's HTTP proxy to:
```
http://127.0.0.1:2123
```

## Architecture

The system learns from traffic patterns and automatically adapts to network conditions, handling:
- Internal/private network targets
- Intermittent accessibility
- Target-specific runway requirements
- Multiple accessible runways with different latencies
- Network vs user-level success validation

### Components

- **DNS Resolver**: Manual DNS packet construction/parsing (RFC 1035)
- **Runway Manager**: Discovers and manages network interfaces and runway combinations
- **Routing Engine**: Selects optimal runway based on routing mode (latency/first/round-robin)
- **Accessibility Tracker**: Tracks success rates and performance metrics per target-runway pair
- **Proxy Server**: HTTP proxy server (RFC 7230, 7231) that routes requests through selected runways
- **Health Monitor**: Background health checks to detect runway accessibility changes

## RFC Compliance

This implementation follows relevant RFCs with inline comments:

- **RFC 1035** - Domain Names - Implementation and Specification (DNS)
- **RFC 7159** - The JavaScript Object Notation (JSON) Data Interchange Format
- **RFC 7230** - HTTP/1.1 Message Syntax and Routing
- **RFC 7231** - HTTP/1.1 Semantics and Content
- **RFC 793** - Transmission Control Protocol (TCP)
- **RFC 1918** - Address Allocation for Private Internets (private IP detection)

## Defensive Coding

The implementation includes defensive coding practices:

- **Input Validation**: All inputs validated before use
- **Bounds Checking**: Array/string bounds checked
- **Null Checks**: All pointers checked before dereference
- **Resource Cleanup**: RAII patterns, proper cleanup on errors
- **Terminal Safety**: Checks terminal state, flushes output, avoids control characters
- **Signal Handling**: Proper SIGINT/SIGTERM handling, cleanup on exit
- **Error Recovery**: Graceful degradation, continues operation on non-fatal errors
- **Memory Safety**: Smart pointers where possible, manual memory management with checks

## Performance

The C++ implementation provides:
- **Low latency**: Efficient socket I/O with select()/poll()
- **High throughput**: Optimized for concurrent connections
- **Memory efficiency**: Minimal allocations, smart pointer usage
- **Small binary size**: No external dependencies

## Troubleshooting

### Build issues

If you encounter build errors:
1. Ensure C++17 compiler is available: `g++ --version` or `clang++ --version`
2. Ensure CMake is installed: `cmake --version`
3. Clean and rebuild: `rm -rf build && ./build.sh`

### Runtime issues

- Check that port 2123 (or configured port) is not in use
- Verify `config.json` is valid JSON
- Ensure network interfaces are available
- Check firewall settings for the proxy port
- On Linux, may need root privileges for interface enumeration

### Terminal issues

The implementation includes defensive terminal handling:
- Checks if output is to a terminal before writing
- Flushes output buffers appropriately
- Avoids control characters that could corrupt terminal state
- Handles terminal resize signals gracefully

## License

[Add your license here]