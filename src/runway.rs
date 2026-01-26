use std::net::IpAddr;
use serde::{Deserialize, Serialize};

use crate::config::{DNSServerConfig, UpstreamProxyConfig};

#[derive(Debug, Clone, PartialEq, Eq, Hash, Serialize, Deserialize)]
pub enum RunwayState {
    Unknown,
    Accessible,
    PartiallyAccessible,
    Inaccessible,
    Testing,
}

#[derive(Debug, Clone)]
pub struct DNSServer {
    pub config: DNSServerConfig,
    pub response_time: f64,
    pub last_success: Option<std::time::Instant>,
    pub failure_count: u32,
}

#[derive(Debug, Clone)]
pub struct UpstreamProxy {
    pub config: UpstreamProxyConfig,
    pub accessible: bool,
    pub last_success: Option<std::time::Instant>,
    pub failure_count: u32,
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
pub struct Runway {
    pub id: String,
    pub interface: String,
    pub source_ip: Option<IpAddr>,
    pub upstream_proxy: Option<UpstreamProxy>,
    pub dns_server: Option<DNSServer>,
    pub resolved_ip: Option<IpAddr>,
    pub is_direct: bool,
}

impl Runway {
    pub fn new(
        id: String,
        interface: String,
        source_ip: Option<IpAddr>,
        upstream_proxy: Option<UpstreamProxy>,
        dns_server: Option<DNSServer>,
    ) -> Self {
        let is_direct = upstream_proxy.is_none();
        Self {
            id,
            interface,
            source_ip,
            upstream_proxy,
            dns_server,
            resolved_ip: None,
            is_direct,
        }
    }
}
