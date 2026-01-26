#ifndef RUNWAY_MANAGER_H
#define RUNWAY_MANAGER_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <mutex>
#include "runway.h"
#include "config.h"
#include "dns.h"

// Interface discovery and runway management
// POSIX: getifaddrs() (Linux/Unix)
// Windows: GetAdaptersAddresses() (Windows API)

struct InterfaceInfo {
    std::string name;
    std::string ip;
    std::string netmask;
    uint64_t last_seen; // Unix timestamp
    
    InterfaceInfo() : last_seen(0) {}
};

class RunwayManager {
public:
    RunwayManager(const std::vector<std::string>& interfaces,
                  const std::vector<UpstreamProxyConfig>& upstream_proxies,
                  const std::vector<DNSServerConfig>& dns_servers,
                  std::shared_ptr<DNSResolver> dns_resolver);
    
    ~RunwayManager();
    
    // Discover available network interfaces
    void discover_interfaces();
    
    // Refresh interface information
    void refresh_interfaces();
    
    // Discover all possible runway combinations
    std::vector<std::shared_ptr<Runway>> discover_runways();
    
    // Get runway by ID
    std::shared_ptr<Runway> get_runway(const std::string& runway_id);
    
    // Get all runways
    std::vector<std::shared_ptr<Runway>> get_all_runways();
    
    // Test runway accessibility
    // Returns (network_success, user_success, response_time_secs)
    std::tuple<bool, bool, double> test_runway_accessibility(
        const std::string& target, std::shared_ptr<Runway> runway, double timeout_secs);
    
private:
    std::vector<std::string> interfaces_;
    std::vector<std::shared_ptr<UpstreamProxy>> upstream_proxies_;
    std::vector<std::shared_ptr<DNSServer>> dns_servers_;
    std::shared_ptr<DNSResolver> dns_resolver_;
    std::map<std::string, std::shared_ptr<Runway>> runways_;
    std::map<std::string, InterfaceInfo> interface_info_;
    std::mutex mutex_;
    
    uint64_t get_current_time() const;
    bool test_direct_connection(std::shared_ptr<Runway> runway, const std::string& target_ip, double timeout_secs);
    bool test_proxy_connection(std::shared_ptr<Runway> runway, const std::string& target_ip, double timeout_secs);
};

#endif // RUNWAY_MANAGER_H
