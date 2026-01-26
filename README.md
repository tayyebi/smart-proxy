# Smart Proxy

A high-performance, multi-interface SOCKS5 proxy with optional upstream proxies, latency-based selection, and optional authentication. Supports **TCP and UDP**, automatic runway health checks, and a live CLI for monitoring.

---

## Features

* SOCKS5 TCP and UDP proxy support
* Optional username/password authentication
* Automatic interface and upstream proxy detection
* Periodic runway health checks with latency-based selection
* Live CLI for logs, runway status, and latency metrics
* Async-safe relay and probes
* Graceful shutdown and signal handling

---

## Requirements

* **Python 3.11+**
* Linux-based system (tested on Debian/Ubuntu)
* Optional: systemd for journal logging
* Network access to at least one upstream proxy or external host for testing

---

## Installation

1. **Update package index and install dependencies**:

```bash
sudo apt update
sudo apt install -y python3 python3-pip python3-venv python3-dev build-essential
```

2. **Install required Python modules**:

```bash
pip3 install --user systemd-python
```

> `systemd-python` is optional. If not installed, logging will still work to console.

3. **Clone or copy the Smart Proxy project**:

```bash
git clone <your-repo-url> smart-proxy
cd smart-proxy
```

4. **Ensure `smart_proxy.py` and `proxy_config.json` are in the same directory**.

---

## Configuration

Edit `proxy_config.json` to customize:

```json
{
  "upstream_proxies": [
    {"host": "127.0.0.1", "port": 10808}
  ],
  "probe_interval": 10,
  "tcp_timeout": 1.5,
  "selection_mode": "latency",
  "auth": {
    "enabled": true,
    "users": {
      "admin": "securepassword"
    }
  }
}
```

* **upstream_proxies**: List of upstream proxies to route traffic through.
* **probe_interval**: Seconds between automatic runway health checks.
* **tcp_timeout**: Timeout for TCP/proxy probes.
* **selection_mode**: `"first_available"` or `"latency"` for runway selection.
* **auth**: Enable SOCKS5 username/password authentication.

---

## Running the Proxy

Start the proxy:

```bash
python3 smart_proxy.py
```

The CLI will be available immediately:

* `show_runways` – Display live status of all runways
* `show_logs` – View recent logs
* `show_latency` – Show measured latency for all targets
* `exit` – Stop the proxy gracefully

---

## Example SOCKS5 Client Configuration

* **Host:** `127.0.0.1`
* **Port:** `1080`
* **Authentication:** Username/password from `proxy_config.json` (if enabled)

---

## Notes

* Ensure at least one interface or upstream proxy is reachable to see runways as `UP`.
* For testing, you can use Google DNS `8.8.8.8:53` or any reachable proxy/server.
* UDP support is minimal; only direct UDP forwarding via SOCKS5 UDP ASSOCIATE is implemented.

---

## License

MIT License – free to use, modify, and distribute.
