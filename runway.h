#ifndef RUNWAY_H
#define RUNWAY_H

#include <string>
#include <cstdint>
#include <memory>
#include "config.h"

enum class RunwayState {
    Unknown,
    Accessible,
    PartiallyAccessible,
    Inaccessible,
    Testing
};

struct DNSServer {
    DNSServerConfig config;
    double response_time;
    uint64_t last_success; // Unix timestamp
    uint32_t failure_count;
    
    DNSServer() : response_time(0.0), last_success(0), failure_count(0) {}
    DNSServer(const DNSServerConfig& cfg) 
        : config(cfg), response_time(0.0), last_success(0), failure_count(0) {}
};

struct UpstreamProxy {
    UpstreamProxyConfig config;
    bool accessible;
    uint64_t last_success; // Unix timestamp
    uint32_t failure_count;
    
    UpstreamProxy() : accessible(true), last_success(0), failure_count(0) {}
    UpstreamProxy(const UpstreamProxyConfig& cfg)
        : config(cfg), accessible(true), last_success(0), failure_count(0) {}
};

struct Runway {
    std::string id;
    std::string interface;
    std::string source_ip; // IPv4 address as string
    std::shared_ptr<UpstreamProxy> upstream_proxy;
    std::shared_ptr<DNSServer> dns_server;
    std::string resolved_ip; // Resolved target IP
    bool is_direct;
    
    Runway() : is_direct(true) {}
    Runway(const std::string& id, const std::string& interface, 
           const std::string& source_ip,
           std::shared_ptr<UpstreamProxy> proxy,
           std::shared_ptr<DNSServer> dns)
        : id(id), interface(interface), source_ip(source_ip),
          upstream_proxy(proxy), dns_server(dns),
          is_direct(proxy == nullptr) {}
    
    bool operator==(const Runway& other) const {
        return id == other.id;
    }
};

// Hash function for Runway (for use in unordered_map)
struct RunwayHash {
    std::size_t operator()(const Runway& r) const {
        return std::hash<std::string>()(r.id);
    }
};

#endif // RUNWAY_H
