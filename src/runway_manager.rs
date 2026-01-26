use std::collections::HashMap;
use std::net::IpAddr;
use std::sync::Arc;
use parking_lot::RwLock;
use get_if_addrs::get_if_addrs;
use reqwest::Client;
use std::time::Duration;

use crate::config::{DNSServerConfig, UpstreamProxyConfig};
use crate::dns::DNSResolver;
use crate::runway::{DNSServer, Runway, UpstreamProxy};

pub struct RunwayManager {
    interfaces: Vec<String>,
    upstream_proxies: Vec<UpstreamProxy>,
    dns_servers: Vec<DNSServer>,
    dns_resolver: Arc<DNSResolver>,
    runways: Arc<RwLock<HashMap<String, Runway>>>,
    interface_info: Arc<RwLock<HashMap<String, InterfaceInfo>>>,
}

#[derive(Debug, Clone)]
struct InterfaceInfo {
    ip: IpAddr,
    netmask: Option<String>,
}

impl RunwayManager {
    pub fn new(
        interfaces: Vec<String>,
        upstream_proxies: Vec<UpstreamProxyConfig>,
        dns_servers: Vec<DNSServerConfig>,
        dns_resolver: Arc<DNSResolver>,
    ) -> Self {
        let upstream_proxies: Vec<UpstreamProxy> = upstream_proxies
            .into_iter()
            .map(|cfg| UpstreamProxy {
                config: cfg,
                accessible: true,
                last_success: None,
                failure_count: 0,
            })
            .collect();

        let dns_servers: Vec<DNSServer> = dns_servers
            .into_iter()
            .map(|cfg| DNSServer {
                config: cfg,
                response_time: 0.0,
                last_success: None,
                failure_count: 0,
            })
            .collect();

        let manager = Self {
            interfaces,
            upstream_proxies,
            dns_servers,
            dns_resolver,
            runways: Arc::new(RwLock::new(HashMap::new())),
            interface_info: Arc::new(RwLock::new(HashMap::new())),
        };

        manager.discover_interfaces();
        manager
    }

    fn discover_interfaces(&self) {
        match get_if_addrs() {
            Ok(if_addrs) => {
                let mut interface_info = self.interface_info.write();
                let current_interfaces: std::collections::HashSet<String> = if_addrs
                    .iter()
                    .filter_map(|iface| {
                        if let std::net::IpAddr::V4(_) = iface.ip {
                            Some(iface.name.clone())
                        } else {
                            None
                        }
                    })
                    .collect();

                for iface in if_addrs {
                    if let std::net::IpAddr::V4(ipv4) = iface.ip {
                        let old_ip = interface_info.get(&iface.name).map(|info| info.ip);
                        let new_ip = IpAddr::V4(ipv4);

                        interface_info.insert(
                            iface.name.clone(),
                            InterfaceInfo {
                                ip: new_ip,
                                netmask: iface.netmask.map(|n| n.to_string()),
                            },
                        );

                        if let Some(old) = old_ip {
                            if old != new_ip {
                                log::warn!("Interface {} IP changed: {} -> {}", iface.name, old, new_ip);
                            }
                        } else {
                            log::debug!("Discovered interface {}: {}", iface.name, new_ip);
                        }
                    }
                }

                // Remove disconnected interfaces
                let existing: std::collections::HashSet<String> = interface_info.keys().cloned().collect();
                for removed in existing.difference(&current_interfaces) {
                    log::warn!("Interface {} removed/disconnected", removed);
                    interface_info.remove(removed);
                }
            }
            Err(e) => {
                log::error!("Error discovering interfaces: {}", e);
            }
        }
    }

    pub async fn refresh_interfaces(&self) {
        let old: std::collections::HashSet<String> = self.interface_info.read().keys().cloned().collect();
        self.discover_interfaces();
        let new: std::collections::HashSet<String> = self.interface_info.read().keys().cloned().collect();

        let added: Vec<_> = new.difference(&old).collect();
        let removed: Vec<_> = old.difference(&new).collect();

        if !added.is_empty() {
            log::info!("New interfaces detected: {:?}", added);
        }
        if !removed.is_empty() {
            log::warn!("Interfaces removed: {:?}", removed);
        }
    }

    pub async fn discover_runways(&self) -> Vec<Runway> {
        let interface_info = self.interface_info.read();
        let interfaces_to_use: Vec<String> = if self.interfaces.contains(&"auto".to_string()) {
            interface_info.keys().cloned().collect()
        } else {
            self.interfaces
                .iter()
                .filter(|iface| interface_info.contains_key(*iface))
                .cloned()
                .collect()
        };

        let mut runways = Vec::new();
        let mut runway_id_counter = 0;

        // Create direct runways
        for interface in &interfaces_to_use {
            if let Some(info) = interface_info.get(interface) {
                let source_ip = Some(info.ip);
                for dns_server in &self.dns_servers {
                    let runway_id = format!(
                        "direct_{}_{}_{}",
                        interface, dns_server.config.host, runway_id_counter
                    );
                    let runway = Runway::new(
                        runway_id.clone(),
                        interface.clone(),
                        source_ip,
                        None,
                        Some(dns_server.clone()),
                    );
                    runways.push(runway);
                    runway_id_counter += 1;
                }
            }
        }

        // Create proxy runways
        for interface in &interfaces_to_use {
            if let Some(info) = interface_info.get(interface) {
                let source_ip = Some(info.ip);
                for proxy in &self.upstream_proxies {
                    for dns_server in &self.dns_servers {
                        let runway_id = format!(
                            "proxy_{}_{}_{}_{}_{}",
                            interface,
                            proxy.config.proxy_type,
                            proxy.config.host,
                            dns_server.config.host,
                            runway_id_counter
                        );
                        let runway = Runway::new(
                            runway_id.clone(),
                            interface.clone(),
                            source_ip,
                            Some(proxy.clone()),
                            Some(dns_server.clone()),
                        );
                        runways.push(runway);
                        runway_id_counter += 1;
                    }
                }
            }
        }

        {
            let mut runways_map = self.runways.write();
            runways_map.clear();
            for runway in &runways {
                runways_map.insert(runway.id.clone(), runway.clone());
            }
        }

        log::info!("Discovered {} runways", runways.len());
        runways
    }

    pub async fn get_runway(&self, runway_id: &str) -> Option<Runway> {
        self.runways.read().get(runway_id).cloned()
    }

    pub async fn get_all_runways(&self) -> Vec<Runway> {
        self.runways.read().values().cloned().collect()
    }

    pub async fn test_runway_accessibility(
        &self,
        target: &str,
        runway: &Runway,
        timeout: Duration,
    ) -> (bool, bool, Duration) {
        let start = std::time::Instant::now();

        // Resolve target if needed
        let resolved_ip = if self.dns_resolver.is_ip_address(target)
            || self.dns_resolver.is_private_ip(target)
        {
            target.parse().ok()
        } else {
            match self.dns_resolver.resolve(target).await {
                Ok((ip, _)) => ip,
                Err(_) => return (false, false, start.elapsed()),
            }
        };

        if resolved_ip.is_none() {
            return (false, false, start.elapsed());
        }

        // Test connection
        let network_success = if let Some(proxy) = &runway.upstream_proxy {
            if !proxy.accessible {
                return (false, false, start.elapsed());
            }
            self.test_proxy_connection(runway, resolved_ip.unwrap(), timeout)
                .await
        } else {
            self.test_direct_connection(runway, resolved_ip.unwrap(), timeout)
                .await
        };

        let elapsed = start.elapsed();
        let user_success = network_success; // Simplified for now
        (network_success, user_success, elapsed)
    }

    async fn test_direct_connection(&self, runway: &Runway, target_ip: IpAddr, timeout: Duration) -> bool {
        if !self.interface_info.read().contains_key(&runway.interface) {
            log::debug!("Interface {} not available for runway {}", runway.interface, runway.id);
            return false;
        }

        match tokio::time::timeout(
            timeout,
            tokio::net::TcpStream::connect((target_ip, 80)),
        )
        .await
        {
            Ok(Ok(_)) => true,
            Ok(Err(e)) => {
                log::debug!("Connection refused to {}: {}", target_ip, e);
                false
            }
            Err(_) => {
                log::debug!("Connection timeout to {}", target_ip);
                false
            }
        }
    }

    async fn test_proxy_connection(&self, runway: &Runway, target_ip: IpAddr, timeout: Duration) -> bool {
        let proxy = match &runway.upstream_proxy {
            Some(p) => p,
            None => return false,
        };

        if !proxy.accessible {
            log::debug!("Proxy {} marked as inaccessible", proxy.config.host);
            return false;
        }

        let proxy_url = format!(
            "{}://{}:{}",
            proxy.config.proxy_type, proxy.config.host, proxy.config.port
        );

        let client = Client::builder()
            .timeout(timeout)
            .build()
            .ok()?;

        let test_url = format!("http://{}", target_ip);
        match client.get(&test_url).send().await {
            Ok(response) => {
                response.status().is_success()
            }
            Err(e) => {
                log::debug!("Proxy connection error: {}", e);
                false
            }
        }
    }
}
