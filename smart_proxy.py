#!/usr/bin/env python3
import socket, threading, time, os, struct, fcntl, logging, json, sys, signal
from cmd import Cmd
import dns.resolver
from collections import deque
import subprocess

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

console_handler = logging.StreamHandler()
console_handler.setFormatter(formatter)
logger.addHandler(console_handler)

queue_handler = QueueHandler()
queue_handler.setFormatter(formatter)
logger.addHandler(queue_handler)

try:
    import systemd.journal
    journal_handler = systemd.journal.JournalHandler()
    journal_handler.setFormatter(formatter)
    logger.addHandler(journal_handler)
except ImportError:
    pass  # systemd not available

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
# IN-MEMORY STATE
# -----------------------------
dns_cache = {}               # domain -> list of resolved IPs
runways = {}                 # (iface, proxy_host, proxy_port, dns) -> {"status": bool, "last": t, "next": t}
latency_records = {}         # (domain, runway_key) -> last latency in seconds
prev_status = {}             # track previous runway status to log changes

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
def resolve_domain(domain, dns_ip):
    try:
        r = dns.resolver.Resolver(configure=False)
        r.nameservers = [dns_ip]
        r.timeout = 1
        r.lifetime = 1
        return [a.address for a in r.resolve(domain, "A")]
    except:
        return []

# -----------------------------
# TCP / PROXY PROBES
# -----------------------------
TCP_TIMEOUT = 1.5
BUFFER_SIZE = 65536

def tcp_probe(ip, port, iface):
    try:
        src_ip = get_iface_ip(iface)
        if not src_ip: return None
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(TCP_TIMEOUT)
        s.bind((src_ip, 0))
        start = time.time()
        s.connect((ip, port))
        s.close()
        return time.time() - start
    except: return None

def proxy_probe(proxy_host, proxy_port, target_host, target_port):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(TCP_TIMEOUT)
        start = time.time()
        s.connect((proxy_host, proxy_port))
        s.sendall(f"CONNECT {target_host}:{target_port} HTTP/1.1\r\nHost: {target_host}\r\n\r\n".encode())
        resp = s.recv(4096)
        s.close()
        if b"200" in resp:
            return time.time() - start
    except: return None

# -----------------------------
# RUNWAY BUILDING AND LATENCY
# -----------------------------
MODES = ["first_match", "lowest_latency"]
current_mode = "lowest_latency"

def build_runways(host, port):
    """Rebuild all runways for host:port with status"""
    global runways
    runways.clear()
    ifaces = get_interfaces()
    now = time.time()
    for dns_ip in config.get("dns_servers", []):
        ips = resolve_domain(host, dns_ip)
        dns_cache[host] = ips
        if not ips:
            logger.warning(f"No IPs resolved for {host} using {dns_ip}")
            continue
        for ip in ips:
            for iface in ifaces:
                # direct runway
                latency = tcp_probe(ip, port, iface)
                key = (iface, None, None, dns_ip)
                status = latency is not None
                runways[key] = {"status": status, "last": now, "next": now+5}
                if latency is not None:
                    latency_records[(host, key)] = latency
                if prev_status.get(key) != status:
                    logger.info(f"Runway {key} status changed: {'UP' if status else 'DOWN'}")
                    prev_status[key] = status
                # upstream proxy runways
                for up in config.get("upstream_proxies", []):
                    latency = proxy_probe(up["host"], up["port"], host, port)
                    key = (iface, up["host"], up["port"], dns_ip)
                    status = latency is not None
                    runways[key] = {"status": status, "last": now, "next": now+5}
                    if latency is not None:
                        latency_records[(host, key)] = latency
                    if prev_status.get(key) != status:
                        logger.info(f"Runway {key} status changed: {'UP' if status else 'DOWN'}")
                        prev_status[key] = status

def select_runway(host, port):
    """Select runway based on mode and update latency records"""
    build_runways(host, port)
    available = [(k,v) for k,v in runways.items() if v["status"]]
    if not available:
        return None
    if current_mode == "first_match":
        key, _ = available[0]
    else:
        key, _ = min(available, key=lambda x: latency_records.get((host, x[0]), float('inf')))
    return key

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
        runway_key = select_runway(host, port)
        if not runway_key:
            logger.warning(f"No available runway for {host}:{port}")
            client.close()
            return
        iface, up_host, up_port, dns_ip = runway_key
        ips = resolve_domain(host, dns_ip)
        if not ips:
            logger.error(f"DNS failed for {host} on {dns_ip}")
            client.close()
            return
        ip = ips[0]

        if up_host:
            # connect via upstream proxy
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect((up_host, up_port))
            s.sendall(f"CONNECT {host}:{port} HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
            resp = s.recv(4096)
            if b"200" not in resp:
                client.close()
                return
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            threading.Thread(target=relay, args=(client, s), daemon=True).start()
            threading.Thread(target=relay, args=(s, client), daemon=True).start()
        else:
            # direct connection
            src_ip = get_iface_ip(iface)
            client.sendall(b"HTTP/1.1 200 Connection Established\r\n\r\n")
            upstream = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            upstream.bind((src_ip,0))
            upstream.connect((ip, port))
            threading.Thread(target=relay, args=(client, upstream), daemon=True).start()
            threading.Thread(target=relay, args=(upstream, client), daemon=True).start()
    except Exception:
        logger.exception("Client handling failed")
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
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((self.bind_ip, self.port))
        self.sock.listen(1024)
        logger.info(f"Smart Proxy running on {self.bind_ip}:{self.port}")
        while self.running:
            try:
                self.sock.settimeout(1.0)
                client, _ = self.sock.accept()
                threading.Thread(target=handle_client, args=(client,), daemon=True).start()
            except socket.timeout:
                continue
            except Exception as e:
                if self.running:
                    logger.exception(e)

    def stop(self):
        self.running = False
        if self.sock:
            try:
                self.sock.close()
                logger.info("Proxy socket closed")
            except: pass

# -----------------------------
# CLI
# -----------------------------
class ProxyCLI(Cmd):
    intro = "Smart Proxy CLI. Type help or ?"
    prompt = "(proxy) "

    def do_show_logs(self, arg):
        """show_logs [num] - Show last N log lines (default 100)"""
        try: n = int(arg) if arg else 100
        except: n = 100
        lines = list(LOG_QUEUE)[-n:]
        pager = os.environ.get("PAGER","less")
        p = subprocess.Popen(pager, stdin=subprocess.PIPE, shell=True)
        p.communicate(input="\n".join(lines).encode())

    def do_show_runways(self, arg):
        """show_runways - Show all runways with status"""
        for k,v in runways.items():
            print(f"{k}: {'UP' if v['status'] else 'DOWN'} last:{time.ctime(v['last'])} next:{time.ctime(v['next'])}")

    def do_show_latency(self, arg):
        """show_latency - Show last latency per domain and runway"""
        for (domain,key),lat in latency_records.items():
            print(f"{domain} via {key}: {lat:.3f}s")

    def do_show_dns(self,arg):
        """show_dns - Show cached DNS entries"""
        for domain, ips in dns_cache.items():
            print(f"{domain}: {ips}")

    def do_purge_dns(self,arg):
        """purge_dns domain - Remove DNS cache"""
        if arg in dns_cache:
            del dns_cache[arg]
            print(f"Purged DNS cache for {arg}")

    def do_add_dns(self,arg):
        """add_dns ip - Add a DNS server"""
        if arg and arg not in config["dns_servers"]:
            config["dns_servers"].append(arg)
            save_config(config)
            print(f"Added DNS {arg}")

    def do_remove_dns(self,arg):
        """remove_dns ip - Remove a DNS server"""
        if arg in config["dns_servers"]:
            config["dns_servers"].remove(arg)
            save_config(config)
            print(f"Removed DNS {arg}")

    def do_show_dns_servers(self,arg):
        """show_dns_servers - List DNS servers"""
        print(config.get("dns_servers",[]))

    def do_add_upstream(self,arg):
        """add_upstream host port - Add an upstream proxy"""
        try:
            host,port = arg.split()
            port = int(port)
            entry = {"host":host,"port":port}
            if entry not in config["upstream_proxies"]:
                config["upstream_proxies"].append(entry)
                save_config(config)
                print(f"Added upstream {entry}")
        except:
            print("Usage: add_upstream host port")

    def do_remove_upstream(self,arg):
        """remove_upstream host port - Remove an upstream proxy"""
        try:
            host,port = arg.split()
            port = int(port)
            entry = {"host":host,"port":port}
            if entry in config["upstream_proxies"]:
                config["upstream_proxies"].remove(entry)
                save_config(config)
                print(f"Removed upstream {entry}")
        except:
            print("Usage: remove_upstream host port")

    def do_show_upstreams(self,arg):
        """show_upstreams - Show upstream proxies"""
        for up in config.get("upstream_proxies",[]):
            print(up)

    def do_set_mode(self,arg):
        """set_mode first_match|lowest_latency - Set selection mode"""
        global current_mode
        if arg in MODES:
            current_mode = arg
            print(f"Mode set to {arg}")

    def do_route_monitor(self,arg):
        """route_monitor - Live runway status monitor"""
        try:
            import curses
        except ImportError:
            print("curses required")
            return
        def draw(stdscr):
            curses.curs_set(0)
            curses.start_color()
            curses.init_pair(1, curses.COLOR_RED, curses.COLOR_BLACK)
            curses.init_pair(2, curses.COLOR_GREEN, curses.COLOR_BLACK)
            stdscr.nodelay(True)
            while True:
                stdscr.clear()
                stdscr.addstr(0,0,"Live Runway Monitor (press 'q' to exit)")
                row = 2
                for k,v in runways.items():
                    status = "●" if v['status'] else "○"
                    color = curses.color_pair(2) if v['status'] else curses.color_pair(1)
                    stdscr.addstr(row,0,status,color)
                    stdscr.addstr(row,2,f"{k} last:{time.ctime(v['last'])} next:{time.ctime(v['next'])}")
                    row +=1
                stdscr.refresh()
                try:
                    if stdscr.getch() == ord('q'):
                        break
                except:
                    pass
                time.sleep(1)
        curses.wrapper(draw)

    def do_exit(self,arg):
        """exit - Exit CLI"""
        return True

# -----------------------------
# MAIN
# -----------------------------
def main():
    proxy = ProxyServer()
    proxy.start()
    def signal_handler(sig, frame):
        print("\nStopping proxy...")
        proxy.stop()
        sys.exit(0)
    signal.signal(signal.SIGINT, signal_handler)

    ProxyCLI().cmdloop()
    proxy.stop()

if __name__=="__main__":
    main()