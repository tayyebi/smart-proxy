use std::sync::Arc;
use std::time::Duration;
use hyper::server::conn::http1;
use hyper::service::service_fn;
use hyper::{Method, Request, Response, StatusCode, Uri};
use hyper_util::rt::TokioIo;
use http_body_util::Full;
use hyper_util::client::legacy::connect::HttpConnector;
use hyper_util::client::legacy::Client;
use tokio::net::TcpListener;
use anyhow::Result;
use reqwest::Client as ReqwestClient;

use crate::config::Config;
use crate::runway::Runway;
use crate::runway_manager::RunwayManager;
use crate::routing::RoutingEngine;
use crate::tracker::TargetAccessibilityTracker;
use crate::dns::DNSResolver;
use crate::validator::SuccessValidator;

#[derive(Clone)]
pub struct ProxyServer {
    config: Config,
    runway_manager: Arc<RunwayManager>,
    routing_engine: Arc<RoutingEngine>,
    tracker: Arc<TargetAccessibilityTracker>,
    dns_resolver: Arc<DNSResolver>,
    validator: Arc<SuccessValidator>,
}

impl ProxyServer {
    pub fn new(
        config: Config,
        runway_manager: Arc<RunwayManager>,
        routing_engine: Arc<RoutingEngine>,
        tracker: Arc<TargetAccessibilityTracker>,
        dns_resolver: Arc<DNSResolver>,
        validator: Arc<SuccessValidator>,
    ) -> Self {
        Self {
            config,
            runway_manager,
            routing_engine,
            tracker,
            dns_resolver,
            validator,
        }
    }

    pub async fn start(self: Arc<Self>) -> Result<()> {
        let host = self.config.proxy_listen_host.as_deref().unwrap_or("127.0.0.1");
        let port = self.config.proxy_listen_port.unwrap_or(2123);
        let addr = format!("{}:{}", host, port).parse()?;

        let listener = TcpListener::bind(&addr).await?;
        log::info!("Proxy server started on {}:{}", host, port);

        let server_clone = Arc::clone(&self);
        tokio::spawn(async move {
            loop {
                match listener.accept().await {
                    Ok((stream, _)) => {
                        let io = TokioIo::new(stream);
                        let server_handle = Arc::clone(&server_clone);
                        tokio::spawn(async move {
                            if let Err(err) = http1::Builder::new()
                                .serve_connection(io, service_fn({
                                    let s = Arc::clone(&server_handle);
                                    move |req| {
                                        let s = Arc::clone(&s);
                                        async move {
                                            s.handle_request(req).await
                                        }
                                    }
                                }))
                                .await
                            {
                                log::error!("Error serving connection: {}", err);
                            }
                        });
                    }
                    Err(e) => {
                        log::error!("Error accepting connection: {}", e);
                    }
                }
            }
        });

        Ok(())
    }

    async fn handle_request(&self, req: Request<hyper::body::Incoming>) -> Result<Response<Full<bytes::Bytes>>, hyper::Error> {
        let start = std::time::Instant::now();
        
        let (parts, body) = req.into_parts();
        let method = parts.method.clone();
        let uri = parts.uri.clone();
        
        // Extract target from request
        let (target_host, target_port) = if method == Method::CONNECT {
            // CONNECT method
            let host = parts.headers.get("host")
                .and_then(|h| h.to_str().ok())
                .unwrap_or("");
            let parts: Vec<&str> = host.split(':').collect();
            let host = parts[0];
            let port = parts.get(1).and_then(|p| p.parse::<u16>().ok()).unwrap_or(443);
            (host.to_string(), port)
        } else {
            // Regular HTTP request
            let host = uri.host().unwrap_or("");
            let port = uri.port_u16().unwrap_or(if uri.scheme_str() == Some("https") { 443 } else { 80 });
            (host.to_string(), port)
        };

        if method == Method::CONNECT {
            return Ok(Response::builder()
                .status(StatusCode::NOT_IMPLEMENTED)
                .body(Full::new(bytes::Bytes::from("CONNECT method not fully implemented")))
                .unwrap());
        }

        // Select runway
        let all_runways = self.runway_manager.get_all_runways().await;
        let mut runway = self.routing_engine.select_runway(&target_host, &all_runways).await;

        if runway.is_none() {
            log::debug!("No known accessible runway for {}, testing all runways", target_host);
            let server = Arc::new(self.clone());
            runway = server.test_all_runways(&target_host, &all_runways).await;
        }

        let runway = match runway {
            Some(r) => r,
            None => {
                log::warn!("No accessible runway found for {}", target_host);
                return Ok(Response::builder()
                    .status(StatusCode::BAD_GATEWAY)
                    .body(Full::new(bytes::Bytes::from("No accessible runway found")))
                    .unwrap());
            }
        };

        // Retry logic
        let max_retries = 2;
        for attempt in 0..max_retries {
            match self.make_http_request(&method, &uri, &parts.headers, &target_host, target_port, &runway).await {
                Ok((network_success, user_success, status, headers, body)) => {
                    let response_time = start.elapsed();
                    self.tracker.update(&target_host, &runway.id, network_success, user_success, response_time).await;

                    if network_success {
                        let mut response_builder = Response::builder().status(status);
                        for (key, value) in headers {
                            if let (Ok(k), Ok(v)) = (key.to_str(), value.to_str()) {
                                response_builder = response_builder.header(k, v);
                            }
                        }
                        return Ok(response_builder.body(Full::new(body)).unwrap());
                    } else if attempt < max_retries - 1 {
                        log::debug!("Request failed, trying alternative runway (attempt {})", attempt + 1);
                        if let Some(alt) = self.get_alternative_runway(&target_host, &runway.id).await {
                            runway = alt;
                            continue;
                        }
                    }
                    return Ok(Response::builder()
                        .status(StatusCode::BAD_GATEWAY)
                        .body(Full::new(bytes::Bytes::from("Request failed")))
                        .unwrap());
                }
                Err(e) => {
                    log::error!("Error making request (attempt {}): {}", attempt + 1, e);
                    if attempt < max_retries - 1 {
                        if let Some(alt) = self.get_alternative_runway(&target_host, &runway.id).await {
                            runway = alt;
                            continue;
                        }
                    }
                    return Ok(Response::builder()
                        .status(StatusCode::BAD_GATEWAY)
                        .body(Full::new(bytes::Bytes::from(format!("Request failed: {}", e))))
                        .unwrap());
                }
            }
        }

        Ok(Response::builder()
            .status(StatusCode::BAD_GATEWAY)
            .body(Full::new(bytes::Bytes::from("Request failed")))
            .unwrap())
    }

    async fn make_http_request(
        &self,
        method: &Method,
        uri: &Uri,
        headers: &hyper::HeaderMap,
        target_host: &str,
        target_port: u16,
        runway: &Runway,
    ) -> Result<(bool, bool, u16, Vec<(String, String)>, bytes::Bytes)> {
        // Resolve target
        let resolved_ip = match self.dns_resolver.resolve(target_host).await {
            Ok((Some(ip), _)) => ip,
            Ok((None, _)) => return Ok((false, false, 502, vec![], bytes::Bytes::new())),
            Err(e) => {
                log::error!("DNS resolution failed: {}", e);
                return Ok((false, false, 502, vec![], bytes::Bytes::new()));
            }
        };

        // Build proxy URL if needed
        let proxy_url = runway.upstream_proxy.as_ref().and_then(|p| {
            if p.accessible {
                Some(format!("{}://{}:{}", p.config.proxy_type, p.config.host, p.config.port))
            } else {
                None
            }
        });

        // Build target URL
        let scheme = if target_port == 443 { "https" } else { "http" };
        let path = uri.path_and_query().map(|pq| pq.as_str()).unwrap_or("/");
        let url = format!("{}://{}:{}{}", scheme, resolved_ip, target_port, path);

        // Create HTTP client
        let timeout = Duration::from_secs(self.config.network_timeout.unwrap_or(10) as u64);
        let mut client_builder = ReqwestClient::builder().timeout(timeout);

        if let Some(proxy_url_str) = &proxy_url {
            let proxy = reqwest::Proxy::all(proxy_url_str)?;
            client_builder = client_builder.proxy(proxy);
        }

        let client = client_builder.build()?;

        // Prepare request
        let mut request_builder = match *method {
            Method::GET => client.get(&url),
            Method::POST => client.post(&url),
            Method::PUT => client.put(&url),
            Method::DELETE => client.delete(&url),
            Method::PATCH => client.patch(&url),
            Method::HEAD => client.head(&url),
            _ => client.get(&url),
        };

        // Copy headers (skip hop-by-hop headers)
        for (key, value) in headers {
            if let Some(key_str) = key.as_str() {
                if !matches!(key_str, "host" | "connection" | "proxy-connection") {
                    if let Ok(value_str) = value.to_str() {
                        request_builder = request_builder.header(key_str, value_str);
                    }
                }
            }
        }
        request_builder = request_builder.header("Host", format!("{}:{}", target_host, target_port));

        // Execute request
        match request_builder.send().await {
            Ok(response) => {
                let status = response.status().as_u16();
                let network_success = response.status().is_success() || response.status().is_redirection();
                
                let headers: Vec<(String, String)> = response
                    .headers()
                    .iter()
                    .filter_map(|(k, v)| {
                        k.as_str()
                            .and_then(|k_str| v.to_str().ok().map(|v_str| (k_str.to_string(), v_str.to_string())))
                    })
                    .collect();

                let body = response.bytes().await.unwrap_or_default();
                
                let user_success = if status == 200 {
                    let (_, user_ok) = self.validator.validate_http(status, &body).await;
                    user_ok
                } else {
                    false
                };

                Ok((network_success, user_success, status, headers, body))
            }
            Err(e) => {
                log::error!("Request failed: {}", e);
                Ok((false, false, 502, vec![], bytes::Bytes::new()))
            }
        }
    }

    async fn test_all_runways(&self, target: &str, all_runways: &[Runway]) -> Option<Runway> {
        let direct_runways: Vec<&Runway> = all_runways.iter().filter(|r| r.is_direct).collect();
        let proxy_runways: Vec<&Runway> = all_runways.iter().filter(|r| !r.is_direct).collect();
        let prioritized: Vec<&Runway> = direct_runways.into_iter().chain(proxy_runways).collect();

        let timeout = Duration::from_secs(self.config.accessibility_timeout.unwrap_or(5) as u64);
        let max_concurrent = 5;

        for chunk in prioritized.chunks(max_concurrent) {
            let tasks: Vec<_> = chunk
                .iter()
                .map(|runway| {
                    let manager = Arc::clone(&self.runway_manager);
                    let tracker = Arc::clone(&self.tracker);
                    let target = target.to_string();
                    let runway = (*runway).clone();
                    async move {
                        let result = manager.test_runway_accessibility(&target, &runway, timeout).await;
                        tracker.update(&target, &runway.id, result.0, result.1, result.2).await;
                        result
                    }
                })
                .collect();

            let results = futures::future::join_all(tasks).await;

            for (runway, result) in chunk.iter().zip(results) {
                let (net_success, user_success, _) = result;
                if user_success {
                    return Some((*runway).clone());
                }
            }
        }

        None
    }

    async fn get_alternative_runway(&self, target: &str, current_id: &str) -> Option<Runway> {
        let accessible_ids = self.tracker.get_accessible_runways(target).await;
        let alternative_ids: Vec<String> = accessible_ids
            .into_iter()
            .filter(|id| id != current_id)
            .collect();

        if let Some(first_id) = alternative_ids.first() {
            self.runway_manager.get_runway(first_id).await
        } else {
            None
        }
    }
}

