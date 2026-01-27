#include "health.h"
#include <chrono>
#include <thread>
#include <algorithm>

// Undefine Windows min/max macros that conflict with std::min/std::max
#ifdef _WIN32
#ifdef min
#undef min
#endif
#ifdef max
#undef max
#endif
#endif

HealthMonitor::HealthMonitor(
    std::shared_ptr<RunwayManager> runway_manager,
    std::shared_ptr<TargetAccessibilityTracker> tracker,
    uint64_t interval_secs)
    : runway_manager_(runway_manager)
    , tracker_(tracker)
    , interval_secs_(interval_secs)
    , running_(false) {
}

HealthMonitor::~HealthMonitor() {
    stop();
}

void HealthMonitor::start() {
    if (running_) {
        return;
    }
    
    running_ = true;
    monitor_thread_ = std::thread(&HealthMonitor::monitor_loop, this);
}

void HealthMonitor::stop() {
    if (!running_) {
        return;
    }
    
    running_ = false;
    
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void HealthMonitor::monitor_loop() {
    while (running_) {
        try {
            health_check_cycle();
        } catch (...) {
            // Defensive: continue on errors
        }
        
        // Sleep for interval
        for (uint64_t i = 0; i < interval_secs_ && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void HealthMonitor::health_check_cycle() {
    // Refresh interface information
    runway_manager_->refresh_interfaces();
    
    // Get all known targets
    std::vector<std::string> targets = tracker_->get_all_targets();
    if (targets.empty()) {
        return;
    }
    
    // Limit targets per cycle to avoid overload
    const size_t max_targets_per_cycle = 10;
    size_t targets_to_check = std::min(targets.size(), max_targets_per_cycle);
    
    for (size_t i = 0; i < targets_to_check; ++i) {
        const std::string& target = targets[i];
        
        try {
            auto metrics = tracker_->get_target_metrics(target);
            
            // Prioritize recently failed runways
            std::vector<std::string> failed_runways;
            for (const auto& pair : metrics) {
                if (pair.second.state == RunwayState::Inaccessible) {
                    failed_runways.push_back(pair.first);
                }
            }
            
            // Test failed runways (limit to 5 per target)
            size_t max_failed = std::min(failed_runways.size(), size_t(5));
            for (size_t j = 0; j < max_failed; ++j) {
                auto runway = runway_manager_->get_runway(failed_runways[j]);
                if (runway) {
                    auto result = runway_manager_->test_runway_accessibility(
                        target, runway, 5.0);
                    bool net_success = std::get<0>(result);
                    bool user_success = std::get<1>(result);
                    double response_time = std::get<2>(result);
                    tracker_->update(target, runway->id, net_success, user_success, response_time);
                }
            }
            
            // Also test partially accessible runways (limit to 3 per target)
            std::vector<std::string> partial_runways;
            for (const auto& pair : metrics) {
                if (pair.second.state == RunwayState::PartiallyAccessible) {
                    partial_runways.push_back(pair.first);
                }
            }
            
            size_t max_partial = std::min(partial_runways.size(), size_t(3));
            for (size_t j = 0; j < max_partial; ++j) {
                auto runway = runway_manager_->get_runway(partial_runways[j]);
                if (runway) {
                    auto result = runway_manager_->test_runway_accessibility(
                        target, runway, 5.0);
                    bool net_success = std::get<0>(result);
                    bool user_success = std::get<1>(result);
                    double response_time = std::get<2>(result);
                    tracker_->update(target, runway->id, net_success, user_success, response_time);
                }
            }
        } catch (...) {
            // Defensive: continue on errors
        }
    }
}
