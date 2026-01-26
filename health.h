#ifndef HEALTH_H
#define HEALTH_H

#include <thread>
#include <atomic>
#include <memory>
#include "runway_manager.h"
#include "tracker.h"

class HealthMonitor {
public:
    HealthMonitor(std::shared_ptr<RunwayManager> runway_manager,
                  std::shared_ptr<TargetAccessibilityTracker> tracker,
                  uint64_t interval_secs);
    
    ~HealthMonitor();
    
    // Start health monitoring (runs in background thread)
    void start();
    
    // Stop health monitoring
    void stop();
    
    bool is_running() const { return running_; }
    
private:
    std::shared_ptr<RunwayManager> runway_manager_;
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    uint64_t interval_secs_;
    std::atomic<bool> running_;
    std::thread monitor_thread_;
    
    void monitor_loop();
    void health_check_cycle();
};

#endif // HEALTH_H
