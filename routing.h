#ifndef ROUTING_H
#define ROUTING_H

#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "runway.h"
#include "tracker.h"
#include "config.h"

class RoutingEngine {
public:
    RoutingEngine(std::shared_ptr<TargetAccessibilityTracker> tracker, RoutingMode mode);
    
    void set_mode(RoutingMode mode);
    RoutingMode get_mode() const;
    
    // Select optimal runway for target
    std::shared_ptr<Runway> select_runway(const std::string& target, 
                                          const std::vector<std::shared_ptr<Runway>>& runways);
    
private:
    std::shared_ptr<TargetAccessibilityTracker> tracker_;
    mutable RoutingMode mode_;
    mutable std::mutex mode_mutex_;
    std::map<std::string, size_t> round_robin_index_;
    mutable std::mutex rr_mutex_;
    
    std::shared_ptr<Runway> select_by_latency(const std::string& target,
                                               const std::vector<std::shared_ptr<Runway>>& runways);
    std::shared_ptr<Runway> select_first_accessible(const std::string& target,
                                                     const std::vector<std::shared_ptr<Runway>>& runways);
    std::shared_ptr<Runway> select_round_robin(const std::string& target,
                                               const std::vector<std::shared_ptr<Runway>>& runways);
};

#endif // ROUTING_H
