#include "tracker.h"
#include <ctime>
#include <algorithm>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <sys/time.h>
#endif

TargetAccessibilityTracker::TargetAccessibilityTracker(size_t success_rate_window, double success_rate_threshold)
    : success_rate_window_(success_rate_window)
    , success_rate_threshold_(success_rate_threshold) {
}

uint64_t TargetAccessibilityTracker::get_current_time() const {
#ifdef _WIN32
    return static_cast<uint64_t>(time(nullptr));
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec);
#endif
}

TargetMetrics& TargetAccessibilityTracker::get_or_create_metrics(
    const std::string& target, const std::string& runway_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_[target][runway_id];
}

void TargetMetrics::update_success_rate(size_t /*window*/) {
    if (recent_attempts.empty()) {
        success_rate = 0.0;
        return;
    }
    
    size_t success_count = 0;
    for (bool attempt : recent_attempts) {
        if (attempt) success_count++;
    }
    success_rate = static_cast<double>(success_count) / recent_attempts.size();
}

void TargetAccessibilityTracker::update(const std::string& target, const std::string& runway_id,
                                         bool network_success, bool user_success, double response_time_secs) {
    std::lock_guard<std::mutex> lock(mutex_);
    TargetMetrics& metrics = metrics_[target][runway_id];
    
    if (metrics.target.empty()) {
        metrics.target = target;
        metrics.runway_id = runway_id;
    }
    
    metrics.total_attempts++;
    uint64_t current_time = get_current_time();
    
    // Update recent attempts
    metrics.recent_attempts.push_back(user_success);
    if (metrics.recent_attempts.size() > success_rate_window_) {
        metrics.recent_attempts.erase(metrics.recent_attempts.begin());
    }
    
    if (network_success && user_success) {
        metrics.network_success_count++;
        metrics.user_success_count++;
        metrics.state = RunwayState::Accessible;
        metrics.last_success_time = current_time;
        metrics.consecutive_failures = 0;
        
        // Update average response time (exponential moving average)
        if (metrics.avg_response_time == 0.0) {
            metrics.avg_response_time = response_time_secs;
        } else {
            metrics.avg_response_time = metrics.avg_response_time * 0.7 + response_time_secs * 0.3;
        }
    } else if (network_success && !user_success) {
        metrics.network_success_count++;
        metrics.partial_success_count++;
        metrics.state = RunwayState::PartiallyAccessible;
    } else {
        metrics.failure_count++;
        metrics.last_failure_time = current_time;
        metrics.consecutive_failures++;
        
        if (metrics.consecutive_failures > 3) {
            metrics.state = RunwayState::Inaccessible;
        }
    }
    
    // Check for recovery
    if (metrics.state == RunwayState::Inaccessible && user_success) {
        metrics.recovery_count++;
        metrics.state = RunwayState::Accessible;
    }
    
    metrics.update_success_rate(success_rate_window_);
}

std::vector<std::string> TargetAccessibilityTracker::get_accessible_runways(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> accessible;
    
    auto target_it = metrics_.find(target);
    if (target_it == metrics_.end()) {
        return accessible;
    }
    
    for (const auto& pair : target_it->second) {
        const TargetMetrics& metrics = pair.second;
        if (metrics.state == RunwayState::Accessible) {
            accessible.push_back(pair.first);
        } else if (metrics.state == RunwayState::PartiallyAccessible) {
            if (metrics.success_rate >= success_rate_threshold_) {
                accessible.push_back(pair.first);
            }
        }
    }
    
    return accessible;
}

std::shared_ptr<TargetMetrics> TargetAccessibilityTracker::get_metrics(
    const std::string& target, const std::string& runway_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto target_it = metrics_.find(target);
    if (target_it == metrics_.end()) {
        return nullptr;
    }
    
    auto runway_it = target_it->second.find(runway_id);
    if (runway_it == target_it->second.end()) {
        return nullptr;
    }
    
    return std::make_shared<TargetMetrics>(runway_it->second);
}

std::vector<std::string> TargetAccessibilityTracker::get_all_targets() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> targets;
    for (const auto& pair : metrics_) {
        targets.push_back(pair.first);
    }
    return targets;
}

std::map<std::string, TargetMetrics> TargetAccessibilityTracker::get_target_metrics(const std::string& target) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto target_it = metrics_.find(target);
    if (target_it == metrics_.end()) {
        return std::map<std::string, TargetMetrics>();
    }
    
    return target_it->second;
}
