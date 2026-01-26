use serde::{Deserialize, Serialize};
use std::collections::HashMap;

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct Config {
    pub routing_mode: Option<String>,
    pub dns_servers: Vec<DNSServerConfig>,
    pub upstream_proxies: Vec<UpstreamProxyConfig>,
    pub interfaces: Vec<String>,
    pub health_check_interval: Option<u64>,
    pub accessibility_timeout: Option<u64>,
    pub dns_timeout: Option<f64>,
    pub network_timeout: Option<u64>,
    pub user_validation_timeout: Option<u64>,
    pub max_concurrent_connections: Option<usize>,
    pub max_connections_per_runway: Option<usize>,
    pub success_rate_threshold: Option<f64>,
    pub success_rate_window: Option<usize>,
    pub log_level: Option<String>,
    pub log_file: Option<String>,
    pub log_max_bytes: Option<u64>,
    pub log_backup_count: Option<usize>,
    pub proxy_listen_host: Option<String>,
    pub proxy_listen_port: Option<u16>,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct DNSServerConfig {
    pub host: String,
    #[serde(default = "default_dns_port")]
    pub port: u16,
    #[serde(default)]
    pub name: String,
}

#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct UpstreamProxyConfig {
    #[serde(rename = "type")]
    pub proxy_type: String,
    pub host: String,
    pub port: u16,
}

fn default_dns_port() -> u16 {
    53
}

impl Default for Config {
    fn default() -> Self {
        Self {
            routing_mode: Some("latency".to_string()),
            dns_servers: vec![],
            upstream_proxies: vec![],
            interfaces: vec!["auto".to_string()],
            health_check_interval: Some(60),
            accessibility_timeout: Some(5),
            dns_timeout: Some(3.0),
            network_timeout: Some(10),
            user_validation_timeout: Some(15),
            max_concurrent_connections: Some(100),
            max_connections_per_runway: Some(10),
            success_rate_threshold: Some(0.5),
            success_rate_window: Some(10),
            log_level: Some("INFO".to_string()),
            log_file: Some("logs/proxy.log".to_string()),
            log_max_bytes: Some(10_485_760),
            log_backup_count: Some(5),
            proxy_listen_host: Some("127.0.0.1".to_string()),
            proxy_listen_port: Some(2123),
        }
    }
}

impl Config {
    pub fn load(path: &str) -> anyhow::Result<Self> {
        let content = std::fs::read_to_string(path)?;
        let config: Config = serde_json::from_str(&content)?;
        Ok(config)
    }

    pub fn routing_mode(&self) -> RoutingMode {
        match self.routing_mode.as_deref() {
            Some("latency") => RoutingMode::Latency,
            Some("first_accessible") => RoutingMode::FirstAccessible,
            Some("round_robin") => RoutingMode::RoundRobin,
            _ => RoutingMode::Latency,
        }
    }
}

use crate::routing::RoutingMode;
