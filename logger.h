#ifndef LOGGER_H
#define LOGGER_H

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <cstdint>
#include <sstream>
#include <iomanip>

// Structured logging for connection details
// Logs are formatted for easy parsing by log readers (JSON-like structure)

enum class LogLevel {
    DEBUG,
    INFO,
    WARN,
    ERROR_LEVEL
};

struct ConnectionLog {
    uint64_t timestamp;
    std::string level;
    std::string event; // "connect", "disconnect", "error", "request", "response"
    std::string client_ip;
    uint16_t client_port;
    std::string target_host;
    uint16_t target_port;
    std::string runway_id;
    std::string method; // HTTP method
    std::string path; // Request path
    uint16_t status_code; // HTTP status code
    uint64_t bytes_sent;
    uint64_t bytes_received;
    double duration_ms;
    std::string error;
    
    ConnectionLog() 
        : timestamp(0)
        , client_port(0)
        , target_port(0)
        , status_code(0)
        , bytes_sent(0)
        , bytes_received(0)
        , duration_ms(0.0) {}
};

class Logger {
public:
    static Logger& instance();
    
    void init(const std::string& log_file);
    void log(LogLevel level, const std::string& message);
    void log_connection(const ConnectionLog& conn_log);
    void flush();
    void close();
    
private:
    Logger() : log_file_(), file_stream_(), mutex_(), initialized_(false) {}
    ~Logger() { close(); }
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    std::string log_file_;
    std::ofstream file_stream_;
    std::mutex mutex_;
    bool initialized_;
    
    std::string format_timestamp(uint64_t timestamp);
    std::string escape_json_string(const std::string& str);
    std::string level_to_string(LogLevel level);
};

#endif // LOGGER_H
