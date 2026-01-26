#include "tui.h"
#include "utils.h"
#include "logger.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <thread>
#include <chrono>
#include <map>

#ifdef _WIN32
#include <windows.h>
#include <io.h>
#include <conio.h>
#else
#include <unistd.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
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
    , current_section_(FocusSection::Runways)
    , selected_index_(0)
    , detail_view_(false)
    , detail_item_id_("") {
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
    
    // Set input mode for non-blocking reads
    DWORD inMode = 0;
    GetConsoleMode(hIn, &inMode);
    inMode &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
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
}

void TUI::restore_terminal() {
    if (!utils::is_terminal()) {
        return;
    }
    
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

void TUI::run() {
    if (!utils::is_terminal()) {
        // Not a terminal, just wait
        while (running_) {
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
            draw();
            should_redraw_ = false;
            terminal_resized_ = false;
            last_update = now;
        }
        
        // Minimal sleep for maximum responsiveness (10ms = 100fps theoretical max)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    restore_terminal();
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
    
    if (rows < 10 || cols < 40) {
        std::cout << "\033[2J\033[1;1H"; // Clear and move to top
        std::cout << "Terminal too small (min 40x10)\n";
        std::cout.flush();
        return;
    }
    
    // Build entire frame in stringstream for single atomic output
    std::stringstream output;
    output << "\033[2J\033[1;1H"; // Clear screen and move to top
    
    // Draw detail view if active
    if (detail_view_) {
        draw_detail_view_to_stream(output, cols, rows);
    } else {
        // Draw header
        draw_header_to_stream(output, cols);
        
        int header_height = 3;
        int runways_height = std::min(8, (rows - header_height - 8) / 3);
        int targets_height = std::min(6, (rows - header_height - runways_height - 6) / 2);
        
        // Draw sections with selection highlighting
        draw_runways_to_stream(output, cols, runways_height);
        draw_targets_to_stream(output, cols, targets_height);
        draw_connections_to_stream(output, cols, rows - header_height - runways_height - targets_height - 2);
        draw_footer_to_stream(output, cols, rows);
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
    switch (config_.routing_mode) {
        case RoutingMode::Latency: mode_str = "Latency"; break;
        case RoutingMode::FirstAccessible: mode_str = "First Accessible"; break;
        case RoutingMode::RoundRobin: mode_str = "Round Robin"; break;
    }
    std::cout << mode_str;
    
    std::cout << " | Uptime: " << format_uptime(start_time_);
    std::cout << " | Active: " << proxy_server_->get_active_connections();
    std::cout << " | Total: " << proxy_server_->get_total_connections();
    std::cout << " | Sent: " << utils::format_bytes(proxy_server_->get_total_bytes_sent());
    std::cout << " | Recv: " << utils::format_bytes(proxy_server_->get_total_bytes_received());
    
    int remaining = cols - 120; // Increased for additional stats
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
        
        std::cout << " " << runway->interface;
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

void TUI::draw_header_to_stream(std::stringstream& output, int cols) {
    // Title bar
    output << "\033[1;37;44m"; // Bold white on blue
    output << " Smart Proxy Monitor ";
    for (int i = 22; i < cols; ++i) {
        output << " ";
    }
    output << "\033[0m\n";
    
    // Status line
    output << "\033[1m"; // Bold
    output << "Mode: ";
    
    std::string mode_str;
    switch (config_.routing_mode) {
        case RoutingMode::Latency: mode_str = "Latency"; break;
        case RoutingMode::FirstAccessible: mode_str = "First Accessible"; break;
        case RoutingMode::RoundRobin: mode_str = "Round Robin"; break;
    }
    output << mode_str;
    
    output << " | Uptime: " << format_uptime(start_time_);
    output << " | Active: " << proxy_server_->get_active_connections();
    output << " | Total: " << proxy_server_->get_total_connections();
    output << " | Sent: " << utils::format_bytes(proxy_server_->get_total_bytes_sent());
    output << " | Recv: " << utils::format_bytes(proxy_server_->get_total_bytes_received());
    
    int remaining = cols - 120; // Increased for additional stats
    if (remaining > 0) {
        for (int i = 0; i < remaining; ++i) {
            output << " ";
        }
    }
    output << "\033[0m\n";
    
    // Separator
    for (int i = 0; i < cols; ++i) {
        output << "-";
    }
    output << "\n";
}

void TUI::draw_runways_to_stream(std::stringstream& output, int cols, int max_rows) {
    auto runways = get_runways_snapshot();
    
    // Section header with focus indicator
    std::string header = "Runways (" + std::to_string(runways.size()) + ")";
    if (current_section_ == FocusSection::Runways && !detail_view_) {
        output << "\033[1;7m" << header << "\033[0m\n"; // Highlighted when focused
    } else {
        output << "\033[1m" << header << ":\033[0m\n";
    }
    
    if (runways.empty()) {
        output << "  No runways discovered\n";
        return;
    }
    
    // Show first N runways (limited by max_rows)
    size_t max_show = std::min(runways.size(), static_cast<size_t>(max_rows - 1));
    for (size_t i = 0; i < max_show; ++i) {
        auto runway = runways[i];
        
        // Highlight selected item
        bool is_selected = (current_section_ == FocusSection::Runways && 
                           static_cast<int>(i) == selected_index_ && !detail_view_);
        if (is_selected) {
            output << "\033[7m> "; // Reverse video for selection
        } else {
            output << "  ";
        }
        
        output << truncate_string(runway->id, 30);
        
        std::string status = get_runway_status_string(runway, "");
        if (status == "Accessible") {
            output << " \033[32m[OK]\033[0m";
        } else if (status == "Partially Accessible") {
            output << " \033[33m[PARTIAL]\033[0m";
        } else {
            output << " \033[31m[FAIL]\033[0m";
        }
        
        output << " " << runway->interface;
        if (runway->upstream_proxy) {
            output << " -> " << runway->upstream_proxy->config.host;
        }
        
        int remaining = cols - 70;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                output << " ";
            }
        }
        if (is_selected) {
            output << "\033[0m"; // Reset after selection
        }
        output << "\n";
    }
    
    if (runways.size() > max_show) {
        output << "  ... and " << (runways.size() - max_show) << " more\n";
    }
}

void TUI::draw_targets_to_stream(std::stringstream& output, int cols, int max_rows) {
    auto targets = get_targets_snapshot();
    
    // Section header with focus indicator
    std::string header = "Targets (" + std::to_string(targets.size()) + ")";
    if (current_section_ == FocusSection::Targets && !detail_view_) {
        output << "\033[1;7m" << header << "\033[0m\n"; // Highlighted when focused
    } else {
        output << "\033[1m" << header << ":\033[0m\n";
    }
    
    if (targets.empty()) {
        output << "  No targets accessed yet\n";
        return;
    }
    
    // Show first N targets (limited by max_rows)
    size_t max_show = std::min(targets.size(), static_cast<size_t>(max_rows - 1));
    for (size_t i = 0; i < max_show; ++i) {
        std::string target = targets[i];
        
        // Highlight selected item
        bool is_selected = (current_section_ == FocusSection::Targets && 
                           static_cast<int>(i) == selected_index_ && !detail_view_);
        if (is_selected) {
            output << "\033[7m> "; // Reverse video for selection
        } else {
            output << "  ";
        }
        auto metrics_map = tracker_->get_target_metrics(target);
        
        output << "  " << truncate_string(target, 40);
        
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
            output << " \033[32m[OK]\033[0m";
        } else if (best_status == "Partial") {
            output << " \033[33m[PARTIAL]\033[0m";
        } else {
            output << " \033[31m[FAIL]\033[0m";
        }
        
        int remaining = cols - 60;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                output << " ";
            }
        }
        if (is_selected) {
            output << "\033[0m"; // Reset after selection
        }
        output << "\n";
    }
    
    if (targets.size() > max_show) {
        output << "  ... and " << (targets.size() - max_show) << " more\n";
    }
}

void TUI::draw_connections_to_stream(std::stringstream& output, int cols, int max_rows) {
    auto conns = get_connections_snapshot();
    
    // Section header with focus indicator
    std::string header = "Active Connections (" + std::to_string(conns.size()) + ")";
    if (current_section_ == FocusSection::Connections && !detail_view_) {
        output << "\033[1;7m" << header << "\033[0m\n"; // Highlighted when focused
    } else {
        output << "\033[1m" << header << ":\033[0m\n";
    }
    
    if (conns.empty()) {
        output << "  No active connections\n";
        return;
    }
    
    // Show first N connections (limited by max_rows)
    size_t max_show = std::min(conns.size(), static_cast<size_t>(max_rows - 1));
    for (size_t i = 0; i < max_show; ++i) {
        const auto& conn = conns[i];
        
        // Highlight selected item
        bool is_selected = (current_section_ == FocusSection::Connections && 
                           static_cast<int>(i) == selected_index_ && !detail_view_);
        if (is_selected) {
            output << "\033[7m> "; // Reverse video for selection
        } else {
            output << "  ";
        }
        output << truncate_string(conn.client_ip + ":" + std::to_string(conn.client_port), 18);
        output << " -> " << truncate_string(conn.target_host + ":" + std::to_string(conn.target_port), 22);
        output << " [" << truncate_string(conn.runway_id, 12) << "]";
        output << " " << truncate_string(conn.method, 6);
        
        // Show live duration
        if (conn.start_time > 0) {
            uint64_t now = std::time(nullptr);
            uint64_t duration = now - conn.start_time;
            output << " " << duration << "s";
        }
        
        // Show live bytes
        uint64_t total_bytes = conn.bytes_sent + conn.bytes_received;
        if (total_bytes > 0) {
            output << " " << format_bytes(total_bytes);
        }
        
        // Show status indicator
        if (conn.status == "active") {
            output << " \033[32m●\033[0m"; // Green dot for active
        } else if (conn.status == "connecting") {
            output << " \033[33m●\033[0m"; // Yellow dot for connecting
        } else if (conn.status == "error") {
            output << " \033[31m●\033[0m"; // Red dot for error
        }
        
        int remaining = cols - 95;
        if (remaining > 0) {
            for (int j = 0; j < remaining; ++j) {
                output << " ";
            }
        }
        if (is_selected) {
            output << "\033[0m"; // Reset after selection
        }
        output << "\n";
    }
    
    if (conns.size() > max_show) {
        output << "  ... and " << (conns.size() - max_show) << " more\n";
    }
}

void TUI::draw_footer_to_stream(std::stringstream& output, int cols, int /*row*/) {
    output << "\033[" << get_terminal_rows() << ";1H"; // Move to last row
    output << "\033[1;37;44m"; // Bold white on blue
    if (detail_view_) {
        output << " ESC: Back | Ctrl+C: Stop ";
    } else {
        output << " Tab: Switch | ↑↓: Navigate | Enter: Details | Ctrl+C: Stop ";
    }
    for (int i = (detail_view_ ? 30 : 60); i < cols; ++i) {
        output << " ";
    }
    output << "\033[0m";
}

void TUI::draw_detail_view_to_stream(std::stringstream& output, int cols, int rows) {
    output << "\033[1;37;44m"; // Bold white on blue
    output << " Detail View ";
    for (int i = 13; i < cols; ++i) {
        output << " ";
    }
    output << "\033[0m\n";
    
    // Determine what type of item we're viewing
    if (current_section_ == FocusSection::Runways) {
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
            output << "  Interface: " << runway->interface << "\n";
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
    } else if (current_section_ == FocusSection::Targets) {
        output << "\033[1mTarget:\033[0m " << detail_item_id_ << "\n\n";
        auto metrics_map = tracker_->get_target_metrics(detail_item_id_);
        
        if (!metrics_map.empty()) {
            output << "\033[1mRunway Metrics:\033[0m\n";
            for (const auto& pair : metrics_map) {
                const auto& metrics = pair.second;
                output << "  " << pair.first << ":\n";
                output << "    State: ";
                switch (metrics.state) {
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
                output << "\n";
                output << "    Success Rate: " << static_cast<int>(metrics.success_rate * 100) << "%\n";
                output << "    Avg Response Time: " << std::fixed << std::setprecision(2) 
                       << metrics.avg_response_time << "s\n";
                output << "    Total Attempts: " << metrics.total_attempts << "\n";
                output << "    Network Success: " << metrics.network_success_count << "\n";
                output << "    User Success: " << metrics.user_success_count << "\n";
                output << "    Failures: " << metrics.failure_count << "\n";
            }
        }
    } else if (current_section_ == FocusSection::Connections) {
        std::lock_guard<std::mutex> lock(connections_mutex_);
        auto it = connections_.find(detail_item_id_);
        if (it != connections_.end()) {
            const auto& conn = it->second;
            output << "\033[1mConnection ID:\033[0m " << conn.id << "\n\n";
            output << "\033[1mDetails:\033[0m\n";
            output << "  Client: " << conn.client_ip << ":" << conn.client_port << "\n";
            output << "  Target: " << conn.target_host << ":" << conn.target_port << "\n";
            output << "  Runway: " << conn.runway_id << "\n";
            output << "  Method: " << conn.method << "\n";
            output << "  Path: " << conn.path << "\n";
            output << "  Status: " << conn.status << "\n";
            output << "  Bytes Sent: " << utils::format_bytes(conn.bytes_sent) << "\n";
            output << "  Bytes Received: " << utils::format_bytes(conn.bytes_received) << "\n";
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
    return runway_manager_->get_all_runways();
}

std::vector<std::string> TUI::get_targets_snapshot() {
    return tracker_->get_all_targets();
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
