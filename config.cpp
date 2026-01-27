#include <iomanip>
// Helper: convert config to JSON string
static std::string config_to_json(const Config& config) {
    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"routing_mode\": \"";
    switch (config.routing_mode) {
        case RoutingMode::Latency: oss << "Latency"; break;
        case RoutingMode::FirstAccessible: oss << "FirstAccessible"; break;
        case RoutingMode::RoundRobin: oss << "RoundRobin"; break;
    }
    oss << "\",\n";
    oss << "  \"dns_servers\": [";
    for (size_t i = 0; i < config.dns_servers.size(); ++i) {
        const auto& dns = config.dns_servers[i];
        oss << "{\"host\":\"" << dns.host << "\",\"port\":" << dns.port << ",\"name\":\"" << dns.name << "\"}";
        if (i + 1 < config.dns_servers.size()) oss << ", ";
    }
    oss << "],\n";
    oss << "  \"upstream_proxies\": [";
    for (size_t i = 0; i < config.upstream_proxies.size(); ++i) {
        const auto& up = config.upstream_proxies[i];
        oss << "{\"proxy_type\":\"" << up.proxy_type << "\",\"host\":\"" << up.host << "\",\"port\":" << up.port << "}";
        if (i + 1 < config.upstream_proxies.size()) oss << ", ";
    }
    oss << "],\n";
    oss << "  \"interfaces\": [";
    for (size_t i = 0; i < config.interfaces.size(); ++i) {
        oss << "\"" << config.interfaces[i] << "\"";
        if (i + 1 < config.interfaces.size()) oss << ", ";
    }
    oss << "],\n";
    oss << "  \"health_check_interval\": " << config.health_check_interval << ",\n";
    oss << "  \"accessibility_timeout\": " << config.accessibility_timeout << ",\n";
    oss << "  \"dns_timeout\": " << config.dns_timeout << ",\n";
    oss << "  \"network_timeout\": " << config.network_timeout << ",\n";
    oss << "  \"user_validation_timeout\": " << config.user_validation_timeout << ",\n";
    oss << "  \"max_concurrent_connections\": " << config.max_concurrent_connections << ",\n";
    oss << "  \"max_connections_per_runway\": " << config.max_connections_per_runway << ",\n";
    oss << "  \"success_rate_threshold\": " << config.success_rate_threshold << ",\n";
    oss << "  \"success_rate_window\": " << config.success_rate_window << ",\n";
    oss << "  \"log_level\": \"" << config.log_level << "\",\n";
    oss << "  \"log_file\": \"" << config.log_file << "\",\n";
    oss << "  \"log_max_bytes\": " << config.log_max_bytes << ",\n";
    oss << "  \"log_backup_count\": " << config.log_backup_count << ",\n";
    oss << "  \"proxy_listen_host\": \"" << config.proxy_listen_host << "\",\n";
    oss << "  \"proxy_listen_port\": " << config.proxy_listen_port << ",\n";
    oss << "  \"mouse_enabled\": " << (config.mouse_enabled ? "true" : "false") << ",\n";
    oss << "  \"webui_enabled\": " << (config.webui_enabled ? "true" : "false") << ",\n";
    oss << "  \"webui_listen_host\": \"" << config.webui_listen_host << "\",\n";
    oss << "  \"webui_listen_port\": " << config.webui_listen_port << "\n";
    oss << "}";
    return oss.str();
}

bool Config::save(const std::string& path) const {
    std::ofstream out(path);
    if (!out.is_open()) return false;
    out << config_to_json(*this) << std::endl;
    return true;
}
#include "config.h"
#include "utils.h"
#include <fstream>
#include <sstream>
#include <cctype>
#include <algorithm>
#include <map>

// RFC 7159 - JSON Data Interchange Format
// This is a simplified parser for the config subset we need

Config::Config() 
    : routing_mode(RoutingMode::Latency)
    , health_check_interval(60)
    , accessibility_timeout(5)
    , dns_timeout(3.0)
    , network_timeout(10)
    , user_validation_timeout(15)
    , max_concurrent_connections(100)
    , max_connections_per_runway(10)
    , success_rate_threshold(0.5)
    , success_rate_window(10)
    , log_level("INFO")
    , log_file("logs/proxy.log")
    , log_max_bytes(10485760)
    , log_backup_count(5)
    , proxy_listen_host("127.0.0.1")
    , proxy_listen_port(2123)
    , mouse_enabled(false) // Disabled by default
    , webui_enabled(false) // Disabled by default
    , webui_listen_host("127.0.0.1")
    , webui_listen_port(8080)
{
    interfaces.push_back("auto");
}

Config Config::load(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) {
        return Config(); // Return default config
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return parse_json(buffer.str());
}

std::string Config::skip_whitespace(const std::string& str, size_t& pos) {
    while (pos < str.length() && std::isspace(static_cast<unsigned char>(str[pos]))) {
        pos++;
    }
    return str.substr(pos);
}

bool Config::parse_string(const std::string& str, size_t& pos, std::string& result) {
    if (pos >= str.length() || str[pos] != '"') return false;
    pos++; // Skip opening quote
    
    result.clear();
    while (pos < str.length()) {
        if (str[pos] == '"') {
            pos++; // Skip closing quote
            return true;
        }
        if (str[pos] == '\\' && pos + 1 < str.length()) {
            // Handle escape sequences (RFC 7159 Section 7)
            pos++;
            switch (str[pos]) {
                case '"': result += '"'; break;
                case '\\': result += '\\'; break;
                case '/': result += '/'; break;
                case 'b': result += '\b'; break;
                case 'f': result += '\f'; break;
                case 'n': result += '\n'; break;
                case 'r': result += '\r'; break;
                case 't': result += '\t'; break;
                case 'u': // Unicode escape (simplified - just skip)
                    if (pos + 4 < str.length()) pos += 4;
                    break;
                default: result += str[pos]; break;
            }
            pos++;
        } else {
            result += str[pos++];
        }
    }
    return false; // Unterminated string
}

bool Config::parse_number(const std::string& str, size_t& pos, double& result) {
    size_t start = pos;
    bool has_dot = false;
    bool has_e = false;
    
    if (pos < str.length() && str[pos] == '-') pos++;
    if (pos >= str.length() || !std::isdigit(static_cast<unsigned char>(str[pos]))) {
        return false;
    }
    
    while (pos < str.length()) {
        char c = str[pos];
        if (std::isdigit(static_cast<unsigned char>(c))) {
            pos++;
        } else if (c == '.' && !has_dot && !has_e) {
            has_dot = true;
            pos++;
        } else if ((c == 'e' || c == 'E') && !has_e) {
            has_e = true;
            pos++;
            if (pos < str.length() && (str[pos] == '+' || str[pos] == '-')) {
                pos++;
            }
        } else {
            break;
        }
    }
    
    std::string num_str = str.substr(start, pos - start);
    return utils::safe_str_to_double(num_str, result);
}

bool Config::parse_boolean(const std::string& str, size_t& pos, bool& result) {
    if (str.substr(pos, 4) == "true") {
        result = true;
        pos += 4;
        return true;
    }
    if (str.substr(pos, 5) == "false") {
        result = false;
        pos += 5;
        return true;
    }
    return false;
}

bool Config::parse_null(const std::string& str, size_t& pos) {
    if (str.substr(pos, 4) == "null") {
        pos += 4;
        return true;
    }
    return false;
}

bool Config::parse_object(const std::string& str, size_t& pos, std::map<std::string, std::string>& obj) {
    skip_whitespace(str, pos);
    if (pos >= str.length() || str[pos] != '{') return false;
    pos++; // Skip '{'
    
    obj.clear();
    skip_whitespace(str, pos);
    
    if (pos < str.length() && str[pos] == '}') {
        pos++; // Empty object
        return true;
    }
    
    while (pos < str.length()) {
        skip_whitespace(str, pos);
        
        std::string key;
        if (!parse_string(str, pos, key)) return false;
        
        skip_whitespace(str, pos);
        if (pos >= str.length() || str[pos] != ':') return false;
        pos++; // Skip ':'
        
        skip_whitespace(str, pos);
        
        // Parse value as string representation (simplified)
        size_t value_start = pos;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        
        while (pos < str.length()) {
            char c = str[pos];
            if (escaped) {
                escaped = false;
                pos++;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                pos++;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                pos++;
                continue;
            }
            if (!in_string) {
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (depth == 0 && (c == ',' || c == '}')) {
                    break;
                }
            }
            pos++;
        }
        
        std::string value = str.substr(value_start, pos - value_start);
        obj[key] = utils::trim(value);
        
        skip_whitespace(str, pos);
        if (pos < str.length() && str[pos] == ',') {
            pos++;
            continue;
        }
        if (pos < str.length() && str[pos] == '}') {
            pos++;
            return true;
        }
    }
    
    return false;
}

bool Config::parse_array(const std::string& str, size_t& pos, std::vector<std::string>& arr) {
    skip_whitespace(str, pos);
    if (pos >= str.length() || str[pos] != '[') return false;
    pos++; // Skip '['
    
    arr.clear();
    skip_whitespace(str, pos);
    
    if (pos < str.length() && str[pos] == ']') {
        pos++; // Empty array
        return true;
    }
    
    while (pos < str.length()) {
        skip_whitespace(str, pos);
        
        size_t item_start = pos;
        int depth = 0;
        bool in_string = false;
        bool escaped = false;
        
        while (pos < str.length()) {
            char c = str[pos];
            if (escaped) {
                escaped = false;
                pos++;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                pos++;
                continue;
            }
            if (c == '"') {
                in_string = !in_string;
                pos++;
                continue;
            }
            if (!in_string) {
                if (c == '{' || c == '[') depth++;
                else if (c == '}' || c == ']') {
                    if (depth == 0) break;
                    depth--;
                } else if (depth == 0 && c == ',') {
                    break;
                }
            }
            pos++;
        }
        
        std::string item = str.substr(item_start, pos - item_start);
        arr.push_back(utils::trim(item));
        
        skip_whitespace(str, pos);
        if (pos < str.length() && str[pos] == ',') {
            pos++;
            continue;
        }
        if (pos < str.length() && str[pos] == ']') {
            pos++;
            return true;
        }
    }
    
    return false;
}

Config Config::parse_json(const std::string& json_str) {
    Config config;
    size_t pos = 0;
    std::map<std::string, std::string> root;
    
    if (!parse_object(json_str, pos, root)) {
        return config; // Return default on parse error
    }
    
    // Parse routing_mode
    if (root.find("routing_mode") != root.end()) {
        std::string mode = utils::to_lower(utils::trim(root["routing_mode"]));
        // Remove quotes if present
        if (mode.length() >= 2 && mode[0] == '"' && mode[mode.length()-1] == '"') {
            mode = mode.substr(1, mode.length() - 2);
        }
        if (mode == "latency") config.routing_mode = RoutingMode::Latency;
        else if (mode == "first_accessible") config.routing_mode = RoutingMode::FirstAccessible;
        else if (mode == "round_robin") config.routing_mode = RoutingMode::RoundRobin;
    }
    
    // Parse numeric fields
    if (root.find("health_check_interval") != root.end()) {
        uint64_t val;
        std::string s = utils::trim(root["health_check_interval"]);
        if (utils::safe_str_to_uint64(s, val)) config.health_check_interval = val;
    }
    if (root.find("accessibility_timeout") != root.end()) {
        uint64_t val;
        std::string s = utils::trim(root["accessibility_timeout"]);
        if (utils::safe_str_to_uint64(s, val)) config.accessibility_timeout = val;
    }
    if (root.find("dns_timeout") != root.end()) {
        double val;
        std::string s = utils::trim(root["dns_timeout"]);
        if (utils::safe_str_to_double(s, val)) config.dns_timeout = val;
    }
    if (root.find("network_timeout") != root.end()) {
        uint64_t val;
        std::string s = utils::trim(root["network_timeout"]);
        if (utils::safe_str_to_uint64(s, val)) config.network_timeout = val;
    }
    if (root.find("proxy_listen_port") != root.end()) {
        uint16_t val;
        std::string s = utils::trim(root["proxy_listen_port"]);
        if (utils::safe_str_to_uint16(s, val)) config.proxy_listen_port = val;
    }
    if (root.find("proxy_listen_host") != root.end()) {
        std::string host = utils::trim(root["proxy_listen_host"]);
        if (host.length() >= 2 && host[0] == '"' && host[host.length()-1] == '"') {
            config.proxy_listen_host = host.substr(1, host.length() - 2);
        }
    }
    
    // Parse mouse_enabled boolean
    if (root.find("mouse_enabled") != root.end()) {
        std::string val = utils::to_lower(utils::trim(root["mouse_enabled"]));
        // Remove quotes if present
        if (val.length() >= 2 && val[0] == '"' && val[val.length()-1] == '"') {
            val = val.substr(1, val.length() - 2);
        }
        config.mouse_enabled = (val == "true" || val == "1");
    }
    
    // Parse webui_enabled boolean
    if (root.find("webui_enabled") != root.end()) {
        std::string val = utils::to_lower(utils::trim(root["webui_enabled"]));
        // Remove quotes if present
        if (val.length() >= 2 && val[0] == '"' && val[val.length()-1] == '"') {
            val = val.substr(1, val.length() - 2);
        }
        config.webui_enabled = (val == "true" || val == "1");
    }
    
    // Parse webui_listen_host
    if (root.find("webui_listen_host") != root.end()) {
        std::string host = utils::trim(root["webui_listen_host"]);
        if (host.length() >= 2 && host[0] == '"' && host[host.length()-1] == '"') {
            config.webui_listen_host = host.substr(1, host.length() - 2);
        } else {
            config.webui_listen_host = host;
        }
    }
    
    // Parse webui_listen_port
    if (root.find("webui_listen_port") != root.end()) {
        uint16_t val;
        std::string s = utils::trim(root["webui_listen_port"]);
        if (utils::safe_str_to_uint16(s, val)) config.webui_listen_port = val;
    }
    
    // Parse arrays (simplified - would need full array parsing for nested objects)
    // For now, we'll parse dns_servers and upstream_proxies manually from the JSON string
    
    // Simple extraction of DNS servers
    size_t dns_start = json_str.find("\"dns_servers\"");
    if (dns_start != std::string::npos) {
        size_t arr_start = json_str.find('[', dns_start);
        if (arr_start != std::string::npos) {
            size_t arr_end = json_str.find(']', arr_start);
            if (arr_end != std::string::npos) {
                std::string dns_array = json_str.substr(arr_start + 1, arr_end - arr_start - 1);
                // Simple parsing: look for host/port pairs
                size_t host_pos = 0;
                while ((host_pos = dns_array.find("\"host\"", host_pos)) != std::string::npos) {
                    size_t colon = dns_array.find(':', host_pos);
                    if (colon != std::string::npos) {
                        size_t quote1 = dns_array.find('"', colon);
                        size_t quote2 = dns_array.find('"', quote1 + 1);
                        if (quote1 != std::string::npos && quote2 != std::string::npos) {
                            DNSServerConfig dns;
                            dns.host = dns_array.substr(quote1 + 1, quote2 - quote1 - 1);
                            dns.port = 53; // Default
                            
                            // Find port
                            size_t port_pos = dns_array.find("\"port\"", host_pos);
                            if (port_pos != std::string::npos && port_pos < quote2 + 100) {
                                size_t port_colon = dns_array.find(':', port_pos);
                                if (port_colon != std::string::npos) {
                                    uint16_t port_val;
                                    std::string port_str = utils::trim(dns_array.substr(port_colon + 1, 10));
                                    if (utils::safe_str_to_uint16(port_str, port_val)) {
                                        dns.port = port_val;
                                    }
                                }
                            }
                            config.dns_servers.push_back(dns);
                        }
                    }
                    host_pos++;
                }
            }
        }
    }
    
    // Similar parsing for upstream_proxies
    size_t proxy_start = json_str.find("\"upstream_proxies\"");
    if (proxy_start != std::string::npos) {
        size_t arr_start = json_str.find('[', proxy_start);
        if (arr_start != std::string::npos) {
            size_t arr_end = json_str.find(']', arr_start);
            if (arr_end != std::string::npos) {
                std::string proxy_array = json_str.substr(arr_start + 1, arr_end - arr_start - 1);
                size_t type_pos = 0;
                while ((type_pos = proxy_array.find("\"type\"", type_pos)) != std::string::npos) {
                    size_t colon = proxy_array.find(':', type_pos);
                    if (colon != std::string::npos) {
                        size_t quote1 = proxy_array.find('"', colon);
                        size_t quote2 = proxy_array.find('"', quote1 + 1);
                        if (quote1 != std::string::npos && quote2 != std::string::npos) {
                            UpstreamProxyConfig proxy;
                            proxy.proxy_type = proxy_array.substr(quote1 + 1, quote2 - quote1 - 1);
                            
                            // Find host
                            size_t host_pos = proxy_array.find("\"host\"", type_pos);
                            if (host_pos != std::string::npos && host_pos < quote2 + 200) {
                                size_t host_colon = proxy_array.find(':', host_pos);
                                if (host_colon != std::string::npos) {
                                    size_t hq1 = proxy_array.find('"', host_colon);
                                    size_t hq2 = proxy_array.find('"', hq1 + 1);
                                    if (hq1 != std::string::npos && hq2 != std::string::npos) {
                                        proxy.host = proxy_array.substr(hq1 + 1, hq2 - hq1 - 1);
                                    }
                                }
                            }
                            
                            // Find port
                            size_t port_pos = proxy_array.find("\"port\"", type_pos);
                            if (port_pos != std::string::npos && port_pos < quote2 + 200) {
                                size_t port_colon = proxy_array.find(':', port_pos);
                                if (port_colon != std::string::npos) {
                                    uint16_t port_val;
                                    std::string port_str = utils::trim(proxy_array.substr(port_colon + 1, 10));
                                    if (utils::safe_str_to_uint16(port_str, port_val)) {
                                        proxy.port = port_val;
                                    }
                                }
                            }
                            config.upstream_proxies.push_back(proxy);
                        }
                    }
                    type_pos++;
                }
            }
        }
    }
    
    // Parse interfaces array
    size_t iface_start = json_str.find("\"interfaces\"");
    if (iface_start != std::string::npos) {
        size_t arr_start = json_str.find('[', iface_start);
        if (arr_start != std::string::npos) {
            size_t arr_end = json_str.find(']', arr_start);
            if (arr_end != std::string::npos) {
                std::string iface_array = json_str.substr(arr_start + 1, arr_end - arr_start - 1);
                size_t quote_pos = 0;
                while ((quote_pos = iface_array.find('"', quote_pos)) != std::string::npos) {
                    size_t quote_end = iface_array.find('"', quote_pos + 1);
                    if (quote_end != std::string::npos) {
                        std::string iface = iface_array.substr(quote_pos + 1, quote_end - quote_pos - 1);
                        config.interfaces.push_back(iface);
                        quote_pos = quote_end + 1;
                    } else {
                        break;
                    }
                }
            }
        }
    }
    
    return config;
}
