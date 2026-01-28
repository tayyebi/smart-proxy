#include "utils.h"
#include <cctype>
#include <algorithm>
#include <cstring>
#include <iostream>
#include <fstream>
#include <sys/stat.h>
#include <sys/types.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
// On Windows, use _mkdir which takes only path parameter
#define MKDIR(path) _mkdir(path)
#else
#include <unistd.h>
// On POSIX, use mkdir which takes path and mode parameters
#define MKDIR(path) mkdir(path, 0755)
#endif

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define fileno _fileno
#else
#include <unistd.h>
#endif

namespace utils {

bool is_valid_ipv4(const std::string& ip) {
    if (ip.empty() || ip.length() > 15) return false;
    
    std::vector<std::string> parts = split(ip, '.');
    if (parts.size() != 4) return false;
    
    for (const auto& part : parts) {
        if (part.empty()) return false;
        uint32_t num;
        if (!safe_str_to_uint32(part, num) || num > 255) {
            return false;
        }
    }
    return true;
}

bool is_private_ip(const std::string& ip) {
    // RFC 1918: Private Address Space
    // 10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16
    if (!is_valid_ipv4(ip)) return false;
    
    uint32_t addr = ip_to_uint32(ip);
    
    // 10.0.0.0/8
    if ((addr & 0xFF000000) == 0x0A000000) return true;
    
    // 172.16.0.0/12
    if ((addr & 0xFFF00000) == 0xAC100000) return true;
    
    // 192.168.0.0/16
    if ((addr & 0xFFFF0000) == 0xC0A80000) return true;
    
    return false;
}

uint32_t ip_to_uint32(const std::string& ip) {
    uint32_t result = 0;
    std::vector<std::string> parts = split(ip, '.');
    if (parts.size() != 4) return 0;
    
    for (size_t i = 0; i < 4; ++i) {
        uint32_t num;
        if (!safe_str_to_uint32(parts[i], num)) return 0;
        result |= (num << (24 - i * 8));
    }
    return result;
}

std::string uint32_to_ip(uint32_t ip) {
    std::ostringstream oss;
    oss << ((ip >> 24) & 0xFF) << "."
        << ((ip >> 16) & 0xFF) << "."
        << ((ip >> 8) & 0xFF) << "."
        << (ip & 0xFF);
    return oss.str();
}

std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\n\r");
    return str.substr(start, end - start + 1);
}

std::vector<std::string> split(const std::string& str, char delimiter) {
    std::vector<std::string> result;
    std::stringstream ss(str);
    std::string item;
    while (std::getline(ss, item, delimiter)) {
        result.push_back(item);
    }
    return result;
}

std::string to_lower(const std::string& str) {
    std::string result = str;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

std::string format_bytes(uint64_t bytes) {
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    double size = static_cast<double>(bytes);
    int unit = 0;
    
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        unit++;
    }
    
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return oss.str();
}

bool safe_str_to_uint16(const std::string& str, uint16_t& result) {
    if (str.empty()) return false;
    char* end;
    unsigned long val = std::strtoul(str.c_str(), &end, 10);
    if (*end != '\0' || val > UINT16_MAX) return false;
    result = static_cast<uint16_t>(val);
    return true;
}

bool safe_str_to_uint32(const std::string& str, uint32_t& result) {
    if (str.empty()) return false;
    char* end;
    unsigned long val = std::strtoul(str.c_str(), &end, 10);
    if (*end != '\0' || val > UINT32_MAX) return false;
    result = static_cast<uint32_t>(val);
    return true;
}

bool safe_str_to_uint64(const std::string& str, uint64_t& result) {
    if (str.empty()) return false;
    char* end;
    unsigned long long val = std::strtoull(str.c_str(), &end, 10);
    if (*end != '\0') return false;
    result = static_cast<uint64_t>(val);
    return true;
}

bool safe_str_to_double(const std::string& str, double& result) {
    if (str.empty()) return false;
    char* end;
    result = std::strtod(str.c_str(), &end);
    return *end == '\0';
}

bool is_terminal() {
    // Defensive: Check if stdout is a terminal
    return isatty(fileno(stdout)) != 0;
}

void safe_print(const std::string& message) {
    // Defensive: Only print if terminal is available or message is safe
    if (is_terminal() || message.find_first_of("\x1B\x07\x08") == std::string::npos) {
        std::cout << message;
        safe_flush();
    }
}

void safe_flush() {
    if (is_terminal()) {
        std::cout.flush();
    }
}

bool create_directory(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    
    // Check if directory already exists
    struct stat info;
    if (stat(path.c_str(), &info) == 0) {
        if (info.st_mode & S_IFDIR) {
            return true; // Directory exists
        }
        return false; // Path exists but is not a directory
    }
    
    // Create directory
    if (MKDIR(path.c_str()) == 0) {
        return true;
    }
    
    // Try to create parent directories if needed
    size_t pos = path.find_last_of("/\\");
    if (pos != std::string::npos) {
        std::string parent = path.substr(0, pos);
        if (!parent.empty() && create_directory(parent)) {
            // Retry creating the directory
            return MKDIR(path.c_str()) == 0;
        }
    }
    
    return false;
}

bool ensure_log_file(const std::string& log_file_path) {
    if (log_file_path.empty()) {
        return false;
    }
    
    // Extract directory from file path
    size_t last_slash = log_file_path.find_last_of("/\\");
    if (last_slash != std::string::npos) {
        std::string log_dir = log_file_path.substr(0, last_slash);
        if (!log_dir.empty()) {
            if (!create_directory(log_dir)) {
                return false; // Failed to create directory
            }
        }
    }
    
    // Create or touch the log file
    std::ofstream file(log_file_path, std::ios::app);
    if (file.is_open()) {
        file.close();
        return true;
    }
    
    return false;
}

bool file_exists(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

} // namespace utils
