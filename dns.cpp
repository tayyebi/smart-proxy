#include "dns.h"
#include "utils.h"
#include <cstring>
#include <algorithm>
#include <ctime>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

// RFC 1035 - Domain Names - Implementation and Specification

DNSResolver::DNSResolver(const std::vector<DNSServerConfig>& servers, double timeout_secs)
    : servers_(servers), timeout_secs_(timeout_secs) {
}

DNSResolver::~DNSResolver() {
}

uint64_t DNSResolver::get_current_time() const {
#ifdef _WIN32
    return static_cast<uint64_t>(time(nullptr));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
#endif
}

bool DNSResolver::is_ip_address(const std::string& target) const {
    return utils::is_valid_ipv4(target);
}

bool DNSResolver::is_private_ip(const std::string& ip) const {
    return utils::is_private_ip(ip);
}

void DNSResolver::encode_domain_name(const std::string& domain, std::vector<uint8_t>& buffer) const {
    // RFC 1035 Section 3.1 - Domain name encoding
    std::vector<std::string> labels = utils::split(domain, '.');
    for (const auto& label : labels) {
        if (label.length() > 63) return; // Invalid label length
        buffer.push_back(static_cast<uint8_t>(label.length()));
        for (char c : label) {
            buffer.push_back(static_cast<uint8_t>(c));
        }
    }
    buffer.push_back(0); // Null terminator
}

std::vector<uint8_t> DNSResolver::build_dns_query(const std::string& domain) const {
    // RFC 1035 Section 4.1.1 - Message format
    std::vector<uint8_t> packet;
    
    // Header (12 bytes)
    uint16_t id = static_cast<uint16_t>(time(nullptr) & 0xFFFF);
    packet.push_back((id >> 8) & 0xFF);
    packet.push_back(id & 0xFF);
    
    // Flags: Standard query, recursion desired
    packet.push_back(0x01); // QR=0, Opcode=0, AA=0, TC=0, RD=1
    packet.push_back(0x00); // RA=0, Z=0, RCODE=0
    
    // QDCOUNT: 1 question
    packet.push_back(0x00);
    packet.push_back(0x01);
    
    // ANCOUNT: 0 answers
    packet.push_back(0x00);
    packet.push_back(0x00);
    
    // NSCOUNT: 0 authority records
    packet.push_back(0x00);
    packet.push_back(0x00);
    
    // ARCOUNT: 0 additional records
    packet.push_back(0x00);
    packet.push_back(0x00);
    
    // Question section
    encode_domain_name(domain, packet);
    
    // QTYPE: A record (1)
    packet.push_back(0x00);
    packet.push_back(0x01);
    
    // QCLASS: IN (1)
    packet.push_back(0x00);
    packet.push_back(0x01);
    
    return packet;
}

bool DNSResolver::decode_domain_name(const std::vector<uint8_t>& response, size_t& pos, std::string& domain) const {
    domain.clear();
    size_t start_pos = pos;
    bool jumped = false;
    int jumps = 0;
    
    while (pos < response.size() && jumps < 10) { // Prevent infinite loops
        uint8_t len = response[pos++];
        
        if (len == 0) {
            break; // End of name
        }
        
        // Check for compression pointer (RFC 1035 Section 4.1.4)
        if ((len & 0xC0) == 0xC0) {
            if (pos >= response.size()) return false;
            uint16_t offset = ((len & 0x3F) << 8) | response[pos++];
            if (!jumped) {
                start_pos = pos; // Save position for return
            }
            pos = offset;
            jumped = true;
            jumps++;
            continue;
        }
        
        if (len > 63 || pos + len > response.size()) return false;
        
        if (!domain.empty()) domain += ".";
        domain.append(reinterpret_cast<const char*>(&response[pos]), len);
        pos += len;
    }
    
    if (jumped) {
        pos = start_pos; // Return to position after compression pointer
    }
    
    return true;
}

bool DNSResolver::parse_dns_response(const std::vector<uint8_t>& response, std::string& ip) const {
    // RFC 1035 Section 4.1.3 - Response format
    if (response.size() < 12) return false;
    
    // Check response code
    uint8_t rcode = response[3] & 0x0F;
    if (rcode != 0) return false; // Error response
    
    // Get answer count
    uint16_t ancount = (response[6] << 8) | response[7];
    if (ancount == 0) return false;
    
    // Skip question section
    size_t pos = 12;
    std::string domain;
    if (!decode_domain_name(response, pos, domain)) return false;
    pos += 4; // Skip QTYPE and QCLASS
    
    // Parse answer section
    for (uint16_t i = 0; i < ancount && pos < response.size(); ++i) {
        // Decode name (may be compressed)
        std::string name;
        if (!decode_domain_name(response, pos, name)) break;
        
        if (pos + 10 > response.size()) break;
        
        // Read TYPE, CLASS, TTL, RDLENGTH
        uint16_t type = (response[pos] << 8) | response[pos + 1];
        pos += 2;
        uint16_t class_val = (response[pos] << 8) | response[pos + 1];
        pos += 2;
        pos += 4; // Skip TTL
        uint16_t rdlength = (response[pos] << 8) | response[pos + 1];
        pos += 2;
        
        // Check if A record (type 1)
        if (type == 1 && class_val == 1 && rdlength == 4) {
            if (pos + 4 > response.size()) break;
            
            // Extract IP address
            char ip_str[16];
            snprintf(ip_str, sizeof(ip_str), "%d.%d.%d.%d",
                    response[pos], response[pos + 1],
                    response[pos + 2], response[pos + 3]);
            ip = ip_str;
            return true;
        }
        
        pos += rdlength; // Skip RDATA
    }
    
    return false;
}

std::pair<std::string, double> DNSResolver::resolve(const std::string& domain) {
    // Skip DNS for IP addresses
    if (is_ip_address(domain)) {
        return std::make_pair(domain, 0.0);
    }
    
    // Check cache
    uint64_t current_time = get_current_time();
    auto cache_it = cache_.find(domain);
    if (cache_it != cache_.end() && !cache_it->second.is_expired(current_time)) {
        return std::make_pair(cache_it->second.ip, 0.0);
    }
    
    // Build query packet
    std::vector<uint8_t> query = build_dns_query(domain);
    
    // Try each DNS server
    for (const auto& server : servers_) {
        socket_t sock = network::create_udp_socket();
        if (sock == network::INVALID_SOCKET_VALUE) continue;
        
        // Set timeout
        struct timeval timeout;
        timeout.tv_sec = static_cast<long>(timeout_secs_);
        timeout.tv_usec = static_cast<long>((timeout_secs_ - timeout.tv_sec) * 1000000);
        
#ifdef _WIN32
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
        
        // Send query
        struct sockaddr_in server_addr;
        if (!network::ip_to_sockaddr(server.host, server.port, server_addr)) {
            network::close_socket(sock);
            continue;
        }
        
        ssize_t sent = sendto(sock, reinterpret_cast<const char*>(query.data()), query.size(), 0,
                              reinterpret_cast<struct sockaddr*>(&server_addr), sizeof(server_addr));
        if (sent != static_cast<ssize_t>(query.size())) {
            network::close_socket(sock);
            continue;
        }
        
        // Receive response
        std::vector<uint8_t> response(512);
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        
        ssize_t received = recvfrom(sock, reinterpret_cast<char*>(response.data()), response.size(), 0,
                                    reinterpret_cast<struct sockaddr*>(&from_addr), &from_len);
        
        network::close_socket(sock);
        
        if (received > 0) {
            response.resize(static_cast<size_t>(received));
            std::string ip;
            if (parse_dns_response(response, ip)) {
                // Cache with TTL (default 300 seconds)
                uint64_t expiry = current_time + 300;
                cache_[domain] = DNSCacheEntry(ip, expiry);
                return std::make_pair(ip, 0.0); // Simplified timing
            }
        }
    }
    
    return std::make_pair("", 0.0);
}
