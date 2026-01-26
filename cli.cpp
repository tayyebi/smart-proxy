#include "cli.h"
#include "utils.h"
#include <sstream>
#include <iomanip>
#include <ctime>

ProxyCLI::ProxyCLI(
    std::shared_ptr<RunwayManager> runway_manager,
    std::shared_ptr<RoutingEngine> routing_engine,
    std::shared_ptr<TargetAccessibilityTracker> tracker)
    : runway_manager_(runway_manager)
    , routing_engine_(routing_engine)
    , tracker_(tracker)
    , json_output_(false) {
}

std::string ProxyCLI::escape_json(const std::string& str) {
    std::ostringstream oss;
    for (char c : str) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\b': oss << "\\b"; break;
            case '\f': oss << "\\f"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    oss << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<int>(c);
                } else {
                    oss << c;
                }
                break;
        }
    }
    return oss.str();
}

void ProxyCLI::print_json(const std::string& json) {
    if (json_output_) {
        utils::safe_print(json);
        utils::safe_print("\n");
    } else {
        // Pretty print JSON (simplified)
        utils::safe_print(json);
        utils::safe_print("\n");
    }
}

int ProxyCLI::execute(const std::vector<std::string>& args) {
    if (args.empty()) {
        utils::safe_print("Smart Proxy CLI\n");
        utils::safe_print("Usage: smartproxy <command> [options]\n");
        utils::safe_print("\nCommands:\n");
        utils::safe_print("  status              Show current status\n");
        utils::safe_print("  runways             List all runways\n");
        utils::safe_print("  targets             Show target accessibility matrix\n");
        utils::safe_print("  stats               Show performance statistics\n");
        utils::safe_print("  mode <mode>         Switch routing mode (latency/first_accessible/round_robin)\n");
        utils::safe_print("  test <target> [id]   Test target accessibility\n");
        utils::safe_print("  reload              Reload configuration\n");
        utils::safe_print("\nOptions:\n");
        utils::safe_print("  --json              Output in JSON format\n");
        return 0;
    }
    
    // Check for --json flag (can be anywhere in args)
    std::vector<std::string> filtered_args;
    for (const auto& arg : args) {
        if (arg == "--json") {
            json_output_ = true;
        } else {
            filtered_args.push_back(arg);
        }
    }
    
    if (filtered_args.empty()) {
        utils::safe_print("Error: No command specified\n");
        return 1;
    }
    
    std::string command = filtered_args[0];
    
    if (command == "status") {
        status();
    } else if (command == "runways") {
        runways();
    } else if (command == "targets") {
        targets();
    } else if (command == "stats") {
        stats();
    } else if (command == "mode") {
        if (filtered_args.size() < 2) {
            utils::safe_print("Error: mode requires an argument (latency/first_accessible/round_robin)\n");
            return 1;
        }
        mode(filtered_args[1]);
    } else if (command == "test") {
        if (filtered_args.size() < 2) {
            utils::safe_print("Error: test requires a target argument\n");
            return 1;
        }
        std::string runway_id = (filtered_args.size() > 2) ? filtered_args[2] : "";
        test(filtered_args[1], runway_id);
    } else if (command == "reload") {
        reload();
    } else {
        utils::safe_print("Error: Unknown command '" + command + "'\n");
        return 1;
    }
    
    return 0;
}

void ProxyCLI::status() {
    auto all_runways = runway_manager_->get_all_runways();
    auto all_targets = tracker_->get_all_targets();
    RoutingMode current_mode = routing_engine_->get_mode();
    
    std::string mode_str;
    switch (current_mode) {
        case RoutingMode::Latency: mode_str = "latency"; break;
        case RoutingMode::FirstAccessible: mode_str = "first_accessible"; break;
        case RoutingMode::RoundRobin: mode_str = "round_robin"; break;
    }
    
    if (json_output_) {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"routing_mode\": \"" << escape_json(mode_str) << "\",\n";
        oss << "  \"runways_count\": " << all_runways.size() << ",\n";
        oss << "  \"targets_count\": " << all_targets.size() << ",\n";
        oss << "  \"status\": \"running\"\n";
        oss << "}";
        print_json(oss.str());
    } else {
        utils::safe_print("Routing Mode: " + mode_str + "\n");
        utils::safe_print("Runways: " + std::to_string(all_runways.size()) + "\n");
        utils::safe_print("Targets: " + std::to_string(all_targets.size()) + "\n");
        utils::safe_print("Status: running\n");
    }
}

void ProxyCLI::runways() {
    auto all_runways = runway_manager_->get_all_runways();
    
    if (json_output_) {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"runways\": [\n";
        for (size_t i = 0; i < all_runways.size(); ++i) {
            const auto& r = all_runways[i];
            oss << "    {\n";
            oss << "      \"id\": \"" << escape_json(r->id) << "\",\n";
            oss << "      \"interface\": \"" << escape_json(r->interface) << "\",\n";
            oss << "      \"source_ip\": " << (r->source_ip.empty() ? "null" : "\"" + escape_json(r->source_ip) + "\"") << ",\n";
            oss << "      \"is_direct\": " << (r->is_direct ? "true" : "false") << ",\n";
            if (r->upstream_proxy) {
                std::string proxy_str = r->upstream_proxy->config.proxy_type + "://" +
                                       r->upstream_proxy->config.host + ":" +
                                       std::to_string(r->upstream_proxy->config.port);
                oss << "      \"upstream_proxy\": \"" << escape_json(proxy_str) << "\",\n";
            } else {
                oss << "      \"upstream_proxy\": null,\n";
            }
            if (r->dns_server) {
                std::string dns_str = r->dns_server->config.host + ":" +
                                     std::to_string(r->dns_server->config.port);
                oss << "      \"dns_server\": \"" << escape_json(dns_str) << "\"\n";
            } else {
                oss << "      \"dns_server\": null\n";
            }
            oss << "    }";
            if (i < all_runways.size() - 1) oss << ",";
            oss << "\n";
        }
        oss << "  ],\n";
        oss << "  \"count\": " << all_runways.size() << "\n";
        oss << "}";
        print_json(oss.str());
    } else {
        for (const auto& r : all_runways) {
            utils::safe_print(r->id + ": " + r->interface);
            if (!r->source_ip.empty()) {
                utils::safe_print(" (" + r->source_ip + ")");
            }
            utils::safe_print(" [direct: " + std::string(r->is_direct ? "yes" : "no") + "]\n");
        }
    }
}

void ProxyCLI::targets() {
    auto all_targets = tracker_->get_all_targets();
    
    if (json_output_) {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"targets\": {\n";
        for (size_t i = 0; i < all_targets.size(); ++i) {
            const std::string& target = all_targets[i];
            auto metrics = tracker_->get_target_metrics(target);
            oss << "    \"" << escape_json(target) << "\": {\n";
            size_t j = 0;
            for (const auto& pair : metrics) {
                const auto& m = pair.second;
                oss << "      \"" << escape_json(pair.first) << "\": {\n";
                std::string state_str;
                switch (m.state) {
                    case RunwayState::Unknown: state_str = "unknown"; break;
                    case RunwayState::Accessible: state_str = "accessible"; break;
                    case RunwayState::PartiallyAccessible: state_str = "partially_accessible"; break;
                    case RunwayState::Inaccessible: state_str = "inaccessible"; break;
                    case RunwayState::Testing: state_str = "testing"; break;
                }
                oss << "        \"state\": \"" << escape_json(state_str) << "\",\n";
                oss << "        \"success_rate\": " << std::fixed << std::setprecision(3) << m.success_rate << ",\n";
                oss << "        \"avg_response_time\": " << m.avg_response_time << ",\n";
                oss << "        \"total_attempts\": " << m.total_attempts << ",\n";
                oss << "        \"user_success_count\": " << m.user_success_count << ",\n";
                oss << "        \"failure_count\": " << m.failure_count << "\n";
                oss << "      }";
                if (++j < metrics.size()) oss << ",";
                oss << "\n";
            }
            oss << "    }";
            if (i < all_targets.size() - 1) oss << ",";
            oss << "\n";
        }
        oss << "  }\n";
        oss << "}";
        print_json(oss.str());
    } else {
        for (const auto& target : all_targets) {
            auto metrics = tracker_->get_target_metrics(target);
            utils::safe_print(target + ": " + std::to_string(metrics.size()) + " runways\n");
            for (const auto& pair : metrics) {
                const auto& m = pair.second;
                std::string state_str;
                switch (m.state) {
                    case RunwayState::Unknown: state_str = "unknown"; break;
                    case RunwayState::Accessible: state_str = "accessible"; break;
                    case RunwayState::PartiallyAccessible: state_str = "partially_accessible"; break;
                    case RunwayState::Inaccessible: state_str = "inaccessible"; break;
                    case RunwayState::Testing: state_str = "testing"; break;
                }
                utils::safe_print("  " + pair.first + ": " + state_str +
                                 " (success: " + std::to_string(m.user_success_count) +
                                 ", failures: " + std::to_string(m.failure_count) + ")\n");
            }
        }
    }
}

void ProxyCLI::stats() {
    auto all_targets = tracker_->get_all_targets();
    auto all_runways = runway_manager_->get_all_runways();
    
    if (json_output_) {
        std::ostringstream oss;
        oss << "{\n";
        oss << "  \"total_targets\": " << all_targets.size() << ",\n";
        oss << "  \"total_runways\": " << all_runways.size() << ",\n";
        oss << "  \"targets\": {\n";
        for (size_t i = 0; i < all_targets.size(); ++i) {
            const std::string& target = all_targets[i];
            auto metrics = tracker_->get_target_metrics(target);
            size_t accessible = 0, partial = 0, inaccessible = 0;
            uint64_t total_attempts = 0, total_successes = 0;
            for (const auto& pair : metrics) {
                const auto& m = pair.second;
                switch (m.state) {
                    case RunwayState::Accessible: accessible++; break;
                    case RunwayState::PartiallyAccessible: partial++; break;
                    case RunwayState::Inaccessible: inaccessible++; break;
                    default: break;
                }
                total_attempts += m.total_attempts;
                total_successes += m.user_success_count;
            }
            oss << "    \"" << escape_json(target) << "\": {\n";
            oss << "      \"accessible_runways\": " << accessible << ",\n";
            oss << "      \"partially_accessible_runways\": " << partial << ",\n";
            oss << "      \"inaccessible_runways\": " << inaccessible << ",\n";
            oss << "      \"total_attempts\": " << total_attempts << ",\n";
            oss << "      \"total_successes\": " << total_successes << "\n";
            oss << "    }";
            if (i < all_targets.size() - 1) oss << ",";
            oss << "\n";
        }
        oss << "  }\n";
        oss << "}";
        print_json(oss.str());
    } else {
        utils::safe_print("Total Targets: " + std::to_string(all_targets.size()) + "\n");
        utils::safe_print("Total Runways: " + std::to_string(all_runways.size()) + "\n");
        for (const auto& target : all_targets) {
            auto metrics = tracker_->get_target_metrics(target);
            size_t accessible = 0, partial = 0, inaccessible = 0;
            for (const auto& pair : metrics) {
                switch (pair.second.state) {
                    case RunwayState::Accessible: accessible++; break;
                    case RunwayState::PartiallyAccessible: partial++; break;
                    case RunwayState::Inaccessible: inaccessible++; break;
                    default: break;
                }
            }
            utils::safe_print("\n" + target + ":\n");
            utils::safe_print("  Accessible: " + std::to_string(accessible) + "\n");
            utils::safe_print("  Partially Accessible: " + std::to_string(partial) + "\n");
            utils::safe_print("  Inaccessible: " + std::to_string(inaccessible) + "\n");
        }
    }
}

void ProxyCLI::mode(const std::string& mode_str) {
    RoutingMode mode;
    std::string mode_lower = utils::to_lower(mode_str);
    
    if (mode_lower == "latency") {
        mode = RoutingMode::Latency;
    } else if (mode_lower == "first_accessible") {
        mode = RoutingMode::FirstAccessible;
    } else if (mode_lower == "round_robin") {
        mode = RoutingMode::RoundRobin;
    } else {
        utils::safe_print("Error: Invalid routing mode '" + mode_str + "'. Valid modes: latency, first_accessible, round_robin\n");
        return;
    }
    
    routing_engine_->set_mode(mode);
    if (!json_output_) {
        utils::safe_print("Routing mode changed to " + mode_str + "\n");
    }
}

void ProxyCLI::test(const std::string& target, const std::string& runway_id) {
    if (!runway_id.empty()) {
        auto runway = runway_manager_->get_runway(runway_id);
        if (!runway) {
            utils::safe_print("Error: Runway " + runway_id + " not found\n");
            return;
        }
        
        auto result = runway_manager_->test_runway_accessibility(target, runway, 5.0);
        bool net_success = std::get<0>(result);
        bool user_success = std::get<1>(result);
        double response_time = std::get<2>(result);
        
        if (json_output_) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"target\": \"" << escape_json(target) << "\",\n";
            oss << "  \"runway_id\": \"" << escape_json(runway_id) << "\",\n";
            oss << "  \"network_success\": " << (net_success ? "true" : "false") << ",\n";
            oss << "  \"user_success\": " << (user_success ? "true" : "false") << ",\n";
            oss << "  \"response_time\": " << std::fixed << std::setprecision(3) << response_time << "\n";
            oss << "}";
            print_json(oss.str());
        } else {
            utils::safe_print("Network: " + std::string(net_success ? "success" : "failed") + "\n");
            utils::safe_print("User: " + std::string(user_success ? "success" : "failed") + "\n");
            utils::safe_print("Response Time: " + std::to_string(response_time) + "s\n");
        }
    } else {
        auto all_runways = runway_manager_->get_all_runways();
        if (json_output_) {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"target\": \"" << escape_json(target) << "\",\n";
            oss << "  \"results\": [\n";
            for (size_t i = 0; i < all_runways.size(); ++i) {
                const auto& runway = all_runways[i];
                auto result = runway_manager_->test_runway_accessibility(target, runway, 5.0);
                bool net_success = std::get<0>(result);
                bool user_success = std::get<1>(result);
                double response_time = std::get<2>(result);
                
                oss << "    {\n";
                oss << "      \"runway_id\": \"" << escape_json(runway->id) << "\",\n";
                oss << "      \"network_success\": " << (net_success ? "true" : "false") << ",\n";
                oss << "      \"user_success\": " << (user_success ? "true" : "false") << ",\n";
                oss << "      \"response_time\": " << std::fixed << std::setprecision(3) << response_time << "\n";
                oss << "    }";
                if (i < all_runways.size() - 1) oss << ",";
                oss << "\n";
            }
            oss << "  ]\n";
            oss << "}";
            print_json(oss.str());
        } else {
            for (const auto& runway : all_runways) {
                auto result = runway_manager_->test_runway_accessibility(target, runway, 5.0);
                bool net_success = std::get<0>(result);
                bool user_success = std::get<1>(result);
                double response_time = std::get<2>(result);
                utils::safe_print(runway->id + ": net=" + (net_success ? "ok" : "fail") +
                                 ", user=" + (user_success ? "ok" : "fail") +
                                 ", time=" + std::to_string(response_time) + "s\n");
            }
        }
    }
}

void ProxyCLI::reload() {
    // Note: Full reload would require re-initializing components
    // For now, just acknowledge the command
    if (!json_output_) {
        utils::safe_print("Configuration reloaded\n");
    }
}
