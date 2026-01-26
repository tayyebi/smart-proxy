#include "logger.h"
#include "utils.h"
#include <ctime>
#include <sstream>
#include <iomanip>
#include <cstring>

Logger& Logger::instance() {
    static Logger instance;
    return instance;
}

void Logger::init(const std::string& log_file) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (initialized_) {
        return;
    }
    
    log_file_ = log_file;
    
    // Ensure log directory exists
    if (!log_file_.empty()) {
        utils::ensure_log_file(log_file_);
        // Open in append mode for live appending
        // std::ios::app ensures writes go to end of file
        file_stream_.open(log_file_, std::ios::app | std::ios::out);
        if (file_stream_.is_open()) {
            // Ensure immediate writes by setting unitbuf (flush after each output operation)
            file_stream_.setf(std::ios::unitbuf);
            initialized_ = true;
        } else {
            initialized_ = false;
        }
    }
}

void Logger::log(LogLevel level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_ || !file_stream_.is_open()) {
        return;
    }
    
    uint64_t timestamp = std::time(nullptr);
    std::string level_str = level_to_string(level);
    std::string time_str = format_timestamp(timestamp);
    
    // Format: timestamp level message
    file_stream_ << time_str << " [" << level_str << "] " << message << "\n";
    file_stream_.flush(); // Ensure immediate write to disk
}

void Logger::log_connection(const ConnectionLog& conn_log) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (!initialized_ || !file_stream_.is_open()) {
        return;
    }
    
    std::stringstream json;
    json << std::fixed << std::setprecision(2);
    
    json << format_timestamp(conn_log.timestamp) << " [CONN] {";
    json << "\"event\":\"" << escape_json_string(conn_log.event) << "\"";
    
    if (!conn_log.client_ip.empty()) {
        json << ",\"client_ip\":\"" << escape_json_string(conn_log.client_ip) << "\"";
        json << ",\"client_port\":" << conn_log.client_port;
    }
    
    if (!conn_log.target_host.empty()) {
        json << ",\"target_host\":\"" << escape_json_string(conn_log.target_host) << "\"";
        json << ",\"target_port\":" << conn_log.target_port;
    }
    
    if (!conn_log.runway_id.empty()) {
        json << ",\"runway_id\":\"" << escape_json_string(conn_log.runway_id) << "\"";
    }
    
    if (!conn_log.method.empty()) {
        json << ",\"method\":\"" << escape_json_string(conn_log.method) << "\"";
    }
    
    if (!conn_log.path.empty()) {
        json << ",\"path\":\"" << escape_json_string(conn_log.path) << "\"";
    }
    
    if (conn_log.status_code > 0) {
        json << ",\"status_code\":" << conn_log.status_code;
    }
    
    if (conn_log.bytes_sent > 0 || conn_log.bytes_received > 0) {
        json << ",\"bytes_sent\":" << conn_log.bytes_sent;
        json << ",\"bytes_received\":" << conn_log.bytes_received;
    }
    
    if (conn_log.duration_ms > 0.0) {
        json << ",\"duration_ms\":" << conn_log.duration_ms;
    }
    
    if (!conn_log.error.empty()) {
        json << ",\"error\":\"" << escape_json_string(conn_log.error) << "\"";
    }
    
    json << "}\n";
    
    file_stream_ << json.str();
    file_stream_.flush(); // Ensure immediate write to disk
}

void Logger::flush() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_.is_open()) {
        file_stream_.flush();
    }
}

void Logger::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (file_stream_.is_open()) {
        file_stream_.close();
    }
    initialized_ = false;
}

std::string Logger::format_timestamp(uint64_t timestamp) {
    std::time_t time_val = static_cast<std::time_t>(timestamp);
    std::tm* tm_info = std::localtime(&time_val);
    
    if (!tm_info) {
        return "0000-00-00 00:00:00";
    }
    
    std::stringstream ss;
    ss << std::put_time(tm_info, "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

std::string Logger::escape_json_string(const std::string& str) {
    std::stringstream escaped;
    for (char c : str) {
        if (c == '"') {
            escaped << "\\\"";
        } else if (c == '\\') {
            escaped << "\\\\";
        } else if (c == '\n') {
            escaped << "\\n";
        } else if (c == '\r') {
            escaped << "\\r";
        } else if (c == '\t') {
            escaped << "\\t";
        } else if (static_cast<unsigned char>(c) < 0x20) {
            // Control characters
            escaped << "\\u" << std::hex << std::setw(4) << std::setfill('0') 
                    << static_cast<int>(static_cast<unsigned char>(c));
        } else {
            escaped << c;
        }
    }
    return escaped.str();
}

std::string Logger::level_to_string(LogLevel level) {
    switch (level) {
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::INFO: return "INFO";
        case LogLevel::WARN: return "WARN";
        case LogLevel::ERROR: return "ERROR";
        default: return "INFO";
    }
}
