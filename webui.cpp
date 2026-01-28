#include "webui.h"
#include "webui_json.h"
#include "utils.h"
#include "logger.h"
#include "runway.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <ctime>
#include <cstring>
#include <vector>
#include <thread>
#include <chrono>

using namespace network;

WebUI::WebUI(std::shared_ptr<RunwayManager> runway_manager,
             std::shared_ptr<RoutingEngine> routing_engine,
             std::shared_ptr<TargetAccessibilityTracker> tracker,
             std::shared_ptr<ProxyServer> proxy_server,
             const Config& config)
    : runway_manager_(runway_manager)
    , routing_engine_(routing_engine)
    , tracker_(tracker)
    , proxy_server_(proxy_server)
    , config_(config)
    , running_(false)
    , listen_socket_(INVALID_SOCKET_VALUE)
    , start_time_(std::time(nullptr))
    , session_counter_(0) {
}

WebUI::~WebUI() {
    stop();
}

bool WebUI::start() {
    if (running_) {
        return false;
    }
    
    listen_socket_ = create_tcp_socket();
    if (listen_socket_ == INVALID_SOCKET_VALUE) {
        return false;
    }
    
    // Set socket options
    set_socket_option(listen_socket_, SOL_SOCKET, SO_REUSEADDR, 1);
    
    // Bind to address
    if (!bind_socket(listen_socket_, config_.webui_listen_host, config_.webui_listen_port)) {
        close_socket(listen_socket_);
        listen_socket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    
    // Listen
    if (!listen_socket(listen_socket_, 128)) {
        close_socket(listen_socket_);
        listen_socket_ = INVALID_SOCKET_VALUE;
        return false;
    }
    
    running_ = true;
    server_thread_ = std::thread(&WebUI::server_loop, this);
    
    return true;
}

void WebUI::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (listen_socket_ != INVALID_SOCKET_VALUE) {
        close_socket(listen_socket_);
        listen_socket_ = INVALID_SOCKET_VALUE;
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void WebUI::server_loop() {
    while (running_) {
        std::string client_ip;
        uint16_t client_port;
        socket_t client_sock = accept_connection(listen_socket_, client_ip, client_port);
        
        if (client_sock == INVALID_SOCKET_VALUE) {
            if (running_) {
                // Error accepting connection, but server still running
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            continue;
        }
        
        // Handle connection in same thread (simple implementation)
        // For production, could use thread pool
        handle_connection(client_sock);
        close_socket(client_sock);
    }
}

void WebUI::handle_connection(socket_t client_sock) {
    // Read request (simple - read up to 8KB)
    char buffer[8192];
    ssize_t bytes_received = recv_data(client_sock, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_received <= 0) {
        return;
    }
    
    buffer[bytes_received] = '\0';
    std::string raw_request(buffer, bytes_received);
    
    // Parse HTTP request
    HTTPRequest req;
    if (!parse_http_request(raw_request, req)) {
        // Send 400 Bad Request
        std::string response = build_http_response(400, "text/plain", "Bad Request");
        send_data(client_sock, response.c_str(), response.length());
        return;
    }
    
    // Route request
    std::string response_body;
    std::string content_type = "text/html";
    
    if (req.path == "/") {
        response_body = handle_root();
    } else if (req.path == "/api/status") {
        response_body = handle_api_status();
        content_type = "application/json";
    } else if (req.path == "/api/runways") {
        // Extract session ID from query or cookie
        std::string session_id = req.headers.count("X-Session-Id") ? 
                                 req.headers.at("X-Session-Id") : create_session();
        response_body = handle_api_runways(session_id);
        content_type = "application/json";
    } else if (req.path == "/api/targets") {
        std::string session_id = req.headers.count("X-Session-Id") ? 
                                 req.headers.at("X-Session-Id") : create_session();
        response_body = handle_api_targets(session_id);
        content_type = "application/json";
    } else if (req.path == "/api/connections") {
        std::string session_id = req.headers.count("X-Session-Id") ? 
                                 req.headers.at("X-Session-Id") : create_session();
        response_body = handle_api_connections(session_id);
        content_type = "application/json";
    } else if (req.path == "/api/stats") {
        response_body = handle_api_stats();
        content_type = "application/json";
    } else if (req.path == "/api/action" && req.method == "POST") {
        response_body = handle_api_action(req.body);
        content_type = "application/json";
    } else {
        // 404 Not Found
        response_body = "Not Found";
        std::string response = build_http_response(404, "text/plain", response_body);
        send_data(client_sock, response.c_str(), response.length());
        return;
    }
    
    // Send response
    std::string response = build_http_response(200, content_type, response_body);
    send_data(client_sock, response.c_str(), response.length());
}

bool WebUI::parse_http_request(const std::string& raw_request, HTTPRequest& req) {
    std::istringstream iss(raw_request);
    std::string line;
    
    // Parse request line
    if (!std::getline(iss, line)) {
        return false;
    }
    
    // Remove \r if present
    if (!line.empty() && line.back() == '\r') {
        line.pop_back();
    }
    
    // Parse method, path, version
    std::istringstream request_line(line);
    request_line >> req.method >> req.path;
    
    // Parse headers
    while (std::getline(iss, line)) {
        if (line.empty() || (line.length() == 1 && line[0] == '\r')) {
            break; // End of headers
        }
        
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = utils::trim(line.substr(0, colon_pos));
            std::string value = utils::trim(line.substr(colon_pos + 1));
            req.headers[utils::to_lower(key)] = value;
        }
    }
    
    // Parse body (if present)
    if (req.method == "POST") {
        std::string body;
        while (std::getline(iss, line)) {
            body += line + "\n";
        }
        req.body = body;
        // Remove trailing newline
        if (!req.body.empty() && req.body.back() == '\n') {
            req.body.pop_back();
        }
    }
    
    return true;
}

std::string WebUI::build_http_response(int status_code, const std::string& content_type, 
                                         const std::string& body) {
    std::ostringstream oss;
    
    std::string status_text;
    switch (status_code) {
        case 200: status_text = "OK"; break;
        case 400: status_text = "Bad Request"; break;
        case 404: status_text = "Not Found"; break;
        case 500: status_text = "Internal Server Error"; break;
        default: status_text = "Unknown"; break;
    }
    
    oss << "HTTP/1.1 " << status_code << " " << status_text << "\r\n";
    oss << "Content-Type: " << content_type << "\r\n";
    oss << "Content-Length: " << body.length() << "\r\n";
    oss << "Access-Control-Allow-Origin: *\r\n";
    oss << "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n";
    oss << "Access-Control-Allow-Headers: Content-Type, X-Session-Id\r\n";
    oss << "Connection: close\r\n";
    oss << "\r\n";
    oss << body;
    
    return oss.str();
}

std::string WebUI::create_session() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    std::string session_id = "session_" + std::to_string(++session_counter_) + "_" + 
                            std::to_string(std::time(nullptr));
    sessions_[session_id] = SessionState();
    sessions_[session_id].last_access_time = std::time(nullptr);
    return session_id;
}

SessionState* WebUI::get_session(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        return &it->second;
    }
    return nullptr;
}

void WebUI::update_session_access(const std::string& session_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(session_id);
    if (it != sessions_.end()) {
        it->second.last_access_time = std::time(nullptr);
    }
}

std::vector<std::shared_ptr<Runway>> WebUI::get_runways_snapshot() {
    // Cache runway count to avoid blocking
    static std::vector<std::shared_ptr<Runway>> cached_runways;
    static uint64_t last_cache_time = 0;
    uint64_t now = std::time(nullptr);
    
    // Refresh cache every 2 seconds
    if (cached_runways.empty() || (now - last_cache_time) >= 2) {
        try {
            cached_runways = runway_manager_->get_all_runways();
            last_cache_time = now;
        } catch (...) {
            // Return cached on error
        }
    }
    
    return cached_runways;
}

std::vector<std::string> WebUI::get_targets_snapshot() {
    // Cache targets to avoid blocking
    static std::vector<std::string> cached_targets;
    static uint64_t last_cache_time = 0;
    uint64_t now = std::time(nullptr);
    
    // Refresh cache every 2 seconds
    if (cached_targets.empty() || (now - last_cache_time) >= 2) {
        try {
            cached_targets = tracker_->get_all_targets();
            last_cache_time = now;
        } catch (...) {
            // Return cached on error
        }
    }
    
    return cached_targets;
}

std::vector<ConnectionInfo> WebUI::get_connections_snapshot() {
    // Get live connection data from proxy server
    auto live_conns = proxy_server_->get_active_connections_info();
    std::vector<ConnectionInfo> result;
    
    for (const auto& conn_map : live_conns) {
        ConnectionInfo conn;
        conn.id = conn_map.count("id") ? conn_map.at("id") : "";
        conn.client_ip = conn_map.count("client_ip") ? conn_map.at("client_ip") : "";
        if (conn_map.count("client_port")) {
            utils::safe_str_to_uint16(conn_map.at("client_port"), conn.client_port);
        }
        conn.target_host = conn_map.count("target_host") ? conn_map.at("target_host") : "";
        if (conn_map.count("target_port")) {
            utils::safe_str_to_uint16(conn_map.at("target_port"), conn.target_port);
        }
        conn.runway_id = conn_map.count("runway_id") ? conn_map.at("runway_id") : "";
        conn.method = conn_map.count("method") ? conn_map.at("method") : "";
        conn.path = conn_map.count("path") ? conn_map.at("path") : "";
        conn.status = conn_map.count("status") ? conn_map.at("status") : "unknown";
        if (conn_map.count("start_time")) {
            utils::safe_str_to_uint64(conn_map.at("start_time"), conn.start_time);
        }
        if (conn_map.count("bytes_sent")) {
            utils::safe_str_to_uint64(conn_map.at("bytes_sent"), conn.bytes_sent);
        }
        if (conn_map.count("bytes_received")) {
            utils::safe_str_to_uint64(conn_map.at("bytes_received"), conn.bytes_received);
        }
        result.push_back(conn);
    }
    
    return result;
}

std::string WebUI::format_uptime(uint64_t start_time) {
    uint64_t now = std::time(nullptr);
    uint64_t diff = now - start_time;
    
    uint64_t hours = diff / 3600;
    uint64_t minutes = (diff % 3600) / 60;
    uint64_t seconds = diff % 60;
    
    std::stringstream ss;
    if (hours > 0) {
        ss << hours << "h ";
    }
    if (minutes > 0 || hours > 0) {
        ss << minutes << "m ";
    }
    ss << seconds << "s";
    return ss.str();
}

std::string WebUI::format_bytes(uint64_t bytes) {
    return utils::format_bytes(bytes);
}

std::string WebUI::get_routing_mode_string() {
    RoutingMode mode = routing_engine_->get_mode();
    switch (mode) {
        case RoutingMode::Latency: return "latency";
        case RoutingMode::FirstAccessible: return "first_accessible";
        case RoutingMode::RoundRobin: return "round_robin";
        default: return "unknown";
    }
}

// API endpoint handlers
std::string WebUI::handle_root() {
    return get_html_page();
}

std::string WebUI::handle_api_status() {
    using namespace webui_json;
    
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.push_back({"routing_mode", encode_string(get_routing_mode_string())});
    pairs.push_back({"uptime", encode_string(format_uptime(start_time_))});
    pairs.push_back({"active_connections", encode_int(static_cast<int64_t>(proxy_server_->get_active_connections()))});
    pairs.push_back({"total_connections", encode_int(static_cast<int64_t>(proxy_server_->get_total_connections()))});
    pairs.push_back({"bytes_sent", encode_string(format_bytes(proxy_server_->get_total_bytes_sent()))});
    pairs.push_back({"bytes_received", encode_string(format_bytes(proxy_server_->get_total_bytes_received()))});
    
    return build_object(pairs);
}

std::string WebUI::handle_api_runways(const std::string& session_id) {
    using namespace webui_json;
    
    update_session_access(session_id);
    SessionState* session = get_session(session_id);
    if (!session) {
        std::vector<std::pair<std::string, std::string>> error_pairs;
        error_pairs.push_back({"error", encode_string("Invalid session")});
        return build_object(error_pairs);
    }
    
    auto runways = get_runways_snapshot();
    std::vector<std::string> runway_objects;
    
    for (const auto& runway : runways) {
        // Get status
        std::string status = "unknown";
        std::string status_symbol = "?";
        auto targets = tracker_->get_all_targets();
        for (const auto& target : targets) {
            auto metrics = tracker_->get_metrics(target, runway->id);
            if (metrics) {
                if (metrics->state == RunwayState::Accessible) {
                    status = "accessible";
                    status_symbol = "✓";
                    break;
                } else if (metrics->state == RunwayState::PartiallyAccessible && status != "accessible") {
                    status = "partially_accessible";
                    status_symbol = "⚠";
                } else if (metrics->state == RunwayState::Inaccessible && status == "unknown") {
                    status = "inaccessible";
                    status_symbol = "✗";
                }
            }
        }
        if (status == "unknown") {
            status = runway->is_direct ? "accessible" : "partially_accessible";
            status_symbol = runway->is_direct ? "✓" : "⚠";
        }
        
        std::string proxy_str = runway->upstream_proxy ? 
            runway->upstream_proxy->config.host : "null";
        if (proxy_str != "null") {
            proxy_str = encode_string(proxy_str);
        }
        
        std::vector<std::pair<std::string, std::string>> rw_pairs;
        rw_pairs.push_back({"id", encode_string(runway->id)});
        rw_pairs.push_back({"status", encode_string(status)});
        rw_pairs.push_back({"status_symbol", encode_string(status_symbol)});
        rw_pairs.push_back({"interface", encode_string(runway->interface_name)});
        rw_pairs.push_back({"proxy", proxy_str});
        rw_pairs.push_back({"latency", encode_string("N/A")});
        
        runway_objects.push_back(build_object(rw_pairs));
    }
    
    std::vector<std::pair<std::string, std::string>> response_pairs;
    response_pairs.push_back({"runways", build_array(runway_objects)});
    response_pairs.push_back({"selected_index", encode_int(session->selected_index)});
    response_pairs.push_back({"scroll_offset", encode_int(session->scroll_offset)});
    
    return build_object(response_pairs);
}

std::string WebUI::handle_api_targets(const std::string& session_id) {
    using namespace webui_json;
    
    update_session_access(session_id);
    SessionState* session = get_session(session_id);
    if (!session) {
        std::vector<std::pair<std::string, std::string>> error_pairs;
        error_pairs.push_back({"error", encode_string("Invalid session")});
        return build_object(error_pairs);
    }
    
    auto targets = get_targets_snapshot();
    std::vector<std::string> target_objects;
    
    for (const auto& target : targets) {
        auto metrics_map = tracker_->get_target_metrics(target);
        
        std::string best_runway = "-";
        std::string status = "unknown";
        std::string status_symbol = "?";
        int success_rate = 0;
        double avg_latency = 0.0;
        
        for (const auto& pair : metrics_map) {
            const auto& metrics = pair.second;
            if (metrics.state == RunwayState::Accessible) {
                best_runway = pair.first;
                status = "accessible";
                status_symbol = "✓";
                success_rate = static_cast<int>(metrics.success_rate * 100);
                avg_latency = metrics.avg_response_time;
                break;
            } else if (metrics.state == RunwayState::PartiallyAccessible && status != "accessible") {
                best_runway = pair.first;
                status = "partially_accessible";
                status_symbol = "⚠";
                success_rate = static_cast<int>(metrics.success_rate * 100);
                avg_latency = metrics.avg_response_time;
            }
        }
        
        std::string latency_str = (avg_latency > 0.0) ? 
            (std::to_string(static_cast<int>(avg_latency * 100) / 100.0) + "s") : "N/A";
        
        std::vector<std::pair<std::string, std::string>> tgt_pairs;
        tgt_pairs.push_back({"target", encode_string(target)});
        tgt_pairs.push_back({"status", encode_string(status)});
        tgt_pairs.push_back({"status_symbol", encode_string(status_symbol)});
        tgt_pairs.push_back({"best_runway", encode_string(best_runway)});
        tgt_pairs.push_back({"success_rate", encode_string(std::to_string(success_rate) + "%")});
        tgt_pairs.push_back({"latency", encode_string(latency_str)});
        
        target_objects.push_back(build_object(tgt_pairs));
    }
    
    std::vector<std::pair<std::string, std::string>> response_pairs;
    response_pairs.push_back({"targets", build_array(target_objects)});
    response_pairs.push_back({"selected_index", encode_int(session->selected_index)});
    response_pairs.push_back({"scroll_offset", encode_int(session->scroll_offset)});
    
    return build_object(response_pairs);
}

std::string WebUI::handle_api_connections(const std::string& session_id) {
    using namespace webui_json;
    
    update_session_access(session_id);
    SessionState* session = get_session(session_id);
    if (!session) {
        std::vector<std::pair<std::string, std::string>> error_pairs;
        error_pairs.push_back({"error", encode_string("Invalid session")});
        return build_object(error_pairs);
    }
    
    auto conns = get_connections_snapshot();
    std::vector<std::string> conn_objects;
    
    for (const auto& conn : conns) {
        std::string status_color = "";
        if (conn.status == "active") {
            status_color = "green";
        } else if (conn.status == "connecting") {
            status_color = "yellow";
        } else if (conn.status == "error") {
            status_color = "red";
        }
        
        uint64_t total_bytes = conn.bytes_sent + conn.bytes_received;
        std::string data_str = (total_bytes > 0) ? format_bytes(total_bytes) : "0 B";
        
        std::vector<std::pair<std::string, std::string>> conn_pairs;
        conn_pairs.push_back({"id", encode_string(conn.id)});
        conn_pairs.push_back({"client", encode_string(conn.client_ip + ":" + std::to_string(conn.client_port))});
        conn_pairs.push_back({"target", encode_string(conn.target_host + ":" + std::to_string(conn.target_port))});
        conn_pairs.push_back({"runway", encode_string(conn.runway_id)});
        conn_pairs.push_back({"method", encode_string(conn.method)});
        conn_pairs.push_back({"path", encode_string(conn.path)});
        conn_pairs.push_back({"data", encode_string(data_str)});
        conn_pairs.push_back({"status", encode_string(conn.status)});
        conn_pairs.push_back({"status_color", encode_string(status_color)});
        
        conn_objects.push_back(build_object(conn_pairs));
    }
    
    std::vector<std::pair<std::string, std::string>> response_pairs;
    response_pairs.push_back({"connections", build_array(conn_objects)});
    response_pairs.push_back({"selected_index", encode_int(session->selected_index)});
    response_pairs.push_back({"scroll_offset", encode_int(session->scroll_offset)});
    
    return build_object(response_pairs);
}

std::string WebUI::handle_api_stats() {
    using namespace webui_json;
    
    auto runways = get_runways_snapshot();
    auto targets = get_targets_snapshot();
    size_t conn_count = proxy_server_->get_active_connections();
    
    uint64_t total_bytes = proxy_server_->get_total_bytes_sent() + 
                          proxy_server_->get_total_bytes_received();
    uint64_t uptime_secs = std::time(nullptr) - start_time_;
    double throughput = (uptime_secs > 0) ? (static_cast<double>(total_bytes) / uptime_secs) : 0.0;
    
    std::vector<std::pair<std::string, std::string>> pairs;
    pairs.push_back({"runways", encode_int(static_cast<int64_t>(runways.size()))});
    pairs.push_back({"targets", encode_int(static_cast<int64_t>(targets.size()))});
    pairs.push_back({"active_connections", encode_int(static_cast<int64_t>(conn_count))});
    pairs.push_back({"total_connections", encode_int(static_cast<int64_t>(proxy_server_->get_total_connections()))});
    pairs.push_back({"bytes_sent", encode_string(format_bytes(proxy_server_->get_total_bytes_sent()))});
    pairs.push_back({"bytes_received", encode_string(format_bytes(proxy_server_->get_total_bytes_received()))});
    pairs.push_back({"throughput", encode_string(format_bytes(static_cast<uint64_t>(throughput)) + "/s")});
    pairs.push_back({"uptime", encode_string(format_uptime(start_time_))});
    pairs.push_back({"routing_mode", encode_string(get_routing_mode_string())});
    pairs.push_back({"listen_address", encode_string(config_.proxy_listen_host + ":" + std::to_string(config_.proxy_listen_port))});
    
    return build_object(pairs);
}

std::string WebUI::handle_api_action(const std::string& body) {
    using namespace webui_json;
    
    // Simple JSON parsing for action request
    // Expected: {"action": "navigate_up", "session_id": "..."}
    std::string session_id;
    std::string action;
    
    // Extract session_id
    size_t session_pos = body.find("\"session_id\"");
    if (session_pos != std::string::npos) {
        size_t colon = body.find(':', session_pos);
        if (colon != std::string::npos) {
            size_t quote1 = body.find('"', colon);
            size_t quote2 = body.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                session_id = body.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
    }
    
    // Extract action
    size_t action_pos = body.find("\"action\"");
    if (action_pos != std::string::npos) {
        size_t colon = body.find(':', action_pos);
        if (colon != std::string::npos) {
            size_t quote1 = body.find('"', colon);
            size_t quote2 = body.find('"', quote1 + 1);
            if (quote1 != std::string::npos && quote2 != std::string::npos) {
                action = body.substr(quote1 + 1, quote2 - quote1 - 1);
            }
        }
    }
    
    if (session_id.empty()) {
        session_id = create_session();
    }
    
    SessionState* session = get_session(session_id);
    if (!session) {
        session_id = create_session();
        session = get_session(session_id);
    }
    
    if (!session) {
        std::vector<std::pair<std::string, std::string>> error_pairs;
        error_pairs.push_back({"error", encode_string("Failed to create session")});
        return build_object(error_pairs);
    }
    
    update_session_access(session_id);
    
    // Handle actions
    if (action == "navigate_up") {
        if (session->selected_index > 0) {
            session->selected_index--;
        }
    } else if (action == "navigate_down") {
        int max_items = 0;
        if (session->current_tab == TUI::Tab::Runways) {
            auto runways = this->get_runways_snapshot();
            max_items = static_cast<int>(runways.size());
        } else if (session->current_tab == TUI::Tab::Targets) {
            auto targets = this->get_targets_snapshot();
            max_items = static_cast<int>(targets.size());
        } else if (session->current_tab == TUI::Tab::Connections) {
            std::vector<ConnectionInfo> conns_vec = this->get_connections_snapshot();
            max_items = static_cast<int>(conns_vec.size());
        }
        if (session->selected_index < max_items - 1) {
            session->selected_index++;
        }
    } else if (action == "navigate_page_up") {
        session->selected_index = (std::max)(0, session->selected_index - 20);
    } else if (action == "navigate_page_down") {
        int max_items = 0;
        if (session->current_tab == TUI::Tab::Runways) {
            auto runways = this->get_runways_snapshot();
            max_items = static_cast<int>(runways.size());
        } else if (session->current_tab == TUI::Tab::Targets) {
            auto targets = this->get_targets_snapshot();
            max_items = static_cast<int>(targets.size());
        } else if (session->current_tab == TUI::Tab::Connections) {
            std::vector<ConnectionInfo> conns_vec = this->get_connections_snapshot();
            max_items = static_cast<int>(conns_vec.size());
        }
        session->selected_index = (std::min)(max_items - 1, session->selected_index + 20);
    } else if (action == "switch_tab") {
        // Extract tab number from body
        size_t tab_pos = body.find("\"tab\"");
        if (tab_pos != std::string::npos) {
            size_t colon = body.find(':', tab_pos);
            if (colon != std::string::npos) {
                std::string tab_str = utils::trim(body.substr(colon + 1, 10));
                // Parse as integer directly
                int tab_num = 0;
                try {
                    tab_num = std::stoi(tab_str);
                } catch (...) {
                    tab_num = 0;
                }
                if (tab_num >= 0 && tab_num <= 4) {
                    session->current_tab = static_cast<TUI::Tab>(tab_num);
                    session->selected_index = 0;
                    session->scroll_offset = 0;
                    session->detail_view = false;
                }
            }
        }
    } else if (action == "show_detail") {
        session->detail_view = true;
        // Get current item ID based on tab and selected_index
        if (session->current_tab == TUI::Tab::Runways) {
            auto runways = get_runways_snapshot();
            if (session->selected_index >= 0 && session->selected_index < static_cast<int>(runways.size())) {
                session->detail_item_id = runways[session->selected_index]->id;
            }
        } else if (session->current_tab == TUI::Tab::Targets) {
            auto targets = get_targets_snapshot();
            if (session->selected_index >= 0 && session->selected_index < static_cast<int>(targets.size())) {
                session->detail_item_id = targets[session->selected_index];
            }
        } else if (session->current_tab == TUI::Tab::Connections) {
            std::vector<ConnectionInfo> conns_vec = this->get_connections_snapshot();
            if (session->selected_index >= 0 && session->selected_index < static_cast<int>(conns_vec.size())) {
                session->detail_item_id = conns_vec[session->selected_index].id;
            }
        }
    } else if (action == "hide_detail") {
        session->detail_view = false;
        session->detail_item_id.clear();
    } else if (action == "cycle_routing_mode") {
        RoutingMode current_mode = routing_engine_->get_mode();
        RoutingMode next_mode = RoutingMode::Latency;
        switch (current_mode) {
            case RoutingMode::Latency:
                next_mode = RoutingMode::FirstAccessible;
                break;
            case RoutingMode::FirstAccessible:
                next_mode = RoutingMode::RoundRobin;
                break;
            case RoutingMode::RoundRobin:
                next_mode = RoutingMode::Latency;
                break;
        }
        routing_engine_->set_mode(next_mode);
    }
    
    std::vector<std::pair<std::string, std::string>> response_pairs;
    response_pairs.push_back({"success", encode_bool(true)});
    response_pairs.push_back({"session_id", encode_string(session_id)});
    
    return build_object(response_pairs);
}

std::string WebUI::get_html_page() {
    // Embedded HTML/CSS/JavaScript - terminal-inspired web UI
    // Split into multiple string literals to avoid MSVC's 16380 byte limit
    std::string html;
    
    // Part 1: HTML head and CSS
    html += R"(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>Smart Proxy Monitor</title>
    <style>)
        * { margin: 0; padding: 0; box-sizing: border-box; }
        body {
            font-family: 'Courier New', monospace;
            background: #1e1e1e;
            color: #d4d4d4;
            overflow: hidden;
            height: 100vh;
            display: flex;
            flex-direction: column;
        }
        #status-bar {
            background: #007acc;
            color: white;
            padding: 4px 8px;
            font-weight: bold;
            display: flex;
            justify-content: space-between;
            align-items: center;
        }
        #tab-bar {
            background: #007acc;
            display: flex;
            border-bottom: 1px solid #555;
        }
        .tab {
            padding: 4px 12px;
            cursor: pointer;
            background: #007acc;
            color: white;
            border: none;
            font-family: inherit;
            font-size: 14px;
        }
        .tab.active {
            background: #1e1e1e;
            color: white;
            font-weight: bold;
        }
        .tab:hover {
            background: #005a9e;
        }
        #content-area {
            flex: 1;
            overflow-y: auto;
            padding: 8px;
            background: #1e1e1e;
        }
        #summary-bar {
            padding: 4px 8px;
            border-top: 1px solid #555;
            background: #252526;
            font-size: 12px;
        }
        #command-bar {
            padding: 4px 8px;
            background: #007acc;
            color: white;
            font-size: 12px;
        }
        table {
            width: 100%;
            border-collapse: collapse;
            font-size: 13px;
        }
        th {
            background: #252526;
            padding: 4px 8px;
            text-align: left;
            border-bottom: 1px solid #555;
        }
        td {
            padding: 4px 8px;
            border-bottom: 1px solid #333;
        }
        tr.selected {
            background: #264f78;
        }
        tr:hover {
            background: #2d2d30;
        }
        .status-ok { color: #4ec9b0; }
        .status-warn { color: #dcdcaa; }
        .status-error { color: #f48771; }
        .detail-view {
            padding: 16px;
            background: #252526;
            border: 1px solid #555;
            margin: 8px;
        }
        .detail-view h2 {
            margin-bottom: 12px;
            color: #4ec9b0;
        }
        .detail-view p {
            margin: 8px 0;
            line-height: 1.6;
        }
        .help-content {
            padding: 16px;
            line-height: 1.8;
        }
        .help-content h3 {
            color: #4ec9b0;
            margin-top: 16px;
            margin-bottom: 8px;
        }
        .help-content kbd {
            background: #3c3c3c;
            padding: 2px 6px;
            border-radius: 3px;
            font-family: monospace;
        }
    </style>
</head>
<body>)";

    // Part 2: HTML body structure
    html += R"(
    <div id="status-bar">
        <span>Smart Proxy Monitor</span>
        <span id="status-info">Loading...</span>
    </div>
    <div id="tab-bar">
        <button class="tab active" data-tab="0">Runways</button>
        <button class="tab" data-tab="1">Targets</button>
        <button class="tab" data-tab="2">Connections</button>
        <button class="tab" data-tab="3">Stats</button>
        <button class="tab" data-tab="4">Help</button>
    </div>
    <div id="content-area"></div>
    <div id="summary-bar">Stats: <span id="summary-text">Loading...</span></div>
    <div id="command-bar">[1-5] Tabs  [↑↓] Navigate  [Enter] Details  [Esc] Back  [Ctrl+B] Mode</div>
    
    <script>)";

    // Part 3: JavaScript - API calls
    html += R"(
        let sessionId = null;
        let currentTab = 0;
        let selectedIndex = 0;
        let scrollOffset = 0;
        let detailView = false;
        let pollInterval = null;
        
        // Initialize session
        function initSession() {
            // Session will be created on first API call
        }
        
        // API calls
        async function fetchStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();
                document.getElementById('status-info').textContent = 
                    `Mode: ${data.routing_mode} | Uptime: ${data.uptime} | Active: ${data.active_connections} | Total: ${data.total_connections}`;
                return data;
            } catch (e) {
                console.error('Status fetch error:', e);
            }
        }
        
        async function fetchRunways() {
            try {
                const headers = sessionId ? {'X-Session-Id': sessionId} : {};
                const response = await fetch('/api/runways', {headers});
                const data = await response.json();
                if (data.session_id) sessionId = data.session_id;
                if (data.selected_index !== undefined) selectedIndex = data.selected_index;
                if (data.scroll_offset !== undefined) scrollOffset = data.scroll_offset;
                return data;
            } catch (e) {
                console.error('Runways fetch error:', e);
            }
        }
        
        async function fetchTargets() {
            try {
                const headers = sessionId ? {'X-Session-Id': sessionId} : {};
                const response = await fetch('/api/targets', {headers});
                const data = await response.json();
                if (data.session_id) sessionId = data.session_id;
                if (data.selected_index !== undefined) selectedIndex = data.selected_index;
                if (data.scroll_offset !== undefined) scrollOffset = data.scroll_offset;
                return data;
            } catch (e) {
                console.error('Targets fetch error:', e);
            }
        }
        
        async function fetchConnections() {
            try {
                const headers = sessionId ? {'X-Session-Id': sessionId} : {};
                const response = await fetch('/api/connections', {headers});
                const data = await response.json();
                if (data.session_id) sessionId = data.session_id;
                if (data.selected_index !== undefined) selectedIndex = data.selected_index;
                if (data.scroll_offset !== undefined) scrollOffset = data.scroll_offset;
                return data;
            } catch (e) {
                console.error('Connections fetch error:', e);
            }
        }
        
        async function fetchStats() {
            try {
                const response = await fetch('/api/stats');
                const data = await response.json();
                document.getElementById('summary-text').textContent = 
                    `${data.runways} runways | ${data.targets} targets | ${data.active_connections} active | ${data.throughput}`;
                return data;
            } catch (e) {
                console.error('Stats fetch error:', e);
            }
        }
        
        async function sendAction(action, extra = {}) {
            try {
                const body = JSON.stringify({action, session_id: sessionId, ...extra});
                const response = await fetch('/api/action', {
                    method: 'POST',
                    headers: {'Content-Type': 'application/json'},
                    body
                });
                const data = await response.json();
                if (data.session_id) sessionId = data.session_id;
                return data;
            } catch (e) {
                console.error('Action error:', e);
            }
        }
        
        // Rendering)";

    // Part 4: JavaScript - Rendering functions
    html += R"(
        function renderRunways(data) {
            if (!data || !data.runways) return;
            let html = '<table><tr><th>ID</th><th>Status</th><th>Interface</th><th>Proxy</th><th>Latency</th></tr>';
            data.runways.forEach((rw, idx) => {
                const isSelected = idx === selectedIndex && !detailView;
                const statusClass = rw.status === 'accessible' ? 'status-ok' : 
                                   rw.status === 'partially_accessible' ? 'status-warn' : 'status-error';
                html += `<tr class="${isSelected ? 'selected' : ''}" data-index="${idx}">
                    <td>${escapeHtml(rw.id)}</td>
                    <td class="${statusClass}">${rw.status_symbol}</td>
                    <td>${escapeHtml(rw.interface)}</td>
                    <td>${rw.proxy === null ? '-' : escapeHtml(rw.proxy)}</td>
                    <td>${escapeHtml(rw.latency)}</td>
                </tr>`;
            });
            html += '</table>';
            document.getElementById('content-area').innerHTML = html;
        }
        
        function renderTargets(data) {
            if (!data || !data.targets) return;
            let html = '<table><tr><th>Target</th><th>Status</th><th>Best Runway</th><th>Success</th><th>Latency</th></tr>';
            data.targets.forEach((tgt, idx) => {
                const isSelected = idx === selectedIndex && !detailView;
                const statusClass = tgt.status === 'accessible' ? 'status-ok' : 
                                   tgt.status === 'partially_accessible' ? 'status-warn' : 'status-error';
                html += `<tr class="${isSelected ? 'selected' : ''}" data-index="${idx}">
                    <td>${escapeHtml(tgt.target)}</td>
                    <td class="${statusClass}">${tgt.status_symbol}</td>
                    <td>${escapeHtml(tgt.best_runway)}</td>
                    <td>${tgt.success_rate}%</td>
                    <td>${escapeHtml(tgt.latency)}</td>
                </tr>`;
            });
            html += '</table>';
            document.getElementById('content-area').innerHTML = html;
        }
        
        function renderConnections(data) {
            if (!data || !data.connections) return;
            let html = '<table><tr><th>Client</th><th>Target</th><th>Runway</th><th>Method</th><th>Data</th><th>Status</th></tr>';
            data.connections.forEach((conn, idx) => {
                const isSelected = idx === selectedIndex && !detailView;
                html += `<tr class="${isSelected ? 'selected' : ''}" data-index="${idx}">
                    <td>${escapeHtml(conn.client)}</td>
                    <td>${escapeHtml(conn.target)}</td>
                    <td>${escapeHtml(conn.runway)}</td>
                    <td>${escapeHtml(conn.method)}</td>
                    <td>${escapeHtml(conn.data)}</td>
                    <td class="status-${conn.status_color}">●</td>
                </tr>`;
            });
            html += '</table>';
            document.getElementById('content-area').innerHTML = html;
        }
        
        function renderStats(data) {
            if (!data) return;
            let html = '<div class="help-content">';
            html += '<h3>Overview</h3>';
            html += `<p>Runways: ${data.runways}</p>`;
            html += `<p>Targets: ${data.targets}</p>`;
            html += `<p>Active Connections: ${data.active_connections}</p>`;
            html += `<p>Total Connections: ${data.total_connections}</p>`;
            html += '<h3>Performance</h3>';
            html += `<p>Bytes Sent: ${data.bytes_sent}</p>`;
            html += `<p>Bytes Received: ${data.bytes_received}</p>`;
            html += `<p>Throughput: ${data.throughput}</p>`;
            html += `<p>Uptime: ${data.uptime}</p>`;
            html += `<p>Routing Mode: ${data.routing_mode}</p>`;
            html += `<p>Listen Address: ${data.listen_address}</p>`;
            html += '</div>';
            document.getElementById('content-area').innerHTML = html;
        }
        
        function renderHelp() {
            const html = `<div class="help-content">
                <h3>Keyboard Shortcuts</h3>
                <p><kbd>1-5</kbd> Switch tabs</p>
                <p><kbd>↑</kbd> <kbd>↓</kbd> Navigate items</p>
                <p><kbd>←</kbd> <kbd>→</kbd> Switch tabs</p>
                <p><kbd>Page Up</kbd> <kbd>Page Down</kbd> Scroll one page</p>
                <p><kbd>Home</kbd> <kbd>End</kbd> Go to first/last item</p>
                <p><kbd>Enter</kbd> View details</p>
                <p><kbd>Esc</kbd> Back/Close details</p>
                <p><kbd>Ctrl+B</kbd> Cycle routing mode</p>
                <p><kbd>?</kbd> Show this help</p>
            </div>`;
            document.getElementById('content-area').innerHTML = html;
        }
        
        function escapeHtml(text) {
            const div = document.createElement('div');
            div.textContent = text;
            return div.innerHTML;
        })";

    // Part 5: JavaScript - Update display and event handlers
    html += R"(
        
        // Update display
        async function updateDisplay() {
            await fetchStatus();
            await fetchStats();
            
            if (detailView) {
                // Show detail view
                return;
            }
            
            switch (currentTab) {
                case 0: // Runways
                    const runwaysData = await fetchRunways();
                    renderRunways(runwaysData);
                    break;
                case 1: // Targets
                    const targetsData = await fetchTargets();
                    renderTargets(targetsData);
                    break;
                case 2: // Connections
                    const connsData = await fetchConnections();
                    renderConnections(connsData);
                    break;
                case 3: // Stats
                    const statsData = await fetchStats();
                    renderStats(statsData);
                    break;
                case 4: // Help
                    renderHelp();
                    break;
            }
        }
        
        // Event handlers)";

    // Part 6: JavaScript - Event handlers and initialization
    html += R"(
        document.querySelectorAll('.tab').forEach(tab => {
            tab.addEventListener('click', async () => {
                const tabNum = parseInt(tab.dataset.tab);
                currentTab = tabNum;
                document.querySelectorAll('.tab').forEach(t => t.classList.remove('active'));
                tab.classList.add('active');
                selectedIndex = 0;
                scrollOffset = 0;
                detailView = false;
                await sendAction('switch_tab', {tab: tabNum});
                await updateDisplay();
            });
        });
        
        document.addEventListener('keydown', async (e) => {
            if (e.key >= '1' && e.key <= '5') {
                const tabNum = parseInt(e.key) - 1;
                currentTab = tabNum;
                document.querySelectorAll('.tab').forEach((t, idx) => {
                    t.classList.toggle('active', idx === tabNum);
                });
                selectedIndex = 0;
                scrollOffset = 0;
                detailView = false;
                await sendAction('switch_tab', {tab: tabNum});
                await updateDisplay();
            } else if (e.key === 'ArrowUp') {
                e.preventDefault();
                await sendAction('navigate_up');
                await updateDisplay();
            } else if (e.key === 'ArrowDown') {
                e.preventDefault();
                await sendAction('navigate_down');
                await updateDisplay();
            } else if (e.key === 'PageUp') {
                e.preventDefault();
                await sendAction('navigate_page_up');
                await updateDisplay();
            } else if (e.key === 'PageDown') {
                e.preventDefault();
                await sendAction('navigate_page_down');
                await updateDisplay();
            } else if (e.key === 'Enter') {
                e.preventDefault();
                if (!detailView) {
                    await sendAction('show_detail');
                    detailView = true;
                    await updateDisplay();
                }
            } else if (e.key === 'Escape') {
                e.preventDefault();
                if (detailView) {
                    await sendAction('hide_detail');
                    detailView = false;
                    await updateDisplay();
                }
            } else if (e.key === 'b' && e.ctrlKey) {
                e.preventDefault();
                await sendAction('cycle_routing_mode');
                await updateDisplay();
            }
        });
        
        // Start polling
        function startPolling() {
            pollInterval = setInterval(updateDisplay, 100);
        }
        
        // Initialize
        initSession();
        updateDisplay();
        startPolling();
    </script>
</body>
</html>)";

    return html;
}
