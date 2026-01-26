#!/usr/bin/env python3
"""
Smart Proxy Server - Production Ready
Supports SOCKS5 and HTTP/HTTPS CONNECT protocols with intelligent runway selection
"""
import asyncio
import socket
import os
import struct
import fcntl
import logging
import json
import sys
import threading
import time
import base64
from abc import ABC, abstractmethod
from cmd import Cmd
from collections import deque, defaultdict
from dataclasses import dataclass, asdict
from typing import Optional, Dict, List, Tuple, Any
from enum import Enum

# =============================================================================
# CONSTANTS & ENUMS
# =============================================================================

BUFFER_SIZE = 65536
MAX_RETRIES = 2
CONFIG_FILE = "proxy_config.json"

class SelectionMode(Enum):
    LATENCY = "latency"
    FIRST_AVAILABLE = "first_available"
    ROUND_ROBIN = "round_robin"

class Protocol(Enum):
    SOCKS5 = "socks5"
    HTTP_CONNECT = "http_connect"

# =============================================================================
# LOGGING SETUP
# =============================================================================

LOG_QUEUE = deque(maxlen=1000)

class QueueHandler(logging.Handler):
    def emit(self, record):
        try:
            msg = self.format(record)
            LOG_QUEUE.append(msg)
        except Exception:
            pass

def setup_logging():
    logger = logging.getLogger()
    logger.setLevel(logging.INFO)
    formatter = logging.Formatter("%(asctime)s [%(levelname)s] %(message)s")
    
    console_handler = logging.StreamHandler()
    console_handler.setFormatter(formatter)
    logger.addHandler(console_handler)
    
    queue_handler = QueueHandler()
    queue_handler.setFormatter(formatter)
    logger.addHandler(queue_handler)
    
    return logger

logger = setup_logging()

# =============================================================================
# CONFIGURATION
# =============================================================================

@dataclass
class ProxyConfig:
    upstream_proxies: List[Dict[str, Any]]
    probe_interval: int
    tcp_timeout: float
    selection_mode: str
    auth_enabled: bool
    auth_users: Dict[str, str]
    bind_ip: str
    bind_port: int
    
    @classmethod
    def default(cls):
        return cls(
            upstream_proxies=[{"host": "127.0.0.1", "port": 10808}],
            probe_interval=10,
            tcp_timeout=5.0,
            selection_mode="latency",
            auth_enabled=False,
            auth_users={},
            bind_ip="0.0.0.0",
            bind_port=3123
        )
    
    def to_dict(self):
        return {
            "upstream_proxies": self.upstream_proxies,
            "probe_interval": self.probe_interval,
            "tcp_timeout": self.tcp_timeout,
            "selection_mode": self.selection_mode,
            "auth": {
                "enabled": self.auth_enabled,
                "users": self.auth_users
            },
            "bind_ip": self.bind_ip,
            "bind_port": self.bind_port
        }
    
    @classmethod
    def from_dict(cls, data):
        auth = data.get("auth", {})
        return cls(
            upstream_proxies=data.get("upstream_proxies", []),
            probe_interval=data.get("probe_interval", 10),
            tcp_timeout=data.get("tcp_timeout", 5.0),
            selection_mode=data.get("selection_mode", "latency"),
            auth_enabled=auth.get("enabled", False),
            auth_users=auth.get("users", {}),
            bind_ip=data.get("bind_ip", "0.0.0.0"),
            bind_port=data.get("bind_port", 3123)
        )

class ConfigManager:
    def __init__(self, config_file: str = CONFIG_FILE):
        self.config_file = config_file
        self.config = self.load()
    
    def load(self) -> ProxyConfig:
        try:
            with open(self.config_file, "r") as f:
                data = json.load(f)
                return ProxyConfig.from_dict(data)
        except FileNotFoundError:
            logger.info(f"Config file not found, creating default: {self.config_file}")
            config = ProxyConfig.default()
            self.save(config)
            return config
        except Exception as e:
            logger.error(f"Error loading config: {e}, using defaults")
            return ProxyConfig.default()
    
    def save(self, config: ProxyConfig):
        try:
            with open(self.config_file, "w") as f:
                json.dump(config.to_dict(), f, indent=2)
        except Exception as e:
            logger.error(f"Error saving config: {e}")
    
    def reload(self):
        self.config = self.load()
        return self.config

# =============================================================================
# NETWORK UTILITIES
# =============================================================================

class NetworkUtils:
    @staticmethod
    def get_interfaces() -> List[str]:
        """Return all non-loopback interfaces that are up"""
        ifaces = []
        try:
            for i in os.listdir("/sys/class/net"):
                if i == "lo":
                    continue
                try:
                    with open(f"/sys/class/net/{i}/operstate") as f:
                        if f.read().strip() == "up":
                            ifaces.append(i)
                except Exception:
                    continue
        except FileNotFoundError:
            # Fallback for non-Linux systems
            ifaces = ["0.0.0.0"]
        
        return ifaces if ifaces else ["0.0.0.0"]
    
    @staticmethod
    def get_iface_ip(iface: str) -> Optional[str]:
        """Return IPv4 address of interface (Linux only)"""
        if iface == "0.0.0.0":
            return "0.0.0.0"
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            ip_bytes = fcntl.ioctl(
                s.fileno(), 
                0x8915,  # SIOCGIFADDR
                struct.pack("256s", iface[:15].encode())
            )[20:24]
            return socket.inet_ntoa(ip_bytes)
        except Exception:
            return None

# =============================================================================
# RUNWAY MANAGEMENT
# =============================================================================

@dataclass
class Runway:
    iface: str
    proxy_host: Optional[str]
    proxy_port: Optional[int]
    status: bool = False
    last_probe: float = 0.0
    
    def key(self) -> Tuple:
        return (self.iface, self.proxy_host, self.proxy_port)
    
    def is_direct(self) -> bool:
        return self.proxy_host is None
    
    def __str__(self):
        if self.is_direct():
            return f"{self.iface} -> Direct"
        return f"{self.iface} -> {self.proxy_host}:{self.proxy_port}"

class RunwayManager:
    def __init__(self, config: ProxyConfig):
        self.config = config
        self.runways: Dict[Tuple, Runway] = {}
        self.latency_records: Dict[str, Dict[Tuple, float]] = defaultdict(dict)
        self.prev_status: Dict[Tuple, bool] = {}
        self.round_robin_index = 0
        self.stop_event = threading.Event()
        
    def initialize_runways(self):
        """Create runway objects for all interface/proxy combinations"""
        ifaces = NetworkUtils.get_interfaces()
        
        # Direct connection runways
        for iface in ifaces:
            runway = Runway(iface, None, None)
            self.runways[runway.key()] = runway
        
        # Upstream proxy runways
        for proxy in self.config.upstream_proxies:
            for iface in ifaces:
                runway = Runway(iface, proxy["host"], proxy["port"])
                self.runways[runway.key()] = runway
        
        logger.info(f"Initialized {len(self.runways)} runways")
    
    async def probe_tcp(self, ip: str, port: int, iface: str, timeout: float) -> Optional[float]:
        """Probe target via direct TCP connection"""
        try:
            src_ip = NetworkUtils.get_iface_ip(iface) or "0.0.0.0"
            loop = asyncio.get_running_loop()
            return await loop.run_in_executor(
                None, 
                self._sync_tcp_probe, 
                ip, port, src_ip, timeout
            )
        except Exception as e:
            logger.debug(f"TCP probe error {ip}:{port} via {iface}: {e}")
            return None
    
    def _sync_tcp_probe(self, ip: str, port: int, src_ip: str, timeout: float) -> Optional[float]:
        """Synchronous TCP probe"""
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.settimeout(timeout)
            if src_ip != "0.0.0.0":
                s.bind((src_ip, 0))
            start = time.time()
            s.connect((ip, port))
            s.close()
            return time.time() - start
        except Exception:
            return None
    
    async def probe_proxy(self, proxy_host: str, proxy_port: int, 
                         target_ip: str, target_port: int, timeout: float) -> Optional[float]:
        """Probe target via upstream HTTP CONNECT proxy"""
        try:
            reader, writer = await asyncio.wait_for(
                asyncio.open_connection(proxy_host, proxy_port), 
                timeout
            )
            start = time.time()
            
            # Send CONNECT request
            connect_req = f"CONNECT {target_ip}:{target_port} HTTP/1.1\r\n"
            connect_req += f"Host: {target_ip}:{target_port}\r\n\r\n"
            writer.write(connect_req.encode())
            await writer.drain()
            
            # Read response
            resp = await asyncio.wait_for(reader.read(4096), timeout)
            writer.close()
            await writer.wait_closed()
            
            if b"200" in resp:
                return time.time() - start
            return None
        except Exception as e:
            logger.debug(f"Proxy probe error {proxy_host}:{proxy_port}: {e}")
            return None
    
    async def probe_runway(self, runway: Runway, target_ip: str, target_port: int):
        """Probe a single runway and update its status"""
        timeout = self.config.tcp_timeout
        latency = None
        
        if runway.is_direct():
            latency = await self.probe_tcp(target_ip, target_port, runway.iface, timeout)
        else:
            latency = await self.probe_proxy(
                runway.proxy_host, runway.proxy_port,
                target_ip, target_port, timeout
            )
        
        status = latency is not None
        runway.status = status
        runway.last_probe = time.time()
        
        if latency:
            self.latency_records[target_ip][runway.key()] = latency
        
        # Log status changes
        key = runway.key()
        if self.prev_status.get(key) != status:
            self.prev_status[key] = status
            status_str = "UP" if status else "DOWN"
            lat_str = f"{latency:.3f}s" if latency else "N/A"
            logger.info(f"[RUNWAY] {runway} -> {target_ip}:{target_port} {status_str} ({lat_str})")
    
    async def probe_all_runways(self, targets: List[Tuple[str, int]]):
        """Probe all runways for all targets"""
        tasks = []
        for target_ip, target_port in targets:
            for runway in self.runways.values():
                tasks.append(self.probe_runway(runway, target_ip, target_port))
        
        if tasks:
            await asyncio.gather(*tasks, return_exceptions=True)
    
    async def periodic_probe(self, targets: List[Tuple[str, int]]):
        """Continuously probe all runways"""
        interval = self.config.probe_interval
        while not self.stop_event.is_set():
            try:
                await self.probe_all_runways(targets)
            except Exception as e:
                logger.error(f"Probe error: {e}")
            await asyncio.sleep(interval)
    
    def select_runway(self, target_ip: str) -> Optional[Runway]:
        """Select best runway based on configured selection mode"""
        mode = SelectionMode(self.config.selection_mode)
        available = [r for r in self.runways.values() if r.status]
        
        if not available:
            return None
        
        if mode == SelectionMode.FIRST_AVAILABLE:
            # Prefer direct connections first
            for r in available:
                if r.is_direct():
                    return r
            return available[0]
        
        elif mode == SelectionMode.ROUND_ROBIN:
            # Simple round-robin
            runway = available[self.round_robin_index % len(available)]
            self.round_robin_index += 1
            return runway
        
        elif mode == SelectionMode.LATENCY:
            # Sort by latency
            runway_latencies = []
            for r in available:
                lat = self.latency_records.get(target_ip, {}).get(r.key(), float('inf'))
                runway_latencies.append((r, lat))
            
            runway_latencies.sort(key=lambda x: x[1])
            return runway_latencies[0][0]
        
        return available[0]
    
    def get_available_runways(self, prefer_direct: bool = True) -> List[Runway]:
        """Get list of available runways, optionally prioritizing direct connections"""
        available = [r for r in self.runways.values() if r.status]
        
        if prefer_direct:
            direct = [r for r in available if r.is_direct()]
            proxied = [r for r in available if not r.is_direct()]
            return direct + proxied
        
        return available
    
    def stop(self):
        self.stop_event.set()

# =============================================================================
# PROTOCOL HANDLERS
# =============================================================================

class ProtocolHandler(ABC):
    @abstractmethod
    async def handle(self, reader: asyncio.StreamReader, 
                    writer: asyncio.StreamWriter) -> Tuple[str, int, Optional[str]]:
        """
        Handle protocol-specific handshake
        Returns: (target_host, target_port, error_message)
        """
        pass

class SOCKS5Handler(ProtocolHandler):
    def __init__(self, config: ProxyConfig):
        self.config = config
    
    async def handle(self, reader: asyncio.StreamReader, 
                    writer: asyncio.StreamWriter) -> Tuple[str, int, Optional[str]]:
        try:
            # Read greeting
            header = await asyncio.wait_for(reader.read(2), timeout=5.0)
            if len(header) != 2 or header[0] != 5:
                return None, None, "Invalid SOCKS5 greeting"
            
            nmethods = header[1]
            methods = await asyncio.wait_for(reader.read(nmethods), timeout=5.0)
            
            # Check authentication
            if self.config.auth_enabled:
                if 2 not in methods:  # Username/password auth
                    writer.write(b"\x05\xFF")  # No acceptable methods
                    await writer.drain()
                    return None, None, "Authentication required but not supported by client"
                
                # Select username/password auth
                writer.write(b"\x05\x02")
                await writer.drain()
                
                # Authenticate
                if not await self._authenticate(reader, writer):
                    return None, None, "Authentication failed"
            else:
                # No authentication
                writer.write(b"\x05\x00")
                await writer.drain()
            
            # Read request
            req = await asyncio.wait_for(reader.read(4), timeout=5.0)
            if len(req) != 4:
                return None, None, "Invalid SOCKS5 request"
            
            cmd, atype = req[1], req[3]
            
            if cmd != 1:  # Only CONNECT supported
                writer.write(b"\x05\x07\x00\x01\x00\x00\x00\x00\x00\x00")  # Command not supported
                await writer.drain()
                return None, None, f"Unsupported SOCKS5 command: {cmd}"
            
            # Parse address
            addr, port = await self._parse_address(reader, atype)
            if not addr:
                writer.write(b"\x05\x08\x00\x01\x00\x00\x00\x00\x00\x00")  # Address type not supported
                await writer.drain()
                return None, None, "Invalid address type"
            
            return addr, port, None
            
        except asyncio.TimeoutError:
            return None, None, "SOCKS5 handshake timeout"
        except Exception as e:
            return None, None, f"SOCKS5 error: {e}"
    
    async def _authenticate(self, reader: asyncio.StreamReader, 
                           writer: asyncio.StreamWriter) -> bool:
        """Handle SOCKS5 username/password authentication"""
        try:
            auth_ver = await asyncio.wait_for(reader.read(1), timeout=5.0)
            if auth_ver[0] != 1:
                return False
            
            ulen = (await reader.read(1))[0]
            username = (await reader.read(ulen)).decode('utf-8')
            
            plen = (await reader.read(1))[0]
            password = (await reader.read(plen)).decode('utf-8')
            
            # Verify credentials
            if username in self.config.auth_users:
                if self.config.auth_users[username] == password:
                    writer.write(b"\x01\x00")  # Success
                    await writer.drain()
                    return True
            
            writer.write(b"\x01\x01")  # Failure
            await writer.drain()
            return False
        except Exception:
            return False
    
    async def _parse_address(self, reader: asyncio.StreamReader, 
                            atype: int) -> Tuple[Optional[str], Optional[int]]:
        """Parse SOCKS5 address"""
        try:
            if atype == 1:  # IPv4
                raw_ip = await reader.read(4)
                if len(raw_ip) != 4:
                    return None, None
                addr = socket.inet_ntoa(raw_ip)
            elif atype == 3:  # Domain
                domlen = (await reader.read(1))[0]
                addr_bytes = await reader.read(domlen)
                if len(addr_bytes) != domlen:
                    return None, None
                addr = addr_bytes.decode('utf-8')
            elif atype == 4:  # IPv6
                raw_ip = await reader.read(16)
                if len(raw_ip) != 16:
                    return None, None
                addr = socket.inet_ntop(socket.AF_INET6, raw_ip)
            else:
                return None, None
            
            port_bytes = await reader.read(2)
            if len(port_bytes) != 2:
                return None, None
            port = int.from_bytes(port_bytes, "big")
            
            return addr, port
        except Exception:
            return None, None
    
    async def send_success(self, writer: asyncio.StreamWriter, bind_ip: str, bind_port: int):
        """Send SOCKS5 success response"""
        try:
            response = b"\x05\x00\x00\x01"
            response += socket.inet_aton(bind_ip)
            response += bind_port.to_bytes(2, "big")
            writer.write(response)
            await writer.drain()
        except Exception as e:
            logger.error(f"Error sending SOCKS5 success: {e}")
    
    async def send_error(self, writer: asyncio.StreamWriter, error_code: int = 1):
        """Send SOCKS5 error response"""
        try:
            response = bytes([5, error_code, 0, 1, 0, 0, 0, 0, 0, 0])
            writer.write(response)
            await writer.drain()
        except Exception:
            pass

class HTTPConnectHandler(ProtocolHandler):
    def __init__(self, config: ProxyConfig):
        self.config = config
    
    async def handle(self, reader: asyncio.StreamReader, 
                    writer: asyncio.StreamWriter) -> Tuple[str, int, Optional[str]]:
        try:
            # Read HTTP request line
            request_line = await asyncio.wait_for(reader.readline(), timeout=5.0)
            request_line = request_line.decode('utf-8').strip()
            
            # Parse CONNECT request
            parts = request_line.split()
            if len(parts) != 3 or parts[0] != "CONNECT":
                return None, None, "Invalid HTTP CONNECT request"
            
            # Parse target
            target = parts[1]
            if ':' in target:
                host, port_str = target.rsplit(':', 1)
                port = int(port_str)
            else:
                host = target
                port = 443  # Default HTTPS port
            
            # Read headers
            headers = {}
            while True:
                line = await asyncio.wait_for(reader.readline(), timeout=5.0)
                line = line.decode('utf-8').strip()
                if not line:
                    break
                if ':' in line:
                    key, value = line.split(':', 1)
                    headers[key.strip().lower()] = value.strip()
            
            # Check authentication if enabled
            if self.config.auth_enabled:
                auth_header = headers.get('proxy-authorization')
                if not auth_header or not self._verify_auth(auth_header):
                    await self._send_auth_required(writer)
                    return None, None, "Authentication required"
            
            return host, port, None
            
        except asyncio.TimeoutError:
            return None, None, "HTTP CONNECT timeout"
        except Exception as e:
            return None, None, f"HTTP CONNECT error: {e}"
    
    def _verify_auth(self, auth_header: str) -> bool:
        """Verify HTTP Basic authentication"""
        try:
            if not auth_header.startswith('Basic '):
                return False
            
            encoded = auth_header[6:]
            decoded = base64.b64decode(encoded).decode('utf-8')
            username, password = decoded.split(':', 1)
            
            return (username in self.config.auth_users and 
                   self.config.auth_users[username] == password)
        except Exception:
            return False
    
    async def _send_auth_required(self, writer: asyncio.StreamWriter):
        """Send 407 Proxy Authentication Required"""
        response = "HTTP/1.1 407 Proxy Authentication Required\r\n"
        response += "Proxy-Authenticate: Basic realm=\"Smart Proxy\"\r\n"
        response += "\r\n"
        writer.write(response.encode())
        await writer.drain()
    
    async def send_success(self, writer: asyncio.StreamWriter):
        """Send HTTP 200 Connection Established"""
        response = "HTTP/1.1 200 Connection Established\r\n\r\n"
        writer.write(response.encode())
        await writer.drain()
    
    async def send_error(self, writer: asyncio.StreamWriter, status: int = 500):
        """Send HTTP error response"""
        response = f"HTTP/1.1 {status} Error\r\n\r\n"
        writer.write(response.encode())
        await writer.drain()

# =============================================================================
# RELAY
# =============================================================================

class RelayManager:
    @staticmethod
    async def relay(reader: asyncio.StreamReader, writer: asyncio.StreamWriter, 
                   direction: str = ""):
        """Relay data between two streams"""
        try:
            while True:
                data = await reader.read(BUFFER_SIZE)
                if not data:
                    break
                writer.write(data)
                await writer.drain()
        except Exception as e:
            logger.debug(f"Relay error {direction}: {e}")
        finally:
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
    
    @staticmethod
    async def bidirectional_relay(client_reader: asyncio.StreamReader,
                                  client_writer: asyncio.StreamWriter,
                                  remote_reader: asyncio.StreamReader,
                                  remote_writer: asyncio.StreamWriter,
                                  peer: str, target: str):
        """Bidirectional relay between client and remote"""
        logger.info(f"[RELAY START] {peer} <-> {target}")
        try:
            await asyncio.gather(
                RelayManager.relay(client_reader, remote_writer, f"C->R:{target}"),
                RelayManager.relay(remote_reader, client_writer, f"R->C:{target}"),
                return_exceptions=True
            )
        finally:
            logger.info(f"[RELAY END] {peer} <-> {target}")
            for writer in [client_writer, remote_writer]:
                try:
                    writer.close()
                    await writer.wait_closed()
                except Exception:
                    pass

# =============================================================================
# PROXY SERVER
# =============================================================================

class SmartProxyServer:
    def __init__(self, config_manager: ConfigManager, runway_manager: RunwayManager):
        self.config_manager = config_manager
        self.runway_manager = runway_manager
        self.server = None
        self.stats = {
            'total_connections': 0,
            'active_connections': 0,
            'failed_connections': 0
        }
    
    async def detect_protocol(self, reader: asyncio.StreamReader) -> Optional[Protocol]:
        """Detect whether client is using SOCKS5 or HTTP CONNECT"""
        try:
            # Peek at first few bytes
            first_byte = await asyncio.wait_for(reader.read(1), timeout=2.0)
            if not first_byte:
                return None
            
            # SOCKS5 starts with version byte 0x05
            if first_byte[0] == 5:
                # Put the byte back by reading into a buffer
                return Protocol.SOCKS5
            
            # HTTP starts with method name (CONNECT, GET, etc.)
            # Read more to check for "CONNECT"
            more_bytes = await asyncio.wait_for(reader.read(7), timeout=2.0)
            full = first_byte + more_bytes
            if full.startswith(b'CONNECT'):
                return Protocol.HTTP_CONNECT
            
            return None
        except Exception as e:
            logger.debug(f"Protocol detection error: {e}")
            return None
    
    async def handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        peer = writer.get_extra_info("peername")
        self.stats['total_connections'] += 1
        self.stats['active_connections'] += 1
        
        target_host = "unknown"
        target_port = 0
        
        try:
            # Detect protocol
            protocol = await self.detect_protocol(reader)
            if not protocol:
                logger.warning(f"[UNKNOWN PROTOCOL] {peer}")
                writer.close()
                await writer.wait_closed()
                return
            
            # Select handler
            config = self.config_manager.config
            if protocol == Protocol.SOCKS5:
                handler = SOCKS5Handler(config)
            else:
                handler = HTTPConnectHandler(config)
            
            # Handle protocol handshake
            target_host, target_port, error = await handler.handle(reader, writer)
            
            if error:
                logger.warning(f"[HANDSHAKE ERROR] {peer}: {error}")
                if hasattr(handler, 'send_error'):
                    await handler.send_error(writer)
                writer.close()
                await writer.wait_closed()
                return
            
            logger.info(f"[{protocol.value.upper()}] {peer} -> {target_host}:{target_port}")
            
            # Resolve domain if necessary
            resolved_ip = await self._resolve_host(target_host, target_port)
            if not resolved_ip:
                logger.error(f"[DNS FAIL] Could not resolve {target_host}")
                if hasattr(handler, 'send_error'):
                    await handler.send_error(writer, error_code=4)  # Host unreachable
                writer.close()
                await writer.wait_closed()
                return
            
            # Try to establish connection through runways
            remote_reader, remote_writer, runway = await self._connect_via_runways(
                target_host, resolved_ip, target_port
            )
            
            if not remote_reader:
                logger.error(f"[CONNECT FAIL] {peer} -> {target_host}:{target_port}")
                if hasattr(handler, 'send_error'):
                    await handler.send_error(writer, error_code=5)  # Connection refused
                self.stats['failed_connections'] += 1
                writer.close()
                await writer.wait_closed()
                return
            
            logger.info(f"[SUCCESS] {peer} -> {target_host}:{target_port} via {runway}")
            
            # Send success response
            if protocol == Protocol.SOCKS5:
                bind_ip = NetworkUtils.get_iface_ip(runway.iface) or "0.0.0.0"
                await handler.send_success(writer, bind_ip, target_port)
            else:
                await handler.send_success(writer)
            
            # Start bidirectional relay
            await RelayManager.bidirectional_relay(
                reader, writer,
                remote_reader, remote_writer,
                str(peer), f"{target_host}:{target_port}"
            )
            
        except Exception as e:
            logger.error(f"[CLIENT ERROR] {peer} -> {target_host}:{target_port}: {e}")
            self.stats['failed_connections'] += 1
        finally:
            self.stats['active_connections'] -= 1
            try:
                writer.close()
                await writer.wait_closed()
            except Exception:
                pass
    
    async def _resolve_host(self, host: str, port: int) -> Optional[str]:
        """Resolve hostname to IP address, preferring IPv4"""
        # If already an IP, return it
        try:
            socket.inet_aton(host)
            return host  # Valid IPv4
        except socket.error:
            pass
        
        try:
            socket.inet_pton(socket.AF_INET6, host)
            return host  # Valid IPv6
        except socket.error:
            pass
        
        # Resolve domain
        try:
            loop = asyncio.get_event_loop()
            infos = await loop.getaddrinfo(host, port, family=socket.AF_INET)
            if not infos:
                infos = await loop.getaddrinfo(host, port, family=socket.AF_INET6)
            if infos:
                return infos[0][4][0]
        except Exception as e:
            logger.error(f"DNS resolution failed for {host}: {e}")
        
        return None
    
    async def _connect_via_runways(self, display_host: str, resolved_ip: str, 
                                   port: int) -> Tuple[Optional[asyncio.StreamReader], 
                                                       Optional[asyncio.StreamWriter],
                                                       Optional[Runway]]:
        """Try to connect through available runways"""
        runways = self.runway_manager.get_available_runways(prefer_direct=True)
        
        if not runways:
            logger.warning(f"No available runways for {display_host}:{port}")
            return None, None, None
        
        for runway in runways:
            for attempt in range(1, MAX_RETRIES + 1):
                try:
                    if runway.is_direct():
                        # Direct connection
                        reader, writer = await asyncio.wait_for(
                            asyncio.open_connection(resolved_ip, port),
                            timeout=self.config_manager.config.tcp_timeout
                        )
                        return reader, writer, runway
                    else:
                        # Via upstream proxy
                        reader, writer = await asyncio.wait_for(
                            asyncio.open_connection(runway.proxy_host, runway.proxy_port),
                            timeout=self.config_manager.config.tcp_timeout
                        )
                        
                        # Send CONNECT to upstream
                        connect_req = f"CONNECT {display_host}:{port} HTTP/1.1\r\n"
                        connect_req += f"Host: {display_host}:{port}\r\n\r\n"
                        writer.write(connect_req.encode())
                        await writer.drain()
                        
                        # Read response
                        resp = await asyncio.wait_for(reader.read(4096), timeout=5.0)
                        if b"200" not in resp:
                            raise ConnectionError(f"Upstream CONNECT failed")
                        
                        return reader, writer, runway
                        
                except Exception as e:
                    logger.debug(f"Runway {runway} attempt {attempt} failed: {e}")
                    await asyncio.sleep(0.1)
        
        return None, None, None
    
    async def start(self):
        """Start the proxy server"""
        config = self.config_manager.config
        self.server = await asyncio.start_server(
            self.handle_client, 
            config.bind_ip, 
            config.bind_port
        )
        
        logger.info(f"Smart Proxy Server running on {config.bind_ip}:{config.bind_port}")
        logger.info(f"Protocols: SOCKS5, HTTP CONNECT")
        logger.info(f"Selection mode: {config.selection_mode}")
        
        async with self.server:
            await self.server.serve_forever()

# =============================================================================
# CLI
# =============================================================================

def live_loop(callback, interval=1):
    """Non-blocking live update loop"""
    import tty, termios, select
    
    fd = sys.stdin.fileno()
    old_settings = termios.tcgetattr(fd)
    try:
        tty.setcbreak(fd)
        print("\nPress 'q' or Esc to exit\n")
        while True:
            callback()
            if select.select([sys.stdin], [], [], interval)[0]:
                ch = sys.stdin.read(1)
                if ch.lower() == 'q' or ord(ch) == 27:
                    break
    finally:
        termios.tcsetattr(fd, termios.TCSADRAIN, old_settings)

class ProxyCLI(Cmd):
    intro = "Smart Proxy CLI - Type 'help' for commands"
    prompt = "(proxy) "
    
    def __init__(self, runway_manager: RunwayManager, server: SmartProxyServer):
        super().__init__()
        self.runway_manager = runway_manager
        self.server = server
    
    def do_show_logs(self, arg):
        """show_logs [num] - Show last N log lines (default 100)"""
        try:
            n = int(arg) if arg else 100
        except ValueError:
            n = 100
        
        last_index = max(len(LOG_QUEUE) - n, 0)
        
        def callback():
            nonlocal last_index
            logs = list(LOG_QUEUE)
            new_logs = logs[last_index:]
            if new_logs:
                for line in new_logs:
                    print(line)
                last_index = len(logs)
        
        live_loop(callback)
    
    def do_show_runways(self, arg):
        """show_runways - Live runway status display"""
        def callback():
            os.system("clear" if os.name != "nt" else "cls")
            print("=" * 80)
            print(f"{'Runway':<50} {'Status':<10} {'Last Probe':<15}")
            print("=" * 80)
            
            for runway in self.runway_manager.runways.values():
                status = "🟢 UP" if runway.status else "🔴 DOWN"
                last = int(time.time() - runway.last_probe) if runway.last_probe > 0 else 9999
                last_str = f"{last}s ago" if last < 9999 else "never"
                print(f"{str(runway):<50} {status:<10} {last_str:<15}")
        
        live_loop(callback)
    
    def do_show_latency(self, arg):
        """show_latency - Show latency records"""
        if not self.runway_manager.latency_records:
            print("No latency records available")
            return
        
        for target, records in self.runway_manager.latency_records.items():
            print(f"\nTarget: {target}")
            sorted_records = sorted(records.items(), key=lambda x: x[1])
            for runway_key, latency in sorted_records:
                runway = self.runway_manager.runways[runway_key]
                print(f"  {runway}: {latency:.3f}s")
    
    def do_show_stats(self, arg):
        """show_stats - Show connection statistics"""
        stats = self.server.stats
        print(f"\nConnection Statistics:")
        print(f"  Total: {stats['total_connections']}")
        print(f"  Active: {stats['active_connections']}")
        print(f"  Failed: {stats['failed_connections']}")
        
        total_runways = len(self.runway_manager.runways)
        active_runways = sum(1 for r in self.runway_manager.runways.values() if r.status)
        print(f"\nRunway Statistics:")
        print(f"  Total: {total_runways}")
        print(f"  Active: {active_runways}")
        print(f"  Inactive: {total_runways - active_runways}")
    
    def do_reload_config(self, arg):
        """reload_config - Reload configuration from file"""
        try:
            self.runway_manager.config = self.runway_manager.config_manager.reload()
            print("Configuration reloaded successfully")
        except Exception as e:
            print(f"Error reloading config: {e}")
    
    def do_exit(self, arg):
        """exit - Exit CLI and stop server"""
        self.runway_manager.stop()
        return True
    
    def do_quit(self, arg):
        """quit - Same as exit"""
        return self.do_exit(arg)

# =============================================================================
# MAIN
# =============================================================================

async def main():
    # Initialize components
    config_manager = ConfigManager()
    runway_manager = RunwayManager(config_manager.config)
    runway_manager.config_manager = config_manager
    
    # Initialize runways
    runway_manager.initialize_runways()
    
    # Create server
    server = SmartProxyServer(config_manager, runway_manager)
    
    # Probe targets
    targets = [("8.8.8.8", 53)]  # Google DNS for direct connections
    for proxy in config_manager.config.upstream_proxies:
        targets.append((proxy["host"], proxy["port"]))
    
    # Start server and probing
    asyncio.create_task(server.start())
    asyncio.create_task(runway_manager.periodic_probe(targets))
    
    # Start CLI in separate thread
    cli = ProxyCLI(runway_manager, server)
    cli_thread = threading.Thread(target=cli.cmdloop, daemon=True)
    cli_thread.start()
    
    # Keep running until stopped
    try:
        while not runway_manager.stop_event.is_set() and cli_thread.is_alive():
            await asyncio.sleep(1)
    finally:
        runway_manager.stop()
        logger.info("Shutting down Smart Proxy Server...")

if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        print("\nStopping Smart Proxy Server...")