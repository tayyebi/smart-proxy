#include "runway_manager.h"
#include "network.h"
#include "utils.h"
#include <sstream>
#include <ctime>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#include <iphlpapi.h>
#pragma comment(lib, "iphlpapi.lib")
#else
#include <ifaddrs.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

RunwayManager::RunwayManager(
    const std::vector<std::string>& interfaces,
    const std::vector<UpstreamProxyConfig>& upstream_proxies,
    const std::vector<DNSServerConfig>& dns_servers,
    std::shared_ptr<DNSResolver> dns_resolver)
    : interfaces_(interfaces)
    , dns_resolver_(dns_resolver) {
    
    // Convert configs to runtime objects
    for (const auto& proxy_cfg : upstream_proxies) {
        upstream_proxies_.push_back(std::make_shared<UpstreamProxy>(proxy_cfg));
    }
    
    for (const auto& dns_cfg : dns_servers) {
        dns_servers_.push_back(std::make_shared<DNSServer>(dns_cfg));
    }
    
    discover_interfaces();
}

RunwayManager::~RunwayManager() {
}

uint64_t RunwayManager::get_current_time() const {
#ifdef _WIN32
    return static_cast<uint64_t>(time(nullptr));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
#endif
}

void RunwayManager::discover_interfaces() {
    std::lock_guard<std::mutex> lock(mutex_);
    
#ifdef _WIN32
    // Windows: Use GetAdaptersAddresses
    ULONG buffer_size = 15000;
    std::vector<uint8_t> buffer(buffer_size);
    PIP_ADAPTER_ADDRESSES adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
    
    ULONG result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &buffer_size);
    if (result == ERROR_BUFFER_OVERFLOW) {
        buffer.resize(buffer_size);
        adapters = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buffer.data());
        result = GetAdaptersAddresses(AF_INET, GAA_FLAG_INCLUDE_PREFIX, nullptr, adapters, &buffer_size);
    }
    
    if (result == NO_ERROR) {
        std::map<std::string, InterfaceInfo> current_interfaces;
        
        for (PIP_ADAPTER_ADDRESSES adapter = adapters; adapter != nullptr; adapter = adapter->Next) {
            if (adapter->IfType == IF_TYPE_ETHERNET_CSMACD || adapter->IfType == IF_TYPE_IEEE80211) {
                for (PIP_ADAPTER_UNICAST_ADDRESS addr = adapter->FirstUnicastAddress;
                     addr != nullptr; addr = addr->Next) {
                    if (addr->Address.lpSockaddr->sa_family == AF_INET) {
                        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(addr->Address.lpSockaddr);
                        char ip_str[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &sin->sin_addr, ip_str, INET_ADDRSTRLEN);
                        
                        InterfaceInfo info;
                        info.name = adapter->AdapterName;
                        info.ip = ip_str;
                        info.last_seen = get_current_time();
                        current_interfaces[info.name] = info;
                    }
                }
            }
        }
        
        interface_info_ = current_interfaces;
    }
#else
    // POSIX: Use getifaddrs
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) == -1) {
        return;
    }
    
    std::map<std::string, InterfaceInfo> current_interfaces;
    
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == nullptr) continue;
        if (ifa->ifa_addr->sa_family != AF_INET) continue;
        
        struct sockaddr_in* sin = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr);
        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &sin->sin_addr, ip_str, INET_ADDRSTRLEN);
        
        InterfaceInfo info;
        info.name = ifa->ifa_name;
        info.ip = ip_str;
        if (ifa->ifa_netmask) {
            struct sockaddr_in* mask = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_netmask);
            char mask_str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &mask->sin_addr, mask_str, INET_ADDRSTRLEN);
            info.netmask = mask_str;
        }
        info.last_seen = get_current_time();
        current_interfaces[info.name] = info;
    }
    
    freeifaddrs(ifaddr);
    interface_info_ = current_interfaces;
#endif
}

void RunwayManager::refresh_interfaces() {
    std::map<std::string, InterfaceInfo> old_interfaces = interface_info_;
    discover_interfaces();
    
    // Log changes (defensive: check terminal before logging)
    for (const auto& pair : interface_info_) {
        if (old_interfaces.find(pair.first) == old_interfaces.end()) {
            // New interface
        }
    }
    
    for (const auto& pair : old_interfaces) {
        if (interface_info_.find(pair.first) == interface_info_.end()) {
            // Removed interface
        }
    }
}

std::vector<std::shared_ptr<Runway>> RunwayManager::discover_runways() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    std::vector<std::string> interfaces_to_use;
    if (std::find(interfaces_.begin(), interfaces_.end(), std::string("auto")) != interfaces_.end()) {
        for (const auto& pair : interface_info_) {
            interfaces_to_use.push_back(pair.first);
        }
    } else {
        for (const auto& iface : interfaces_) {
            if (interface_info_.find(iface) != interface_info_.end()) {
                interfaces_to_use.push_back(iface);
            }
        }
    }
    
    std::vector<std::shared_ptr<Runway>> runways;
    size_t runway_id_counter = 0;
    
    // Create direct runways (no upstream proxy)
    for (const auto& iface : interfaces_to_use) {
        const auto& info = interface_info_[iface];
        for (const auto& dns_server : dns_servers_) {
            std::ostringstream oss;
            oss << "direct_" << iface << "_" << dns_server->config.host << "_" << runway_id_counter++;
            std::string runway_id = oss.str();
            
            auto runway = std::make_shared<Runway>(
                runway_id, iface, info.ip, nullptr, dns_server);
            runways.push_back(runway);
            runways_[runway_id] = runway;
        }
    }
    
    // Create proxy runways (with upstream proxy)
    for (const auto& iface : interfaces_to_use) {
        const auto& info = interface_info_[iface];
        for (const auto& proxy : upstream_proxies_) {
            for (const auto& dns_server : dns_servers_) {
                std::ostringstream oss;
                oss << "proxy_" << iface << "_" << proxy->config.proxy_type 
                    << "_" << proxy->config.host << "_" << dns_server->config.host 
                    << "_" << runway_id_counter++;
                std::string runway_id = oss.str();
                
                auto runway = std::make_shared<Runway>(
                    runway_id, iface, info.ip, proxy, dns_server);
                runways.push_back(runway);
                runways_[runway_id] = runway;
            }
        }
    }
    
    return runways;
}

std::shared_ptr<Runway> RunwayManager::get_runway(const std::string& runway_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = runways_.find(runway_id);
    if (it != runways_.end()) {
        return it->second;
    }
    return nullptr;
}

std::vector<std::shared_ptr<Runway>> RunwayManager::get_all_runways() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::shared_ptr<Runway>> result;
    for (const auto& pair : runways_) {
        result.push_back(pair.second);
    }
    return result;
}

std::tuple<bool, bool, double> RunwayManager::test_runway_accessibility(
    const std::string& target, std::shared_ptr<Runway> runway, double timeout_secs) {
    
    // Resolve target if needed
    std::string resolved_ip;
    if (dns_resolver_->is_ip_address(target) || dns_resolver_->is_private_ip(target)) {
        resolved_ip = target;
    } else {
        auto result = dns_resolver_->resolve(target);
        if (result.first.empty()) {
            return std::make_tuple(false, false, 0.0);
        }
        resolved_ip = result.first;
    }
    
    // Test connection
    bool network_success = false;
    if (runway->upstream_proxy && runway->upstream_proxy->accessible) {
        network_success = test_proxy_connection(runway, resolved_ip, timeout_secs);
    } else {
        network_success = test_direct_connection(runway, resolved_ip, timeout_secs);
    }
    
    double response_time = 0.0; // Simplified
    bool user_success = network_success; // Simplified for now
    return std::make_tuple(network_success, user_success, response_time);
}

bool RunwayManager::test_direct_connection(
    std::shared_ptr<Runway> runway, const std::string& target_ip, double timeout_secs) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    if (interface_info_.find(runway->interface) == interface_info_.end()) {
        return false;
    }
    
    socket_t sock = network::create_tcp_socket();
    if (sock == network::INVALID_SOCKET_VALUE) {
        return false;
    }
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = static_cast<long>(timeout_secs);
    timeout.tv_usec = static_cast<long>((timeout_secs - timeout.tv_sec) * 1000000);
    
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    
    bool success = network::connect_socket(sock, target_ip, 80);
    network::close_socket(sock);
    return success;
}

bool RunwayManager::test_proxy_connection(
    std::shared_ptr<Runway> runway, const std::string& /*target_ip*/, double timeout_secs) {
    
    if (!runway->upstream_proxy || !runway->upstream_proxy->accessible) {
        return false;
    }
    
    // Simplified proxy test - would need full HTTP CONNECT or proxy protocol
    // For now, just test if we can connect to the proxy
    socket_t sock = network::create_tcp_socket();
    if (sock == network::INVALID_SOCKET_VALUE) {
        return false;
    }
    
    struct timeval timeout;
    timeout.tv_sec = static_cast<long>(timeout_secs);
    timeout.tv_usec = static_cast<long>((timeout_secs - timeout.tv_sec) * 1000000);
    
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    
    bool success = network::connect_socket(sock, 
                                           runway->upstream_proxy->config.host,
                                           runway->upstream_proxy->config.port);
    network::close_socket(sock);
    return success;
}
