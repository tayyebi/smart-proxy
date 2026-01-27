#ifndef CONFIG_H
#define CONFIG_H

#include <string>
#include <vector>
#include <memory>
#include <map>

// JSON config parser (manual, RFC 7159 compliant)
// Reference: RFC 7159 - The JavaScript Object Notation (JSON) Data Interchange Format

enum class RoutingMode {
    Latency,
    FirstAccessible,
    RoundRobin
};

struct DNSServerConfig {
    std::string host;
    uint16_t port;
    std::string name;
    
    DNSServerConfig() : port(53) {}
};

struct UpstreamProxyConfig {
    std::string proxy_type; // http, https, socks4, socks5
    std::string host;
    uint16_t port;
    
    UpstreamProxyConfig() : port(0) {}
};

struct Config {
        // Save config to file as JSON
        bool save(const std::string& path) const;
    RoutingMode routing_mode;
    std::vector<DNSServerConfig> dns_servers;
    std::vector<UpstreamProxyConfig> upstream_proxies;
    std::vector<std::string> interfaces;
    uint64_t health_check_interval;
    uint64_t accessibility_timeout;
    double dns_timeout;
    uint64_t network_timeout;
    uint64_t user_validation_timeout;
    size_t max_concurrent_connections;
    size_t max_connections_per_runway;
    double success_rate_threshold;
    size_t success_rate_window;
    std::string log_level;
    std::string log_file;
    uint64_t log_max_bytes;
    size_t log_backup_count;
    std::string proxy_listen_host;
    uint16_t proxy_listen_port;
    bool mouse_enabled; // Enable mouse support in TUI
    bool webui_enabled; // Enable web UI server
    std::string webui_listen_host; // Web UI listen host
    uint16_t webui_listen_port; // Web UI listen port
    
    Config();
    static Config load(const std::string& path);
    static Config parse_json(const std::string& json_str);
    
private:
    // Simple JSON parser helpers
    static std::string skip_whitespace(const std::string& str, size_t& pos);
    static bool parse_string(const std::string& str, size_t& pos, std::string& result);
    static bool parse_number(const std::string& str, size_t& pos, double& result);
    static bool parse_boolean(const std::string& str, size_t& pos, bool& result);
    static bool parse_null(const std::string& str, size_t& pos);
    static bool parse_object(const std::string& str, size_t& pos, std::map<std::string, std::string>& obj);
    static bool parse_array(const std::string& str, size_t& pos, std::vector<std::string>& arr);
    static std::string unescape_string(const std::string& str);
};

#endif // CONFIG_H
