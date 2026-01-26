# Smart Proxy Service

A smart proxy service that routes traffic through optimal runways (interface + optional upstream proxy + DNS server + resolved IP) based on target accessibility, response times, and configured routing modes.

**Now written in Rust for high performance and reliability!**

## Features

- **Multi-Protocol Support**: HTTP/HTTPS/FTP/SOCKS4/SOCKS5
- **Intelligent Routing**: Latency-based, first-accessible, or round-robin modes
- **Learning System**: Tracks accessibility and performance per runway per target
- **User-Level Success Validation**: Measures actual usability, not just network connectivity
- **Automatic Health Checks**: Detects runway accessibility changes automatically
- **Edge Case Handling**: Comprehensive handling of network failures, DNS issues, and more

## Requirements

- Rust 1.70+ (install from [rustup.rs](https://rustup.rs/))
- Linux/Unix system with network interfaces

## Installation

### Build the binaries

```bash
./build.sh
```

This will build two binaries:
- `target/release/service` - The main proxy service
- `target/release/cli` - The CLI management tool

### Manual build

```bash
cargo build --release
```

The binaries will be in `target/release/`:
- `service` - Main proxy service binary
- `cli` - CLI management tool binary

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

```bash
./target/release/service
```

Or if you've added it to your PATH:
```bash
service
```

The service will:
- Discover available network interfaces
- Create runways (combinations of interfaces, proxies, and DNS servers)
- Start listening on the configured host/port (default: 127.0.0.1:2123)
- Begin health monitoring and routing optimization

### Use the CLI for monitoring and management

```bash
./target/release/cli <command>
```

Or:
```bash
cli <command>
```

## CLI Commands

- `status` - Show current status
- `runways` - List all runways with metrics
- `targets` - Show target accessibility matrix
- `mode <mode>` - Switch routing mode (latency/first_accessible/round_robin)
- `test <target> [runway_id]` - Test target accessibility
- `reload` - Reload configuration
- `stats` - Show performance statistics

### Examples

```bash
# Show status
./target/release/cli status

# List all runways
./target/release/cli runways

# Test a target
./target/release/cli test example.com

# Test specific runway
./target/release/cli test example.com direct_eth0_8.8.8.8_0

# Change routing mode
./target/release/cli mode latency

# JSON output
./target/release/cli --json status
```

## Architecture

The system learns from traffic patterns and automatically adapts to network conditions, handling:
- Internal/private network targets
- Intermittent accessibility
- Target-specific runway requirements
- Multiple accessible runways with different latencies
- Network vs user-level success validation

### Components

- **DNS Resolver**: Resolves domain names using configured DNS servers with caching
- **Runway Manager**: Discovers and manages network interfaces and runway combinations
- **Routing Engine**: Selects optimal runway based on routing mode (latency/first/round-robin)
- **Accessibility Tracker**: Tracks success rates and performance metrics per target-runway pair
- **Proxy Server**: HTTP proxy server that routes requests through selected runways
- **Health Monitor**: Background health checks to detect runway accessibility changes

## Performance

The Rust implementation provides:
- **Low latency**: Efficient async runtime with Tokio
- **High throughput**: Optimized for concurrent connections
- **Memory safety**: Rust's ownership system prevents common bugs
- **Small binary size**: Optimized release builds

## Troubleshooting

### Build issues

If you encounter build errors:
1. Ensure Rust is up to date: `rustup update`
2. Clean and rebuild: `cargo clean && cargo build --release`

### Runtime issues

- Check logs for error messages
- Verify `config.json` is valid JSON
- Ensure network interfaces are available
- Check firewall settings for the proxy port

## License

[Add your license here]
