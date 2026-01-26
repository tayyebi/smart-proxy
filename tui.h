#ifndef TUI_H
#define TUI_H

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>
#include "config.h"
#include "runway_manager.h"
#include "routing.h"
#include "tracker.h"
#include "proxy.h"

// Terminal User Interface for live monitoring
// Uses ANSI escape codes for terminal control (zero dependencies)
// Layout follows system topology

struct ConnectionInfo {
    std::string id;
    std::string client_ip;
    uint16_t client_port;
    std::string target_host;
    uint16_t target_port;
    std::string runway_id;
    std::string method;
    std::string path;
    uint64_t start_time;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    std::string status; // "connecting", "active", "completed", "error"
    
    ConnectionInfo() 
        : client_port(0)
        , target_port(0)
        , start_time(0)
        , bytes_sent(0)
        , bytes_received(0) {}
};

class TUI {
public:
    TUI(std::shared_ptr<RunwayManager> runway_manager,
        std::shared_ptr<RoutingEngine> routing_engine,
        std::shared_ptr<TargetAccessibilityTracker> tracker,
        std::shared_ptr<ProxyServer> proxy_server,
        const Config& config);
    
    ~TUI();
    
    // Start TUI (runs in main thread, blocks)
    void run();
    
    // Stop TUI
    void stop();
    
    // Update connection info (thread-safe)
    void update_connection(const ConnectionInfo& conn);
    void remove_connection(const std::string& conn_id);
    
    // Check if TUI is running
    bool is_running() const { return running_; }
    
private:
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<RoutingEngine> routing_engine_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    std::shared_ptr<ProxyServer> proxy_server_;
    Config config_;
    
    std::atomic<bool> running_;
    std::atomic<bool> should_redraw_;
    std::mutex connections_mutex_;
    std::map<std::string, ConnectionInfo> connections_;
    uint64_t start_time_;
    
    // Terminal control
    void setup_terminal();
    void restore_terminal();
    void clear_screen();
    void move_cursor(int row, int col);
    void hide_cursor();
    void show_cursor();
    
    // Drawing
    void draw();
    void draw_header();
    void draw_runways();
    void draw_targets();
    void draw_connections();
    void draw_footer();
    
    // Layout calculations
    int get_terminal_rows();
    int get_terminal_cols();
    
    // Data collection
    std::vector<std::shared_ptr<Runway>> get_runways_snapshot();
    std::vector<std::string> get_targets_snapshot();
    std::vector<ConnectionInfo> get_connections_snapshot();
    
    // Formatting helpers
    std::string format_uptime(uint64_t start_time);
    std::string format_bytes(uint64_t bytes);
    std::string format_duration(uint64_t start_time);
    std::string truncate_string(const std::string& str, size_t max_len);
    std::string get_runway_status_string(std::shared_ptr<Runway> runway, const std::string& target);
};

#endif // TUI_H
