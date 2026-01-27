#include "tui.h"
#include "utils.h"
#include "logger.h"
#include "routing.h"
#include "config.h"
#include <iostream>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <sys/select.h>
#include <sys/time.h>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <unistd.h>
#include <termios.h>
#include <fcntl.h>
#include <sys/select.h>
#include <sys/time.h>
#endif

// Non-blocking keyboard input handling (zero dependencies, standard library only)

void TUI::handle_input() {
    if (!utils::is_terminal()) {
        return;
    }
    
#ifdef _WIN32
    // Windows: Check for mouse or keyboard input
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD numEvents = 0;
    GetNumberOfConsoleInputEvents(hIn, &numEvents);
    
    if (numEvents > 0) {
        INPUT_RECORD inputRecord;
        DWORD numRead = 0;
        PeekConsoleInput(hIn, &inputRecord, 1, &numRead);
        
        if (numRead > 0) {
            if (inputRecord.EventType == MOUSE_EVENT) {
                // Mouse events are only enabled if config.mouse_enabled is true
                // If we receive mouse events, they're already enabled, so process them
                MOUSE_EVENT_RECORD& mer = inputRecord.Event.MouseEvent;
                DWORD buttonState = mer.dwButtonState;
                COORD pos = mer.dwMousePosition;
                
                // Handle mouse wheel
                if (buttonState & 0xFF000000) { // Wheel
                    short wheelDelta = (buttonState >> 16) & 0xFFFF;
                    int direction = (wheelDelta > 0) ? -1 : 1; // Positive = scroll up
                    handle_mouse_scroll(direction, pos.X + 1, pos.Y + 1); // Convert to 1-based
                } else if (mer.dwEventFlags == 0) {
                    // Regular click
                    int button = 0;
                    if (buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) button = 0; // Left
                    else if (buttonState & FROM_LEFT_2ND_BUTTON_PRESSED) button = 1; // Middle
                    else if (buttonState & RIGHTMOST_BUTTON_PRESSED) button = 2; // Right
                    
                    if (button >= 0) {
                        handle_mouse_click(button, pos.X + 1, pos.Y + 1); // Convert to 1-based
                    }
                }
                
                // Consume the event
                ReadConsoleInput(hIn, &inputRecord, 1, &numRead);
            } else if (inputRecord.EventType == KEY_EVENT && inputRecord.Event.KeyEvent.bKeyDown) {
                // Handle keyboard
                WORD vk = inputRecord.Event.KeyEvent.wVirtualKeyCode;
                DWORD ctrl = inputRecord.Event.KeyEvent.dwControlKeyState;
                
                ReadConsoleInput(hIn, &inputRecord, 1, &numRead);
                
                if (vk == VK_TAB) {
                    if (ctrl & SHIFT_PRESSED) {
                        navigate_prev_section();
                    } else {
                        navigate_next_section();
                    }
                } else if (vk == VK_UP) {
                    navigate_up();
                } else if (vk == VK_DOWN) {
                    navigate_down();
                } else if (vk == VK_PRIOR) { // Page Up
                    navigate_page_up();
                } else if (vk == VK_NEXT) { // Page Down
                    navigate_page_down();
                } else if (vk == VK_HOME) {
                    if (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
                        switch_tab(Tab::Runways); // Ctrl+Home: go to first tab
                    } else {
                        navigate_to_top(); // Home: go to first item
                    }
                } else if (vk == VK_END) {
                    if (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
                        switch_tab(Tab::Help); // Ctrl+End: go to last tab
                    } else {
                        navigate_to_bottom(); // End: go to last item
                    }
                } else if (vk == VK_LEFT) {
                    navigate_prev_section();
                } else if (vk == VK_RIGHT) {
                    navigate_next_section();
                } else if (vk == VK_RETURN) {
                    show_detail();
                } else if (vk == VK_ESCAPE) {
                    hide_detail();
                } else if (vk == VK_BACK || vk == VK_DELETE) {
                    navigate_up(); // Backspace/Delete: go up one item
                } else if (vk == VK_SPACE) {
                    navigate_page_down(); // Space: page down
                } else if (vk == 'B' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    cycle_routing_mode();
                } else if (vk == 'F' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    navigate_page_down(); // Ctrl+F: page down
                } else if (vk == 'D' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    navigate_half_page_down(); // Ctrl+D: half page down
                } else if (vk == 'U' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    navigate_half_page_up(); // Ctrl+U: half page up
                } else if (vk == 'B' && !(ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    navigate_page_up(); // b: page up (vim-style)
                } else if (vk == 'H' || vk == 'h') {
                    navigate_prev_section(); // h: left (vim-style)
                } else if (vk == 'J' || vk == 'j') {
                    navigate_down(); // j: down (vim-style)
                } else if (vk == 'K' || vk == 'k') {
                    navigate_up(); // k: up (vim-style)
                } else if (vk == 'L' || vk == 'l') {
                    navigate_next_section(); // l: right (vim-style)
                } else if (vk == 'G' && (ctrl & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
                    navigate_to_bottom(); // Ctrl+G: go to bottom (vim-style)
                } else if (vk == 'G' || vk == 'g') {
                    // g: go to top (vim-style, but need double-g, so handle on second press)
                    static uint64_t last_g_press = 0;
                    uint64_t now = std::time(nullptr);
                    if (now - last_g_press < 1) { // Within 1 second
                        navigate_to_top();
                    }
                    last_g_press = now;
                } else if (vk >= '1' && vk <= '5') {
                    // Switch tabs with 1-5 keys
                    int tab_num = vk - '1';
                    switch_tab(static_cast<Tab>(tab_num));
                } else if (vk == 'q') {
                    // 'q' behaves like ESC: hides detail view when in detail view, quits otherwise
                    if (detail_view_) {
                        hide_detail();
                    } else {
                        show_quit_confirmation();
                    }
                } else if (vk == 'Q') {
                    // 'Q' always quits (with confirmation)
                    show_quit_confirmation();
                } else if (vk == VK_OEM_2 || vk == 0xBF) { // ? key
                    switch_tab(Tab::Help);
                } else if (vk == VK_F1) {
                    switch_tab(Tab::Help); // F1: help
                } else if (vk == VK_F5) {
                    should_redraw_ = true; // F5: refresh
                }
            }
        }
    }
#else
    // POSIX: Non-blocking input check
    struct timeval tv = {0, 0};
    fd_set rdfs;
    FD_ZERO(&rdfs);
    FD_SET(STDIN_FILENO, &rdfs);
    
    if (select(STDIN_FILENO + 1, &rdfs, nullptr, nullptr, &tv) > 0) {
        char buf[16]; // Increased for mouse sequences
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        
        if (n > 0) {
            // Handle mouse events (SGR format: \033[<button;x;yM or m) - only if enabled
            // Note: We need to check config, but we don't have direct access here
            // The mouse tracking is only enabled in setup_terminal if config allows it
            // So if we receive mouse events, they're already enabled, but we can still check
            // For now, we'll process mouse events - the enable_mouse_tracking() won't be called if disabled
            if (buf[0] == '\033' && n >= 6 && buf[1] == '[' && buf[2] == '<') {
                // Parse SGR mouse sequence: \033[<button;x;yM
                int button = 0, x = 0, y = 0;
                if (sscanf(buf + 3, "%d;%d;%d", &button, &x, &y) == 3) {
                    // Check if it's a mouse event (M) or release (m)
                    char event_type = buf[n - 1];
                    
                    if (event_type == 'M' || event_type == 'm') {
                        // Extract button info
                        int btn_code = button & 0x3F; // Lower 6 bits
                        int btn_type = (button >> 6) & 0x3; // Button type
                        bool is_press = (event_type == 'M');
                        bool is_wheel = (btn_type == 1); // Wheel event
                        
                        if (is_wheel && is_press) {
                            // Mouse wheel: button 64 = up, 65 = down
                            int wheel_dir = (btn_code == 64) ? -1 : 1;
                            handle_mouse_scroll(wheel_dir, x, y);
                        } else if (is_press && btn_type == 0) {
                            // Regular mouse click
                            // Button: 0 = left, 1 = middle, 2 = right
                            int click_button = btn_code;
                            handle_mouse_click(click_button, x, y);
                        }
                    }
                }
                return; // Mouse event handled
            }
            
            // Handle escape sequences (arrow keys, Page Up/Down, Home/End, etc.)
            if (buf[0] == '\033' && n >= 3) {
                if (buf[1] == '[') {
                    // Check for Shift+Tab (ESC [ Z)
                    if (n >= 3 && buf[2] == 'Z') {
                        navigate_prev_section();
                    } else if (buf[2] == 'A') { // Up arrow
                        navigate_up();
                    } else if (buf[2] == 'B') { // Down arrow
                        navigate_down();
                    } else if (buf[2] == 'C') { // Right arrow (next tab)
                        navigate_next_section();
                    } else if (buf[2] == 'D') { // Left arrow (previous tab)
                        navigate_prev_section();
                    } else if (buf[2] == 'H') { // Home
                        navigate_to_top();
                    } else if (buf[2] == 'F') { // End
                        navigate_to_bottom();
                    } else if (n >= 4) {
                        // Page Up: ESC [ 5 ~ or ESC [ ? 5 ~ (some terminals)
                        if (buf[2] == '5' && buf[3] == '~') {
                            navigate_page_up();
                        } else if (buf[2] == '6' && buf[3] == '~') { // Page Down: ESC [ 6 ~
                            navigate_page_down();
                        } else if (n >= 5 && buf[2] == '?' && buf[3] == '5' && buf[4] == '~') {
                            // Alternative Page Up sequence
                            navigate_page_up();
                        } else if (n >= 5 && buf[2] == '?' && buf[3] == '6' && buf[4] == '~') {
                            // Alternative Page Down sequence
                            navigate_page_down();
                        } else if (n >= 4 && buf[2] == '1' && buf[3] == '~') {
                            // Home key (some terminals)
                            navigate_to_top();
                        } else if (n >= 4 && buf[2] == '4' && buf[3] == '~') {
                            // End key (some terminals)
                            navigate_to_bottom();
                        }
                    }
                } else if (buf[1] == 'O') {
                    // Function keys: ESC O P = F1, ESC O Q = F2, etc.
                    if (n >= 3) {
                        if (buf[2] == 'P') { // F1
                            switch_tab(Tab::Help);
                        } else if (buf[2] == 'Q') { // F2
                            switch_tab(Tab::Runways);
                        } else if (buf[2] == 'R') { // F3
                            switch_tab(Tab::Targets);
                        } else if (buf[2] == 'S') { // F4
                            switch_tab(Tab::Connections);
                        }
                    }
                }
            } else if (buf[0] == '\t' || buf[0] == 9) { // Tab
                navigate_next_section();
            } else if (buf[0] == '\002') { // Ctrl+B (ASCII 2)
                cycle_routing_mode();
            } else if (buf[0] == '\004') { // Ctrl+D (ASCII 4)
                navigate_half_page_down();
            } else if (buf[0] == '\025') { // Ctrl+U (ASCII 21)
                navigate_half_page_up();
            } else if (buf[0] == '\006') { // Ctrl+F (ASCII 6)
                navigate_page_down();
            } else if (buf[0] == ' ') { // Space
                navigate_page_down();
            } else if (buf[0] == '\b' || buf[0] == 127) { // Backspace
                navigate_up();
            } else if (buf[0] >= '1' && buf[0] <= '5') {
                // Switch tabs with 1-5 keys
                int tab_num = buf[0] - '1';
                switch_tab(static_cast<Tab>(tab_num));
            } else if (buf[0] == 'q') {
                // 'q' behaves like ESC: hides detail view when in detail view, quits otherwise
                if (detail_view_) {
                    hide_detail();
                } else {
                    show_quit_confirmation();
                }
            } else if (buf[0] == 'Q') {
                // 'Q' always quits (with confirmation)
                show_quit_confirmation();
            } else if (buf[0] == '?') {
                switch_tab(Tab::Help);
            } else if (buf[0] == '\n' || buf[0] == '\r' || buf[0] == 13) { // Enter
                show_detail();
            } else if (buf[0] == '\033' && n == 1) { // Escape (single ESC)
                hide_detail();
            } else if (buf[0] == 'h' || buf[0] == 'H') {
                navigate_prev_section(); // h: left (vim-style)
            } else if (buf[0] == 'j' || buf[0] == 'J') {
                navigate_down(); // j: down (vim-style)
            } else if (buf[0] == 'k' || buf[0] == 'K') {
                navigate_up(); // k: up (vim-style)
            } else if (buf[0] == 'l' || buf[0] == 'L') {
                navigate_next_section(); // l: right (vim-style)
            } else if (buf[0] == 'b' || buf[0] == 'B') {
                navigate_page_up(); // b: page up (vim-style)
            } else if (buf[0] == 'g') {
                // g: go to top (vim-style, but need double-g, so handle on second press)
                static uint64_t last_g_press = 0;
                uint64_t now = std::time(nullptr);
                if (now - last_g_press < 1) { // Within 1 second
                    navigate_to_top();
                }
                last_g_press = now;
            } else if (buf[0] == 'G') {
                navigate_to_bottom(); // G: go to bottom (vim-style)
            } else if (buf[0] >= 1 && buf[0] <= 26) {
                // Control characters (Ctrl+A = 1, Ctrl+B = 2, Ctrl+C = 3, etc.)
                if (buf[0] == 2) { // Ctrl+B
                    cycle_routing_mode();
                } else if (buf[0] == 5) { // Ctrl+E (scroll down one line)
                    navigate_down();
                } else if (buf[0] == 25) { // Ctrl+Y (scroll up one line)
                    navigate_up();
                }
                // Note: Ctrl+C (3) is handled by signal handler
            }
        }
    }
#endif
}

void TUI::navigate_up() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    if (selected_index_ > 0) {
        selected_index_--;
        should_redraw_ = true;
    }
}

void TUI::navigate_down() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    int max_items = get_current_tab_size();
    if (selected_index_ < max_items - 1) {
        selected_index_++;
        should_redraw_ = true;
    }
}

void TUI::navigate_page_up() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    // Calculate visible items based on terminal size
    int rows = get_terminal_rows();
    
    // Account for margins and UI elements - use centralized constants
    int available_rows = rows - MARGIN_TOP - MARGIN_BOTTOM;
    int content_h = available_rows - STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT - SUMMARY_BAR_HEIGHT - COMMAND_BAR_HEIGHT;
    
    // Visible items in the content area (minus header and borders)
    int visible_items = content_h - 3; // Leave space for header and borders
    if (visible_items < 1) visible_items = 1;
    
    // Scroll up by visible_items
    selected_index_ = std::max(0, selected_index_ - visible_items);
    should_redraw_ = true;
}

void TUI::navigate_page_down() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    int max_items = get_current_tab_size();
    
    // Calculate visible items based on terminal size
    int rows = get_terminal_rows();
    
    // Account for margins and UI elements - use centralized constants
    int available_rows = rows - MARGIN_TOP - MARGIN_BOTTOM;
    int content_h = available_rows - STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT - SUMMARY_BAR_HEIGHT - COMMAND_BAR_HEIGHT;
    
    // Visible items in the content area (minus header and borders)
    int visible_items = content_h - 3; // Leave space for header and borders
    if (visible_items < 1) visible_items = 1;
    
    // Scroll down by visible_items
    selected_index_ = std::min(max_items - 1, selected_index_ + visible_items);
    should_redraw_ = true;
}

void TUI::navigate_half_page_up() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    // Calculate visible items based on terminal size
    int rows = get_terminal_rows();
    
    // Account for margins and UI elements - use centralized constants
    int available_rows = rows - MARGIN_TOP - MARGIN_BOTTOM;
    int content_h = available_rows - STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT - SUMMARY_BAR_HEIGHT - COMMAND_BAR_HEIGHT;
    
    // Half page = half of visible items
    int visible_items = content_h - 3;
    if (visible_items < 1) visible_items = 1;
    int half_page = visible_items / 2;
    if (half_page < 1) half_page = 1;
    
    // Scroll up by half page
    selected_index_ = std::max(0, selected_index_ - half_page);
    should_redraw_ = true;
}

void TUI::navigate_half_page_down() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    int max_items = get_current_tab_size();
    
    // Calculate visible items based on terminal size
    int rows = get_terminal_rows();
    
    // Account for margins and UI elements - use centralized constants
    int available_rows = rows - MARGIN_TOP - MARGIN_BOTTOM;
    int content_h = available_rows - STATUS_BAR_HEIGHT - TAB_BAR_HEIGHT - SUMMARY_BAR_HEIGHT - COMMAND_BAR_HEIGHT;
    
    // Half page = half of visible items
    int visible_items = content_h - 3;
    if (visible_items < 1) visible_items = 1;
    int half_page = visible_items / 2;
    if (half_page < 1) half_page = 1;
    
    // Scroll down by half page
    selected_index_ = std::min(max_items - 1, selected_index_ + half_page);
    should_redraw_ = true;
}

void TUI::navigate_to_top() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    selected_index_ = 0;
    scroll_offset_ = 0;
    should_redraw_ = true;
}

void TUI::navigate_to_bottom() {
    if (detail_view_) {
        return; // No navigation in detail view
    }
    
    // Only allow navigation in tabs with selectable items
    if (current_tab_ == Tab::Stats || current_tab_ == Tab::Help) {
        return;
    }
    
    int max_items = get_current_tab_size();
    if (max_items > 0) {
        selected_index_ = max_items - 1;
        should_redraw_ = true;
    }
}

void TUI::switch_tab(Tab tab) {
    if (detail_view_) {
        hide_detail();
    }
    
    current_tab_ = tab;
    selected_index_ = 0;
    scroll_offset_ = 0;
    should_redraw_ = true;
}

void TUI::navigate_next_section() {
    if (detail_view_) {
        hide_detail();
        return;
    }
    
    // Cycle to next tab
    int current = static_cast<int>(current_tab_);
    current = (current + 1) % 5;
    switch_tab(static_cast<Tab>(current));
}

void TUI::navigate_prev_section() {
    if (detail_view_) {
        hide_detail();
        return;
    }
    
    // Cycle to previous tab
    int current = static_cast<int>(current_tab_);
    current = (current - 1 + 5) % 5;
    switch_tab(static_cast<Tab>(current));
}

void TUI::show_detail() {
    if (detail_view_) {
        return; // Already in detail view
    }
    
    // Get current item ID based on section and index
    std::string item_id = get_current_item_id();
    if (!item_id.empty()) {
        detail_item_id_ = item_id;
        detail_view_ = true;
        should_redraw_ = true;
    }
}

void TUI::hide_detail() {
    if (detail_view_) {
        detail_view_ = false;
        detail_item_id_.clear();
        should_redraw_ = true;
    }
}

int TUI::get_current_tab_size() {
    switch (current_tab_) {
        case Tab::Runways:
            return static_cast<int>(get_runways_snapshot().size());
        case Tab::Targets:
            return static_cast<int>(get_targets_snapshot().size());
        case Tab::Connections:
            return static_cast<int>(get_connections_snapshot().size());
        case Tab::Stats:
        case Tab::Help:
            return 0; // These tabs don't have selectable items
    }
    return 0;
}

std::string TUI::get_current_item_id() {
    switch (current_tab_) {
        case Tab::Runways: {
            auto runways = get_runways_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(runways.size())) {
                return runways[selected_index_]->id;
            }
            break;
        }
        case Tab::Targets: {
            auto targets = get_targets_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(targets.size())) {
                return targets[selected_index_];
            }
            break;
        }
        case Tab::Connections: {
            auto conns = get_connections_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(conns.size())) {
                return conns[selected_index_].id;
            }
            break;
        }
        case Tab::Stats:
        case Tab::Help:
            return ""; // No selectable items
    }
    return "";
}

void TUI::cycle_routing_mode() {
    RoutingMode current_mode = routing_engine_->get_mode();
    RoutingMode next_mode;
    
    // Cycle through modes: Latency -> FirstAccessible -> RoundRobin -> Latency
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
    should_redraw_ = true;
    
    // Log the change
    std::string mode_str;
    switch (next_mode) {
        case RoutingMode::Latency: mode_str = "Latency"; break;
        case RoutingMode::FirstAccessible: mode_str = "First Accessible"; break;
        case RoutingMode::RoundRobin: mode_str = "Round Robin"; break;
    }
    Logger::instance().log(LogLevel::INFO, "Routing mode changed to: " + mode_str);
}
