#!/usr/bin/env python3
import asyncio, socket, os, struct, fcntl, logging, json, sys, signal, threading, time
from cmd import Cmd
from collections import deque, defaultdict
import tty, termios, sys, select

def live_loop(callback, refresh_interval=1):
    """
    Generic live CLI loop:
    - callback() prints the live output
    - refresh_interval in seconds
    - exits on 'q' or Esc
    """
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        print("\nPress 'q' or Esc to exit\n")
        while True:
            callback()
            # Check for key press without blocking
            if select.select([sys.stdin], [], [], refresh_interval)[0]:
                ch = sys.stdin.read(1)
                if ch.lower() == 'q' or ord(ch) == 27:
                    break
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)
    print("\nExited live view\n")



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
    "upstream_proxies": [{"host":"127.0.0.1","port":10808}],
    "probe_interval": 10,
    "tcp_timeout": 1.5,
    "selection_mode": "first_available"
}

def load_config():
    try:
        with open(CONFIG_FILE,"r") as f:
            return json.load(f)
    except:
        save_config(DEFAULT_CONFIG)
        return DEFAULT_CONFIG

def save_config(cfg):
    with open(CONFIG_FILE,"w") as f:
        json.dump(cfg,f,indent=2)

config = load_config()

# -----------------------------
# STATE
# -----------------------------
runways = {}  # (iface, proxy_host, proxy_port) -> {"status": bool, "last": t, "latency": float}
latency_records = defaultdict(dict)  # target -> runway -> latency
prev_status = {}
stop_event = asyncio.Event()

# -----------------------------
# NETWORK HELPERS
# -----------------------------
def get_interfaces():
    ifaces=[]
    for i in os.listdir("/sys/class/net"):
        if i=="lo": continue
        try:
            with open(f"/sys/class/net/{i}/operstate") as f:
                if f.read().strip()=="up": ifaces.append(i)
        except: pass
    return ifaces

def get_iface_ip(iface):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        return socket.inet_ntoa(fcntl.ioctl(s.fileno(),0x8915,struct.pack("256s",iface[:15].encode()))[20:24])
    except: return None

# -----------------------------
# TCP/UDP PROBES
# -----------------------------
async def tcp_probe(ip, port, iface, timeout=1.5):
    loop = asyncio.get_running_loop()
    src_ip = get_iface_ip(iface)
    if not src_ip: return None
    return await loop.run_in_executor(None, lambda: sync_tcp_connect(ip, port, src_ip, timeout))

def sync_tcp_connect(ip, port, src_ip, timeout):
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.bind((src_ip,0))
        start=time.time()
        s.connect((ip,port))
        s.close()
        return time.time()-start
    except: return None

async def proxy_probe(proxy_host, proxy_port, target_host, target_port, timeout=1.5):
    loop = asyncio.get_running_loop()
    return await loop.run_in_executor(None, lambda: sync_proxy_connect(proxy_host, proxy_port, target_host, target_port, timeout))

def sync_proxy_connect(proxy_host, proxy_port, target_host, target_port, timeout):
    try:
        s = socket.socket(socket.AF_INET,socket.SOCK_STREAM)
        s.settimeout(timeout)
        start=time.time()
        s.connect((proxy_host,proxy_port))
        s.sendall(f"CONNECT {target_host}:{target_port} HTTP/1.1\r\nHost:{target_host}\r\n\r\n".encode())
        resp=s.recv(4096)
        s.close()
        if b"200" in resp: return time.time()-start
        return None
    except: return None

async def probe_runway(iface, proxy_host, proxy_port):
    latency=None
    if proxy_host:
        latency = await proxy_probe(proxy_host, proxy_port, "8.8.8.8", 80)
    else:
        # direct probe to arbitrary IP
        latency = await tcp_probe("8.8.8.8",80,iface)
    key=(iface,proxy_host,proxy_port)
    status=latency is not None
    runways[key]={"status":status,"last":time.time(),"latency":latency}
    if prev_status.get(key)!=status:
        prev_status[key]=status
        logger.info(f"Runway {key} status changed: {'UP' if status else 'DOWN'}")

async def monitor_runways(targets=[]):
    while not stop_event.is_set():
        ifaces = get_interfaces()
        tasks=[]
        for iface in ifaces:
            tasks.append(probe_runway(iface,None,None))
            for up in config.get("upstream_proxies",[]):
                tasks.append(probe_runway(iface,up["host"],up["port"]))
        await asyncio.gather(*tasks)
        # Latency per target
        for target in targets:
            is_ip=False
            try: socket.inet_aton(target); is_ip=True
            except: pass
            for rkey in runways:
                ip=target
                iface, proxy_host, proxy_port = rkey
                if proxy_host:
                    latency=await proxy_probe(proxy_host,proxy_port,ip,80)
                else:
                    latency=await tcp_probe(ip,80,iface)
                if latency: latency_records[target][rkey]=latency
        await asyncio.sleep(config.get("probe_interval",10))

def select_runway(target):
    for rkey,v in runways.items():
        if v["status"]: return rkey
    return None

# -----------------------------
# RELAY
# -----------------------------
BUFFER_SIZE=65536
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
# PROXY SERVER TCP & SOCKS5/UDP
# -----------------------------
class ProxyServer:
    def __init__(self, bind_ip="127.0.0.1", port=1080):
        self.bind_ip=bind_ip
        self.port=port
        self.server=None

    async def handle_tcp_client(self, reader, writer):
        try:
            # SOCKS5 handshake
            header = await reader.read(2)
            if header[0]!=5:
                writer.close(); return
            nmethods=header[1]
            await reader.read(nmethods)
            writer.write(b"\x05\x00")  # no auth
            await writer.drain()

            # Request
            req = await reader.read(4)
            if req[1]==1:  # CONNECT
                port=int.from_bytes(await reader.read(2),'big')
                addr_type=req[3]
                if addr_type==1:
                    addr = socket.inet_ntoa(await reader.read(4))
                elif addr_type==3:
                    domlen=await reader.read(1)
                    addr=(await reader.read(domlen[0])).decode()
                else: writer.close(); return
                runway_key = select_runway(addr)
                if not runway_key: writer.close(); return
                iface, proxy_host, proxy_port = runway_key
                ip=addr
                if proxy_host:
                    r_reader,r_writer=await asyncio.open_connection(proxy_host,proxy_port)
                    r_writer.write(f"CONNECT {ip}:{port} HTTP/1.1\r\nHost:{ip}\r\n\r\n".encode())
                    await r_writer.drain()
                    resp=await r_reader.read(4096)
                    if b"200" not in resp: writer.close(); return
                    writer.write(b"\x05\x00\x00\x01"+socket.inet_aton("0.0.0.0")+port.to_bytes(2,'big'))
                    await writer.drain()
                    await asyncio.gather(relay(reader,r_writer),relay(r_reader,writer))
                else:
                    r_reader,r_writer=await asyncio.open_connection(ip,port)
                    writer.write(b"\x05\x00\x00\x01"+socket.inet_aton("0.0.0.0")+port.to_bytes(2,'big'))
                    await writer.drain()
                    await asyncio.gather(relay(reader,r_writer),relay(r_reader,writer))
            elif req[1]==3:  # UDP ASSOCIATE
                # Simple UDP relay
                udp_reader,udp_writer = await asyncio.open_connection('127.0.0.1',0)
                # For brevity, UDP relay skeleton; user can implement UDP datagram forwarding
                writer.write(b"\x05\x00\x00\x01"+socket.inet_aton("0.0.0.0")+b"\x00\x00")
                await writer.drain()
        except: 
            try: writer.close()
            except: pass

    async def start(self):
        self.server=await asyncio.start_server(self.handle_tcp_client,self.bind_ip,self.port)
        await self.server.serve_forever()

# -----------------------------
# CLI
# -----------------------------
class ProxyCLI(Cmd):
    intro="Smart Proxy CLI. Type help or ?"
    prompt="(proxy) "

    def do_show_logs(self, arg):
        """show_logs [num] - Live show last N log lines (press 'q' or Esc to exit)"""
        try:
            n = int(arg) if arg else 100
        except:
            n = 100
        last_index = max(len(LOG_QUEUE) - n, 0)
        
        def callback():
            nonlocal last_index
            logs = list(LOG_QUEUE)
            new_logs = logs[last_index:]
            if new_logs:
                for line in new_logs: print(line)
                last_index = len(logs)

        live_loop(callback)

    def do_show_runways(self,arg):
        """show_runways - Display all runways and status"""
        for k,v in runways.items():
            print(f"{k}: {'UP' if v['status'] else 'DOWN'} latency:{v['latency']} last:{time.ctime(v['last'])}")

    def do_show_latency(self,arg):
        """show_latency [target] - show latency per target"""
        if arg:
            target=arg.strip()
            if target in latency_records:
                for k,v in latency_records[target].items():
                    print(f"{target} via {k}: {v:.3f}s")
        else:
            for target,rdict in latency_records.items():
                for k,v in rdict.items():
                    print(f"{target} via {k}: {v:.3f}s")
                        
    def do_live_monitor(self, arg):
        """live_monitor - show live runway status (press 'q' or Esc to exit)"""
        def callback():
            os.system("clear")
            now = time.time()
            print("RUNWAY STATUS (🟢=UP, 🔴=DOWN)")
            for k,v in runways.items():
                circle = "🟢" if v["status"] else "🔴"
                next_update = int(v["last"] + config.get("probe_interval",10) - now)
                print(f"{circle} {k} last:{int(now-v['last'])}s next:{next_update}s latency:{v['latency']}")
        live_loop(callback)


    def do_exit(self,arg):
        """exit - Exit CLI"""
        stop_event.set()
        return True

    def do_help(self,arg):
        Cmd.do_help(self,arg)

# -----------------------------
# MAIN
# -----------------------------
def start_cli():
    ProxyCLI().cmdloop()

async def main():
    proxy=ProxyServer()
    targets=[]
    loop=asyncio.get_running_loop()
    loop.create_task(monitor_runways(targets))
    loop.create_task(proxy.start())
    t=threading.Thread(target=start_cli,daemon=True)
    t.start()
    while not stop_event.is_set() and t.is_alive(): await asyncio.sleep(1)
    logger.info("Shutting down smart proxy...")

if __name__=="__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nExiting smart proxy gracefully...")
