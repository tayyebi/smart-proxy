use std::sync::Arc;
use std::time::Duration;
use tokio::time;

use crate::runway_manager::RunwayManager;
use crate::tracker::{TargetAccessibilityTracker, RunwayState};
use crate::runway::RunwayState as RS;

pub struct HealthMonitor {
    runway_manager: Arc<RunwayManager>,
    tracker: Arc<TargetAccessibilityTracker>,
    interval: Duration,
    running: Arc<std::sync::atomic::AtomicBool>,
}

impl HealthMonitor {
    pub fn new(
        runway_manager: Arc<RunwayManager>,
        tracker: Arc<TargetAccessibilityTracker>,
        interval_secs: u64,
    ) -> Self {
        Self {
            runway_manager,
            tracker,
            interval: Duration::from_secs(interval_secs),
            running: Arc::new(std::sync::atomic::AtomicBool::new(false)),
        }
    }

    pub async fn start(&self) {
        self.running.store(true, std::sync::atomic::Ordering::Relaxed);
        log::info!("Health monitor started (interval: {:?})", self.interval);

        while self.running.load(std::sync::atomic::Ordering::Relaxed) {
            if let Err(e) = self.health_check_cycle().await {
                log::error!("Error in health check cycle: {}", e);
            }
            time::sleep(self.interval).await;
        }
    }

    async fn health_check_cycle(&self) -> Result<(), Box<dyn std::error::Error>> {
        // Refresh interface information
        self.runway_manager.refresh_interfaces().await;

        // Get all known targets
        let targets = self.tracker.get_all_targets().await;
        if targets.is_empty() {
            return Ok(());
        }

        // Test runways for each target (limit to avoid overload)
        let max_targets_per_cycle = 10;
        let targets_to_check: Vec<String> = targets.into_iter().take(max_targets_per_cycle).collect();

        for target in targets_to_check {
            let metrics = self.tracker.get_target_metrics(&target).await;

            // Prioritize recently failed runways
            let failed_runways: Vec<String> = metrics
                .iter()
                .filter_map(|(rw_id, m)| {
                    if matches!(m.state, RS::Inaccessible) {
                        Some(rw_id.clone())
                    } else {
                        None
                    }
                })
                .take(5)
                .collect();

            let timeout = Duration::from_secs(5);
            for runway_id in failed_runways {
                if let Some(runway) = self.runway_manager.get_runway(&runway_id).await {
                    match self
                        .runway_manager
                        .test_runway_accessibility(&target, &runway, timeout)
                        .await
                    {
                        (net_success, user_success, response_time) => {
                            self.tracker
                                .update(&target, &runway_id, net_success, user_success, response_time)
                                .await;
                        }
                    }
                }
            }

            // Also test partially accessible runways
            let partial_runways: Vec<String> = metrics
                .iter()
                .filter_map(|(rw_id, m)| {
                    if matches!(m.state, RS::PartiallyAccessible) {
                        Some(rw_id.clone())
                    } else {
                        None
                    }
                })
                .take(3)
                .collect();

            for runway_id in partial_runways {
                if let Some(runway) = self.runway_manager.get_runway(&runway_id).await {
                    match self
                        .runway_manager
                        .test_runway_accessibility(&target, &runway, timeout)
                        .await
                    {
                        (net_success, user_success, response_time) => {
                            self.tracker
                                .update(&target, &runway_id, net_success, user_success, response_time)
                                .await;
                        }
                    }
                }
            }
        }

        Ok(())
    }

    pub fn stop(&self) {
        self.running.store(false, std::sync::atomic::Ordering::Relaxed);
        log::info!("Health monitor stopped");
    }
}
