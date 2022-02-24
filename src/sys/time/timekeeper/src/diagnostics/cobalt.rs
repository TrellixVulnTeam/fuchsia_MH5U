// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        diagnostics::{Diagnostics, Event},
        enums::{
            ClockCorrectionStrategy, ClockUpdateReason, InitialClockState, StartClockSource, Track,
        },
        MonitorTrack, PrimaryTrack, TimeSource,
    },
    fuchsia_async as fasync,
    fuchsia_cobalt::{CobaltConnector, CobaltSender, ConnectionType},
    fuchsia_zircon as zx,
    parking_lot::Mutex,
    std::sync::Arc,
    time_metrics_registry::{
        RealTimeClockEventsMetricDimensionEventType as RtcEvent,
        TimeMetricDimensionDirection as Direction, TimeMetricDimensionExperiment as Experiment,
        TimeMetricDimensionIteration as Iteration, TimeMetricDimensionRole as CobaltRole,
        TimeMetricDimensionTrack as CobaltTrack,
        TimekeeperLifecycleEventsMetricDimensionEventType as LifecycleEvent,
        TimekeeperTimeSourceEventsMetricDimensionEventType as TimeSourceEvent,
        TimekeeperTrackEventsMetricDimensionEventType as TrackEvent,
        REAL_TIME_CLOCK_EVENTS_METRIC_ID, TIMEKEEPER_CLOCK_CORRECTION_METRIC_ID,
        TIMEKEEPER_FREQUENCY_ABS_ESTIMATE_METRIC_ID, TIMEKEEPER_LIFECYCLE_EVENTS_METRIC_ID,
        TIMEKEEPER_MONITOR_DIFFERENCE_METRIC_ID, TIMEKEEPER_SQRT_COVARIANCE_METRIC_ID,
        TIMEKEEPER_TIME_SOURCE_EVENTS_METRIC_ID, TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
    },
    time_util::time_at_monotonic,
};

/// The period duration in micros. This field is required for Cobalt 1.0 EVENT_COUNT but not used.
const PERIOD_DURATION: i64 = 0;

/// The number of parts in a million.
const ONE_MILLION: i64 = 1_000_000;

/// A connection to the real Cobalt service.
pub struct CobaltDiagnostics {
    /// The wrapped CobaltSender used to log metrics.
    sender: Mutex<CobaltSender>,
    /// The experiment to record on all experiment-based events.
    experiment: Experiment,
    /// The UTC clock used in the primary track.
    primary_clock: Arc<zx::Clock>,
    /// The UTC clock used in the monitor track.
    monitor_clock: Option<Arc<zx::Clock>>,
    // TODO(fxbug.dev/57677): Move back to an owned fasync::Task instead of detaching the spawned
    // Task once the lifecycle of timekeeper ensures CobaltDiagnostics objects will last long enough
    // to finish their logging.
}

impl CobaltDiagnostics {
    /// Contructs a new `CobaltDiagnostics` instance.
    pub(crate) fn new<T: TimeSource>(
        experiment: Experiment,
        primary: &PrimaryTrack<T>,
        optional_monitor: &Option<MonitorTrack<T>>,
    ) -> Self {
        let (sender, fut) = CobaltConnector::default()
            .serve(ConnectionType::project_id(time_metrics_registry::PROJECT_ID));
        fasync::Task::spawn(fut).detach();
        Self {
            sender: Mutex::new(sender),
            experiment,
            primary_clock: Arc::clone(&primary.clock),
            monitor_clock: optional_monitor.as_ref().map(|track| Arc::clone(&track.clock)),
        }
    }

    /// Records an update to the Kalman filter state, including an event and a covariance report.
    fn record_kalman_filter_update(&self, track: Track, sqrt_covariance: zx::Duration) {
        let mut locked_sender = self.sender.lock();
        let cobalt_track = Into::<CobaltTrack>::into(track);
        locked_sender.log_event_count(
            TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
            (TrackEvent::EstimatedOffsetUpdated, cobalt_track, self.experiment),
            PERIOD_DURATION,
            1,
        );
        locked_sender.log_event_count(
            TIMEKEEPER_SQRT_COVARIANCE_METRIC_ID,
            (Into::<CobaltTrack>::into(track), self.experiment),
            PERIOD_DURATION,
            // Unfortunately Cobalt does not follow the standard of nanoseconds everywhere.
            sqrt_covariance.into_micros(),
        );
    }

    /// Records an update to the estimated frequency.
    fn record_frequency_update(&self, track: Track, rate_adjust_ppm: i32, window_count: u32) {
        let iteration = match window_count {
            1 => Iteration::First,
            2 => Iteration::Second,
            _ => Iteration::Subsequent,
        };
        // Frequency arrives as a deviation in ppm from a 1Hz clock which can be positive or
        // negative, but to keep the events we report to cobalt positive we output an absolute
        // utc parts per million monotonic parts.
        self.sender.lock().log_event_count(
            TIMEKEEPER_FREQUENCY_ABS_ESTIMATE_METRIC_ID,
            (iteration, Into::<CobaltTrack>::into(track), self.experiment),
            PERIOD_DURATION,
            rate_adjust_ppm as i64 + ONE_MILLION,
        );
    }

    /// Records a clock correction, including an event and a report on the magnitude of change.
    fn record_clock_correction(
        &self,
        track: Track,
        correction: zx::Duration,
        strategy: ClockCorrectionStrategy,
    ) {
        let mut locked_sender = self.sender.lock();
        let cobalt_track = Into::<CobaltTrack>::into(track);
        let direction =
            if correction.into_nanos() >= 0 { Direction::Positive } else { Direction::Negative };
        locked_sender.log_event_count(
            TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
            (Into::<TrackEvent>::into(strategy), cobalt_track, self.experiment),
            PERIOD_DURATION,
            1,
        );
        locked_sender.log_event_count(
            TIMEKEEPER_CLOCK_CORRECTION_METRIC_ID,
            (direction, cobalt_track, self.experiment),
            PERIOD_DURATION,
            // Unfortunately Cobalt does not follow the standard of nanoseconds everywhere.
            correction.into_micros().abs(),
        );
    }

    /// Records relevant information following a clock update.
    ///
    /// All updates record the reason, an update to the monitor track additionally records the
    /// difference between the monitor and primary clocks.
    fn record_clock_update(&self, track: Track, reason: ClockUpdateReason) {
        let mut locked_sender = self.sender.lock();
        locked_sender.log_event_count(
            TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
            (Into::<TrackEvent>::into(reason), Into::<CobaltTrack>::into(track), self.experiment),
            PERIOD_DURATION,
            1,
        );
        if track == Track::Monitor {
            if let Some(monitor_clock) = self.monitor_clock.as_ref() {
                let monotonic_ref = zx::Time::get_monotonic();
                let primary = time_at_monotonic(&self.primary_clock, monotonic_ref);
                let monitor = time_at_monotonic(monitor_clock, monotonic_ref);
                let direction =
                    if monitor >= primary { Direction::Positive } else { Direction::Negative };
                locked_sender.log_event_count(
                    TIMEKEEPER_MONITOR_DIFFERENCE_METRIC_ID,
                    (direction, self.experiment),
                    PERIOD_DURATION,
                    // Unfortunately Cobalt does not follow the standard of nanoseconds everywhere.
                    (monitor - primary).into_micros().abs(),
                );
            }
        }
    }
}

impl Diagnostics for CobaltDiagnostics {
    fn record(&self, event: Event) {
        match event {
            Event::Initialized { clock_state } => {
                let event = match clock_state {
                    InitialClockState::NotSet => LifecycleEvent::InitializedBeforeUtcStart,
                    InitialClockState::PreviouslySet => LifecycleEvent::InitializedAfterUtcStart,
                };
                self.sender.lock().log_event(TIMEKEEPER_LIFECYCLE_EVENTS_METRIC_ID, event);
            }
            Event::InitializeRtc { outcome, .. } => {
                self.sender
                    .lock()
                    .log_event(REAL_TIME_CLOCK_EVENTS_METRIC_ID, Into::<RtcEvent>::into(outcome));
            }
            Event::TimeSourceFailed { role, error } => {
                let event = Into::<TimeSourceEvent>::into(error);
                self.sender.lock().log_event_count(
                    TIMEKEEPER_TIME_SOURCE_EVENTS_METRIC_ID,
                    (event, Into::<CobaltRole>::into(role), self.experiment),
                    PERIOD_DURATION,
                    1,
                );
            }
            Event::TimeSourceStatus { .. } => {}
            Event::SampleRejected { role, error } => {
                let event = Into::<TimeSourceEvent>::into(error);
                self.sender.lock().log_event_count(
                    TIMEKEEPER_TIME_SOURCE_EVENTS_METRIC_ID,
                    (event, Into::<CobaltRole>::into(role), self.experiment),
                    PERIOD_DURATION,
                    1,
                );
            }
            Event::FrequencyWindowDiscarded { track, reason } => {
                if let Some(event) = Into::<Option<TrackEvent>>::into(reason) {
                    self.sender.lock().log_event_count(
                        TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                        (event, Into::<CobaltTrack>::into(track), self.experiment),
                        PERIOD_DURATION,
                        1,
                    );
                }
            }
            Event::KalmanFilterUpdated { track, sqrt_covariance, .. } => {
                self.record_kalman_filter_update(track, sqrt_covariance);
            }
            Event::FrequencyUpdated { track, rate_adjust_ppm, window_count, .. } => {
                self.record_frequency_update(track, rate_adjust_ppm, window_count);
            }
            Event::ClockCorrection { track, correction, strategy } => {
                self.record_clock_correction(track, correction, strategy);
            }
            Event::WriteRtc { outcome } => {
                self.sender
                    .lock()
                    .log_event(REAL_TIME_CLOCK_EVENTS_METRIC_ID, Into::<RtcEvent>::into(outcome));
            }
            Event::StartClock { track, source } => {
                if track == Track::Primary {
                    let event = match source {
                        StartClockSource::Rtc => LifecycleEvent::StartedUtcFromRtc,
                        StartClockSource::External(_) => LifecycleEvent::StartedUtcFromTimeSource,
                    };
                    self.sender.lock().log_event(TIMEKEEPER_LIFECYCLE_EVENTS_METRIC_ID, event);
                }
            }
            Event::UpdateClock { track, reason } => {
                self.record_clock_update(track, reason);
            }
        }
    }
}

#[cfg(test)]
mod test {
    use {
        super::*,
        crate::enums::{
            ClockUpdateReason, FrequencyDiscardReason, InitializeRtcOutcome, Role,
            SampleValidationError, TimeSourceError, WriteRtcOutcome,
        },
        fidl_fuchsia_cobalt::{CobaltEvent, CountEvent, Event as EmptyEvent, EventPayload},
        futures::{channel::mpsc, FutureExt, StreamExt},
        test_util::{assert_geq, assert_leq},
    };

    const TEST_EXPERIMENT: Experiment = Experiment::B;
    const MONITOR_OFFSET: zx::Duration = zx::Duration::from_seconds(444);

    fn create_clock(time: zx::Time) -> Arc<zx::Clock> {
        let clk = zx::Clock::create(zx::ClockOpts::empty(), None).unwrap();
        clk.update(zx::ClockUpdate::builder().approximate_value(time)).unwrap();
        Arc::new(clk)
    }

    /// Creates a test CobaltDiagnostics and an mpsc receiver that may be used to verify the
    /// events it sends. The primary and monitor clocks will have a difference of MONITOR_OFFSET.
    fn create_test_object() -> (CobaltDiagnostics, mpsc::Receiver<CobaltEvent>) {
        let (mpsc_sender, mpsc_receiver) = futures::channel::mpsc::channel(1);
        let sender = CobaltSender::new(mpsc_sender);
        let diagnostics = CobaltDiagnostics {
            sender: Mutex::new(sender),
            experiment: TEST_EXPERIMENT,
            primary_clock: create_clock(zx::Time::ZERO),
            monitor_clock: Some(create_clock(zx::Time::ZERO + MONITOR_OFFSET)),
        };
        (diagnostics, mpsc_receiver)
    }

    /// Creates an `EventPayload` containing the supplied count.
    fn event_count_payload(count: i64) -> EventPayload {
        EventPayload::EventCount(CountEvent { period_duration_micros: 0, count })
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_initialization_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        diagnostics.record(Event::Initialized { clock_state: InitialClockState::NotSet });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_LIFECYCLE_EVENTS_METRIC_ID,
                event_codes: vec![LifecycleEvent::InitializedBeforeUtcStart as u32],
                component: None,
                payload: EventPayload::Event(EmptyEvent),
            })
        );
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_clock_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        diagnostics.record(Event::StartClock {
            track: Track::Monitor,
            source: StartClockSource::External(Role::Monitor),
        });
        diagnostics.record(Event::StartClock {
            track: Track::Primary,
            source: StartClockSource::External(Role::Primary),
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_LIFECYCLE_EVENTS_METRIC_ID,
                event_codes: vec![LifecycleEvent::StartedUtcFromTimeSource as u32],
                component: None,
                payload: EventPayload::Event(EmptyEvent),
            })
        );
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_rtc_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        diagnostics
            .record(Event::InitializeRtc { outcome: InitializeRtcOutcome::Succeeded, time: None });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::REAL_TIME_CLOCK_EVENTS_METRIC_ID,
                event_codes: vec![RtcEvent::ReadSucceeded as u32],
                component: None,
                payload: EventPayload::Event(EmptyEvent),
            })
        );

        diagnostics.record(Event::WriteRtc { outcome: WriteRtcOutcome::Failed });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::REAL_TIME_CLOCK_EVENTS_METRIC_ID,
                event_codes: vec![RtcEvent::WriteFailed as u32],
                component: None,
                payload: EventPayload::Event(EmptyEvent),
            })
        );
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_time_source_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        diagnostics.record(Event::SampleRejected {
            role: Role::Primary,
            error: SampleValidationError::MonotonicTooOld,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TIME_SOURCE_EVENTS_METRIC_ID,
                event_codes: vec![
                    TimeSourceEvent::SampleRejectedMonotonicTooOld as u32,
                    CobaltRole::Primary as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );

        diagnostics.record(Event::TimeSourceFailed {
            role: Role::Monitor,
            error: TimeSourceError::CallFailed,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TIME_SOURCE_EVENTS_METRIC_ID,
                event_codes: vec![
                    TimeSourceEvent::RestartedCallFailed as u32,
                    CobaltRole::Monitor as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_time_track_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        diagnostics.record(Event::FrequencyWindowDiscarded {
            track: Track::Primary,
            reason: FrequencyDiscardReason::InsufficientSamples,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                event_codes: vec![
                    TrackEvent::FrequencyWindowDiscardedSampleCount as u32,
                    CobaltTrack::Primary as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );

        diagnostics.record(Event::KalmanFilterUpdated {
            track: Track::Primary,
            monotonic: zx::Time::from_nanos(333_000_000_000),
            utc: zx::Time::from_nanos(4455445544_000_000_000),
            sqrt_covariance: zx::Duration::from_micros(55555),
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                event_codes: vec![
                    TrackEvent::EstimatedOffsetUpdated as u32,
                    CobaltTrack::Primary as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_SQRT_COVARIANCE_METRIC_ID,
                event_codes: vec![CobaltTrack::Primary as u32, TEST_EXPERIMENT as u32],
                component: None,
                payload: event_count_payload(55555),
            })
        );

        diagnostics.record(Event::FrequencyUpdated {
            track: Track::Primary,
            monotonic: zx::Time::from_nanos(888_000_000_000),
            rate_adjust_ppm: -4,
            window_count: 7,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_FREQUENCY_ABS_ESTIMATE_METRIC_ID,
                event_codes: vec![
                    Iteration::Subsequent as u32,
                    CobaltTrack::Primary as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1_000_000 - 4),
            })
        );

        diagnostics.record(Event::ClockCorrection {
            track: Track::Monitor,
            correction: zx::Duration::from_micros(-777),
            strategy: ClockCorrectionStrategy::NominalRateSlew,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                event_codes: vec![
                    TrackEvent::CorrectionByNominalRateSlew as u32,
                    CobaltTrack::Monitor as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_CLOCK_CORRECTION_METRIC_ID,
                event_codes: vec![
                    Direction::Negative as u32,
                    CobaltTrack::Monitor as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(777),
            })
        );
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn record_update_clock_events() {
        let (diagnostics, mut mpsc_receiver) = create_test_object();

        // Updates to the primary track should only log the reason.
        diagnostics.record(Event::UpdateClock {
            track: Track::Primary,
            reason: ClockUpdateReason::TimeStep,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                event_codes: vec![
                    TrackEvent::ClockUpdateTimeStep as u32,
                    CobaltTrack::Primary as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );
        assert!(mpsc_receiver.next().now_or_never().is_none());

        // Updates to the monitor track should lead to an update event and a difference report.
        // Unfortunately the latter is cumbersome to verify since we don't know the exact clock
        // difference.
        diagnostics.record(Event::UpdateClock {
            track: Track::Monitor,
            reason: ClockUpdateReason::BeginSlew,
        });
        assert_eq!(
            mpsc_receiver.next().await,
            Some(CobaltEvent {
                metric_id: time_metrics_registry::TIMEKEEPER_TRACK_EVENTS_METRIC_ID,
                event_codes: vec![
                    TrackEvent::ClockUpdateBeginSlew as u32,
                    CobaltTrack::Monitor as u32,
                    TEST_EXPERIMENT as u32
                ],
                component: None,
                payload: event_count_payload(1),
            })
        );
        let event = mpsc_receiver.next().await.unwrap();
        assert_eq!(event.metric_id, TIMEKEEPER_MONITOR_DIFFERENCE_METRIC_ID);
        assert_eq!(event.event_codes, vec![Direction::Positive as u32, TEST_EXPERIMENT as u32]);
        match event.payload {
            EventPayload::EventCount(CountEvent { period_duration_micros, count }) => {
                assert_eq!(period_duration_micros, 0);
                assert_geq!(count, MONITOR_OFFSET.into_micros() - 5000);
                assert_leq!(count, MONITOR_OFFSET.into_micros() + 5000);
            }
            _ => panic!("monitor clock update did not produce event count payload"),
        }
    }
}
