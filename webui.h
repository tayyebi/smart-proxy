#ifndef WEBUI_H
#define WEBUI_H

#include <string>
#include <memory>
#include <atomic>
#include <mutex>
#include <map>
#include <thread>
#include <cstdint>
#include "config.h"
#include "runway_manager.h"
#include "routing.h"
#include "tracker.h"
#include "proxy.h"
#include "tui.h"
#include "network.h"

// Web UI server for TUI over HTTP
// Pure C++17 with zero external dependencies
// Serves HTML/CSS/JS embedded as string literals
// Provides JSON API endpoints for data and actions

struct SessionState {
    TUI::Tab current_tab;
    int selected_index;
    int scroll_offset;
    bool detail_view;
    std::string detail_item_id;
    uint64_t last_access_time;
    
    SessionState() 
        : current_tab(TUI::Tab::Runways)
        , selected_index(0)
        , scroll_offset(0)
        , detail_view(false)
        , last_access_time(0) {}
};

class WebUI {
public:
    WebUI(std::shared_ptr<RunwayManager> runway_manager,
          std::shared_ptr<RoutingEngine> routing_engine,
          std::shared_ptr<TargetAccessibilityTracker> tracker,
          std::shared_ptr<ProxyServer> proxy_server,
          const Config& config);
    
    ~WebUI();
    
    // Start web UI server (runs in background thread)
    bool start();
    
    // Stop web UI server
    void stop();
    
    // Check if server is running
    bool is_running() const { return running_; }
    
private:
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<RoutingEngine> routing_engine_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    std::shared_ptr<ProxyServer> proxy_server_;
    Config config_;
    
    std::atomic<bool> running_;
    socket_t listen_socket_;
    std::thread server_thread_;
    uint64_t start_time_;
    
    // Session management
    mutable std::mutex sessions_mutex_;
    std::map<std::string, SessionState> sessions_;
    uint64_t session_counter_;
    
    // Server main loop
    void server_loop();
    
    // Handle client connection
    void handle_connection(socket_t client_sock);
    
    // HTTP request parsing
    struct HTTPRequest {
        std::string method;
        std::string path;
        std::map<std::string, std::string> headers;
        std::string body;
    };
    
    bool parse_http_request(const std::string& raw_request, HTTPRequest& req);
    
    // HTTP response generation
    std::string build_http_response(int status_code, const std::string& content_type, 
                                     const std::string& body);
    
    // Route handlers
    std::string handle_root();
    std::string handle_api_status();
    std::string handle_api_runways(const std::string& session_id);
    std::string handle_api_targets(const std::string& session_id);
    std::string handle_api_connections(const std::string& session_id);
    std::string handle_api_stats();
    std::string handle_api_action(const std::string& body);
    
    // Session management
    std::string create_session();
    SessionState* get_session(const std::string& session_id);
    void update_session_access(const std::string& session_id);
    
    // Data gathering (reuse TUI methods)
    std::vector<std::shared_ptr<Runway>> get_runways_snapshot();
    std::vector<std::string> get_targets_snapshot();
    std::vector<ConnectionInfo> get_connections_snapshot();
    
    // Formatting helpers
    std::string format_uptime(uint64_t start_time);
    std::string format_bytes(uint64_t bytes);
    std::string get_routing_mode_string();
    
    // Embedded HTML/CSS/JavaScript
    std::string get_html_page();
};

#endif // WEBUI_H
