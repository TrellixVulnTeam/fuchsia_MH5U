// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(missing_docs)]

//! `timekeeper` is responsible for external time synchronization in Fuchsia.

mod clock_manager;
mod diagnostics;
mod enums;
mod estimator;
mod rtc;
mod time_source;
mod time_source_manager;

use {
    crate::{
        clock_manager::ClockManager,
        diagnostics::{
            CobaltDiagnostics, CompositeDiagnostics, Diagnostics, Event, InspectDiagnostics,
        },
        enums::{InitialClockState, InitializeRtcOutcome, Role, StartClockSource, Track},
        rtc::{Rtc, RtcCreationError, RtcImpl},
        time_source::{PushTimeSource, TimeSource},
        time_source_manager::TimeSourceManager,
    },
    anyhow::{Context as _, Error},
    chrono::prelude::*,
    fidl_fuchsia_time as ftime, fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_zircon as zx,
    futures::{
        future::{self, OptionFuture},
        stream::StreamExt as _,
    },
    log::{error, info, warn},
    std::sync::Arc,
    time_metrics_registry::TimeMetricDimensionExperiment,
};

/// A definition which time sources to install, along with the URL for each.
struct TimeSourceUrls {
    primary: &'static str,
    monitor: Option<&'static str>,
}

/// The time sources to install in the default (i.e. non-test) case. In the future, these values
/// belong in a config file.
const DEFAULT_SOURCES: TimeSourceUrls = TimeSourceUrls {
    primary: "fuchsia-pkg://fuchsia.com/httpsdate-time-source#meta/httpsdate_time_source.cmx",
    monitor: None,
};

/// The time sources to install when the dev_time_sources flag is set. In the future, these values
/// belong in a config file.
const DEV_TEST_SOURCES: TimeSourceUrls = TimeSourceUrls {
    primary: "fuchsia-pkg://fuchsia.com/timekeeper-integration#meta/dev_time_source.cmx",
    monitor: None,
};

/// The experiment to record on Cobalt events in the non-test case.
const COBALT_EXPERIMENT: TimeMetricDimensionExperiment = TimeMetricDimensionExperiment::None;
/// The experiment to record on Cobalt events to install when the dev_time_sources flag is set.
const DEV_COBALT_EXPERIMENT: TimeMetricDimensionExperiment = TimeMetricDimensionExperiment::None;

/// The information required to maintain UTC for the primary track.
struct PrimaryTrack<T: TimeSource> {
    time_source: T,
    clock: Arc<zx::Clock>,
}

/// The information required to maintain UTC for the monitor track.
struct MonitorTrack<T: TimeSource> {
    time_source: T,
    clock: Arc<zx::Clock>,
}

/// Command line arguments supplied to Timekeeper.
#[derive(argh::FromArgs)]
struct Options {
    /// flag indicating to use the dev time sources.
    #[argh(switch)]
    dev_time_sources: bool,
    /// flag disabling delays, allowing a test to push samples frequently.
    #[argh(switch)]
    disable_delays: bool,
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["time"]).context("initializing logging").unwrap();
    fuchsia_syslog::set_severity(fuchsia_syslog::levels::INFO);
    let options = argh::from_env::<Options>();

    info!("retrieving UTC clock handle");
    let time_maintainer =
        fuchsia_component::client::connect_to_protocol::<ftime::MaintenanceMarker>().unwrap();
    let utc_clock = zx::Clock::from(
        time_maintainer
            .get_writable_utc_clock()
            .await
            .context("failed to get UTC clock from maintainer")?,
    );

    info!("constructing time sources");
    let time_source_urls = match options.dev_time_sources {
        true => DEV_TEST_SOURCES,
        false => DEFAULT_SOURCES,
    };
    let primary_track = PrimaryTrack {
        time_source: PushTimeSource::new(time_source_urls.primary.to_string()),
        clock: Arc::new(utc_clock),
    };
    let monitor_track = time_source_urls.monitor.map(|url| MonitorTrack {
        time_source: PushTimeSource::new(url.to_string()),
        clock: Arc::new(create_monitor_clock(&primary_track.clock)),
    });

    info!("initializing diagnostics and serving inspect on servicefs");
    let cobalt_experiment = match options.dev_time_sources {
        true => DEV_COBALT_EXPERIMENT,
        false => COBALT_EXPERIMENT,
    };
    let diagnostics = Arc::new(CompositeDiagnostics::new(
        InspectDiagnostics::new(diagnostics::INSPECTOR.root(), &primary_track, &monitor_track),
        CobaltDiagnostics::new(cobalt_experiment, &primary_track, &monitor_track),
    ));
    let mut fs = ServiceFs::new();
    inspect_runtime::serve(&diagnostics::INSPECTOR, &mut fs)?;

    info!("connecting to real time clock");
    let optional_rtc = match RtcImpl::only_device() {
        Ok(rtc) => Some(rtc),
        Err(err) => {
            match err {
                RtcCreationError::NoDevices => info!("no RTC devices found."),
                _ => warn!("failed to connect to RTC: {}", err),
            };
            diagnostics.record(Event::InitializeRtc { outcome: err.into(), time: None });
            None
        }
    };

    fasync::Task::spawn(async move {
        maintain_utc(
            primary_track,
            monitor_track,
            optional_rtc,
            diagnostics,
            options.disable_delays,
        )
        .await;
    })
    .detach();

    fs.take_and_serve_directory_handle()?;
    Ok(fs.collect().await)
}

/// Creates a new userspace clock for use in the monitor track, set to the same backstop time as
/// the supplied primary clock.
fn create_monitor_clock(primary_clock: &zx::Clock) -> zx::Clock {
    // Note: Failure should not be possible from a valid zx::Clock.
    let backstop = primary_clock.get_details().expect("failed to get UTC clock details").backstop;
    // Note: Only failure mode is an OOM which we handle via panic.
    zx::Clock::create(zx::ClockOpts::empty(), Some(backstop))
        .expect("failed to create new monitor clock")
}

/// Determines whether the supplied clock has previously been set.
fn initial_clock_state(utc_clock: &zx::Clock) -> InitialClockState {
    // Note: Failure should not be possible from a valid zx::Clock.
    let clock_details = utc_clock.get_details().expect("failed to get UTC clock details");
    // When the clock is first initialized to the backstop time, its synthetic offset should
    // be identical. Once the clock is updated, this is no longer true.
    if clock_details.backstop.into_nanos() == clock_details.ticks_to_synthetic.synthetic_offset {
        InitialClockState::NotSet
    } else {
        InitialClockState::PreviouslySet
    }
}

/// Attempts to initialize a userspace clock from the current value of the real time clock.
/// sending progress to diagnostics as appropriate.
async fn set_clock_from_rtc<R: Rtc, D: Diagnostics>(
    rtc: &R,
    clock: &zx::Clock,
    diagnostics: Arc<D>,
) {
    info!("reading initial RTC time.");
    let mono_before = zx::Time::get_monotonic();
    let rtc_time = match rtc.get().await {
        Err(err) => {
            error!("failed to read RTC time: {}", err);
            diagnostics.record(Event::InitializeRtc {
                outcome: InitializeRtcOutcome::ReadFailed,
                time: None,
            });
            return;
        }
        Ok(time) => time,
    };
    let mono_after = zx::Time::get_monotonic();
    let mono_time = mono_before + (mono_after - mono_before) / 2;

    let rtc_chrono = Utc.timestamp_nanos(rtc_time.into_nanos());
    let backstop = clock.get_details().expect("failed to get UTC clock details").backstop;
    if rtc_time < backstop {
        warn!("initial RTC time before backstop: {}", rtc_chrono);
        diagnostics.record(Event::InitializeRtc {
            outcome: InitializeRtcOutcome::InvalidBeforeBackstop,
            time: Some(rtc_time),
        });
        return;
    }

    diagnostics.record(Event::InitializeRtc {
        outcome: InitializeRtcOutcome::Succeeded,
        time: Some(rtc_time),
    });
    if let Err(status) =
        clock.update(zx::ClockUpdate::builder().absolute_value(mono_time, rtc_time))
    {
        error!("failed to start UTC clock from RTC at time {}: {}", rtc_chrono, status);
    } else {
        diagnostics
            .record(Event::StartClock { track: Track::Primary, source: StartClockSource::Rtc });
        info!("started UTC clock from RTC at time: {}", rtc_chrono);
    }
}

/// The top-level control loop for time synchronization.
///
/// Maintains the utc clock using updates received over the `fuchsia.time.external` protocols.
async fn maintain_utc<R: 'static, T: 'static, D: 'static>(
    mut primary: PrimaryTrack<T>,
    optional_monitor: Option<MonitorTrack<T>>,
    optional_rtc: Option<R>,
    diagnostics: Arc<D>,
    disable_delays: bool,
) where
    R: Rtc,
    T: TimeSource,
    D: Diagnostics,
{
    info!("record the state at initialization.");
    let initial_clock_state = initial_clock_state(&primary.clock);
    diagnostics.record(Event::Initialized { clock_state: initial_clock_state });

    if let Some(rtc) = optional_rtc.as_ref() {
        match initial_clock_state {
            InitialClockState::NotSet => {
                set_clock_from_rtc(rtc, &mut primary.clock, Arc::clone(&diagnostics)).await;
            }
            InitialClockState::PreviouslySet => {
                diagnostics.record(Event::InitializeRtc {
                    outcome: InitializeRtcOutcome::ReadNotAttempted,
                    time: None,
                });
            }
        }
    }

    info!("launching time source managers...");
    let time_source_fn = match disable_delays {
        true => TimeSourceManager::new_with_delays_disabled,
        false => TimeSourceManager::new,
    };
    let backstop = primary.clock.get_details().expect("failed to get UTC clock details").backstop;
    let primary_source_manager =
        time_source_fn(backstop, Role::Primary, primary.time_source, Arc::clone(&diagnostics));
    let monitor_source_manager_and_clock = optional_monitor.map(|monitor| {
        let source_manager =
            time_source_fn(backstop, Role::Monitor, monitor.time_source, Arc::clone(&diagnostics));
        (source_manager, monitor.clock)
    });

    info!("launching clock managers...");
    let fut1 = ClockManager::execute(
        primary.clock,
        primary_source_manager,
        optional_rtc,
        Arc::clone(&diagnostics),
        Track::Primary,
    );
    let fut2: OptionFuture<_> = monitor_source_manager_and_clock
        .map(|(source_manager, clock)| {
            ClockManager::<T, R, D>::execute(
                clock,
                source_manager,
                None,
                diagnostics,
                Track::Monitor,
            )
        })
        .into();
    future::join(fut1, fut2).await;
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            diagnostics::FakeDiagnostics,
            enums::{InitialClockState, InitializeRtcOutcome, WriteRtcOutcome},
            rtc::FakeRtc,
            time_source::{Event as TimeSourceEvent, FakeTimeSource, Sample},
        },
        fidl_fuchsia_time_external as ftexternal, fuchsia_zircon as zx,
        futures::FutureExt,
        lazy_static::lazy_static,
    };

    const NANOS_PER_SECOND: i64 = 1_000_000_000;
    const OFFSET: zx::Duration = zx::Duration::from_seconds(1111_000);
    const OFFSET_2: zx::Duration = zx::Duration::from_seconds(1111_333);
    const STD_DEV: zx::Duration = zx::Duration::from_millis(44);
    const INVALID_RTC_TIME: zx::Time = zx::Time::from_nanos(111111 * NANOS_PER_SECOND);
    const BACKSTOP_TIME: zx::Time = zx::Time::from_nanos(222222 * NANOS_PER_SECOND);
    const VALID_RTC_TIME: zx::Time = zx::Time::from_nanos(333333 * NANOS_PER_SECOND);

    lazy_static! {
        static ref CLOCK_OPTS: zx::ClockOpts = zx::ClockOpts::empty();
    }

    /// Creates and starts a new clock with default options, returning a tuple of the clock and its
    /// initial update time in ticks.
    fn create_clock() -> (Arc<zx::Clock>, i64) {
        let clock = zx::Clock::create(*CLOCK_OPTS, Some(BACKSTOP_TIME)).unwrap();
        clock.update(zx::ClockUpdate::builder().approximate_value(BACKSTOP_TIME)).unwrap();
        let initial_update_ticks = clock.get_details().unwrap().last_value_update_ticks;
        (Arc::new(clock), initial_update_ticks)
    }

    #[fuchsia::test]
    fn successful_update_with_monitor() {
        let mut executor = fasync::TestExecutor::new().unwrap();
        let (primary_clock, primary_ticks) = create_clock();
        let (monitor_clock, monitor_ticks) = create_clock();
        let rtc = FakeRtc::valid(INVALID_RTC_TIME);
        let diagnostics = Arc::new(FakeDiagnostics::new());

        let monotonic_ref = zx::Time::get_monotonic();

        // Maintain UTC until no more work remains
        let mut fut = maintain_utc(
            PrimaryTrack {
                clock: Arc::clone(&primary_clock),
                time_source: FakeTimeSource::events(vec![
                    TimeSourceEvent::StatusChange { status: ftexternal::Status::Ok },
                    TimeSourceEvent::from(Sample::new(
                        monotonic_ref + OFFSET,
                        monotonic_ref,
                        STD_DEV,
                    )),
                ]),
            },
            Some(MonitorTrack {
                clock: Arc::clone(&monitor_clock),
                time_source: FakeTimeSource::events(vec![
                    TimeSourceEvent::StatusChange { status: ftexternal::Status::Network },
                    TimeSourceEvent::StatusChange { status: ftexternal::Status::Ok },
                    TimeSourceEvent::from(Sample::new(
                        monotonic_ref + OFFSET_2,
                        monotonic_ref,
                        STD_DEV,
                    )),
                ]),
            }),
            Some(rtc.clone()),
            Arc::clone(&diagnostics),
            false,
        )
        .boxed();
        let _ = executor.run_until_stalled(&mut fut);

        // Check that the clocks are set.
        assert!(primary_clock.get_details().unwrap().last_value_update_ticks > primary_ticks);
        assert!(monitor_clock.get_details().unwrap().last_value_update_ticks > monitor_ticks);
        assert!(rtc.last_set().is_some());

        // Check that the correct diagnostic events were logged.
        diagnostics.assert_events(&[
            Event::Initialized { clock_state: InitialClockState::NotSet },
            Event::InitializeRtc {
                outcome: InitializeRtcOutcome::InvalidBeforeBackstop,
                time: Some(INVALID_RTC_TIME),
            },
            Event::TimeSourceStatus { role: Role::Primary, status: ftexternal::Status::Ok },
            Event::KalmanFilterUpdated {
                track: Track::Primary,
                monotonic: monotonic_ref,
                utc: monotonic_ref + OFFSET,
                sqrt_covariance: STD_DEV,
            },
            Event::StartClock {
                track: Track::Primary,
                source: StartClockSource::External(Role::Primary),
            },
            Event::WriteRtc { outcome: WriteRtcOutcome::Succeeded },
            Event::TimeSourceStatus { role: Role::Monitor, status: ftexternal::Status::Network },
            Event::TimeSourceStatus { role: Role::Monitor, status: ftexternal::Status::Ok },
            Event::KalmanFilterUpdated {
                track: Track::Monitor,
                monotonic: monotonic_ref,
                utc: monotonic_ref + OFFSET_2,
                sqrt_covariance: STD_DEV,
            },
            Event::StartClock {
                track: Track::Monitor,
                source: StartClockSource::External(Role::Monitor),
            },
        ]);
    }

    #[fuchsia::test]
    fn no_update_invalid_rtc() {
        let mut executor = fasync::TestExecutor::new().unwrap();
        let (clock, initial_update_ticks) = create_clock();
        let rtc = FakeRtc::valid(INVALID_RTC_TIME);
        let diagnostics = Arc::new(FakeDiagnostics::new());

        let time_source = FakeTimeSource::events(vec![TimeSourceEvent::StatusChange {
            status: ftexternal::Status::Network,
        }]);

        // Maintain UTC until no more work remains
        let mut fut = maintain_utc(
            PrimaryTrack { clock: Arc::clone(&clock), time_source },
            None,
            Some(rtc.clone()),
            Arc::clone(&diagnostics),
            false,
        )
        .boxed();
        let _ = executor.run_until_stalled(&mut fut);

        // Checking that the clock has not been updated yet
        assert_eq!(initial_update_ticks, clock.get_details().unwrap().last_value_update_ticks);
        assert_eq!(rtc.last_set(), None);

        // Checking that the correct diagnostic events were logged.
        diagnostics.assert_events(&[
            Event::Initialized { clock_state: InitialClockState::NotSet },
            Event::InitializeRtc {
                outcome: InitializeRtcOutcome::InvalidBeforeBackstop,
                time: Some(INVALID_RTC_TIME),
            },
            Event::TimeSourceStatus { role: Role::Primary, status: ftexternal::Status::Network },
        ]);
    }

    #[fuchsia::test]
    fn no_update_valid_rtc() {
        let mut executor = fasync::TestExecutor::new().unwrap();
        let (clock, initial_update_ticks) = create_clock();
        let rtc = FakeRtc::valid(VALID_RTC_TIME);
        let diagnostics = Arc::new(FakeDiagnostics::new());

        let time_source = FakeTimeSource::events(vec![TimeSourceEvent::StatusChange {
            status: ftexternal::Status::Network,
        }]);

        // Maintain UTC until no more work remains
        let mut fut = maintain_utc(
            PrimaryTrack { clock: Arc::clone(&clock), time_source },
            None,
            Some(rtc.clone()),
            Arc::clone(&diagnostics),
            false,
        )
        .boxed();
        let _ = executor.run_until_stalled(&mut fut);

        // Checking that the clock was updated to use the valid RTC time.
        assert!(clock.get_details().unwrap().last_value_update_ticks > initial_update_ticks);
        assert!(clock.read().unwrap() >= VALID_RTC_TIME);
        assert_eq!(rtc.last_set(), None);

        // Checking that the correct diagnostic events were logged.
        diagnostics.assert_events(&[
            Event::Initialized { clock_state: InitialClockState::NotSet },
            Event::InitializeRtc {
                outcome: InitializeRtcOutcome::Succeeded,
                time: Some(VALID_RTC_TIME),
            },
            Event::StartClock { track: Track::Primary, source: StartClockSource::Rtc },
            Event::TimeSourceStatus { role: Role::Primary, status: ftexternal::Status::Network },
        ]);
    }

    #[fuchsia::test]
    fn no_update_clock_already_running() {
        let mut executor = fasync::TestExecutor::new().unwrap();

        // Create a clock and set it slightly after backstop
        let (clock, _) = create_clock();
        clock
            .update(
                zx::ClockUpdate::builder()
                    .approximate_value(BACKSTOP_TIME + zx::Duration::from_millis(1)),
            )
            .unwrap();
        let initial_update_ticks = clock.get_details().unwrap().last_value_update_ticks;
        let rtc = FakeRtc::valid(VALID_RTC_TIME);
        let diagnostics = Arc::new(FakeDiagnostics::new());

        let time_source = FakeTimeSource::events(vec![TimeSourceEvent::StatusChange {
            status: ftexternal::Status::Network,
        }]);

        // Maintain UTC until no more work remains
        let mut fut = maintain_utc(
            PrimaryTrack { clock: Arc::clone(&clock), time_source },
            None,
            Some(rtc.clone()),
            Arc::clone(&diagnostics),
            false,
        )
        .boxed();
        let _ = executor.run_until_stalled(&mut fut);

        // Checking that neither the clock nor the RTC were updated.
        assert_eq!(clock.get_details().unwrap().last_value_update_ticks, initial_update_ticks);
        assert_eq!(rtc.last_set(), None);

        // Checking that the correct diagnostic events were logged.
        diagnostics.assert_events(&[
            Event::Initialized { clock_state: InitialClockState::PreviouslySet },
            Event::InitializeRtc { outcome: InitializeRtcOutcome::ReadNotAttempted, time: None },
            Event::TimeSourceStatus { role: Role::Primary, status: ftexternal::Status::Network },
        ]);
    }

    #[fuchsia::test]
    fn test_initial_clock_state() {
        let clock =
            zx::Clock::create(zx::ClockOpts::empty(), Some(zx::Time::from_nanos(1_000))).unwrap();
        // The clock must be started with an initial value.
        clock
            .update(zx::ClockUpdate::builder().approximate_value(zx::Time::from_nanos(1_000)))
            .unwrap();
        assert_eq!(initial_clock_state(&clock), InitialClockState::NotSet);

        // Update the clock, which is already running.
        clock
            .update(zx::ClockUpdate::builder().approximate_value(zx::Time::from_nanos(1_000_000)))
            .unwrap();
        assert_eq!(initial_clock_state(&clock), InitialClockState::PreviouslySet);
    }
}
