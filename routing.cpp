#include "routing.h"
#include <algorithm>

RoutingEngine::RoutingEngine(std::shared_ptr<TargetAccessibilityTracker> tracker, RoutingMode mode)
    : tracker_(tracker), mode_(mode) {
}

void RoutingEngine::set_mode(RoutingMode mode) {
    std::lock_guard<std::mutex> lock(mode_mutex_);
    mode_ = mode;
}

RoutingMode RoutingEngine::get_mode() const {
    std::lock_guard<std::mutex> lock(mode_mutex_);
    return mode_;
}

std::shared_ptr<Runway> RoutingEngine::select_runway(
    const std::string& target,
    const std::vector<std::shared_ptr<Runway>>& runways) {
    
    std::lock_guard<std::mutex> lock(mode_mutex_);
    RoutingMode current_mode = mode_;
    
    // Get accessible runways
    std::vector<std::string> accessible_ids = tracker_->get_accessible_runways(target);
    if (accessible_ids.empty()) {
        return nullptr;
    }
    
    // Filter runways to only accessible ones
    std::vector<std::shared_ptr<Runway>> accessible_runways;
    for (const auto& runway : runways) {
        if (std::find(accessible_ids.begin(), accessible_ids.end(), runway->id) != accessible_ids.end()) {
            accessible_runways.push_back(runway);
        }
    }
    
    if (accessible_runways.empty()) {
        return nullptr;
    }
    
    switch (current_mode) {
        case RoutingMode::Latency:
            return select_by_latency(target, accessible_runways);
        case RoutingMode::FirstAccessible:
            return select_first_accessible(target, accessible_runways);
        case RoutingMode::RoundRobin:
            return select_round_robin(target, accessible_runways);
        default:
            return select_first_accessible(target, accessible_runways);
    }
}

std::shared_ptr<Runway> RoutingEngine::select_by_latency(
    const std::string& target,
    const std::vector<std::shared_ptr<Runway>>& runways) {
    
    std::shared_ptr<Runway> best_runway = nullptr;
    double best_latency = 1e9;
    
    for (const auto& runway : runways) {
        auto metrics = tracker_->get_metrics(target, runway->id);
        if (metrics && metrics->avg_response_time > 0.0) {
            if (metrics->avg_response_time < best_latency) {
                best_latency = metrics->avg_response_time;
                best_runway = runway;
            }
        }
    }
    
    if (best_runway) {
        return best_runway;
    }
    
    // Fallback to first accessible
    return select_first_accessible(target, runways);
}

std::shared_ptr<Runway> RoutingEngine::select_first_accessible(
    const std::string& /*target*/,
    const std::vector<std::shared_ptr<Runway>>& runways) {
    
    if (runways.empty()) {
        return nullptr;
    }
    return runways[0];
}

std::shared_ptr<Runway> RoutingEngine::select_round_robin(
    const std::string& target,
    const std::vector<std::shared_ptr<Runway>>& runways) {
    
    if (runways.empty()) {
        return nullptr;
    }
    
    std::lock_guard<std::mutex> lock(rr_mutex_);
    size_t& index = round_robin_index_[target];
    std::shared_ptr<Runway> selected = runways[index % runways.size()];
    index = (index + 1) % runways.size();
    return selected;
}
