#ifndef PROXY_H
#define PROXY_H

#include <string>
#include <vector>
#include <map>
#include <memory>
#include <thread>
#include <atomic>
#include <cstdint>
#include "config.h"
#include "runway.h"
#include "runway_manager.h"
#include "routing.h"
#include "tracker.h"
#include "dns.h"
#include "validator.h"
#include "network.h"

// HTTP Proxy Server
// RFC 7230 - HTTP/1.1 Message Syntax and Routing
// RFC 7231 - HTTP/1.1 Semantics and Content

struct HTTPRequest {
    std::string method;
    std::string path;
    std::string version;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    
    HTTPRequest() : version("HTTP/1.1") {}
};

struct HTTPResponse {
    std::string version;
    uint16_t status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    
    HTTPResponse() : version("HTTP/1.1"), status_code(200), status_text("OK") {}
};

class ProxyServer {
public:
    ProxyServer(const Config& config,
                std::shared_ptr<RunwayManager> runway_manager,
                std::shared_ptr<RoutingEngine> routing_engine,
                std::shared_ptr<TargetAccessibilityTracker> tracker,
                std::shared_ptr<DNSResolver> dns_resolver,
                std::shared_ptr<SuccessValidator> validator);
    
    ~ProxyServer();
    
    // Start proxy server (runs in background thread)
    bool start();
    
    // Stop proxy server
    void stop();
    
    // Check if server is running
    bool is_running() const { return running_; }
    
private:
    Config config_;
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<RoutingEngine> routing_engine_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    std::shared_ptr<DNSResolver> dns_resolver_;
    std::shared_ptr<SuccessValidator> validator_;
    
    socket_t listen_socket_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    
    // Server main loop
    void server_loop();
    
    // Handle client connection
    void handle_connection(socket_t client_sock);
    
    // Parse HTTP request (RFC 7230 Section 3)
    bool parse_http_request(socket_t sock, HTTPRequest& request);
    
    // Build HTTP response (RFC 7230 Section 3)
    std::vector<uint8_t> build_http_response(const HTTPResponse& response);
    
    // Make HTTP request through runway
    std::tuple<bool, bool, uint16_t, std::map<std::string, std::string>, std::vector<uint8_t>>
    make_http_request(const HTTPRequest& request, const std::string& target_host,
                     uint16_t target_port, std::shared_ptr<Runway> runway);
    
    // Test all runways to find accessible one
    std::shared_ptr<Runway> test_all_runways(const std::string& target,
                                             const std::vector<std::shared_ptr<Runway>>& runways);
    
    // Get alternative runway
    std::shared_ptr<Runway> get_alternative_runway(const std::string& target,
                                                    const std::string& current_runway_id);
    
    // Read line from socket (defensive: prevent buffer overflow)
    bool read_line(socket_t sock, std::string& line, size_t max_length = 8192);
    
    // Read HTTP headers
    bool read_headers(socket_t sock, std::map<std::string, std::string>& headers, size_t max_headers = 100);
    
    // Read HTTP body (Content-Length or chunked)
    bool read_body(socket_t sock, std::vector<uint8_t>& body, 
                   const std::map<std::string, std::string>& headers, size_t max_size = 10 * 1024 * 1024);
};

#endif // PROXY_H
