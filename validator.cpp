#include "validator.h"
#include "utils.h"
#include <algorithm>

SuccessValidator::SuccessValidator() {
}

std::pair<bool, bool> SuccessValidator::validate_http(uint16_t status_code, const std::vector<uint8_t>& body) {
    // Network success: response received
    bool network_success = (status_code >= 200 && status_code < 400);
    
    if (!network_success) {
        return std::make_pair(false, false);
    }
    
    // User success: check for actual content vs error pages
    bool user_success = false;
    if (!body.empty()) {
        // Convert to string (defensive: handle non-UTF8)
        std::string content;
        content.reserve(body.size());
        for (uint8_t byte : body) {
            if (byte >= 32 && byte < 127) { // Printable ASCII
                content += static_cast<char>(byte);
            } else if (byte == '\n' || byte == '\r' || byte == '\t') {
                content += static_cast<char>(byte);
            }
        }
        
        content = utils::to_lower(content);
        user_success = !contains_error_patterns(content);
    }
    
    return std::make_pair(network_success, user_success);
}

bool SuccessValidator::contains_error_patterns(const std::string& content) const {
    const std::vector<std::string> error_patterns = {
        "blocked", "forbidden", "access denied", "error 403", "error 404"
    };
    
    for (const auto& pattern : error_patterns) {
        if (content.find(pattern) != std::string::npos) {
            return true;
        }
    }
    
    return false;
}
