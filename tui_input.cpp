#include "tui.h"
#include "utils.h"
#include <iostream>
#include <cstring>
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
    // Windows: Check if key is available
    if (_kbhit()) {
        int ch = _getch();
        
        if (ch == 0 || ch == 224) {
            // Extended key code
            int ext = _getch();
            if (ext == 72) { // Up arrow
                navigate_up();
            } else if (ext == 80) { // Down arrow
                navigate_down();
            }
        } else if (ch == 9) { // Tab
            navigate_next_section();
        } else if (ch == 13) { // Enter
            show_detail();
        } else if (ch == 27) { // Escape
            hide_detail();
        }
    }
#else
    // POSIX: Non-blocking input check
    struct timeval tv = {0, 0};
    fd_set rdfs;
    FD_ZERO(&rdfs);
    FD_SET(STDIN_FILENO, &rdfs);
    
    if (select(STDIN_FILENO + 1, &rdfs, nullptr, nullptr, &tv) > 0) {
        char buf[4];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        
        if (n > 0) {
            // Handle escape sequences (arrow keys)
            if (buf[0] == '\033' && n >= 3) {
                if (buf[1] == '[') {
                    if (buf[2] == 'A') { // Up arrow
                        navigate_up();
                    } else if (buf[2] == 'B') { // Down arrow
                        navigate_down();
                    } else if (buf[2] == 'C') { // Right arrow (same as Tab)
                        navigate_next_section();
                    } else if (buf[2] == 'D') { // Left arrow (previous section)
                        navigate_prev_section();
                    }
                }
            } else if (buf[0] == '\t' || buf[0] == 9) { // Tab
                navigate_next_section();
            } else if (buf[0] == '\n' || buf[0] == '\r' || buf[0] == 13) { // Enter
                show_detail();
            } else if (buf[0] == '\033' && n == 1) { // Escape (single ESC)
                hide_detail();
            }
        }
    }
#endif
}

void TUI::navigate_up() {
    if (detail_view_) {
        return; // No navigation in detail view
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
    
    int max_items = get_current_section_size();
    if (selected_index_ < max_items - 1) {
        selected_index_++;
        should_redraw_ = true;
    }
}

void TUI::navigate_next_section() {
    if (detail_view_) {
        hide_detail();
        return;
    }
    
    switch (current_section_) {
        case FocusSection::Runways:
            current_section_ = FocusSection::Targets;
            break;
        case FocusSection::Targets:
            current_section_ = FocusSection::Connections;
            break;
        case FocusSection::Connections:
            current_section_ = FocusSection::Runways;
            break;
    }
    selected_index_ = 0;
    should_redraw_ = true;
}

void TUI::navigate_prev_section() {
    if (detail_view_) {
        hide_detail();
        return;
    }
    
    switch (current_section_) {
        case FocusSection::Runways:
            current_section_ = FocusSection::Connections;
            break;
        case FocusSection::Targets:
            current_section_ = FocusSection::Runways;
            break;
        case FocusSection::Connections:
            current_section_ = FocusSection::Targets;
            break;
    }
    selected_index_ = 0;
    should_redraw_ = true;
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

int TUI::get_current_section_size() {
    switch (current_section_) {
        case FocusSection::Runways:
            return static_cast<int>(get_runways_snapshot().size());
        case FocusSection::Targets:
            return static_cast<int>(get_targets_snapshot().size());
        case FocusSection::Connections:
            return static_cast<int>(get_connections_snapshot().size());
    }
    return 0;
}

std::string TUI::get_current_item_id() {
    switch (current_section_) {
        case FocusSection::Runways: {
            auto runways = get_runways_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(runways.size())) {
                return runways[selected_index_]->id;
            }
            break;
        }
        case FocusSection::Targets: {
            auto targets = get_targets_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(targets.size())) {
                return targets[selected_index_];
            }
            break;
        }
        case FocusSection::Connections: {
            auto conns = get_connections_snapshot();
            if (selected_index_ >= 0 && selected_index_ < static_cast<int>(conns.size())) {
                return conns[selected_index_].id;
            }
            break;
        }
    }
    return "";
}
