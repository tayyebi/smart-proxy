#ifndef CLI_H
#define CLI_H

#include <string>
#include <vector>
#include <memory>
#include "config.h"
#include "runway_manager.h"
#include "routing.h"
#include "tracker.h"

// CLI interface for managing and monitoring the proxy service
class ProxyCLI {
public:
    ProxyCLI(std::shared_ptr<RunwayManager> runway_manager,
             std::shared_ptr<RoutingEngine> routing_engine,
             std::shared_ptr<TargetAccessibilityTracker> tracker);
    
    // Execute CLI command
    int execute(const std::vector<std::string>& args);
    
    // Command handlers
    void status();
    void runways();
    void targets();
    void stats();
    void mode(const std::string& mode_str);
    void test(const std::string& target, const std::string& runway_id = "");
    void reload();
    
    // Set JSON output mode
    void set_json_output(bool json) { json_output_ = json; }
    
private:
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<RoutingEngine> routing_engine_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    bool json_output_;
    
    void print_json(const std::string& json);
    std::string escape_json(const std::string& str);
};

#endif // CLI_H
