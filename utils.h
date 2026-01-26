#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <iomanip>

// Utility functions for string manipulation, IP validation, etc.

namespace utils {

// Check if string is a valid IPv4 address (RFC 791)
bool is_valid_ipv4(const std::string& ip);

// Check if IP is private (RFC 1918)
bool is_private_ip(const std::string& ip);

// Parse IP address string to 32-bit integer
uint32_t ip_to_uint32(const std::string& ip);

// Convert 32-bit integer to IP address string
std::string uint32_to_ip(uint32_t ip);

// Trim whitespace from string
std::string trim(const std::string& str);

// Split string by delimiter
std::vector<std::string> split(const std::string& str, char delimiter);

// Convert string to lowercase
std::string to_lower(const std::string& str);

// Format bytes to human-readable size
std::string format_bytes(uint64_t bytes);

// Safe string to number conversion
bool safe_str_to_uint16(const std::string& str, uint16_t& result);
bool safe_str_to_uint32(const std::string& str, uint32_t& result);
bool safe_str_to_uint64(const std::string& str, uint64_t& result);
bool safe_str_to_double(const std::string& str, double& result);

// Check if terminal is available (defensive terminal handling)
bool is_terminal();

// Safe output function (checks terminal state before writing)
void safe_print(const std::string& message);

// Flush output safely
void safe_flush();

// Create directory if it doesn't exist
bool create_directory(const std::string& path);

// Ensure log directory and file exist
bool ensure_log_file(const std::string& log_file_path);

} // namespace utils

#endif // UTILS_H
