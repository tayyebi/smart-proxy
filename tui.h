#ifndef TUI_H
#define TUI_H

#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <cstdint>
#include <csignal>
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
    // shutdown_flag: pointer to external shutdown flag to check
    void run(volatile sig_atomic_t* shutdown_flag = nullptr);
    
    // Stop TUI
    void stop();
    
    // Update connection info (thread-safe)
    void update_connection(const ConnectionInfo& conn);
    void remove_connection(const std::string& conn_id);
    
    // Check if TUI is running
    bool is_running() const { return running_; }
    
    // Navigation state
    enum class Tab {
        Runways = 0,
        Targets = 1,
        Connections = 2,
        Stats = 3,
        Help = 4
    };
    
    // Handle keyboard input (non-blocking)
    void handle_input();
    void navigate_up();
    void navigate_down();
    void navigate_page_up();
    void navigate_page_down();
    void navigate_half_page_up();
    void navigate_half_page_down();
    void navigate_to_top();
    void navigate_to_bottom();
    void navigate_next_section();
    void navigate_prev_section();
    void switch_tab(Tab tab);
    void show_detail();
    void hide_detail();
    int get_current_tab_size();
    std::string get_current_item_id();
    void cycle_routing_mode(); // Cycle through routing modes (Ctrl+B)
    void show_quit_confirmation();
    
    // Mouse handling
    void handle_mouse_click(int button, int x, int y);
    void handle_mouse_scroll(int direction, int x, int y); // direction: -1 = up, 1 = down
    void enable_mouse_tracking();
    void disable_mouse_tracking();
    
private:
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<RoutingEngine> routing_engine_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    std::shared_ptr<ProxyServer> proxy_server_;
    Config config_;
    
    std::atomic<bool> running_;
    std::atomic<bool> should_redraw_;
    std::atomic<bool> terminal_resized_;
    std::mutex connections_mutex_;
    std::map<std::string, ConnectionInfo> connections_;
    uint64_t start_time_;
    int cached_rows_;
    int cached_cols_;
    
    // Cached stats for Stats tab (updated periodically, not on every draw)
    size_t cached_runway_count_;
    size_t cached_target_count_;
    uint64_t last_stats_cache_time_;
    
    // Navigation state
    Tab current_tab_;
    int selected_index_; // Selected item in current tab
    int scroll_offset_; // Scroll offset for current tab (for items that don't fit)
    bool detail_view_; // Whether showing detail view
    std::string detail_item_id_; // ID of item being viewed in detail
    bool quit_confirmed_; // Quit confirmation flag
    
    // Layout constants - centralized for safe TUI rendering
    static constexpr int STATUS_BAR_HEIGHT = 1;
    static constexpr int TAB_BAR_HEIGHT = 1;
    static constexpr int SUMMARY_BAR_HEIGHT = 1;
    static constexpr int COMMAND_BAR_HEIGHT = 1;
    static constexpr int MIN_TERMINAL_ROWS = 15;
    static constexpr int MIN_TERMINAL_COLS = 70;
    
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
    void draw_detail_view();
    
    // New layout system - market ready UX
    void draw_status_bar(std::stringstream& output, int cols);
    void draw_tab_bar(std::stringstream& output, int cols);
    void draw_content_area(std::stringstream& output, int cols, int rows);
    void draw_summary_bar(std::stringstream& output, int cols);
    void draw_command_bar(std::stringstream& output, int cols);
    
    // Tab content renderers
    void draw_runways_tab(std::stringstream& output, int cols, int rows);
    void draw_targets_tab(std::stringstream& output, int cols, int rows);
    void draw_connections_tab(std::stringstream& output, int cols, int rows);
    void draw_stats_tab(std::stringstream& output, int cols, int rows);
    void draw_help_tab(std::stringstream& output, int cols, int rows);
    void draw_detail_view(std::stringstream& output, int cols, int rows);
    
    // Table rendering helpers
    void draw_table_header(std::stringstream& output, const std::vector<std::pair<std::string, int>>& columns, int cols);
    void draw_table_row(std::stringstream& output, const std::vector<std::string>& cells, 
                       const std::vector<int>& widths, bool is_selected, bool is_alternate);
    void draw_table_border(std::stringstream& output, const std::string& title, int cols);
    
    // Helper for safe background color filling
    void fill_line_with_bg(std::stringstream& output, int current_pos, int total_width, const std::string& bg_color);
    
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
