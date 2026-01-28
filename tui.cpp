#include "tui.h"
#include "utils.h"
#include "logger.h"
#include "routing.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>
#include <map>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
// Undefine Windows min/max macros that conflict with std::min/std::max
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#endif

// Cross-platform unused parameter macro
#ifdef _WIN32
#define UNUSED_PARAM(x)
#else
#define UNUSED_PARAM(x) __attribute__((unused))
#endif

TUI::TUI(std::shared_ptr<RunwayManager> runway_manager,
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
    , should_redraw_(true)
    , terminal_resized_(false)
    , start_time_(std::time(nullptr))
    , cached_rows_(0)
    , cached_cols_(0)
    , cached_runway_count_(0)
    , cached_target_count_(0)
    , last_stats_cache_time_(0)
    , current_tab_(Tab::Runways)
    , selected_index_(0)
    , scroll_offset_(0)
    , detail_view_(false)
    , detail_item_id_("")
    , quit_confirmed_(false) {
}

TUI::~TUI() {
    stop();
}

void TUI::setup_terminal() {
    if (!utils::is_terminal()) {
        return;
    }
    
#ifdef _WIN32
    // Windows: Enable ANSI escape codes and raw input
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
    
    // Set input mode for non-blocking reads and mouse input (only if enabled in config)
    DWORD inMode = 0;
    GetConsoleMode(hIn, &inMode);
    inMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    if (config_.mouse_enabled) {
        inMode |= ENABLE_MOUSE_INPUT; // Enable mouse input on Windows
    }
    SetConsoleMode(hIn, inMode);
#else
    // POSIX: Save terminal state and set raw mode
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    term.c_cc[VMIN] = 0;
    term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    
    // Set stdin to non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);
#endif
    
    clear_screen();
    hide_cursor();
    
    // Enable mouse tracking only if enabled in config
    if (config_.mouse_enabled) {
        enable_mouse_tracking();
    }
}

void TUI::restore_terminal() {
    if (!utils::is_terminal()) {
        return;
    }
    
    // Disable mouse tracking first
    disable_mouse_tracking();
    
#ifdef _WIN32
    // Windows: Reset console mode
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
    
    // Restore input mode
    DWORD inMode = 0;
    GetConsoleMode(hIn, &inMode);
    inMode |= (ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
    inMode &= ~ENABLE_MOUSE_INPUT;
    SetConsoleMode(hIn, inMode);
#else
    // POSIX: Restore terminal state
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    
    // Restore blocking mode
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags & ~O_NONBLOCK);
#endif
    
    clear_screen();
    show_cursor();
    move_cursor(1, 1);
}

void TUI::clear_screen() {
    if (utils::is_terminal()) {
        std::cout << "\033[2J";
        std::cout.flush();
    }
}

void TUI::move_cursor(int row, int col) {
    if (utils::is_terminal()) {
        std::cout << "\033[" << row << ";" << col << "H";
        std::cout.flush();
    }
}

void TUI::hide_cursor() {
    if (utils::is_terminal()) {
        std::cout << "\033[?25l";
        std::cout.flush();
    }
}

void TUI::show_cursor() {
    if (utils::is_terminal()) {
        std::cout << "\033[?25h";
        std::cout.flush();
    }
}

int TUI::get_terminal_rows() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        if (rows != cached_rows_) {
            cached_rows_ = rows;
            terminal_resized_ = true;
            should_redraw_ = true;
        }
        return rows > 0 ? rows : 24;
    }
    return cached_rows_ > 0 ? cached_rows_ : 24;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        int rows = w.ws_row > 0 ? w.ws_row : 24;
        if (rows != cached_rows_) {
            cached_rows_ = rows;
            terminal_resized_ = true;
            should_redraw_ = true;
        }
        return rows;
    }
    return cached_rows_ > 0 ? cached_rows_ : 24;
#endif
}

int TUI::get_terminal_cols() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        int cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        if (cols != cached_cols_) {
            cached_cols_ = cols;
            terminal_resized_ = true;
            should_redraw_ = true;
        }
        return cols > 0 ? cols : 80;
    }
    return cached_cols_ > 0 ? cached_cols_ : 80;
#else
    struct winsize w;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &w) == 0) {
        int cols = w.ws_col > 0 ? w.ws_col : 80;
        if (cols != cached_cols_) {
            cached_cols_ = cols;
            terminal_resized_ = true;
            should_redraw_ = true;
        }
        return cols;
    }
    return cached_cols_ > 0 ? cached_cols_ : 80;
#endif
}

void TUI::run(volatile sig_atomic_t* shutdown_flag) {
    if (!utils::is_terminal()) {
        // Not a terminal, just wait
        while (running_) {
            if (shutdown_flag && *shutdown_flag) {
                utils::safe_print("\nShutting down gracefully...\n");
                utils::safe_flush();
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return;
    }
    
    running_ = true;
    setup_terminal();
    
    // Initialize cached size
    cached_rows_ = get_terminal_rows();
    cached_cols_ = get_terminal_cols();
    terminal_resized_ = false;
    
    // Highly responsive update loop - update every 100ms for smooth experience
    auto last_update = std::chrono::steady_clock::now();
    const auto update_interval = std::chrono::milliseconds(100);
    
    while (running_ && proxy_server_->is_running()) {
        // Check shutdown flag (for graceful shutdown)
        if (shutdown_flag && *shutdown_flag) {
            // Display shutdown message before exiting
            restore_terminal();
            utils::safe_print("\n\nShutting down gracefully...\n");
            utils::safe_print("Stopping services...\n");
            utils::safe_flush();
            break; // Exit loop for graceful shutdown
        }
        
        // Check quit confirmation
        if (quit_confirmed_) {
            restore_terminal();
            utils::safe_print("\n\nQuitting...\n");
            utils::safe_flush();
            running_ = false;
            break;
        }
        
        auto now = std::chrono::steady_clock::now();
        
        // Check for keyboard input (non-blocking)
        handle_input();
        
        // Check for terminal resize (always check size to detect changes)
        get_terminal_rows(); // This will detect resize and set flags
        get_terminal_cols();
        
        // Always redraw if enough time has passed, explicitly requested, terminal resized, or for live updates
        // Force update every interval to show live connection data (duration, bytes, etc.)
        bool force_update = (now - last_update) >= update_interval;
        if (should_redraw_ || terminal_resized_ || force_update) {
            // Update stats cache periodically (every 2 seconds) to avoid blocking Stats tab
            uint64_t now_secs = std::time(nullptr);
            if (current_tab_ == Tab::Stats && (now_secs - last_stats_cache_time_) >= 2) {
                // Update cache in background when Stats tab is active
                try {
                    auto runways = get_runways_snapshot();
                    cached_runway_count_ = runways.size();
                } catch (...) {}
                try {
                    auto targets = get_targets_snapshot();
                    cached_target_count_ = targets.size();
                } catch (...) {}
                last_stats_cache_time_ = now_secs;
            }
            
            draw();
            should_redraw_ = false;
            terminal_resized_ = false;
            last_update = now;
        }
        
        // Minimal sleep for maximum responsiveness (10ms = 100fps theoretical max)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Only restore terminal if we didn't already do it for shutdown
    if (!(shutdown_flag && *shutdown_flag)) {
        restore_terminal();
    }
}

void TUI::stop() {
    running_ = false;
}

void TUI::update_connection(const ConnectionInfo& conn) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_[conn.id] = conn;
    should_redraw_ = true;
}

void TUI::remove_connection(const std::string& conn_id) {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    connections_.erase(conn_id);
    should_redraw_ = true;
}

void TUI::draw() {
    // Fast drawing - build entire frame in memory first, then single output
    int rows = get_terminal_rows();
    int cols = get_terminal_cols();
    
    // Use centralized constants for consistent layout
    if (rows < MIN_TERMINAL_ROWS || cols < MIN_TERMINAL_COLS) {
        std::cout << "\033[2J\033[1;1H"; // Clear and move to top
        std::cout << "Terminal too small (min " << MIN_TERMINAL_COLS << "x" << MIN_TERMINAL_ROWS << ")\n";
        std::cout << "Current: " << cols << "x" << rows << "\n";
        std::cout.flush();
        return;
    }
    
    // Build entire frame in stringstream for single atomic output
    std::stringstream output;
    output << "\033[2J\033[1;1H"; // Clear screen and move to top
    
    // Draw detail view if active
    if (detail_view_) {
        draw_detail_view(output, cols, rows);
    } else {
        // Status bar
        draw_status_bar(output, cols);
        
        // Tab bar
        draw_tab_bar(output, cols);
        
        // Content area (remaining space) - use centralized constants
        int content_h = rows - STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT - SUMMARY_BAR_HEIGHT - COMMAND_BAR_HEIGHT;
        
        // Content
        draw_content_area(output, cols, content_h);
        
        // Summary bar
        draw_summary_bar(output, cols);
        
        // Command bar
        draw_command_bar(output, cols);
    }
    
    // Single atomic output for maximum responsiveness
    std::cout << output.str();
    std::cout.flush();
}

void TUI::draw_header() {
    int cols = get_terminal_cols();
    
    // Title bar
    std::cout << "\033[1;37;44m"; // Bold white on blue
    std::cout << " Smart Proxy Monitor ";
    for (int i = 22; i < cols; ++i) {
        std::cout << " ";
    }
    std::cout << "\033[0m\n";
    
    // Status line
    std::cout << "\033[1m"; // Bold
    std::cout << "Mode: ";
    
    std::string mode_str;
    RoutingMode current_mode = routing_engine_->get_mode();
    switch (current_mode) {
        case RoutingMode::Latency: mode_str = "Latency"; break;
        case RoutingMode::FirstAccessible: mode_str = "First Accessible"; break;
        case RoutingMode::RoundRobin: mode_str = "Round Robin"; break;
    }
    // Highlight mode as editable
    std::cout << "\033[33;1m" << mode_str << "\033[0m"; // Yellow bold for editable
    
    std::cout << " | Uptime: " << format_uptime(start_time_);
    std::cout << " | Active: " << proxy_server_->get_active_connections();
    std::cout << " | Total: " << proxy_server_->get_total_connections();
    std::cout << " | Sent: " << utils::format_bytes(proxy_server_->get_total_bytes_sent());
    std::cout << " | Recv: " << utils::format_bytes(proxy_server_->get_total_bytes_received());
    
    // Show editable indicator for mode
    std::cout << " | \033[33mCtrl+B: Mode\033[0m";
    
    int remaining = cols - 140; // Increased for additional stats and hint
    if (remaining > 0) {
        for (int i = 0; i < remaining; ++i) {
            std::cout << " ";
        }
    }
    std::cout << "\033[0m\n";
    
    // Separator
    for (int i = 0; i < cols; ++i) {
        std::cout << "-";
    }
    std::cout << "\n";
}

void TUI::draw_runways() {
    auto runways = get_runways_snapshot();
    int cols = get_terminal_cols();
    
    std::cout << "\033[1mRunways (" << runways.size() << "):\033[0m\n";
    
    if (runways.empty()) {
        std::cout << "  No runways discovered\n";
        return;
    }
    
    // Show first 8 runways
    size_t max_show = std::min(runways.size(), size_t(8));
    for (size_t i = 0; i < max_show; ++i) {
        auto runway = runways[i];
        std::cout << "  " << truncate_string(runway->id, 30);
        
        std::string status = get_runway_status_string(runway, "");
        if (status == "Accessible") {
            std::cout << " \033[32m[OK]\033[0m";
        } else if (status == "Partially Accessible") {
            std::cout << " \033[33m[PARTIAL]\033[0m";
        } else {
            std::cout << " \033[31m[FAIL]\033[0m";
        }
        
        std::cout << " " << runway->interface_name;
        if (runway->upstream_proxy) {
            std::cout << " -> " << runway->upstream_proxy->config.host;
        }
        
        int remaining = cols - 70;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                std::cout << " ";
            }
        }
        std::cout << "\n";
    }
    
    if (runways.size() > max_show) {
        std::cout << "  ... and " << (runways.size() - max_show) << " more\n";
    }
}

void TUI::draw_targets() {
    auto targets = get_targets_snapshot();
    int cols = get_terminal_cols();
    
    std::cout << "\033[1mTargets (" << targets.size() << "):\033[0m\n";
    
    if (targets.empty()) {
        std::cout << "  No targets accessed yet\n";
        return;
    }
    
    // Show first 5 targets
    size_t max_show = std::min(targets.size(), size_t(5));
    for (size_t i = 0; i < max_show; ++i) {
        std::string target = targets[i];
        auto metrics_map = tracker_->get_target_metrics(target);
        
        std::cout << "  " << truncate_string(target, 40);
        
        // Find best runway for this target
        std::string best_status = "Unknown";
        for (const auto& pair : metrics_map) {
            auto metrics = pair.second;
            if (metrics.state == RunwayState::Accessible) {
                best_status = "Accessible";
                break;
            } else if (metrics.state == RunwayState::PartiallyAccessible && best_status != "Accessible") {
                best_status = "Partial";
            }
        }
        
        if (best_status == "Accessible") {
            std::cout << " \033[32m[OK]\033[0m";
        } else if (best_status == "Partial") {
            std::cout << " \033[33m[PARTIAL]\033[0m";
        } else {
            std::cout << " \033[31m[FAIL]\033[0m";
        }
        
        int remaining = cols - 60;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                std::cout << " ";
            }
        }
        std::cout << "\n";
    }
    
    if (targets.size() > max_show) {
        std::cout << "  ... and " << (targets.size() - max_show) << " more\n";
    }
}

void TUI::draw_connections() {
    auto conns = get_connections_snapshot();
    int cols = get_terminal_cols();
    
    std::cout << "\033[1mActive Connections (" << conns.size() << "):\033[0m\n";
    
    if (conns.empty()) {
        std::cout << "  No active connections\n";
        return;
    }
    
    // Show first 10 connections
    size_t max_show = std::min(conns.size(), size_t(10));
    for (size_t i = 0; i < max_show; ++i) {
        const auto& conn = conns[i];
        std::cout << "  " << truncate_string(conn.client_ip + ":" + std::to_string(conn.client_port), 20);
        std::cout << " -> " << truncate_string(conn.target_host + ":" + std::to_string(conn.target_port), 25);
        std::cout << " [" << truncate_string(conn.runway_id, 15) << "]";
        std::cout << " " << truncate_string(conn.method + " " + conn.path, 20);
        std::cout << " " << format_bytes(conn.bytes_sent + conn.bytes_received);
        
        int remaining = cols - 100;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                std::cout << " ";
            }
        }
        std::cout << "\n";
    }
    
    if (conns.size() > max_show) {
        std::cout << "  ... and " << (conns.size() - max_show) << " more\n";
    }
}

void TUI::draw_footer() {
    int rows = get_terminal_rows();
    int cols = get_terminal_cols();
    
    move_cursor(rows, 1);
    std::cout << "\033[1;37;44m"; // Bold white on blue
    std::cout << " Press Ctrl+C to stop (first: graceful, second: force) ";
    for (int i = 55; i < cols; ++i) {
        std::cout << " ";
    }
    std::cout << "\033[0m";
}

void TUI::draw_status_bar(std::stringstream& output, int cols) {
    // Status bar: Left = title, Right = key metrics
    const std::string bg_color = "\033[1;37;44m"; // Bold white on blue
    
    output << bg_color;
    std::string title = " Smart Proxy Monitor ";
    output << title;
    
    // Right side: Status and key metrics
    std::string status_text = "[Status: RUNNING]";
    std::string metrics = "Uptime: " + format_uptime(start_time_) + 
                         " | Active: " + std::to_string(proxy_server_->get_active_connections()) +
                         " | Total: " + std::to_string(proxy_server_->get_total_connections());
    
    // Calculate actual visible width (ANSI codes don't count)
    int title_len = static_cast<int>(title.length());
    int status_len = static_cast<int>(status_text.length());
    int metrics_len = static_cast<int>(metrics.length());
    int space_len = 1; // Space between status and metrics
    
    int total_content_len = title_len + status_len + metrics_len + space_len;
    
    // Fill middle padding with background color
    int current_pos = title_len;
    if (total_content_len < cols) {
        int padding = cols - total_content_len;
        for (int i = 0; i < padding; ++i) {
            output << " ";
        }
        current_pos += padding;
    }
    
    // Output status and metrics with background color maintained
    output << "\033[1;32;44m" << status_text << bg_color << " " << metrics;
    current_pos += status_len + space_len + metrics_len;
    
    // Fill any remaining space to end of line (safety check)
    fill_line_with_bg(output, current_pos, cols, bg_color);
    
    output << "\n";
}

void TUI::draw_tab_bar(std::stringstream& output, int cols) {
    // Tab bar with 5 tabs
    std::vector<std::pair<std::string, Tab>> tabs = {
        {"Runways", Tab::Runways},
        {"Targets", Tab::Targets},
        {"Connections", Tab::Connections},
        {"Stats", Tab::Stats},
        {"Help", Tab::Help}
    };
    
    const std::string inactive_bg = "\033[1;37;44m"; // Bold white on blue
    
    output << "\033[0m"; // Reset
    
    int tab_width_used = 0;
    for (size_t i = 0; i < tabs.size(); ++i) {
        bool is_active = (current_tab_ == tabs[i].second && !detail_view_);
        
        if (is_active) {
            output << "\033[1;7m"; // Bold, reverse video for active
        } else {
            output << inactive_bg;
        }
        
        std::string tab_text = " " + tabs[i].first + " ";
        output << tab_text;
        tab_width_used += static_cast<int>(tab_text.length());
        
        output << "\033[0m";
    }
    
    // Fill remaining space on tab bar line (no background for empty space)
    for (int i = tab_width_used; i < cols; ++i) {
        output << " ";
    }
    output << "\n";
    
    // Separator line
    output << "\033[90m"; // Dark gray
    for (int i = 0; i < cols; ++i) {
        output << "─";
    }
    output << "\033[0m\n";
}

void TUI::draw_content_area(std::stringstream& output, int cols, int max_rows) {
    // Draw the active tab's content
    switch (current_tab_) {
        case Tab::Runways:
            draw_runways_tab(output, cols, max_rows);
            break;
        case Tab::Targets:
            draw_targets_tab(output, cols, max_rows);
            break;
        case Tab::Connections:
            draw_connections_tab(output, cols, max_rows);
            break;
        case Tab::Stats:
            draw_stats_tab(output, cols, max_rows);
            break;
        case Tab::Help:
            draw_help_tab(output, cols, max_rows);
            break;
    }
}

// Table rendering helpers
void TUI::draw_table_border(std::stringstream& output, const std::string& title, int cols) {
    output << "┌─ " << title;
    int used = 3 + static_cast<int>(title.length());
    for (int i = used; i < cols - 1; ++i) {
        output << "─";
    }
    output << "┐\n";
}

void TUI::draw_table_header(std::stringstream& output, const std::vector<std::pair<std::string, int>>& columns, int cols) {
    output << "│";
    for (const auto& col : columns) {
        std::string header = col.first;
        int width = col.second;
        output << " " << header;
        int padding = width - static_cast<int>(header.length()) - 1;
        for (int i = 0; i < padding; ++i) {
            output << " ";
        }
        output << "│";
    }
    // Fill remaining space
    int used = 1;
    for (const auto& col : columns) {
        used += col.second + 1;
    }
    if (used < cols - 1) {
        for (int i = used; i < cols - 1; ++i) {
            output << " ";
        }
    }
    output << "\n";
    
    // Separator line
    output << "├";
    for (const auto& col : columns) {
        for (int i = 0; i < col.second + 1; ++i) {
            output << "─";
        }
        output << "┼";
    }
    // Fill to end
    int used2 = 1;
    for (const auto& col : columns) {
        used2 += col.second + 1;
    }
    if (used2 < cols - 1) {
        for (int i = used2; i < cols - 1; ++i) {
            output << "─";
        }
    }
    output << "┤\n";
}

void TUI::draw_table_row(std::stringstream& output, const std::vector<std::string>& cells, 
                         const std::vector<int>& widths, bool is_selected, bool is_alternate) {
    if (is_selected) {
        output << "\033[7m"; // Reverse video
    } else if (is_alternate) {
        output << "\033[90m"; // Dark gray for alternate rows
    }
    
    output << "│";
    for (size_t i = 0; i < cells.size() && i < widths.size(); ++i) {
        std::string cell = cells[i];
        int width = widths[i];
        
        // Truncate if needed
        if (cell.length() > static_cast<size_t>(width - 1)) {
            cell = cell.substr(0, width - 4) + "...";
        }
        
        output << " " << cell;
        int padding = width - static_cast<int>(cell.length()) - 1;
        for (int j = 0; j < padding; ++j) {
            output << " ";
        }
        output << "│";
    }
    
    output << "\033[0m\n"; // Reset colors
}

void TUI::draw_runways_tab(std::stringstream& output, int cols, int max_rows) {
    auto runways = get_runways_snapshot();
    
    // Adjust scroll to keep selected item visible
    int visible_items = max_rows - 3; // Leave space for header and borders
    if (visible_items < 1) visible_items = 1;
    
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + visible_items) {
        scroll_offset_ = selected_index_ - visible_items + 1;
    }
    
    // Table header
    std::string title = "Runways (" + std::to_string(runways.size()) + ")";
    draw_table_border(output, title, cols);
    
    if (runways.empty()) {
        output << "│ No runways discovered yet                                    │\n";
        output << "└";
        for (int i = 0; i < cols - 2; ++i) output << "─";
        output << "┘\n";
        return;
    }
    
    // Column definitions: Name, Width
    std::vector<std::pair<std::string, int>> columns = {
        {"ID", 25},
        {"Status", 8},
        {"Interface", 12},
        {"Proxy", 20},
        {"Latency", 10}
    };
    
    draw_table_header(output, columns, cols);
    
    // Table rows
    size_t start_idx = static_cast<size_t>(scroll_offset_);
    size_t end_idx = std::min(start_idx + static_cast<size_t>(visible_items), runways.size());
    
    for (size_t i = start_idx; i < end_idx; ++i) {
        auto runway = runways[i];
        int display_idx = static_cast<int>(i);
        bool is_selected = (current_tab_ == Tab::Runways && 
                           display_idx == selected_index_ && !detail_view_);
        bool is_alternate = (i % 2 == 1);
        
        // Get status symbol - try to find a target to check status
        std::string status_symbol = "?";
        std::string status_color = "";
        auto targets = tracker_->get_all_targets();
        bool found_status = false;
        for (const auto& target : targets) {
            auto metrics = tracker_->get_metrics(target, runway->id);
            if (metrics) {
                if (metrics->state == RunwayState::Accessible) {
                    status_symbol = "✓";
                    status_color = "\033[32m";
                    found_status = true;
                    break;
                } else if (metrics->state == RunwayState::PartiallyAccessible && !found_status) {
                    status_symbol = "⚠";
                    status_color = "\033[33m";
                    found_status = true;
                } else if (metrics->state == RunwayState::Inaccessible && !found_status) {
                    status_symbol = "✗";
                    status_color = "\033[31m";
                }
            }
        }
        if (!found_status) {
            // Default based on type
            status_symbol = runway->is_direct ? "✓" : "⚠";
            status_color = runway->is_direct ? "\033[32m" : "\033[33m";
        }
        
        std::string proxy_str = runway->upstream_proxy ? 
            truncate_string(runway->upstream_proxy->config.host, 18) : "-";
        
        std::vector<std::string> cells = {
            truncate_string(runway->id, 23),
            status_color + status_symbol + "\033[0m",
            truncate_string(runway->interface_name, 10),
            proxy_str,
            "N/A" // Latency - can be calculated later
        };
        
        std::vector<int> widths = {25, 8, 12, 20, 10};
        draw_table_row(output, cells, widths, is_selected, is_alternate);
    }
    
    // Bottom border
    output << "└";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┘\n";
}

void TUI::draw_targets_tab(std::stringstream& output, int cols, int max_rows) {
    auto targets = get_targets_snapshot();
    
    int visible_items = max_rows - 3;
    if (visible_items < 1) visible_items = 1;
    
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + visible_items) {
        scroll_offset_ = selected_index_ - visible_items + 1;
    }
    
    std::string title = "Targets (" + std::to_string(targets.size()) + ")";
    draw_table_border(output, title, cols);
    
    if (targets.empty()) {
        output << "│ No targets accessed yet                                      │\n";
        output << "└";
        for (int i = 0; i < cols - 2; ++i) output << "─";
        output << "┘\n";
        return;
    }
    
    std::vector<std::pair<std::string, int>> columns = {
        {"Target", 30},
        {"Status", 8},
        {"Best Runway", 25},
        {"Success", 10},
        {"Latency", 10}
    };
    
    draw_table_header(output, columns, cols);
    
    size_t start_idx = static_cast<size_t>(scroll_offset_);
    size_t end_idx = std::min(start_idx + static_cast<size_t>(visible_items), targets.size());
    
    for (size_t i = start_idx; i < end_idx; ++i) {
        std::string target = targets[i];
        int display_idx = static_cast<int>(i);
        bool is_selected = (current_tab_ == Tab::Targets && 
                           display_idx == selected_index_ && !detail_view_);
        bool is_alternate = (i % 2 == 1);
        
        auto metrics_map = tracker_->get_target_metrics(target);
        
        // Find best runway
        std::string best_runway = "-";
        std::string status_symbol = "?";
        std::string status_color = "";
        int success_rate = 0;
        double avg_latency = 0.0;
        
        for (const auto& pair : metrics_map) {
            const auto& metrics = pair.second;
            if (metrics.state == RunwayState::Accessible) {
                best_runway = truncate_string(pair.first, 23);
                status_symbol = "✓";
                status_color = "\033[32m";
                success_rate = static_cast<int>(metrics.success_rate * 100);
                avg_latency = metrics.avg_response_time;
                break;
            } else if (metrics.state == RunwayState::PartiallyAccessible && status_symbol != "✓") {
                best_runway = truncate_string(pair.first, 23);
                status_symbol = "⚠";
                status_color = "\033[33m";
                success_rate = static_cast<int>(metrics.success_rate * 100);
                avg_latency = metrics.avg_response_time;
            }
        }
        
        std::string latency_str = (avg_latency > 0.0) ? 
            (std::to_string(static_cast<int>(avg_latency * 100) / 100.0) + "s") : "N/A";
        
        std::vector<std::string> cells = {
            truncate_string(target, 28),
            status_color + status_symbol + "\033[0m",
            best_runway,
            std::to_string(success_rate) + "%",
            latency_str
        };
        
        std::vector<int> widths = {30, 8, 25, 10, 10};
        draw_table_row(output, cells, widths, is_selected, is_alternate);
    }
    
    output << "└";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┘\n";
}

void TUI::draw_connections_tab(std::stringstream& output, int cols, int max_rows) {
    auto conns = get_connections_snapshot();
    
    int visible_items = max_rows - 3;
    if (visible_items < 1) visible_items = 1;
    
    if (selected_index_ < scroll_offset_) {
        scroll_offset_ = selected_index_;
    } else if (selected_index_ >= scroll_offset_ + visible_items) {
        scroll_offset_ = selected_index_ - visible_items + 1;
    }
    
    std::string title = "Active Connections (" + std::to_string(conns.size()) + ")";
    draw_table_border(output, title, cols);
    
    if (conns.empty()) {
        output << "│ No active connections                                        │\n";
        output << "└";
        for (int i = 0; i < cols - 2; ++i) output << "─";
        output << "┘\n";
        return;
    }
    
    std::vector<std::pair<std::string, int>> columns = {
        {"Client", 18},
        {"Target", 25},
        {"Runway", 20},
        {"Method", 8},
        {"Data", 12},
        {"Status", 8}
    };
    
    draw_table_header(output, columns, cols);
    
    size_t start_idx = static_cast<size_t>(scroll_offset_);
    size_t end_idx = std::min(start_idx + static_cast<size_t>(visible_items), conns.size());
    
    for (size_t i = start_idx; i < end_idx; ++i) {
        const auto& conn = conns[i];
        int display_idx = static_cast<int>(i);
        bool is_selected = (current_tab_ == Tab::Connections && 
                           display_idx == selected_index_ && !detail_view_);
        bool is_alternate = (i % 2 == 1);
        
        // Status symbol
        std::string status_symbol = "●";
        std::string status_color = "";
        if (conn.status == "active") {
            status_color = "\033[32m";
        } else if (conn.status == "connecting") {
            status_color = "\033[33m";
        } else if (conn.status == "error") {
            status_color = "\033[31m";
        }
        
        // Data
        uint64_t total_bytes = conn.bytes_sent + conn.bytes_received;
        std::string data_str = (total_bytes > 0) ? format_bytes(total_bytes) : "0 B";
        
        std::vector<std::string> cells = {
            truncate_string(conn.client_ip + ":" + std::to_string(conn.client_port), 16),
            truncate_string(conn.target_host + ":" + std::to_string(conn.target_port), 23),
            truncate_string(conn.runway_id, 18),
            truncate_string(conn.method, 6),
            data_str,
            status_color + status_symbol + "\033[0m"
        };
        
        std::vector<int> widths = {18, 25, 20, 8, 12, 8};
        draw_table_row(output, cells, widths, is_selected, is_alternate);
    }
    
    output << "└";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┘\n";
}

// Old function removed - replaced by draw_command_bar
// void TUI::draw_footer_to_stream(std::stringstream& output, int cols, int /*row*/) {

// Old function - replaced by draw_detail_view in tui_new.cpp
void TUI::draw_stats_tab(std::stringstream& output, int cols, int /*max_rows*/) {
    // Use lightweight atomic counters and cached counts to avoid blocking
    // Never call expensive operations that might lock or iterate large collections
    
    std::string title = "Statistics";
    draw_table_border(output, title, cols);
    
    // Update cached counts only every 2 seconds to avoid blocking
    uint64_t now = std::time(nullptr);
    if (cached_runway_count_ == 0 || (now - last_stats_cache_time_) >= 2) {
        // Update cache in background (but still blocking, so we do it infrequently)
        try {
            auto runways = get_runways_snapshot(); // Uses cache
            cached_runway_count_ = runways.size();
        } catch (...) {
            // Keep old value on error
        }
        
        try {
            auto targets = get_targets_snapshot(); // Uses cache
            cached_target_count_ = targets.size();
        } catch (...) {
            // Keep old value on error
        }
        
        last_stats_cache_time_ = now;
    }
    
    // Use cached counts (instant, no blocking)
    size_t runway_count = cached_runway_count_;
    size_t target_count = cached_target_count_;
    
    // Use atomic counter directly (no lock needed, instant)
    size_t conn_count = proxy_server_->get_active_connections();
    
    output << "│ \033[1mOverview\033[0m";
    for (int i = 13; i < cols - 1; ++i) output << " ";
    output << "│\n";
    output << "├";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┤\n";
    
    output << "│ Runways:        " << std::setw(10) << std::left << runway_count;
    output << " Targets:        " << std::setw(10) << std::left << target_count;
    for (int i = 50; i < cols - 1; ++i) output << " ";
    output << "│\n";
    
    output << "│ Active Conn:    " << std::setw(10) << std::left << conn_count;
    output << " Total Conn:     " << std::setw(10) << std::left << proxy_server_->get_total_connections();
    for (int i = 50; i < cols - 1; ++i) output << " ";
    output << "│\n";
    
    // Use atomic counters (no blocking)
    output << "│ Bytes Sent:    " << std::setw(10) << std::left << utils::format_bytes(proxy_server_->get_total_bytes_sent());
    output << " Bytes Recv:     " << std::setw(10) << std::left << utils::format_bytes(proxy_server_->get_total_bytes_received());
    for (int i = 50; i < cols - 1; ++i) output << " ";
    output << "│\n";
    
    // Performance section
    output << "│";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "│\n";
    output << "│ \033[1mPerformance\033[0m";
    for (int i = 15; i < cols - 1; ++i) output << " ";
    output << "│\n";
    output << "├";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┤\n";
    
    // Calculate throughput (atomic operations only)
    uint64_t total_bytes = proxy_server_->get_total_bytes_sent() + proxy_server_->get_total_bytes_received();
    uint64_t uptime_secs = std::time(nullptr) - start_time_;
    double throughput = (uptime_secs > 0) ? (static_cast<double>(total_bytes) / uptime_secs) : 0.0;
    
    output << "│ Throughput:     " << std::setw(10) << std::left << utils::format_bytes(static_cast<uint64_t>(throughput)) + "/s";
    output << " Uptime:         " << std::setw(10) << std::left << format_uptime(start_time_);
    for (int i = 50; i < cols - 1; ++i) output << " ";
    output << "│\n";
    
    // Routing mode (lightweight getter)
    std::string mode_str;
    try {
        RoutingMode mode = routing_engine_->get_mode();
        switch (mode) {
            case RoutingMode::Latency: mode_str = "Latency"; break;
            case RoutingMode::FirstAccessible: mode_str = "First Accessible"; break;
            case RoutingMode::RoundRobin: mode_str = "Round Robin"; break;
        }
    } catch (...) {
        mode_str = "Unknown";
    }
    
    output << "│ Routing Mode:   " << std::setw(10) << std::left << mode_str;
    output << " Listen:         " << std::setw(10) << std::left << (config_.proxy_listen_host + ":" + std::to_string(config_.proxy_listen_port));
    for (int i = 50; i < cols - 1; ++i) output << " ";
    output << "│\n";
    
    output << "└";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┘\n";
}

void TUI::draw_help_tab(std::stringstream& output, int cols, int max_rows UNUSED_PARAM(max_rows)) {
#ifdef _WIN32
    (void)max_rows; // Suppress unused parameter warning on Windows
#endif
    std::string title = "Help & Shortcuts";
    draw_table_border(output, title, cols);
    
    output << "│ \033[1mKeyboard Shortcuts\033[0m";
    for (int i = 20; i < cols - 1; ++i) output << " ";
    output << "│\n";
    output << "├";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┤\n";
    
    std::vector<std::pair<std::string, std::string>> shortcuts = {
        {"1-5", "Switch tabs"},
        {"↑↓ / j/k", "Navigate items"},
        {"←→ / h/l", "Switch tabs"},
        {"Page Up/Down", "Scroll one page"},
        {"Home/End", "Go to first/last item"},
        {"Ctrl+Home/End", "Go to first/last tab"},
        {"Space / Ctrl+F", "Page down"},
        {"b", "Page up"},
        {"Ctrl+D/U", "Half page down/up"},
        {"g / gg", "Go to top (double-g)"},
        {"G / Ctrl+G", "Go to bottom"},
        {"Enter", "View details"},
        {"Esc", "Back/Close details"},
        {"Backspace/Delete", "Go up one item"},
        {"Tab/Shift+Tab", "Switch tabs"},
        {"q", "Quit (with confirmation)"},
        {"Ctrl+B", "Cycle routing mode"},
        {"F1", "Show help"},
        {"F5", "Refresh display"},
        {"?", "Show this help"}
    };
    
    for (const auto& shortcut : shortcuts) {
        output << "│ " << std::setw(12) << std::left << shortcut.first;
        output << " " << shortcut.second;
        int used = 15 + static_cast<int>(shortcut.first.length() + shortcut.second.length());
        for (int i = used; i < cols - 1; ++i) output << " ";
        output << "│\n";
    }
    
    output << "│";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "│\n";
    output << "│ \033[1mMouse Operations\033[0m";
    for (int i = 18; i < cols - 1; ++i) output << " ";
    output << "│\n";
    output << "├";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┤\n";
    
    std::vector<std::pair<std::string, std::string>> mouse_ops = {
        {"Click Tab", "Switch to that tab"},
        {"Click Item", "Select item"},
        {"Double-click", "View details"},
        {"Scroll", "Navigate list"},
        {"Click Mode", "Cycle routing mode (in status bar)"}
    };
    
    for (const auto& op : mouse_ops) {
        output << "│ " << std::setw(15) << std::left << op.first;
        output << " " << op.second;
        int used = 18 + static_cast<int>(op.first.length() + op.second.length());
        for (int i = used; i < cols - 1; ++i) output << " ";
        output << "│\n";
    }
    
    output << "└";
    for (int i = 0; i < cols - 2; ++i) output << "─";
    output << "┘\n";
}

void TUI::draw_summary_bar(std::stringstream& output, int cols) {
    // Separator line
    output << "\033[90m"; // Dark gray
    for (int i = 0; i < cols; ++i) output << "─";
    output << "\033[0m\n";
    
    auto runways = get_runways_snapshot();
    auto targets = tracker_->get_all_targets();
    auto conns = get_connections_snapshot();
    
    // Calculate throughput
    uint64_t total_bytes = proxy_server_->get_total_bytes_sent() + proxy_server_->get_total_bytes_received();
    uint64_t uptime_secs = std::time(nullptr) - start_time_;
    double throughput = (uptime_secs > 0) ? (static_cast<double>(total_bytes) / uptime_secs) : 0.0;
    
    // Build stats content (calculate visible length first)
    std::string stats_label = "Stats: ";
    std::string runways_text = std::to_string(runways.size()) + " runways";
    std::string targets_text = std::to_string(targets.size()) + " targets";
    std::string conns_text = std::to_string(conns.size()) + " active";
    std::string throughput_text = utils::format_bytes(static_cast<uint64_t>(throughput)) + "/s";
    std::string separator = " | ";
    
    // Calculate visible length (3 separators between 4 items)
    int visible_len = static_cast<int>(stats_label.length() + runways_text.length() + targets_text.length() + 
                      conns_text.length() + throughput_text.length() + (3 * separator.length()));
    
    // Output with ANSI codes for bold "Stats:"
    output << "\033[1m" << stats_label << "\033[0m";
    output << runways_text << separator << targets_text << separator << conns_text << separator << throughput_text;
    
    // Fill remaining space
    for (int i = visible_len; i < cols; ++i) output << " ";
    output << "\n";
}

void TUI::draw_command_bar(std::stringstream& output, int cols) {
    // Separator line
    output << "\033[90m"; // Dark gray
    for (int i = 0; i < cols; ++i) output << "─";
    output << "\033[0m\n";
    
    const std::string bg_color = "\033[1;37;44m"; // Bold white on blue
    output << bg_color;
    
    std::string command_text;
    if (detail_view_) {
        command_text = " [Esc/q] Back  [Q] Quit";
    } else {
        command_text = " [1-5] Tabs  [↑↓] Navigate  [Enter] Details  [q] Quit  [Ctrl+B] Mode  [?] Help";
    }
    
    output << command_text;
    int used = static_cast<int>(command_text.length());
    
    // Fill remaining space with background color
    fill_line_with_bg(output, used, cols, bg_color);
    
    output << "\n";
}

void TUI::draw_detail_view(std::stringstream& output, int cols, int rows) {
    // Detail view with clean formatting
    const std::string bg_color = "\033[1;37;44m"; // Bold white on blue
    output << bg_color;
    std::string title = " Detail View ";
    output << title;
    int used = static_cast<int>(title.length());
    
    // Fill remaining space with background color
    fill_line_with_bg(output, used, cols, bg_color);
    output << "\n";
    
    if (current_tab_ == Tab::Runways) {
        auto runway = runway_manager_->get_runway(detail_item_id_);
        if (runway) {
            output << "\033[1mRunway ID:\033[0m " << runway->id << "\n\n";
            
            // Parse and explain runway ID format
            output << "\033[1mID Format Explanation:\033[0m\n";
            if (runway->id.find("direct_") == 0) {
                output << "  Format: direct_<interface>_<dns_host>_<counter>\n";
                output << "  Type: Direct connection (no upstream proxy)\n";
            } else if (runway->id.find("proxy_") == 0) {
                output << "  Format: proxy_<interface>_<proxy_type>_<proxy_host>_<dns_host>_<counter>\n";
                output << "  Type: Via upstream proxy\n";
            }
            
            output << "\n\033[1mDetails:\033[0m\n";
            output << "  Interface: " << runway->interface_name << "\n";
            output << "  Source IP: " << runway->source_ip << "\n";
            output << "  DNS Server: " << runway->dns_server->config.host 
                   << ":" << runway->dns_server->config.port;
            if (!runway->dns_server->config.name.empty()) {
                output << " (" << runway->dns_server->config.name << ")";
            }
            output << "\n";
            
            if (runway->upstream_proxy) {
                output << "  Upstream Proxy: " << runway->upstream_proxy->config.proxy_type 
                       << "://" << runway->upstream_proxy->config.host 
                       << ":" << runway->upstream_proxy->config.port << "\n";
                output << "  Proxy Accessible: " << (runway->upstream_proxy->accessible ? "Yes" : "No") << "\n";
            } else {
                output << "  Upstream Proxy: None (direct)\n";
            }
            
            output << "  Is Direct: " << (runway->is_direct ? "Yes" : "No") << "\n";
            
            // Show metrics for this runway across all targets
            auto all_targets = tracker_->get_all_targets();
            if (!all_targets.empty()) {
                output << "\n\033[1mTarget Metrics:\033[0m\n";
                for (const auto& target : all_targets) {
                    auto metrics = tracker_->get_metrics(target, runway->id);
                    if (metrics) {
                        output << "  " << target << ": ";
                        switch (metrics->state) {
                            case RunwayState::Accessible:
                                output << "\033[32mAccessible\033[0m";
                                break;
                            case RunwayState::PartiallyAccessible:
                                output << "\033[33mPartially Accessible\033[0m";
                                break;
                            case RunwayState::Inaccessible:
                                output << "\033[31mInaccessible\033[0m";
                                break;
                            default:
                                output << "Unknown";
                        }
                        output << " (Success: " << static_cast<int>(metrics->success_rate * 100) << "%, "
                               << "Avg: " << std::fixed << std::setprecision(2) 
                               << metrics->avg_response_time << "s)\n";
                    }
                }
            }
        }
    } else if (current_tab_ == Tab::Targets) {
        output << "\n\033[1mTarget:\033[0m " << detail_item_id_ << "\n\n";
        auto metrics_map = tracker_->get_target_metrics(detail_item_id_);
        
        if (!metrics_map.empty()) {
            output << "\033[1mRunway Performance:\033[0m\n";
            for (const auto& pair : metrics_map) {
                const auto& metrics = pair.second;
                output << "  " << truncate_string(pair.first, 30) << ": ";
                switch (metrics.state) {
                    case RunwayState::Accessible:
                        output << "\033[32m✓\033[0m";
                        break;
                    case RunwayState::PartiallyAccessible:
                        output << "\033[33m⚠\033[0m";
                        break;
                    case RunwayState::Inaccessible:
                        output << "\033[31m✗\033[0m";
                        break;
                    default:
                        output << "?";
                }
                output << " " << static_cast<int>(metrics.success_rate * 100) << "% | "
                       << std::fixed << std::setprecision(2) << metrics.avg_response_time << "s | "
                       << metrics.total_attempts << " attempts\n";
            }
        }
    } else if (current_tab_ == Tab::Connections) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(detail_item_id_);
        if (it != connections_.end()) {
            const auto& conn = it->second;
            output << "\n\033[1mConnection Details:\033[0m\n";
            output << "  ID: " << conn.id << "\n";
            output << "  Client: " << conn.client_ip << ":" << conn.client_port << "\n";
            output << "  Target: " << conn.target_host << ":" << conn.target_port << "\n";
            output << "  Runway: " << conn.runway_id << "\n";
            output << "  Method: " << conn.method << " " << conn.path << "\n";
            output << "  Status: " << conn.status << "\n";
            output << "  Data: " << utils::format_bytes(conn.bytes_sent) << " sent, "
                   << utils::format_bytes(conn.bytes_received) << " received\n";
            if (conn.start_time > 0) {
                uint64_t now = std::time(nullptr);
                uint64_t duration = now - conn.start_time;
                output << "  Duration: " << duration << "s\n";
            }
        }
    }
    
    // Fill remaining space
    for (int i = 0; i < rows - 20; ++i) {
        output << "\n";
    }
}

std::vector<std::shared_ptr<Runway>> TUI::get_runways_snapshot() {
    // Cache runway count to avoid blocking
    static std::vector<std::shared_ptr<Runway>> cached_runways;
    static uint64_t last_cache_time = 0;
    uint64_t now = std::time(nullptr);
    
    // Refresh cache every 2 seconds to avoid blocking
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

std::vector<std::string> TUI::get_targets_snapshot() {
    // Cache targets to avoid blocking
    static std::vector<std::string> cached_targets;
    static uint64_t last_cache_time = 0;
    uint64_t now = std::time(nullptr);
    
    // Refresh cache every 2 seconds to avoid blocking
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

std::vector<ConnectionInfo> TUI::get_connections_snapshot() {
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

std::string TUI::format_uptime(uint64_t start_time) {
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

std::string TUI::format_bytes(uint64_t bytes) {
    return utils::format_bytes(bytes);
}

std::string TUI::format_duration(uint64_t start_time) {
    uint64_t now = std::time(nullptr);
    uint64_t diff = now - start_time;
    return std::to_string(diff) + "s";
}

std::string TUI::truncate_string(const std::string& str, size_t max_len) {
    if (str.length() <= max_len) {
        return str;
    }
    return str.substr(0, max_len - 3) + "...";
}

// Helper function to fill remaining line with background color
// This ensures background color extends to the full width
void TUI::fill_line_with_bg(std::stringstream& output, int current_pos, int total_width, const std::string& bg_color) {
    if (current_pos < total_width) {
        output << bg_color;
        for (int i = current_pos; i < total_width; ++i) {
            output << " ";
        }
        output << "\033[0m"; // Reset
    }
}

std::string TUI::get_runway_status_string(std::shared_ptr<Runway> runway, const std::string& target) {
    if (target.empty()) {
        return "Unknown";
    }
    
    auto metrics = tracker_->get_metrics(target, runway->id);
    if (!metrics) {
        return "Unknown";
    }
    
    switch (metrics->state) {
        case RunwayState::Accessible: return "Accessible";
        case RunwayState::PartiallyAccessible: return "Partially Accessible";
        case RunwayState::Inaccessible: return "Inaccessible";
        case RunwayState::Testing: return "Testing";
        default: return "Unknown";
    }
}

void TUI::enable_mouse_tracking() {
    if (!utils::is_terminal()) {
        return;
    }
    
    // Enable X11 mouse reporting (works in most terminals)
    // Format: \033[?1000h (X11) or \033[?1006h (SGR - better)
    std::cout << "\033[?1006h"; // SGR mouse mode (more detailed)
    std::cout << "\033[?1000h"; // X11 mouse reporting (fallback)
    std::cout.flush();
}

void TUI::disable_mouse_tracking() {
    if (!utils::is_terminal()) {
        return;
    }
    
    // Disable mouse tracking
    std::cout << "\033[?1006l"; // Disable SGR mouse mode
    std::cout << "\033[?1000l"; // Disable X11 mouse reporting
    std::cout.flush();
}

void TUI::handle_mouse_click(int button, int x, int y) {
    if (detail_view_) {
        return;
    }
    
    int rows = get_terminal_rows();
    int status_h = 1;
    int tab_bar_row = status_h + 1;
    
    // Check if click is on tab bar
    if (y == tab_bar_row) {
        // Tab positions (approximate)
        if (x >= 1 && x <= 12) {
            switch_tab(Tab::Runways);
        } else if (x >= 13 && x <= 24) {
            switch_tab(Tab::Targets);
        } else if (x >= 25 && x <= 40) {
            switch_tab(Tab::Connections);
        } else if (x >= 41 && x <= 50) {
            switch_tab(Tab::Stats);
        } else if (x >= 51 && x <= 60) {
            switch_tab(Tab::Help);
        }
        return;
    }
    
    // Check if click is on status bar (mode area)
    if (y == status_h && x >= 7 && x <= 25) {
        cycle_routing_mode();
        return;
    }
    
    // Check if click is in content area
    int content_start_row = status_h + 3; // After status, tab bar, and separator
    if (y >= content_start_row && y < rows - 2) { // Leave room for summary and command bar
        int content_row = y - content_start_row;
        int item_index = scroll_offset_ + content_row;
        int max_items = get_current_tab_size();
        
        if (item_index >= 0 && item_index < max_items) {
            selected_index_ = item_index;
            
            if (button == 2 || button == 3) { // Middle or right button
                show_detail();
            } else if (button == 0) { // Left button
                should_redraw_ = true;
            }
        }
    }
}

void TUI::handle_mouse_scroll(int direction, int x UNUSED_PARAM(x), int y UNUSED_PARAM(y)) {
#ifdef _WIN32
    (void)x; // Suppress unused parameter warning on Windows
    (void)y;
#endif
    if (detail_view_ || current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    int max_items = get_current_tab_size();
    
    if (direction < 0) { // Scroll up
        if (scroll_offset_ > 0) {
            scroll_offset_ = std::max(0, scroll_offset_ - 3);
            if (selected_index_ > scroll_offset_ + 20) {
                selected_index_ = scroll_offset_ + 10;
            }
            should_redraw_ = true;
        }
    } else { // Scroll down
        int visible_items = 20;
        if (scroll_offset_ + visible_items < max_items) {
            scroll_offset_ = std::min(max_items - visible_items, scroll_offset_ + 3);
            if (selected_index_ < scroll_offset_) {
                selected_index_ = scroll_offset_;
            }
            should_redraw_ = true;
        }
    }
}

void TUI::show_quit_confirmation() {
    if (!quit_confirmed_) {
        quit_confirmed_ = true;
        should_redraw_ = true;
        // Will be handled in main loop
    } else {
        // Second time - actually quit
        running_ = false;
    }
}
