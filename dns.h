#ifndef DNS_H
#define DNS_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <cstdint>
#include "config.h"
#include "network.h"

// DNS Resolver (RFC 1035 compliant)
// Reference: RFC 1035 - Domain Names - Implementation and Specification

struct DNSCacheEntry {
    std::string ip;
    uint64_t expiry_time; // Unix timestamp
    
    DNSCacheEntry() : expiry_time(0) {}
    DNSCacheEntry(const std::string& ip, uint64_t expiry)
        : ip(ip), expiry_time(expiry) {}
    
    bool is_expired(uint64_t current_time) const {
        return current_time >= expiry_time;
    }
};

class DNSResolver {
public:
    DNSResolver(const std::vector<DNSServerConfig>& servers, double timeout_secs);
    ~DNSResolver();
    
    // Check if target is already an IP address
    bool is_ip_address(const std::string& target) const;
    
    // Check if IP is private (RFC 1918)
    bool is_private_ip(const std::string& ip) const;
    
    // Resolve domain to IP address
    // Returns (ip_address, response_time_ms) or ("", 0.0) on failure
    std::pair<std::string, double> resolve(const std::string& domain);
    
private:
    std::vector<DNSServerConfig> servers_;
    double timeout_secs_;
    std::map<std::string, DNSCacheEntry> cache_;
    
    // Get current Unix timestamp
    uint64_t get_current_time() const;
    
    // Build DNS query packet (RFC 1035 Section 4.1.1)
    std::vector<uint8_t> build_dns_query(const std::string& domain) const;
    
    // Parse DNS response packet (RFC 1035 Section 4.1.3)
    bool parse_dns_response(const std::vector<uint8_t>& response, std::string& ip) const;
    
    // Encode domain name for DNS (RFC 1035 Section 3.1)
    void encode_domain_name(const std::string& domain, std::vector<uint8_t>& buffer) const;
    
    // Decode domain name from DNS response
    bool decode_domain_name(const std::vector<uint8_t>& response, size_t& pos, std::string& domain) const;
};

#endif // DNS_H
