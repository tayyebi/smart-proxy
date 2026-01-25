#!/usr/bin/env python3
import asyncio, socket, os, struct, fcntl, logging, json, sys, signal, threading, time
from cmd import Cmd
from collections import deque, defaultdict
import dns.asyncresolver

# -----------------------------
# LOGGING
# -----------------------------
LOG_QUEUE = deque(maxlen=1000)

class QueueHandler(logging.Handler):
    def emit(self, record):
        msg = self.format(record)
        LOG_QUEUE.append(msg)

logger = logging.getLogger()
logger.setLevel(logging.INFO)
formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
queue_handler = QueueHandler()
queue_handler.setFormatter(formatter)
logger.addHandler(queue_handler)

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
    "upstream_proxies": [{"host":"127.0.0.1","port":10808}],
    "probe_interval": 10,
    "tcp_timeout": 1.5,
    "selection_mode": "first_available"  # first available by default
}

def load_config():
    try:
        with open(CONFIG_FILE, "r") as f:
            return json.load(f)
    except:
        save_config(DEFAULT_CONFIG)
        return DEFAULT_CONFIG

def save_config(cfg):
    with open(CONFIG_FILE, "w") as f:
        json.dump(cfg, f, indent=2)

config = load_config()

# -----------------------------
# STATE
# -----------------------------
runways = {}  # (iface, proxy_host, proxy_port, dns) -> {"status": bool, "last": t, "latency": float}
latency_records = defaultdict(dict)  # domain -> runway -> latency
dns_cache = {}  # domain -> list of IPs
prev_status = {}

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
            fcntl.ioctl(s.fileno(), 0x8915, struct.pack("256s", iface[:15].encode()))[20:24]
        )
    except: return None

# -----------------------------
# ASYNC PROBES
# -----------------------------
async def resolve_domain(domain, dns_ip):
    try:
        resolver = dns.asyncresolver.Resolver(configure=False)
        resolver.nameservers = [dns_ip]
        resolver.lifetime = 1
        resolver.timeout = 1
        ans = await resolver.resolve(domain, "A")
        return [a.address for a in ans]
    except:
        return []

async def tcp_probe(ip, port, iface, timeout=1.5):
    loop = asyncio.get_event_loop()
    src_ip = get_iface_ip(iface)
    if not src_ip: return None
    return await loop.run_in_executor(None, lambda: sync_tcp_connect(ip, port, src_ip, timeout))

def sync_tcp_connect(ip, port, src_ip, timeout):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.bind((src_ip, 0))
        start = time.time()
        s.connect((ip, port))
        s.close()
        return time.time() - start
    except:
        return None

async def proxy_probe(proxy_host, proxy_port, target_host, target_port, timeout=1.5):
    loop = asyncio.get_event_loop()
    return await loop.run_in_executor(None, lambda: sync_proxy_connect(proxy_host, proxy_port, target_host, target_port, timeout))

def sync_proxy_connect(proxy_host, proxy_port, target_host, target_port, timeout):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        start = time.time()
        s.connect((proxy_host, proxy_port))
        s.sendall(f"CONNECT {target_host}:{target_port} HTTP/1.1\r\nHost: {target_host}\r\n\r\n".encode())
        resp = s.recv(4096)
        s.close()
        if b"200" in resp: return time.time() - start
        return None
    except: return None

async def probe_runway(iface, proxy_host, proxy_port, dns_ip):
    first_dns = config["dns_servers"][0] if config.get("dns_servers") else "8.8.8.8"
    latency = None
    if proxy_host:
        latency = await proxy_probe(proxy_host, proxy_port, "example.com", 80)
    else:
        latency = await tcp_probe(first_dns, 80, iface)
    key = (iface, proxy_host, proxy_port, dns_ip)
    status = latency is not None
    runways[key] = {"status": status, "last": time.time(), "latency": latency}
    if prev_status.get(key) != status:
        prev_status[key] = status
        logger.info(f"Runway {key} status changed: {'UP' if status else 'DOWN'}")

async def monitor_runways(domains=[]):
    while True:
        ifaces = get_interfaces()
        tasks = []
        for dns_ip in config.get("dns_servers", []):
            for iface in ifaces:
                tasks.append(probe_runway(iface, None, None, dns_ip))
                for up in config.get("upstream_proxies", []):
                    tasks.append(probe_runway(iface, up["host"], up["port"], dns_ip))
        await asyncio.gather(*tasks)
        # Update per-domain latency
        for domain in domains:
            is_ip = False
            try: socket.inet_aton(domain); is_ip=True
            except: pass
            for rkey in runways:
                ips = []
                if not is_ip:
                    ips = await resolve_domain(domain, rkey[3])
                    dns_cache[domain] = ips
                ip = domain if is_ip else (ips[0] if ips else config["dns_servers"][0])
                iface, proxy_host, proxy_port, dns_ip = rkey
                if proxy_host:
                    latency = await proxy_probe(proxy_host, proxy_port, domain, 80)
                else:
                    latency = await tcp_probe(ip, 80, iface)
                if latency: latency_records[domain][rkey] = latency
        await asyncio.sleep(config.get("probe_interval",10))

# -----------------------------
# RUNWAY SELECTION
# -----------------------------
def select_runway_for_domain(domain):
    is_ip = False
    try: socket.inet_aton(domain); is_ip=True
    except: pass
    if is_ip:
        for rkey, v in runways.items():
            if v["status"]:
                return rkey
        return None
    if config.get("selection_mode")=="first_available":
        for rkey, v in runways.items():
            if v["status"]:
                return rkey
    elif config.get("selection_mode")=="lowest_latency" and domain in latency_records:
        rdict = latency_records[domain]
        if rdict:
            return min(rdict, key=lambda k: rdict[k])
    return None

# -----------------------------
# RELAY
# -----------------------------
BUFFER_SIZE = 65536
async def relay(reader, writer):
    try:
        while True:
            data = await reader.read(BUFFER_SIZE)
            if not data: break
            writer.write(data)
            await writer.drain()
    except: pass
    finally:
        try: writer.close()
        except: pass

# -----------------------------
# PROXY SERVER
# -----------------------------
class ProxyServer:
    def __init__(self, bind_ip="127.0.0.1", port=3128):
        self.bind_ip = bind_ip
        self.port = port
        self.server = None

    async def handle_client(self, reader, writer):
        try:
            data = await reader.read(4096)
            req = data.decode(errors="ignore")
            if not req.startswith("CONNECT"):
                writer.close()
                return
            host, port = req.split()[1].split(":")
            port = int(port)
            runway_key = select_runway_for_domain(host)
            if not runway_key:
                writer.close()
                return
            iface, proxy_host, proxy_port, dns_ip = runway_key
            is_ip = False
            try: socket.inet_aton(host); is_ip=True
            except: pass
            ips = []
            if not is_ip: ips = dns_cache.get(host, [])
            ip = host if is_ip else (ips[0] if ips else config["dns_servers"][0])

            if proxy_host:
                r_reader, r_writer = await asyncio.open_connection(proxy_host, proxy_port)
                r_writer.write(f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
                await r_writer.drain()
                resp = await r_reader.read(4096)
                if b"200" not in resp:
                    writer.close()
                    return
                writer.write(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                await writer.drain()
                await asyncio.gather(relay(reader, r_writer), relay(r_reader, writer))
            else:
                conn_reader, conn_writer = await asyncio.open_connection(ip, port)
                writer.write(b"HTTP/1.1 200 Connection Established\r\n\r\n")
                await writer.drain()
                await asyncio.gather(relay(reader, conn_writer), relay(conn_reader, writer))
        except:
            try: writer.close()
            except: pass

    async def start(self):
        self.server = await asyncio.start_server(self.handle_client, self.bind_ip, self.port)
        await self.server.serve_forever()

    async def stop(self):
        if self.server:
            self.server.close()
            await self.server.wait_closed()

# -----------------------------
# CLI
# -----------------------------
class ProxyCLI(Cmd):
    intro="Smart Proxy CLI. Type help or ?"
    prompt="(proxy) "

    def do_show_logs(self,arg):
        """show_logs [num] - Live show last N log lines (default 100)"""
        try: n = int(arg) if arg else 100
        except: n = 100
        last_index = max(len(LOG_QUEUE)-n,0)
        print("\nPress Ctrl+C to stop live logs\n")
        try:
            while True:
                logs = list(LOG_QUEUE)
                new_logs = logs[last_index:]
                if new_logs:
                    for line in new_logs:
                        print(line)
                    last_index = len(logs)
                time.sleep(1)
        except KeyboardInterrupt:
            print("\nStopped live logs\n")

    def do_show_runways(self,arg):
        """show_runways - Display all runways and their status"""
        for k,v in runways.items():
            print(f"{k}: {'UP' if v['status'] else 'DOWN'} latency:{v['latency']} last:{time.ctime(v['last'])}")

    def do_show_latency(self,arg):
        """show_latency [domain] - Display latency records for a domain (all if omitted)"""
        if arg:
            domain = arg.strip()
            if domain in latency_records:
                for k,v in latency_records[domain].items():
                    print(f"{domain} via {k}: {v:.3f}s")
        else:
            for domain,rdict in latency_records.items():
                for k,v in rdict.items():
                    print(f"{domain} via {k}: {v:.3f}s")

    def do_live_monitor(self,arg):
        """live_monitor - Show live runway status with green/red indicators"""
        try:
            while True:
                os.system("clear")
                now = time.time()
                print("RUNWAY STATUS (🟢=UP, 🔴=DOWN)")
                for k,v in runways.items():
                    circle = "🟢" if v["status"] else "🔴"
                    next_update = int(v["last"]+config.get("probe_interval",10)-now)
                    print(f"{circle} {k} last:{int(now-v['last'])}s next:{next_update}s latency:{v['latency']}")
                print("\nPress Ctrl+C to exit live monitor")
                time.sleep(1)
        except KeyboardInterrupt:
            pass

    def do_exit(self,arg):
        """exit - Exit CLI"""
        return True

    def do_help(self,arg):
        """help - Show commands"""
        Cmd.do_help(self,arg)

# -----------------------------
# MAIN
# -----------------------------
def start_cli():
    ProxyCLI().cmdloop()

async def main():
    proxy = ProxyServer()
    domains=[]
    loop = asyncio.get_event_loop()
    loop.create_task(monitor_runways(domains))
    loop.create_task(proxy.start())
    # Run CLI in separate thread to avoid blocking
    t = threading.Thread(target=start_cli, daemon=True)
    t.start()
    while t.is_alive():
        await asyncio.sleep(1)

if __name__=="__main__":
    asyncio.run(main())