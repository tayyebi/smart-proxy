use smartproxy::*;
use smartproxy::runway::RunwayState as RS;
use clap::{Parser, Subcommand};
use std::sync::Arc;
use anyhow::Result;
use serde_json::json;

#[derive(Parser)]
#[command(name = "smartproxy-cli")]
#[command(about = "Smart Proxy CLI Management Tool")]
struct Cli {
    #[arg(long)]
    json: bool,
    
    #[arg(long, default_value = "config.json")]
    config: String,

    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    Status,
    Runways,
    Targets,
    Stats,
    Reload,
    Mode {
        mode: String,
    },
    Test {
        target: String,
        runway_id: Option<String>,
    },
}

#[tokio::main]
async fn main() -> Result<()> {
    let cli = Cli::parse();

    // Setup basic logging
    std::env::set_var("RUST_LOG", "warn");
    env_logger::init();

    // Load config and initialize service components
    let config = Config::load(&cli.config).unwrap_or_default();

    let dns_servers = config.dns_servers.clone();
    let dns_timeout = config.dns_timeout.unwrap_or(3.0);
    let dns_resolver = Arc::new(DNSResolver::new(dns_servers.clone(), dns_timeout)?);

    let upstream_proxies = config.upstream_proxies.clone();
    let interfaces = config.interfaces.clone();
    let runway_manager = Arc::new(RunwayManager::new(
        interfaces,
        upstream_proxies,
        dns_servers,
        Arc::clone(&dns_resolver),
    ));

    runway_manager.discover_runways().await;

    let success_rate_window = config.success_rate_window.unwrap_or(10);
    let success_rate_threshold = config.success_rate_threshold.unwrap_or(0.5);
    let tracker = Arc::new(TargetAccessibilityTracker::new(
        success_rate_window,
        success_rate_threshold,
    ));

    let routing_mode = config.routing_mode();
    let routing_engine = Arc::new(RoutingEngine::new(Arc::clone(&tracker), routing_mode));

    match cli.command {
        Commands::Status => {
            let runways_count = runway_manager.get_all_runways().await.len();
            let targets_count = tracker.get_all_targets().await.len();
            let mode = format!("{:?}", routing_engine.mode.read());
            
            let status = json!({
                "routing_mode": mode,
                "runways_count": runways_count,
                "targets_count": targets_count,
                "status": "running"
            });

            if cli.json {
                println!("{}", serde_json::to_string_pretty(&status)?);
            } else {
                println!("Routing Mode: {}", mode);
                println!("Runways: {}", runways_count);
                println!("Targets: {}", targets_count);
            }
        }
        Commands::Runways => {
            let all_runways = runway_manager.get_all_runways().await;
            let runways_data: Vec<_> = all_runways
                .iter()
                .map(|r| {
                    json!({
                        "id": r.id,
                        "interface": r.interface,
                        "source_ip": r.source_ip.map(|ip| ip.to_string()),
                        "is_direct": r.is_direct,
                        "upstream_proxy": r.upstream_proxy.as_ref().map(|p| format!("{}://{}:{}", p.config.proxy_type, p.config.host, p.config.port)),
                        "dns_server": r.dns_server.as_ref().map(|d| format!("{}:{}", d.config.host, d.config.port))
                    })
                })
                .collect();

            let output = json!({
                "runways": runways_data,
                "count": runways_data.len()
            });

            if cli.json {
                println!("{}", serde_json::to_string_pretty(&output)?);
            } else {
                for runway in &all_runways {
                    println!("{}: {} (direct: {})", runway.id, runway.interface, runway.is_direct);
                }
            }
        }
        Commands::Targets => {
            let targets = tracker.get_all_targets().await;
            let mut targets_data = serde_json::Map::new();

            for target in targets {
                let metrics = tracker.get_target_metrics(&target).await;
                let mut target_info = serde_json::Map::new();

                for (runway_id, metric) in metrics {
                    target_info.insert(runway_id, json!({
                        "state": format!("{:?}", metric.state),
                        "success_rate": metric.success_rate,
                        "avg_response_time": metric.avg_response_time,
                        "total_attempts": metric.total_attempts,
                        "user_success_count": metric.user_success_count,
                        "failure_count": metric.failure_count
                    }));
                }

                targets_data.insert(target, json!(target_info));
            }

            let output = json!({ "targets": targets_data });

            if cli.json {
                println!("{}", serde_json::to_string_pretty(&output)?);
            } else {
                for (target, info) in targets_data {
                    println!("{}: {} runways", target, info.as_object().unwrap().len());
                }
            }
        }
        Commands::Mode { mode } => {
            let routing_mode = match mode.to_lowercase().as_str() {
                "latency" => RoutingMode::Latency,
                "first_accessible" => RoutingMode::FirstAccessible,
                "round_robin" => RoutingMode::RoundRobin,
                _ => {
                    eprintln!("Error: Invalid routing mode '{}'. Valid modes: latency, first_accessible, round_robin", mode);
                    return Ok(());
                }
            };
            routing_engine.set_mode(routing_mode);
            if !cli.json {
                println!("Routing mode changed to {}", mode);
            }
        }
        Commands::Test { target, runway_id } => {
            if let Some(runway_id) = runway_id {
                if let Some(runway) = runway_manager.get_runway(&runway_id).await {
                    let timeout = std::time::Duration::from_secs(5);
                    let (net_success, user_success, response_time) = runway_manager
                        .test_runway_accessibility(&target, &runway, timeout)
                        .await;

                    let result = json!({
                        "target": target,
                        "runway_id": runway_id,
                        "network_success": net_success,
                        "user_success": user_success,
                        "response_time": response_time.as_secs_f64()
                    });

                    if cli.json {
                        println!("{}", serde_json::to_string_pretty(&result)?);
                    } else {
                        println!("Network: {}, User: {}, Time: {:?}", net_success, user_success, response_time);
                    }
                } else {
                    eprintln!("Error: Runway {} not found", runway_id);
                }
            } else {
                let all_runways = runway_manager.get_all_runways().await;
                let timeout = std::time::Duration::from_secs(5);
                let mut results = Vec::new();

                for runway in &all_runways {
                    let (net_success, user_success, response_time) = runway_manager
                        .test_runway_accessibility(&target, runway, timeout)
                        .await;
                    results.push(json!({
                        "runway_id": runway.id,
                        "network_success": net_success,
                        "user_success": user_success,
                        "response_time": response_time.as_secs_f64()
                    }));
                }

                let output = json!({
                    "target": target,
                    "results": results
                });

                if cli.json {
                    println!("{}", serde_json::to_string_pretty(&output)?);
                } else {
                    for result in results {
                        println!("{}: net={}, user={}, time={}s",
                            result["runway_id"], result["network_success"], result["user_success"], result["response_time"]);
                    }
                }
            }
        }
        Commands::Stats => {
            let targets = tracker.get_all_targets().await;
            let total_runways = runway_manager.get_all_runways().await.len();
            let mut targets_data = serde_json::Map::new();

            for target in &targets {
                let metrics = tracker.get_target_metrics(target).await;
                let accessible = metrics.values().filter(|m| matches!(m.state, RS::Accessible)).count();
                let partial = metrics.values().filter(|m| matches!(m.state, RS::PartiallyAccessible)).count();
                let inaccessible = metrics.values().filter(|m| matches!(m.state, RS::Inaccessible)).count();
                let total_attempts: u64 = metrics.values().map(|m| m.total_attempts).sum();
                let total_successes: u64 = metrics.values().map(|m| m.user_success_count).sum();

                targets_data.insert(target.clone(), json!({
                    "accessible_runways": accessible,
                    "partially_accessible_runways": partial,
                    "inaccessible_runways": inaccessible,
                    "total_attempts": total_attempts,
                    "total_successes": total_successes
                }));
            }

            let output = json!({
                "total_targets": targets.len(),
                "total_runways": total_runways,
                "targets": targets_data
            });

            if cli.json {
                println!("{}", serde_json::to_string_pretty(&output)?);
            } else {
                println!("Total Targets: {}", targets.len());
                println!("Total Runways: {}", total_runways);
            }
        }
        Commands::Reload => {
            if !cli.json {
                println!("Configuration reloaded");
            }
        }
    }

    Ok(())
}
