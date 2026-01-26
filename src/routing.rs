use std::collections::HashMap;
use std::sync::Arc;
use parking_lot::RwLock;

use crate::runway::Runway;
use crate::tracker::{TargetAccessibilityTracker, TargetMetrics};

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum RoutingMode {
    Latency,
    FirstAccessible,
    RoundRobin,
}

pub struct RoutingEngine {
    tracker: Arc<TargetAccessibilityTracker>,
    mode: Arc<RwLock<RoutingMode>>,
    round_robin_index: Arc<RwLock<HashMap<String, usize>>>,
}

impl RoutingEngine {
    pub fn new(tracker: Arc<TargetAccessibilityTracker>, mode: RoutingMode) -> Self {
        Self {
            tracker,
            mode: Arc::new(RwLock::new(mode)),
            round_robin_index: Arc::new(RwLock::new(HashMap::new())),
        }
    }

    pub fn set_mode(&self, mode: RoutingMode) {
        *self.mode.write() = mode;
        log::info!("Routing mode changed to {:?}", mode);
    }

    pub fn mode(&self) -> Arc<RwLock<RoutingMode>> {
        Arc::clone(&self.mode)
    }

    pub async fn select_runway(
        &self,
        target: &str,
        runways: &[Runway],
    ) -> Option<Runway> {
        let accessible_ids = self.tracker.get_accessible_runways(target).await;
        
        if accessible_ids.is_empty() {
            return None;
        }

        let accessible_runways: Vec<&Runway> = runways
            .iter()
            .filter(|r| accessible_ids.contains(&r.id))
            .collect();

        if accessible_runways.is_empty() {
            return None;
        }

        let mode = *self.mode.read();
        match mode {
            RoutingMode::Latency => self.select_by_latency(target, &accessible_runways).await,
            RoutingMode::FirstAccessible => Some(accessible_runways[0].clone()),
            RoutingMode::RoundRobin => self.select_round_robin(target, &accessible_runways).await,
        }
    }

    async fn select_by_latency(&self, target: &str, runways: &[&Runway]) -> Option<Runway> {
        let mut best: Option<(&Runway, f64)> = None;

        for runway in runways {
            if let Some(metrics) = self.tracker.get_metrics(target, &runway.id).await {
                if metrics.avg_response_time > 0.0 {
                    let is_better = best
                        .as_ref()
                        .map(|(_, time)| metrics.avg_response_time < *time)
                        .unwrap_or(true);
                    
                    if is_better {
                        best = Some((runway, metrics.avg_response_time));
                    }
                }
            }
        }

        best.map(|(r, _)| r.clone())
            .or_else(|| runways.first().map(|r| (*r).clone()))
    }

    async fn select_round_robin(&self, target: &str, runways: &[&Runway]) -> Option<Runway> {
        if runways.is_empty() {
            return None;
        }

        let mut index_map = self.round_robin_index.write();
        let index = index_map.entry(target.to_string()).or_insert(0);
        let selected = runways[*index % runways.len()].clone();
        *index = (*index + 1) % runways.len();
        Some(selected)
    }
}
