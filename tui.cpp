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
    , start_time_(std::time(nullptr)) {
}

TUI::~TUI() {
    stop();
}

void TUI::setup_terminal() {
    if (!utils::is_terminal()) {
        return;
    }
    
#ifdef _WIN32
    // Windows: Enable ANSI escape codes
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#else
    // POSIX: Save terminal state
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag &= ~(ICANON | ECHO);
    term.c_cc[VMIN] = 0;
    term.c_cc[VTIME] = 0;
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
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
    DWORD dwMode = 0;
    GetConsoleMode(hOut, &dwMode);
    dwMode &= ~ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
#else
    // POSIX: Restore terminal state
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_lflag |= (ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
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
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
#else
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_row > 0 ? w.ws_row : 24;
#endif
}

int TUI::get_terminal_cols() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi);
    return csbi.srWindow.Right - csbi.srWindow.Left + 1;
#else
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    return w.ws_col > 0 ? w.ws_col : 80;
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
    
    // Responsive update loop - update every 100ms for smooth experience
    auto last_update = std::chrono::steady_clock::now();
    const auto update_interval = std::chrono::milliseconds(100);
    
    while (running_ && proxy_server_->is_running()) {
        auto now = std::chrono::steady_clock::now();
        
        // Always redraw if enough time has passed or if explicitly requested
        if (should_redraw_ || (now - last_update) >= update_interval) {
            draw();
            should_redraw_ = false;
            last_update = now;
        }
        
        // Small sleep to prevent CPU spinning, but keep it responsive
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
    // Fast drawing - use single clear and cursor positioning
    clear_screen();
    move_cursor(1, 1);
    
    int rows = get_terminal_rows();
    int cols = get_terminal_cols();
    
    if (rows < 10 || cols < 40) {
        std::cout << "Terminal too small (min 40x10)\n";
        std::cout.flush();
        return;
    }
    
    // Build output in stringstream for faster rendering
    std::stringstream output;
    
    // Draw header
    draw_header_to_stream(output, cols);
    
    int header_height = 3;
    int runways_height = std::min(8, (rows - header_height - 8) / 3);
    int targets_height = std::min(6, (rows - header_height - runways_height - 6) / 2);
    
    // Draw sections
    draw_runways_to_stream(output, cols, runways_height);
    draw_targets_to_stream(output, cols, targets_height);
    draw_connections_to_stream(output, cols, rows - header_height - runways_height - targets_height - 2);
    draw_footer_to_stream(output, cols, rows);
    
    // Single output operation for responsiveness
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
    std::cout << " | Connections: " << proxy_server_->get_active_connections();
    std::cout << " | Total: " << proxy_server_->get_total_connections();
    
    int remaining = cols - 80;
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

std::vector<std::shared_ptr<Runway>> TUI::get_runways_snapshot() {
    return runway_manager_->get_all_runways();
}

std::vector<std::string> TUI::get_targets_snapshot() {
    return tracker_->get_all_targets();
}

std::vector<ConnectionInfo> TUI::get_connections_snapshot() {
    std::lock_guard<std::mutex> lock(connections_mutex_);
    std::vector<ConnectionInfo> result;
    for (const auto& pair : connections_) {
        result.push_back(pair.second);
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
