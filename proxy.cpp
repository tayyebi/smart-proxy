#include "proxy.h"
#include "utils.h"
#include "logger.h"
#include <sstream>
#include <algorithm>
#include <ctime>
#include <mutex>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/socket.h>
#endif

// RFC 7230 - HTTP/1.1 Message Syntax and Routing
// RFC 7231 - HTTP/1.1 Semantics and Content

ProxyServer::ProxyServer(
    const Config& config,
    std::shared_ptr<RunwayManager> runway_manager,
    std::shared_ptr<RoutingEngine> routing_engine,
    std::shared_ptr<TargetAccessibilityTracker> tracker,
    std::shared_ptr<DNSResolver> dns_resolver,
    std::shared_ptr<SuccessValidator> validator)
    : config_(config)
    , runway_manager_(runway_manager)
    , routing_engine_(routing_engine)
    , tracker_(tracker)
    , dns_resolver_(dns_resolver)
    , validator_(validator)
    , listen_socket_(network::INVALID_SOCKET_VALUE)
    , running_(false)
    , active_connections_(0)
    , total_connections_(0)
    , total_bytes_sent_(0)
    , total_bytes_received_(0) {
}

ProxyServer::~ProxyServer() {
    stop();
}

bool ProxyServer::start() {
    if (running_) {
        return false;
    }
    
    if (!network::init()) {
        return false;
    }
    
    listen_socket_ = network::create_tcp_socket();
    if (listen_socket_ == network::INVALID_SOCKET_VALUE) {
        return false;
    }
    
    // Set socket options
    network::set_socket_option(listen_socket_, SOL_SOCKET, SO_REUSEADDR, 1);
    
    // Bind to address
    if (!network::bind_socket(listen_socket_, config_.proxy_listen_host, config_.proxy_listen_port)) {
        network::close_socket(listen_socket_);
        listen_socket_ = network::INVALID_SOCKET_VALUE;
        return false;
    }
    
    // Listen
    if (!network::listen_socket(listen_socket_, 128)) {
        network::close_socket(listen_socket_);
        listen_socket_ = network::INVALID_SOCKET_VALUE;
        return false;
    }
    
    running_ = true;
    server_thread_ = std::thread(&ProxyServer::server_loop, this);
    
    return true;
}

void ProxyServer::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (listen_socket_ != network::INVALID_SOCKET_VALUE) {
        network::close_socket(listen_socket_);
        listen_socket_ = network::INVALID_SOCKET_VALUE;
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void ProxyServer::server_loop() {
    while (running_) {
        std::string client_ip;
        uint16_t client_port;
        socket_t client_sock = network::accept_connection(listen_socket_, client_ip, client_port);
        
        if (client_sock == network::INVALID_SOCKET_VALUE) {
            if (running_) {
                // Error accepting connection - check if it's because server is stopping
                // Small delay to prevent busy loop
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            } else {
                break; // Server stopped
            }
        }
        
        // Handle connection in new thread
        std::thread([this, client_sock]() {
            handle_connection(client_sock);
            network::close_socket(client_sock);
        }).detach();
    }
}

bool ProxyServer::read_line(socket_t sock, std::string& line, size_t max_length) {
    line.clear();
    char c;
    size_t length = 0;
    
    while (length < max_length) {
        ssize_t received = network::recv_data(sock, &c, 1);
        if (received <= 0) {
            return false;
        }
        
        if (c == '\r') {
            // Check for \r\n
            received = network::recv_data(sock, &c, 1);
            if (received > 0 && c == '\n') {
                return true; // End of line
            }
            if (received > 0) {
                line += '\r';
                line += c;
                length += 2;
            }
        } else if (c == '\n') {
            return true; // End of line
        } else {
            line += c;
            length++;
        }
    }
    
    return false; // Max length exceeded
}

bool ProxyServer::read_headers(socket_t sock, std::map<std::string, std::string>& headers, size_t max_headers) {
    headers.clear();
    size_t header_count = 0;
    
    while (header_count < max_headers) {
        std::string line;
        if (!read_line(sock, line)) {
            return false;
        }
        
        if (line.empty()) {
            return true; // End of headers
        }
        
        // Parse header line: "Name: Value"
        size_t colon_pos = line.find(':');
        if (colon_pos == std::string::npos) {
            continue; // Invalid header, skip
        }
        
        std::string name = utils::trim(line.substr(0, colon_pos));
        std::string value = utils::trim(line.substr(colon_pos + 1));
        
        // Convert header name to lowercase (RFC 7230 Section 3.2)
        name = utils::to_lower(name);
        
        headers[name] = value;
        header_count++;
    }
    
    return true;
}

bool ProxyServer::read_body(socket_t sock, std::vector<uint8_t>& body,
                            const std::map<std::string, std::string>& headers, size_t max_size) {
    body.clear();
    
    // Check for Content-Length
    auto content_length_it = headers.find("content-length");
    if (content_length_it != headers.end()) {
        uint64_t content_length;
        if (utils::safe_str_to_uint64(content_length_it->second, content_length)) {
            if (content_length > max_size) {
                return false; // Body too large
            }
            
            body.resize(static_cast<size_t>(content_length));
            size_t total_received = 0;
            
            while (total_received < body.size()) {
                ssize_t received = network::recv_data(sock, 
                    body.data() + total_received, 
                    body.size() - total_received);
                if (received <= 0) {
                    return false;
                }
                total_received += static_cast<size_t>(received);
            }
            
            return true;
        }
    }
    
    // Check for Transfer-Encoding: chunked (RFC 7230 Section 4.1)
    auto transfer_encoding_it = headers.find("transfer-encoding");
    if (transfer_encoding_it != headers.end() && 
        utils::to_lower(transfer_encoding_it->second).find("chunked") != std::string::npos) {
        
        // Simplified chunked encoding parser
        while (body.size() < max_size) {
            std::string chunk_size_line;
            if (!read_line(sock, chunk_size_line)) {
                return false;
            }
            
            // Parse chunk size (hexadecimal)
            size_t chunk_size = 0;
            std::istringstream iss(chunk_size_line);
            iss >> std::hex >> chunk_size;
            
            if (chunk_size == 0) {
                // Last chunk, read trailing CRLF
                read_line(sock, chunk_size_line);
                return true;
            }
            
            if (body.size() + chunk_size > max_size) {
                return false; // Would exceed max size
            }
            
            size_t old_size = body.size();
            body.resize(old_size + chunk_size);
            
            size_t total_received = 0;
            while (total_received < chunk_size) {
                ssize_t received = network::recv_data(sock,
                    body.data() + old_size + total_received,
                    chunk_size - total_received);
                if (received <= 0) {
                    return false;
                }
                total_received += static_cast<size_t>(received);
            }
            
            // Read chunk trailing CRLF
            read_line(sock, chunk_size_line);
        }
        
        return false; // Max size exceeded
    }
    
    // No body
    return true;
}

bool ProxyServer::parse_http_request(socket_t sock, HTTPRequest& request) {
    // RFC 7230 Section 3.1.1 - Request Line
    std::string request_line;
    if (!read_line(sock, request_line)) {
        return false;
    }
    
    // Parse: "METHOD PATH VERSION"
    std::vector<std::string> parts = utils::split(request_line, ' ');
    if (parts.size() < 3) {
        return false;
    }
    
    request.method = parts[0];
    request.path = parts[1];
    request.version = parts[2];
    
    // Read headers
    if (!read_headers(sock, request.headers)) {
        return false;
    }
    
    // Read body if present
    if (!read_body(sock, request.body, request.headers)) {
        return false;
    }
    
    return true;
}

std::vector<uint8_t> ProxyServer::build_http_response(const HTTPResponse& response) {
    std::ostringstream oss;
    
    // Status line (RFC 7230 Section 3.1.2)
    oss << response.version << " " << response.status_code << " " << response.status_text << "\r\n";
    
    // Headers
    for (const auto& pair : response.headers) {
        oss << pair.first << ": " << pair.second << "\r\n";
    }
    
    // End of headers
    oss << "\r\n";
    
    std::string header_str = oss.str();
    std::vector<uint8_t> result(header_str.begin(), header_str.end());
    
    // Append body
    result.insert(result.end(), response.body.begin(), response.body.end());
    
    return result;
}

size_t ProxyServer::get_active_connections() const {
    return active_connections_.load();
}

uint64_t ProxyServer::get_total_connections() const {
    return total_connections_.load();
}

uint64_t ProxyServer::get_total_bytes_sent() const {
    return total_bytes_sent_.load();
}

uint64_t ProxyServer::get_total_bytes_received() const {
    return total_bytes_received_.load();
}

std::vector<std::map<std::string, std::string>> ProxyServer::get_active_connections_info() const {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::vector<std::map<std::string, std::string>> result;
    
    uint64_t now = std::time(nullptr);
    
    for (const auto& pair : active_connections_map_) {
        std::map<std::string, std::string> conn_info = pair.second;
        conn_info["id"] = pair.first;
        
        // Calculate live duration
        if (conn_info.find("start_time") != conn_info.end()) {
            uint64_t start_time = 0;
            utils::safe_str_to_uint64(conn_info["start_time"], start_time);
            if (start_time > 0) {
                uint64_t duration = now - start_time;
                conn_info["duration"] = std::to_string(duration);
            }
        }
        
        result.push_back(conn_info);
    }
    
    return result;
}

void ProxyServer::handle_connection(socket_t client_sock) {
    std::string client_ip;
    uint16_t client_port = 0;
    network::get_peer_address(client_sock, client_ip, client_port);
    
    uint64_t conn_start_time = std::time(nullptr);
    std::string conn_id = client_ip + ":" + std::to_string(client_port) + "-" + std::to_string(conn_start_time);
    
    active_connections_++;
    total_connections_++;
    
    // Add to active connections map for live tracking
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_map_[conn_id]["client_ip"] = client_ip;
        active_connections_map_[conn_id]["client_port"] = std::to_string(client_port);
        active_connections_map_[conn_id]["start_time"] = std::to_string(conn_start_time);
        active_connections_map_[conn_id]["status"] = "connecting";
        active_connections_map_[conn_id]["bytes_sent"] = "0";
        active_connections_map_[conn_id]["bytes_received"] = "0";
    }
    
    // Set socket timeouts to prevent hanging
    struct timeval timeout;
    timeout.tv_sec = static_cast<long>(config_.network_timeout);
    timeout.tv_usec = 0;
    
#ifdef _WIN32
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    
    ConnectionLog conn_log;
    conn_log.timestamp = conn_start_time;
    conn_log.level = "INFO";
    conn_log.event = "connect";
    conn_log.client_ip = client_ip;
    conn_log.client_port = client_port;
    
    // Protocol detection: peek at first byte to detect SOCKS5 vs HTTP
    // SOCKS5 starts with 0x05, HTTP starts with ASCII letters (GET, POST, CONNECT, etc.)
    uint8_t first_byte = 0;
#ifdef _WIN32
    ssize_t peeked = recv(client_sock, reinterpret_cast<char*>(&first_byte), 1, MSG_PEEK);
#else
    ssize_t peeked = recv(client_sock, &first_byte, 1, MSG_PEEK);
#endif
    
    if (peeked <= 0) {
        // Connection closed or error
        conn_log.event = "error";
        conn_log.error = "Connection closed before protocol detection";
        conn_log.duration_ms = (std::time(nullptr) - conn_start_time) * 1000.0;
        Logger::instance().log_connection(conn_log);
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_map_.erase(conn_id);
        }
        active_connections_--;
        return;
    }
    
    // Check if it's SOCKS5 (starts with 0x05)
    if (first_byte == 0x05) {
        // SOCKS5 protocol - reject with proper error
        conn_log.event = "error";
        conn_log.error = "SOCKS5 protocol not supported (HTTP proxy only)";
        conn_log.duration_ms = (std::time(nullptr) - conn_start_time) * 1000.0;
        Logger::instance().log_connection(conn_log);
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_map_[conn_id]["status"] = "error";
            active_connections_map_[conn_id]["error"] = "SOCKS5 not supported";
        }
        
        // Send SOCKS5 error response (RFC 1928)
        // Version (1 byte) + Method (1 byte) = 0x05 0xFF (no acceptable methods)
        uint8_t socks5_reject[2] = {0x05, 0xFF};
        network::send_data(client_sock, reinterpret_cast<const char*>(socks5_reject), 2);
        
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_map_.erase(conn_id);
        }
        active_connections_--;
        return;
    }
    
    // Assume HTTP - parse request
    HTTPRequest request;
    if (!parse_http_request(client_sock, request)) {
        conn_log.event = "error";
        conn_log.error = "Failed to parse HTTP request";
        conn_log.duration_ms = (std::time(nullptr) - conn_start_time) * 1000.0;
        Logger::instance().log_connection(conn_log);
        
        // Remove from active connections
        {
            std::lock_guard<std::mutex> lock(connections_mutex_);
            active_connections_map_.erase(conn_id);
        }
        active_connections_--;
        // Send error response
        HTTPResponse error_response;
        error_response.status_code = 400;
        error_response.status_text = "Bad Request";
        error_response.headers["Content-Length"] = "0";
        std::vector<uint8_t> response_data = build_http_response(error_response);
        network::send_data(client_sock, response_data.data(), response_data.size());
        return;
    }
    
    // Extract target from request
    std::string target_host;
    uint16_t target_port = 80;
    
    if (request.method == "CONNECT") {
        // CONNECT method (RFC 7231 Section 4.3.6)
        auto host_it = request.headers.find("host");
        if (host_it == request.headers.end()) {
            HTTPResponse error_response;
            error_response.status_code = 400;
            error_response.status_text = "Bad Request";
            error_response.headers["Content-Length"] = "0";
            std::vector<uint8_t> response_data = build_http_response(error_response);
            network::send_data(client_sock, response_data.data(), response_data.size());
            return;
        }
        
        std::vector<std::string> host_parts = utils::split(host_it->second, ':');
        target_host = host_parts[0];
        if (host_parts.size() > 1) {
            utils::safe_str_to_uint16(host_parts[1], target_port);
        } else {
            target_port = 443;
        }
        
        // CONNECT not fully implemented
        HTTPResponse error_response;
        error_response.status_code = 501;
        error_response.status_text = "Not Implemented";
        error_response.headers["Content-Length"] = "0";
        std::vector<uint8_t> response_data = build_http_response(error_response);
        network::send_data(client_sock, response_data.data(), response_data.size());
        return;
    } else {
        // Extract from path or Host header
        auto host_it = request.headers.find("host");
        if (host_it != request.headers.end()) {
            std::vector<std::string> host_parts = utils::split(host_it->second, ':');
            target_host = host_parts[0];
            if (host_parts.size() > 1) {
                utils::safe_str_to_uint16(host_parts[1], target_port);
            }
        } else {
            // Try to extract from path
            if (request.path.find("http://") == 0) {
                // Parse URL
                size_t host_start = 7; // Skip "http://"
                size_t host_end = request.path.find('/', host_start);
                if (host_end == std::string::npos) {
                    host_end = request.path.find(':', host_start);
                }
                if (host_end != std::string::npos) {
                    target_host = request.path.substr(host_start, host_end - host_start);
                }
            }
        }
    }
    
    if (target_host.empty()) {
        conn_log.event = "error";
        conn_log.error = "No target host specified";
        conn_log.duration_ms = (std::time(nullptr) - conn_start_time) * 1000.0;
        Logger::instance().log_connection(conn_log);
        
        HTTPResponse error_response;
        error_response.status_code = 400;
        error_response.status_text = "Bad Request";
        error_response.headers["Content-Length"] = "0";
        std::vector<uint8_t> response_data = build_http_response(error_response);
        network::send_data(client_sock, response_data.data(), response_data.size());
        active_connections_--;
        return;
    }
    
    conn_log.target_host = target_host;
    conn_log.target_port = target_port;
    conn_log.method = request.method;
    conn_log.path = request.path;
    
    // Update active connection info
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = active_connections_map_.find(conn_id);
        if (it != active_connections_map_.end()) {
            it->second["target_host"] = target_host;
            it->second["target_port"] = std::to_string(target_port);
            it->second["method"] = request.method;
            it->second["path"] = request.path;
            it->second["status"] = "active";
        }
    }
    
    // Select runway
    auto all_runways = runway_manager_->get_all_runways();
    auto runway = routing_engine_->select_runway(target_host, all_runways);
    
    if (!runway) {
        // Test all runways
        runway = test_all_runways(target_host, all_runways);
    }
    
    if (!runway) {
        conn_log.event = "error";
        conn_log.error = "No accessible runway found";
        conn_log.duration_ms = (std::time(nullptr) - conn_start_time) * 1000.0;
        Logger::instance().log_connection(conn_log);
        
        HTTPResponse error_response;
        error_response.status_code = 502;
        error_response.status_text = "Bad Gateway";
        error_response.headers["Content-Length"] = "0";
        std::vector<uint8_t> response_data = build_http_response(error_response);
        network::send_data(client_sock, response_data.data(), response_data.size());
        active_connections_--;
        return;
    }
    
    conn_log.runway_id = runway->id;
    
    // Update runway in active connection
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = active_connections_map_.find(conn_id);
        if (it != active_connections_map_.end()) {
            it->second["runway_id"] = runway->id;
        }
    }
    
    // Make request through runway
    const size_t max_retries = 2;
    for (size_t attempt = 0; attempt < max_retries; ++attempt) {
        auto result = make_http_request(request, target_host, target_port, runway);
        bool network_success = std::get<0>(result);
        bool user_success = std::get<1>(result);
        uint16_t status = std::get<2>(result);
        auto& response_headers = std::get<3>(result);
        auto& response_body = std::get<4>(result);
        
        // Update tracker
        double response_time = 0.0; // Simplified
        tracker_->update(target_host, runway->id, network_success, user_success, response_time);
        
        if (network_success) {
            // Send response to client
            HTTPResponse http_response;
            http_response.status_code = status;
            http_response.status_text = (status == 200) ? "OK" : "Error";
            http_response.headers = response_headers;
            http_response.body = response_body;
            http_response.headers["Content-Length"] = std::to_string(response_body.size());
            
            std::vector<uint8_t> response_data = build_http_response(http_response);
            size_t sent = network::send_data(client_sock, response_data.data(), response_data.size());
            
            uint64_t conn_end_time = std::time(nullptr);
            double duration = (conn_end_time - conn_start_time) * 1000.0;
            
            // Update final connection stats before removing
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                auto it = active_connections_map_.find(conn_id);
                if (it != active_connections_map_.end()) {
                    it->second["bytes_sent"] = std::to_string(sent);
                    it->second["bytes_received"] = std::to_string(request.body.size());
                    it->second["status"] = "completed";
                    it->second["status_code"] = std::to_string(status);
                }
            }
            
            conn_log.event = "disconnect";
            conn_log.status_code = status;
            conn_log.bytes_sent = sent;
            conn_log.bytes_received = request.body.size();
            conn_log.duration_ms = duration;
            Logger::instance().log_connection(conn_log);
            
            total_bytes_sent_ += sent;
            total_bytes_received_ += request.body.size();
            
            // Remove from active connections
            {
                std::lock_guard<std::mutex> lock(connections_mutex_);
                active_connections_map_.erase(conn_id);
            }
            active_connections_--;
            return;
        } else if (attempt < max_retries - 1) {
            // Try alternative runway
            auto alt_runway = get_alternative_runway(target_host, runway->id);
            if (alt_runway) {
                runway = alt_runway;
                continue;
            }
        }
    }
    
    // All attempts failed
    uint64_t conn_end_time = std::time(nullptr);
    double duration = (conn_end_time - conn_start_time) * 1000.0;
    
    // Update connection as failed
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = active_connections_map_.find(conn_id);
        if (it != active_connections_map_.end()) {
            it->second["status"] = "error";
            it->second["error"] = "All runway attempts failed";
        }
    }
    
    conn_log.event = "error";
    conn_log.error = "All runway attempts failed";
    conn_log.status_code = 502;
    conn_log.duration_ms = duration;
    Logger::instance().log_connection(conn_log);
    
    HTTPResponse error_response;
    error_response.status_code = 502;
    error_response.status_text = "Bad Gateway";
    error_response.headers["Content-Length"] = "0";
    std::vector<uint8_t> response_data = build_http_response(error_response);
    network::send_data(client_sock, response_data.data(), response_data.size());
    
    // Remove from active connections
    {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        active_connections_map_.erase(conn_id);
    }
    active_connections_--;
}

std::tuple<bool, bool, uint16_t, std::map<std::string, std::string>, std::vector<uint8_t>>
ProxyServer::make_http_request(const HTTPRequest& request, const std::string& target_host,
                               uint16_t target_port, std::shared_ptr<Runway> /*runway*/) {
    // Resolve target
    std::string resolved_ip;
    if (dns_resolver_->is_ip_address(target_host) || dns_resolver_->is_private_ip(target_host)) {
        resolved_ip = target_host;
    } else {
        auto dns_result = dns_resolver_->resolve(target_host);
        if (dns_result.first.empty()) {
            return std::make_tuple(false, false, 502, 
                                  std::map<std::string, std::string>(), 
                                  std::vector<uint8_t>());
        }
        resolved_ip = dns_result.first;
    }
    
    // Connect to target
    socket_t sock = network::create_tcp_socket();
    if (sock == network::INVALID_SOCKET_VALUE) {
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = static_cast<long>(config_.network_timeout);
    timeout.tv_usec = 0;
    
#ifdef _WIN32
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
    
    bool connected = network::connect_socket(sock, resolved_ip, target_port);
    if (!connected) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    // Build request
    std::ostringstream request_oss;
    request_oss << request.method << " " << request.path << " " << request.version << "\r\n";
    
    // Copy headers (remove hop-by-hop headers, RFC 7230 Section 6.1)
    for (const auto& pair : request.headers) {
        std::string name = utils::to_lower(pair.first);
        if (name != "host" && name != "connection" && name != "proxy-connection") {
            request_oss << pair.first << ": " << pair.second << "\r\n";
        }
    }
    request_oss << "Host: " << target_host;
    if (target_port != 80 && target_port != 443) {
        request_oss << ":" << target_port;
    }
    request_oss << "\r\n";
    request_oss << "\r\n";
    
    std::string request_str = request_oss.str();
    network::send_data(sock, request_str.data(), request_str.size());
    
    if (!request.body.empty()) {
        network::send_data(sock, request.body.data(), request.body.size());
    }
    
    // Read response
    std::string status_line;
    if (!read_line(sock, status_line)) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    // Parse status line: "HTTP/1.1 200 OK"
    std::vector<std::string> status_parts = utils::split(status_line, ' ');
    if (status_parts.size() < 3) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    uint16_t status_code;
    if (!utils::safe_str_to_uint16(status_parts[1], status_code)) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    // Read headers
    std::map<std::string, std::string> response_headers;
    if (!read_headers(sock, response_headers)) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    // Read body
    std::vector<uint8_t> response_body;
    if (!read_body(sock, response_body, response_headers)) {
        network::close_socket(sock);
        return std::make_tuple(false, false, 502,
                              std::map<std::string, std::string>(),
                              std::vector<uint8_t>());
    }
    
    network::close_socket(sock);
    
    // Validate response
    bool network_success = (status_code >= 200 && status_code < 400);
    bool user_success = false;
    if (network_success) {
        auto validation = validator_->validate_http(status_code, response_body);
        user_success = validation.second;
    }
    
    return std::make_tuple(network_success, user_success, status_code,
                          response_headers, response_body);
}

std::shared_ptr<Runway> ProxyServer::test_all_runways(
    const std::string& target,
    const std::vector<std::shared_ptr<Runway>>& runways) {
    
    // Prioritize direct runways
    std::vector<std::shared_ptr<Runway>> direct_runways;
    std::vector<std::shared_ptr<Runway>> proxy_runways;
    
    for (const auto& runway : runways) {
        if (runway->is_direct) {
            direct_runways.push_back(runway);
        } else {
            proxy_runways.push_back(runway);
        }
    }
    
    std::vector<std::shared_ptr<Runway>> prioritized;
    prioritized.insert(prioritized.end(), direct_runways.begin(), direct_runways.end());
    prioritized.insert(prioritized.end(), proxy_runways.begin(), proxy_runways.end());
    
    // Test runways
    for (const auto& runway : prioritized) {
        auto result = runway_manager_->test_runway_accessibility(target, runway, static_cast<double>(config_.accessibility_timeout));
        bool net_success = std::get<0>(result);
        bool user_success = std::get<1>(result);
        double response_time = std::get<2>(result);
        
        tracker_->update(target, runway->id, net_success, user_success, response_time);
        
        if (user_success) {
            return runway;
        }
    }
    
    return nullptr;
}

std::shared_ptr<Runway> ProxyServer::get_alternative_runway(
    const std::string& target, const std::string& current_runway_id) {
    
    std::vector<std::string> accessible_ids = tracker_->get_accessible_runways(target);
    
    for (const auto& id : accessible_ids) {
        if (id != current_runway_id) {
            return runway_manager_->get_runway(id);
        }
    }
    
    return nullptr;
}
