#ifndef TRACKER_H
#define TRACKER_H

#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <cstdint>
#include "runway.h"

struct TargetMetrics {
    std::string target;
    std::string runway_id;
    RunwayState state;
    uint64_t network_success_count;
    uint64_t user_success_count;
    uint64_t failure_count;
    uint64_t partial_success_count;
    uint64_t total_attempts;
    double avg_response_time;
    uint64_t last_success_time; // Unix timestamp
    uint64_t last_failure_time; // Unix timestamp
    uint32_t consecutive_failures;
    uint64_t recovery_count;
    double success_rate;
    std::vector<bool> recent_attempts; // Last N attempts (true=success, false=failure)
    
    TargetMetrics() 
        : state(RunwayState::Unknown)
        , network_success_count(0)
        , user_success_count(0)
        , failure_count(0)
        , partial_success_count(0)
        , total_attempts(0)
        , avg_response_time(0.0)
        , last_success_time(0)
        , last_failure_time(0)
        , consecutive_failures(0)
        , recovery_count(0)
        , success_rate(0.0) {}
    
    TargetMetrics(const std::string& target, const std::string& runway_id)
        : target(target)
        , runway_id(runway_id)
        , state(RunwayState::Unknown)
        , network_success_count(0)
        , user_success_count(0)
        , failure_count(0)
        , partial_success_count(0)
        , total_attempts(0)
        , avg_response_time(0.0)
        , last_success_time(0)
        , last_failure_time(0)
        , consecutive_failures(0)
        , recovery_count(0)
        , success_rate(0.0) {}
    
    void update_success_rate(size_t window);
};

class TargetAccessibilityTracker {
public:
    TargetAccessibilityTracker(size_t success_rate_window, double success_rate_threshold);
    
    void update(const std::string& target, const std::string& runway_id,
                bool network_success, bool user_success, double response_time_secs);
    
    std::vector<std::string> get_accessible_runways(const std::string& target);
    
    std::shared_ptr<TargetMetrics> get_metrics(const std::string& target, const std::string& runway_id);
    
    std::vector<std::string> get_all_targets();
    
    std::map<std::string, TargetMetrics> get_target_metrics(const std::string& target);
    
private:
    std::map<std::string, std::map<std::string, TargetMetrics>> metrics_; // target -> runway_id -> metrics
    size_t success_rate_window_;
    double success_rate_threshold_;
    std::mutex mutex_;
    
    TargetMetrics& get_or_create_metrics(const std::string& target, const std::string& runway_id);
    uint64_t get_current_time() const;
};

#endif // TRACKER_H
