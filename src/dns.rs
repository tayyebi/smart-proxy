use std::collections::HashMap;
use std::net::IpAddr;
use std::sync::Arc;
use std::time::{Duration, Instant};
use parking_lot::RwLock;
use trust_dns_resolver::config::{NameServerConfig, Protocol, ResolverConfig, ResolverOpts};
use trust_dns_resolver::TokioAsyncResolver;
use anyhow::Result;
use ipnetwork::IpNetwork;

use crate::config::DNSServerConfig;

pub struct DNSResolver {
    resolvers: Vec<(DNSServerConfig, TokioAsyncResolver)>,
    cache: Arc<RwLock<HashMap<String, (Option<IpAddr>, Instant)>>>,
    timeout: Duration,
}

impl DNSResolver {
    pub fn new(servers: Vec<DNSServerConfig>, timeout_secs: f64) -> Result<Self> {
        let timeout = Duration::from_secs_f64(timeout_secs);
        let mut resolvers = Vec::new();

        for server in servers {
            let mut config = ResolverConfig::new();
            let ip: IpAddr = server.host.parse()?;
            let socket_addr = (ip, server.port).into();
            config.add_name_server(NameServerConfig::new(socket_addr, Protocol::Udp));
            
            let resolver = TokioAsyncResolver::tokio(config, ResolverOpts::default())?;
            resolvers.push((server, resolver));
        }

        Ok(Self {
            resolvers,
            cache: Arc::new(RwLock::new(HashMap::new())),
            timeout,
        })
    }

    pub fn is_ip_address(&self, target: &str) -> bool {
        target.parse::<IpAddr>().is_ok()
    }

    pub fn is_private_ip(&self, ip: &str) -> bool {
        if let Ok(ip_addr) = ip.parse::<IpAddr>() {
            if let Ok(network) = IpNetwork::new(ip_addr, 32) {
                return network.is_private();
            }
        }
        false
    }

    pub async fn resolve(&self, domain: &str) -> Result<(Option<IpAddr>, Duration)> {
        // Skip DNS for IP addresses
        if self.is_ip_address(domain) {
            return Ok((Some(domain.parse()?), Duration::ZERO));
        }

        // Check cache
        {
            let cache = self.cache.read();
            if let Some((ip, expiry)) = cache.get(domain) {
                if expiry.elapsed() < Duration::from_secs(300) {
                    return Ok((*ip, Duration::ZERO));
                }
            }
        }

        // Try DNS servers
        for (server, resolver) in &self.resolvers {
            let start = Instant::now();
            match tokio::time::timeout(self.timeout, resolver.lookup_ip(domain)).await {
                Ok(Ok(lookup)) => {
                    let elapsed = start.elapsed();
                    if let Some(ip) = lookup.iter().next() {
                        let ip_addr = ip.into();
                        // Cache with TTL
                        {
                            let mut cache = self.cache.write();
                            cache.insert(domain.to_string(), (Some(ip_addr), Instant::now()));
                        }
                        log::debug!("Resolved {} -> {} via {} in {:?}", domain, ip_addr, server.host, elapsed);
                        return Ok((Some(ip_addr), elapsed));
                    }
                }
                Ok(Err(e)) => {
                    log::warning!("DNS error for {} via {}: {}", domain, server.host, e);
                }
                Err(_) => {
                    log::warning!("DNS timeout for {} via {}", domain, server.host);
                }
            }
        }

        log::error!("All DNS servers failed for {}", domain);
        Ok((None, self.timeout))
    }
}
