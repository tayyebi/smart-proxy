#!/usr/bin/env python3
import socket, threading, time, os, struct, fcntl, logging, json, sys, signal
from cmd import Cmd
import dns.resolver

# -----------------------------
# LOGGING
# -----------------------------
logging.basicConfig(level=logging.INFO,
                    format="%(asctime)s [%(levelname)s] %(message)s")

# -----------------------------
# CONFIGURATION
# -----------------------------
CONFIG_FILE = "proxy_config.json"

DEFAULT_CONFIG = {
    "dns_servers": [
        "1.1.1.1", "8.8.8.8", "2.189.44.44", "2.188.21.130",
        "194.225.62.80", "80.191.40.41", "213.176.123.5",
        "172.29.2.100", "172.29.0.100",
        "185.55.226.26", "185.55.225.25", "185.55.224.24",
        "178.22.122.100", "185.51.200.2"
    ],
    "upstream_proxies": [
        {"host": "127.0.0.1", "port": 10808}
    ]
}

def load_config():
    try:
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    except:
        save_config(DEFAULT_CONFIG)
        return DEFAULT_CONFIG

def save_config(config):
    with open(CONFIG_FILE, "w") as f:
        json.dump(config, f, indent=2)

config = load_config()

# -----------------------------
# IN-MEMORY CACHE
# -----------------------------
dns_cache = {}
latency_cache = {}  # {(host, port): (latency, iface, ip, upstream)}

# -----------------------------
# NETWORK HELPERS
# -----------------------------
def get_interfaces():
    ifaces = []
    for i in os.listdir("/sys/class/net"):
        if i == "lo": continue
        try:
            with open(f"/sys/class/net/{i}/operstate") as f:
                if f.read().strip() == "up":
                    ifaces.append(i)
        except: pass
    return ifaces

def get_iface_ip(iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        return socket.inet_ntoa(
            fcntl.ioctl(
                s.fileno(), 0x8915,
                struct.pack("256s", iface[:15].encode())
            )[20:24]
        )
    except: return None

# -----------------------------
# DNS RESOLUTION
# -----------------------------
def resolve_domain(domain):
    if domain in dns_cache:
        return dns_cache[domain]
    ips = set()
    for dns_ip in config.get("dns_servers", []):
        try:
            r = dns.resolver.Resolver(configure=False)
            r.nameservers = [dns_ip]
            r.timeout = 1
            r.lifetime = 1
            for answer in r.resolve(domain, "A"):
                ips.add(answer.address)
        except: pass
    dns_cache[domain] = list(ips)
    return list(ips)

# -----------------------------
# TCP / PROXY PROBES
# -----------------------------
TCP_TIMEOUT = 1.5
BUFFER_SIZE = 65536

def tcp_probe(ip, port, iface, results):
    try:
        src_ip = get_iface_ip(iface)
        if not src_ip: return
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(TCP_TIMEOUT)
        s.bind((src_ip, 0))
        start = time.time()
        s.connect((ip, port))
        results.append((time.time()-start, iface, ip, None))
    except: pass
    finally:
        try: s.close()
        except: pass

def proxy_probe(proxy_host, proxy_port, target_host, target_port, results):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(TCP_TIMEOUT)
        start = time.time()
        s.connect((proxy_host, proxy_port))
        s.sendall(f"CONNECT {target_host}:{target_port} HTTP/1.1\r\nHost: {target_host}\r\n\r\n".encode())
        resp = s.recv(4096)
        if b"200" in resp:
            results.append((time.time()-start, None, target_host, (proxy_host, proxy_port)))
        s.close()
    except: pass

# -----------------------------
# PATH SELECTION
# -----------------------------
MODES = ["first_match", "lowest_latency"]
current_mode = "lowest_latency"

def select_fastest_path(host, port):
    cache_key = (host, port)
    if cache_key in latency_cache:
        return latency_cache[cache_key]

    ips = resolve_domain(host)
    if not ips:
        logging.warning(f"No IPs resolved for {host}")
        return None

    ifaces = get_interfaces()
    threads = []
    results = []

    for ip in ips:
        for iface in ifaces:
            t = threading.Thread(target=tcp_probe, args=(ip, port, iface, results))
            t.start()
            threads.append(t)

    for up in config.get("upstream_proxies", []):
        t = threading.Thread(target=proxy_probe, args=(up["host"], up["port"], host, port, results))
        t.start()
        threads.append(t)

    for t in threads: t.join()

    if not results: return None
    best = results[0] if current_mode=="first_match" else min(results, key=lambda x:x[0])
    latency_cache[cache_key] = best
    return best

# -----------------------------
# RELAY
# -----------------------------
def relay(a, b):
    try:
        while True:
            data = a.recv(BUFFER_SIZE)
            if not data: break
            b.sendall(data)
    except: pass
    finally:
        try: a.close()
        except: pass
        try: b.close()
        except: pass

# -----------------------------
# CLIENT HANDLER
# -----------------------------
def handle_client(client):
    try:
        req = client.recv(4096).decode(errors="ignore")
        if not req.startswith("CONNECT"):
            client.close()
            return
        host, port = req.split()[1].split(":")
        port = int(port)
        best = select_fastest_path(host, port)
        if not best:
            client.close()
            return
        latency, iface, ip, upstream = best
        if upstream:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(upstream)
            s.sendall(f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
            resp = s.recv(4096)
            if b"200" not in resp:
                client.close()
                return
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            threading.Thread(target=relay, args=(client, s), daemon=True).start()
            threading.Thread(target=relay, args=(s, client), daemon=True).start()
        else:
            src_ip = get_iface_ip(iface)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            upstream.bind((src_ip,0))
            upstream.connect((ip, port))
            threading.Thread(target=relay, args=(client, upstream), daemon=True).start()
            threading.Thread(target=relay, args=(upstream, client), daemon=True).start()
    except: 
        try: client.close()
        except: pass

# -----------------------------
# PROXY SERVER
# -----------------------------
class ProxyServer(threading.Thread):
    def __init__(self, bind_ip="127.0.0.1", port=3128):
        super().__init__(daemon=True)
        self.bind_ip = bind_ip
        self.port = port
        self.running = True
        self.sock = None

    def run(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)  # allow immediate reuse
        self.sock.bind((self.bind_ip, self.port))
        self.sock.listen(1024)
        logging.info(f"Smart Proxy running on {self.bind_ip}:{self.port}")
        while self.running:
            try:
                self.sock.settimeout(1.0)
                client, _ = self.sock.accept()
                threading.Thread(target=handle_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    logging.exception(e)

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
                logging.info("Proxy socket closed")
            except:
                pass

# -----------------------------
# CLI
# -----------------------------
# -----------------------------
# CLI
# -----------------------------
class ProxyCLI(Cmd):
    intro = "Proxy CLI - type help or ?"
    prompt = "(proxy) "

    # --- DNS cache commands ---
    def do_show_dns(self, arg):
        "Show in-memory DNS cache: show_dns"
        if not dns_cache:
            print("DNS cache is empty")
            return
        for domain, ips in dns_cache.items():
            print(f"{domain}: {ips}")

    def do_purge_dns(self, domain):
        "Purge DNS cache for a domain: purge_dns example.com"
        if domain in dns_cache:
            del dns_cache[domain]
            print(f"Purged DNS cache for {domain}")
        else:
            print(f"No cached entry for {domain}")

    def do_resolve(self, domain):
        "Force DNS resolution and cache result: resolve example.com"
        ips = resolve_domain(domain)
        print(f"{domain}: {ips}")

    # --- Latency cache commands ---
    def do_show_latency(self, arg):
        "Show latency cache: show_latency"
        if not latency_cache:
            print("Latency cache is empty")
            return
        for k,v in latency_cache.items():
            print(f"{k}: {v}")

    def do_purge_latency(self, arg):
        "Purge all latency cache: purge_latency"
        latency_cache.clear()
        print("Cleared latency cache")

    # --- Proxy modes ---
    def do_set_mode(self, mode):
        "Set selection mode (first_match / lowest_latency): set_mode lowest_latency"
        global current_mode
        if mode in MODES:
            current_mode = mode
            print(f"Mode set to {mode}")
        else:
            print(f"Invalid mode. Available: {MODES}")

    # --- DNS server management ---
    def do_add_dns(self, ip):
        "Add a DNS server to config: add_dns 8.8.8.8"
        if ip and ip not in config["dns_servers"]:
            config["dns_servers"].append(ip)
            save_config(config)
            print(f"Added DNS server {ip}")
        else:
            print(f"{ip} already exists or invalid")

    def do_remove_dns(self, ip):
        "Remove a DNS server from config: remove_dns 8.8.8.8"
        if ip in config["dns_servers"]:
            config["dns_servers"].remove(ip)
            save_config(config)
            print(f"Removed DNS server {ip}")
        else:
            print(f"{ip} not found in config")

    def do_show_dns_servers(self, arg):
        "Show DNS servers in config: show_dns_servers"
        print(config.get("dns_servers", []))

    # --- Upstream proxy management ---
    def do_add_upstream(self, arg):
        "Add upstream proxy: add_upstream 1.2.3.4 1080"
        try:
            host, port = arg.split()
            port = int(port)
            entry = {"host": host, "port": port}
            if entry not in config["upstream_proxies"]:
                config["upstream_proxies"].append(entry)
                save_config(config)
                print(f"Added upstream proxy {entry}")
            else:
                print("Upstream already exists")
        except:
            print("Usage: add_upstream host port")

    def do_remove_upstream(self, arg):
        "Remove upstream proxy: remove_upstream 1.2.3.4 1080"
        try:
            host, port = arg.split()
            port = int(port)
            entry = {"host": host, "port": port}
            if entry in config["upstream_proxies"]:
                config["upstream_proxies"].remove(entry)
                save_config(config)
                print(f"Removed upstream proxy {entry}")
            else:
                print("Upstream not found")
        except:
            print("Usage: remove_upstream host port")

    def do_show_upstreams(self, arg):
        "Show upstream proxies in config: show_upstreams"
        for up in config.get("upstream_proxies", []):
            print(up)

    # --- General commands ---
    def do_show_config(self, arg):
        "Show entire JSON configuration: show_config"
        print(json.dumps(config, indent=2))

    def do_exit(self, arg):
        "Exit CLI"
        print("Exiting CLI.")
        return True

# -----------------------------
# MAIN
# -----------------------------
def main():
    proxy = ProxyServer()
    proxy.start()

    # Handle Ctrl+C globally
    def signal_handler(sig, frame):
        print("\nStopping proxy...")
        proxy.stop()
        sys.exit(0)
    signal.signal(signal.SIGINT, signal_handler)

    ProxyCLI().cmdloop()

if __name__=="__main__":
    main()
