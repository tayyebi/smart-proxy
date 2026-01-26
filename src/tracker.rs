use std::collections::HashMap;
use std::sync::Arc;
use parking_lot::RwLock;
use dashmap::DashMap;

use crate::runway::RunwayState as RS;

#[derive(Debug, Clone)]
pub struct TargetMetrics {
    pub target: String,
    pub runway_id: String,
    pub state: RunwayState,
    pub network_success_count: u64,
    pub user_success_count: u64,
    pub failure_count: u64,
    pub partial_success_count: u64,
    pub total_attempts: u64,
    pub avg_response_time: f64,
    pub last_success_time: Option<std::time::Instant>,
    pub last_failure_time: Option<std::time::Instant>,
    pub consecutive_failures: u32,
    pub recovery_count: u64,
    pub success_rate: f64,
    pub recent_attempts: Vec<bool>,
}

impl TargetMetrics {
    pub fn new(target: String, runway_id: String) -> Self {
        Self {
            target,
            runway_id,
            state: RS::Unknown,
            network_success_count: 0,
            user_success_count: 0,
            failure_count: 0,
            partial_success_count: 0,
            total_attempts: 0,
            avg_response_time: 0.0,
            last_success_time: None,
            last_failure_time: None,
            consecutive_failures: 0,
            recovery_count: 0,
            success_rate: 0.0,
            recent_attempts: Vec::new(),
        }
    }

    fn update_success_rate(&mut self, window: usize) {
        if self.recent_attempts.is_empty() {
            self.success_rate = 0.0;
            return;
        }

        let success_count = self.recent_attempts.iter().filter(|&&x| x).count();
        self.success_rate = success_count as f64 / self.recent_attempts.len() as f64;
    }
}

pub struct TargetAccessibilityTracker {
    metrics: DashMap<String, DashMap<String, TargetMetrics>>,
    success_rate_window: usize,
    success_rate_threshold: f64,
}

impl TargetAccessibilityTracker {
    pub fn new(success_rate_window: usize, success_rate_threshold: f64) -> Self {
        Self {
            metrics: DashMap::new(),
            success_rate_window,
            success_rate_threshold,
        }
    }

    fn get_or_create_metrics(&self, target: &str, runway_id: &str) -> TargetMetrics {
        let target_map = self.metrics.entry(target.to_string()).or_insert_with(DashMap::new);
        
        target_map
            .entry(runway_id.to_string())
            .or_insert_with(|| TargetMetrics::new(target.to_string(), runway_id.to_string()))
            .clone()
    }

    pub async fn update(
        &self,
        target: &str,
        runway_id: &str,
        network_success: bool,
        user_success: bool,
        response_time: std::time::Duration,
    ) {
        let target_map = self.metrics.entry(target.to_string()).or_insert_with(DashMap::new);
        
        let mut metrics = target_map
            .entry(runway_id.to_string())
            .or_insert_with(|| TargetMetrics::new(target.to_string(), runway_id.to_string()))
            .clone();

        metrics.total_attempts += 1;
        let response_time_secs = response_time.as_secs_f64();

        // Update recent attempts
        metrics.recent_attempts.push(user_success);
        if metrics.recent_attempts.len() > self.success_rate_window {
            metrics.recent_attempts.remove(0);
        }

        if network_success && user_success {
            metrics.network_success_count += 1;
            metrics.user_success_count += 1;
            metrics.state = RunwayState::Accessible;
            metrics.last_success_time = Some(std::time::Instant::now());
            metrics.consecutive_failures = 0;

            // Update average response time
            if metrics.avg_response_time == 0.0 {
                metrics.avg_response_time = response_time_secs;
            } else {
                metrics.avg_response_time = metrics.avg_response_time * 0.7 + response_time_secs * 0.3;
            }
        } else if network_success && !user_success {
            metrics.network_success_count += 1;
            metrics.partial_success_count += 1;
            metrics.state = RS::PartiallyAccessible;
        } else {
            metrics.failure_count += 1;
            metrics.last_failure_time = Some(std::time::Instant::now());
            metrics.consecutive_failures += 1;

            if metrics.consecutive_failures > 3 {
                metrics.state = RS::Inaccessible;
            }
        }

        // Check for recovery
        if matches!(metrics.state, RS::Inaccessible) && user_success {
            metrics.recovery_count += 1;
            metrics.state = RS::Accessible;
            log::info!("Recovery detected: {} via {}", target, runway_id);
        }

        metrics.update_success_rate(self.success_rate_window);
        
        target_map.insert(runway_id.to_string(), metrics);
    }

    pub async fn get_accessible_runways(&self, target: &str) -> Vec<String> {
        if let Some(target_map) = self.metrics.get(target) {
            target_map
                .iter()
                .filter_map(|entry| {
                    let metrics = entry.value();
                    match metrics.state {
                        RS::Accessible => Some(entry.key().clone()),
                        RS::PartiallyAccessible => {
                            if metrics.success_rate >= self.success_rate_threshold {
                                Some(entry.key().clone())
                            } else {
                                None
                            }
                        }
                        _ => None,
                    }
                })
                .collect()
        } else {
            Vec::new()
        }
    }

    pub async fn get_metrics(&self, target: &str, runway_id: &str) -> Option<TargetMetrics> {
        self.metrics
            .get(target)?
            .get(runway_id)
            .map(|entry| entry.value().clone())
    }

    pub async fn get_all_targets(&self) -> Vec<String> {
        self.metrics.iter().map(|entry| entry.key().clone()).collect()
    }

    pub async fn get_target_metrics(&self, target: &str) -> HashMap<String, TargetMetrics> {
        if let Some(target_map) = self.metrics.get(target) {
            target_map
                .iter()
                .map(|entry| (entry.key().clone(), entry.value().clone()))
                .collect()
        } else {
            HashMap::new()
        }
    }
}
