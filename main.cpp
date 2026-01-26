#include <iostream>
#include <csignal>
#include <cstdlib>
#include <memory>
#include <thread>
#include <chrono>
#include "config.h"
#include "dns.h"
#include "runway_manager.h"
#include "routing.h"
#include "tracker.h"
#include "validator.h"
#include "proxy.h"
#include "health.h"
#include "network.h"
#include "utils.h"
#include "tui.h"
#include "logger.h"

// Defensive terminal handling with double Ctrl+C support
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_shutdown_requested = 0;

void signal_handler(int signal) {
    // Defensive: safe signal handling
    if (signal == SIGINT || signal == SIGTERM) {
        if (g_shutdown_requested == 0) {
            g_shutdown_requested = 1; // First Ctrl+C: graceful shutdown
        } else {
            g_running = 0; // Second Ctrl+C: force kill
            exit(1); // Force exit
        }
    }
}

int main(int /*argc*/, char* /*argv*/[]) {
    // Always run as service with TUI
    // Defensive: Set up output buffering
    setvbuf(stdout, nullptr, _IOLBF, 0);
    setvbuf(stderr, nullptr, _IOLBF, 0);
    
    // Defensive: Check if terminal is available before writing
    if (!utils::is_terminal()) {
        // Running in non-terminal environment, be more careful with output
    }
    
    // Initialize networking
    if (!network::init()) {
        utils::safe_print("Error: Failed to initialize networking\n");
        return 1;
    }
    
    // Set up signal handlers (defensive: handle SIGINT, SIGTERM)
    // First Ctrl+C = graceful shutdown, Second Ctrl+C = force kill
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
#ifdef _WIN32
    // Windows: Also handle console close
    #include <windows.h>
    SetConsoleCtrlHandler([](DWORD dwCtrlType) -> BOOL {
        if (dwCtrlType == CTRL_C_EVENT || dwCtrlType == CTRL_CLOSE_EVENT) {
            if (g_shutdown_requested == 0) {
                g_shutdown_requested = 1;
            } else {
                g_running = 0;
                exit(1);
            }
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#else
    // POSIX: Ignore SIGPIPE (defensive: prevent crashes on broken pipes)
    signal(SIGPIPE, SIG_IGN);
    // POSIX: Handle terminal resize (SIGWINCH) - TUI will detect it automatically
    // We don't need a handler here since TUI polls for size changes
#endif
    
    // Load configuration
    Config config = Config::load("config.json");
    
    // Ensure log directory and file exist
    if (!config.log_file.empty()) {
        if (!utils::ensure_log_file(config.log_file)) {
            utils::safe_print("Warning: Could not create log file: " + config.log_file + "\n");
            utils::safe_print("Logging will continue to stdout/stderr\n");
        } else {
            // Initialize logger
            Logger::instance().init(config.log_file);
            Logger::instance().log(LogLevel::INFO, "Smart Proxy Service starting");
        }
    }
    
    // Initialize DNS resolver
    std::shared_ptr<DNSResolver> dns_resolver = std::make_shared<DNSResolver>(
        config.dns_servers, config.dns_timeout);
    
    // Initialize runway manager
    std::shared_ptr<RunwayManager> runway_manager = std::make_shared<RunwayManager>(
        config.interfaces, config.upstream_proxies, config.dns_servers, dns_resolver);
    
    // Discover runways
    runway_manager->discover_runways();
    auto all_runways = runway_manager->get_all_runways();
    
    if (utils::is_terminal()) {
        std::cout << "Discovered " << all_runways.size() << " runways\n";
        utils::safe_flush();
    }
    
    // Initialize accessibility tracker
    std::shared_ptr<TargetAccessibilityTracker> tracker = std::make_shared<TargetAccessibilityTracker>(
        config.success_rate_window, config.success_rate_threshold);
    
    // Initialize success validator
    std::shared_ptr<SuccessValidator> validator = std::make_shared<SuccessValidator>();
    
    // Initialize routing engine
    RoutingMode routing_mode = config.routing_mode;
    std::shared_ptr<RoutingEngine> routing_engine = std::make_shared<RoutingEngine>(
        tracker, routing_mode);
    
    // Initialize proxy server
    std::shared_ptr<ProxyServer> proxy_server = std::make_shared<ProxyServer>(
        config, runway_manager, routing_engine, tracker, dns_resolver, validator);
    
    // Initialize health monitor
    std::shared_ptr<HealthMonitor> health_monitor = std::make_shared<HealthMonitor>(
        runway_manager, tracker, config.health_check_interval);
    
    // Start proxy server
    if (!proxy_server->start()) {
        utils::safe_print("Error: Failed to start proxy server\n");
        network::cleanup();
        return 1;
    }
    
    if (utils::is_terminal()) {
        std::cout << "Proxy server started on " << config.proxy_listen_host 
                  << ":" << config.proxy_listen_port << "\n";
        utils::safe_flush();
    }
    
    // Start health monitor
    health_monitor->start();
    
    Logger::instance().log(LogLevel::INFO, "Proxy server started on " + config.proxy_listen_host + ":" + std::to_string(config.proxy_listen_port));
    
    // Create and run TUI
    TUI tui(runway_manager, routing_engine, tracker, proxy_server, config);
    
    // Run TUI in main thread (blocks, but checks shutdown flag)
    // Pass shutdown flag so TUI can exit gracefully
    if (utils::is_terminal()) {
        tui.run(&g_shutdown_requested);
    } else {
        // Not a terminal, just wait for shutdown
        while (g_running && proxy_server->is_running() && g_shutdown_requested == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    // Shutdown requested - TUI has exited, now clean up
    if (g_shutdown_requested) {
        Logger::instance().log(LogLevel::INFO, "Graceful shutdown requested");
        
        // TUI already displayed shutdown message and stopped, now clean up services
        tui.stop();
        
        // Stop services
        if (utils::is_terminal()) {
            utils::safe_print("Stopping health monitor...\n");
            utils::safe_flush();
        }
        health_monitor->stop();
        
        if (utils::is_terminal()) {
            utils::safe_print("Stopping proxy server...\n");
            utils::safe_flush();
        }
        proxy_server->stop();
        
        if (utils::is_terminal()) {
            utils::safe_print("Smart Proxy Service stopped.\n");
            utils::safe_flush();
        }
        
        Logger::instance().log(LogLevel::INFO, "Smart Proxy Service stopped");
        Logger::instance().close();
    }
    
    network::cleanup();
    return 0;
}
