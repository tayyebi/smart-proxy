use smartproxy::*;
use std::sync::Arc;
use anyhow::Result;

#[tokio::main]
async fn main() -> Result<()> {
    // Load config
    let config = Config::load("config.json").unwrap_or_default();
    
    // Setup logging
    let log_level = config.log_level.as_deref().unwrap_or("INFO");
    std::env::set_var("RUST_LOG", log_level);
    env_logger::Builder::from_default_env()
        .filter_level(match log_level {
            "DEBUG" => log::LevelFilter::Debug,
            "INFO" => log::LevelFilter::Info,
            "WARN" => log::LevelFilter::Warn,
            "ERROR" => log::LevelFilter::Error,
            _ => log::LevelFilter::Info,
        })
        .init();

    log::info!("Initializing Smart Proxy Service...");

    // Initialize DNS resolver
    let dns_servers = config.dns_servers.clone();
    let dns_timeout = config.dns_timeout.unwrap_or(3.0);
    let dns_resolver = Arc::new(DNSResolver::new(dns_servers.clone(), dns_timeout)?);

    // Initialize runway manager
    let upstream_proxies = config.upstream_proxies.clone();
    let interfaces = config.interfaces.clone();
    let runway_manager = Arc::new(RunwayManager::new(
        interfaces,
        upstream_proxies,
        dns_servers,
        Arc::clone(&dns_resolver),
    ));

    // Discover runways
    runway_manager.discover_runways().await;
    log::info!("Discovered {} runways", runway_manager.get_all_runways().await.len());

    // Initialize accessibility tracker
    let success_rate_window = config.success_rate_window.unwrap_or(10);
    let success_rate_threshold = config.success_rate_threshold.unwrap_or(0.5);
    let tracker = Arc::new(TargetAccessibilityTracker::new(
        success_rate_window,
        success_rate_threshold,
    ));

    // Initialize success validator
    let validator = Arc::new(SuccessValidator::new());

    // Initialize routing engine
    let routing_mode = config.routing_mode();
    let routing_engine = Arc::new(RoutingEngine::new(Arc::clone(&tracker), routing_mode));

    // Initialize proxy server
    let proxy_server = ProxyServer::new(
        config.clone(),
        Arc::clone(&runway_manager),
        Arc::clone(&routing_engine),
        Arc::clone(&tracker),
        Arc::clone(&dns_resolver),
        Arc::clone(&validator),
    );

    // Initialize health monitor
    let health_interval = config.health_check_interval.unwrap_or(60);
    let health_monitor = Arc::new(HealthMonitor::new(
        Arc::clone(&runway_manager),
        Arc::clone(&tracker),
        health_interval,
    ));

    log::info!("Initialization complete");

    // Start proxy server
    let proxy_server = Arc::new(proxy_server);
    let server_handle = {
        let server = Arc::clone(&proxy_server);
        tokio::spawn(async move {
            if let Err(e) = server.start().await {
                log::error!("Proxy server error: {}", e);
            }
        })
    };

    // Start health monitor
    let health_handle = {
        let monitor = Arc::clone(&health_monitor);
        tokio::spawn(async move {
            monitor.start().await;
        })
    };

    log::info!("Smart Proxy Service started");

    // Wait for shutdown signal
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {
            log::info!("Shutting down...");
        }
        _ = server_handle => {
            log::error!("Server task ended unexpectedly");
        }
        _ = health_handle => {
            log::error!("Health monitor task ended unexpectedly");
        }
    }

    health_monitor.stop();
    log::info!("Smart Proxy Service stopped");

    Ok(())
}
