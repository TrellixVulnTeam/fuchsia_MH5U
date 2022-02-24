// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fidl_fuchsia_cobalt::{
        self as cobalt, CobaltEvent, LoggerFactoryRequest::CreateLoggerFromProjectId,
        LoggerFactoryRequest::CreateLoggerFromProjectSpec,
    },
    fidl_fuchsia_cobalt_test as cobalt_test,
    fidl_fuchsia_metrics::MetricEvent,
    fuchsia_async as fasync,
    fuchsia_cobalt::{CobaltEventExt, MetricEventExt},
    fuchsia_syslog::{self as syslog, fx_log_info},
    fuchsia_zircon_status as zx_status,
    futures::{lock::Mutex, StreamExt, TryStreamExt},
    std::{collections::HashMap, sync::Arc},
};

/// MAX_QUERY_LENGTH is used as a usize in this component
const MAX_QUERY_LENGTH: usize = cobalt_test::MAX_QUERY_LENGTH as usize;

#[derive(Default)]
struct LogState {
    log: Vec<CobaltEvent>,
    hanging: Vec<HangingGetState>,
}

#[derive(Default)]
struct MetricLogState {
    log: Vec<MetricEvent>,
    hanging: Vec<MetricHangingGetState>,
}

/// Send a success response on a watch logs responder. Used to abstract over the WatchLogs and
/// WatchLogs2 fidl methods.
trait WatchLogsResponder {
    fn send_ok(self: Box<Self>, events: Vec<CobaltEvent>, more: bool) -> Result<(), fidl::Error>;
}

impl WatchLogsResponder for cobalt_test::LoggerQuerierWatchLogsResponder {
    fn send_ok(self: Box<Self>, events: Vec<CobaltEvent>, more: bool) -> Result<(), fidl::Error> {
        self.send(&mut Ok((events, more)))
    }
}

impl WatchLogsResponder for cobalt_test::LoggerQuerierWatchLogs2Responder {
    fn send_ok(
        self: Box<Self>,
        mut events: Vec<CobaltEvent>,
        more: bool,
    ) -> Result<(), fidl::Error> {
        self.send(&mut events.iter_mut(), more)
    }
}

// Does not record StartTimer, EndTimer, and LogCustomEvent requests
#[derive(Default)]
struct EventsLog {
    log_event: LogState,
    log_event_count: LogState,
    log_elapsed_time: LogState,
    log_frame_rate: LogState,
    log_memory_usage: LogState,
    log_int_histogram: LogState,
    log_cobalt_event: LogState,
    log_cobalt_events: LogState,

    log_occurrence: MetricLogState,
    log_integer: MetricLogState,
    log_integer_histogram: MetricLogState,
    log_string: MetricLogState,
    log_metric_events: MetricLogState,
}

struct HangingGetState {
    // last_observed is concurrently mutated by calls to run_cobalt_query_service (one for each
    // client of fuchsia.cobalt.test.LoggerQuerier) and calls to handle_cobalt_logger (one for each
    // client of fuchsia.cobalt.Logger).
    last_observed: Arc<Mutex<usize>>,
    responder: Box<dyn WatchLogsResponder>,
}

struct MetricHangingGetState {
    // last_observed is concurrently mutated by calls to run_metrics_query_service (one for each
    // client of fuchsia.metrics.test.MetricEventLoggerQuerier) and calls to
    // handle_metric_event_logger (one for each client of fuchsia.metrics.MetricEventLogger).
    last_observed: Arc<Mutex<usize>>,
    responder: fidl_fuchsia_metrics_test::MetricEventLoggerQuerierWatchLogsResponder,
}

// The LogState#log vectors in EventsLog are mutated by handle_cobalt_logger and
// concurrently observed by run_cobalt_query_service.
//
// The LogState#hanging vectors in EventsLog are concurrently mutated by run_cobalt_query_service
// (new values pushed) and handle_cobalt_logger (new values popped).
type EventsLogHandle = Arc<Mutex<EventsLog>>;

// Entries in the HashMap are concurrently added by run_cobalt_service and
// looked up by run_cobalt_query_service.
type LoggersHandle = Arc<Mutex<HashMap<u32, EventsLogHandle>>>;

/// Create a new Logger. Accepts all `project_id` values and `customer_id` values.
async fn run_cobalt_service(
    stream: cobalt::LoggerFactoryRequestStream,
    loggers: LoggersHandle,
) -> Result<(), fidl::Error> {
    stream
        .try_for_each_concurrent(None, |event| async {
            if let CreateLoggerFromProjectId { project_id, logger, responder } = event {
                let log =
                    loggers.lock().await.entry(project_id).or_insert_with(Default::default).clone();
                let handler = handle_cobalt_logger(logger.into_stream()?, log);
                let () = responder.send(cobalt::Status::Ok)?;
                handler.await
            } else if let CreateLoggerFromProjectSpec { project_id, logger, responder, .. } = event
            {
                let log =
                    loggers.lock().await.entry(project_id).or_insert_with(Default::default).clone();
                let handler = handle_cobalt_logger(logger.into_stream()?, log);
                let () = responder.send(cobalt::Status::Ok)?;
                handler.await
            } else {
                unimplemented!(
                    "Logger factory request of type {:?} not supported by mock cobalt service",
                    event
                );
            }
        })
        .await
}

/// Accepts all incoming log requests and records them in an in-memory store
async fn handle_cobalt_logger(
    stream: cobalt::LoggerRequestStream,
    log: EventsLogHandle,
) -> Result<(), fidl::Error> {
    use cobalt::LoggerRequest::*;
    let fut = stream.try_for_each_concurrent(None, |event| async {
        let mut log = log.lock().await;
        let log_state = match event {
            LogEvent { metric_id, event_code, responder } => {
                let state = &mut log.log_event;
                state
                    .log
                    .push(CobaltEvent::builder(metric_id).with_event_code(event_code).as_event());
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogEventCount {
                metric_id,
                event_code,
                component,
                period_duration_micros,
                count,
                responder,
            } => {
                let state = &mut log.log_event_count;
                state.log.push(
                    CobaltEvent::builder(metric_id)
                        .with_event_code(event_code)
                        .with_component(component)
                        .as_count_event(period_duration_micros, count),
                );
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogElapsedTime { metric_id, event_code, component, elapsed_micros, responder } => {
                let state = &mut log.log_elapsed_time;
                state.log.push(
                    CobaltEvent::builder(metric_id)
                        .with_event_code(event_code)
                        .with_component(component)
                        .as_elapsed_time(elapsed_micros),
                );
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogFrameRate { metric_id, event_code, component, fps, responder } => {
                let state = &mut log.log_frame_rate;
                state.log.push(
                    CobaltEvent::builder(metric_id)
                        .with_event_code(event_code)
                        .with_component(component)
                        .as_frame_rate(fps),
                );
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogMemoryUsage { metric_id, event_code, component, bytes, responder } => {
                let state = &mut log.log_memory_usage;
                state.log.push(
                    CobaltEvent::builder(metric_id)
                        .with_event_code(event_code)
                        .with_component(component)
                        .as_memory_usage(bytes),
                );
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogIntHistogram { metric_id, event_code, component, histogram, responder } => {
                let state = &mut log.log_int_histogram;
                state.log.push(
                    CobaltEvent::builder(metric_id)
                        .with_event_code(event_code)
                        .with_component(component)
                        .as_int_histogram(histogram),
                );
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogCobaltEvent { event, responder } => {
                let state = &mut log.log_cobalt_event;
                state.log.push(event);
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            LogCobaltEvents { mut events, responder } => {
                let state = &mut log.log_cobalt_events;
                state.log.append(&mut events);
                let () = responder.send(cobalt::Status::Ok)?;
                state
            }
            e => unimplemented!("Event {:?} is not supported by the mock cobalt server", e),
        };
        while let Some(hanging_get_state) = log_state.hanging.pop() {
            let mut last_observed = hanging_get_state.last_observed.lock().await;
            let events = (&mut log_state.log)
                .iter()
                .skip(*last_observed)
                .take(MAX_QUERY_LENGTH)
                .map(Clone::clone)
                .collect();
            *last_observed = log_state.log.len();
            let () = hanging_get_state.responder.send_ok(events, false)?;
        }
        Ok(())
    });

    match fut.await {
        // Don't consider PEER_CLOSED to be an error.
        Err(fidl::Error::ServerResponseWrite(zx_status::Status::PEER_CLOSED)) => Ok(()),
        other => other,
    }
}

async fn run_metrics_service(
    stream: fidl_fuchsia_metrics::MetricEventLoggerFactoryRequestStream,
    loggers: LoggersHandle,
) -> Result<(), fidl::Error> {
    use fidl_fuchsia_metrics::MetricEventLoggerFactoryRequest::*;
    stream
        .try_for_each_concurrent(None, |event| async {
            match event {
                CreateMetricEventLogger { project_spec, logger, responder } => {
                    let handler = make_handler(project_spec, &loggers, logger);
                    let () = responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                    handler.await
                }
                CreateMetricEventLoggerWithExperiments {
                    project_spec, logger, responder, ..
                } => {
                    // TODO(fxb/90740): Support experiment_ids.
                    let handler = make_handler(project_spec, &loggers, logger);
                    let () = responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                    handler.await
                }
            }
        })
        .await
}

async fn make_handler(
    project_spec: fidl_fuchsia_metrics::ProjectSpec,
    loggers: &LoggersHandle,
    logger: fidl::endpoints::ServerEnd<fidl_fuchsia_metrics::MetricEventLoggerMarker>,
) -> Result<(), fidl::Error> {
    let log = loggers
        .lock()
        .await
        .entry(project_spec.project_id.unwrap_or(0))
        .or_insert_with(Default::default)
        .clone();
    handle_metric_event_logger(logger.into_stream()?, log).await
}

async fn handle_metric_event_logger(
    stream: fidl_fuchsia_metrics::MetricEventLoggerRequestStream,
    log: EventsLogHandle,
) -> Result<(), fidl::Error> {
    use fidl_fuchsia_metrics::MetricEventLoggerRequest::*;
    let fut = stream.try_for_each_concurrent(None, |event| async {
        let mut log = log.lock().await;
        let log_state = match event {
            LogOccurrence { responder, metric_id, count, event_codes } => {
                let state = &mut log.log_occurrence;
                state.log.push(
                    MetricEvent::builder(metric_id)
                        .with_event_codes(event_codes)
                        .as_occurrence(count),
                );
                responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                state
            }
            LogInteger { responder, metric_id, value, event_codes } => {
                let state = &mut log.log_integer;
                state.log.push(
                    MetricEvent::builder(metric_id).with_event_codes(event_codes).as_integer(value),
                );
                responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                state
            }
            LogIntegerHistogram { responder, metric_id, histogram, event_codes } => {
                let state = &mut log.log_integer_histogram;
                state.log.push(
                    MetricEvent::builder(metric_id)
                        .with_event_codes(event_codes)
                        .as_integer_histogram(histogram),
                );
                responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                state
            }
            LogString { responder, metric_id, string_value, event_codes } => {
                let state = &mut log.log_string;
                state.log.push(
                    MetricEvent::builder(metric_id)
                        .with_event_codes(event_codes)
                        .as_string(string_value),
                );
                responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                state
            }
            LogMetricEvents { responder, mut events } => {
                let state = &mut log.log_metric_events;
                state.log.append(&mut events);
                responder.send(fidl_fuchsia_metrics::Status::Ok)?;
                state
            }
            // We can't easily support CustomEvents because custom events can't
            // be packaged into MetricEvent objects.
            LogCustomEvent { responder, .. } => {
                let _ = responder.send(fidl_fuchsia_metrics::Status::InternalError);
                unimplemented!("Custom Events are not supported by mock_cobalt");
            }
        };

        while let Some(hanging_get_state) = log_state.hanging.pop() {
            let mut last_observed = hanging_get_state.last_observed.lock().await;
            let mut events: Vec<MetricEvent> = (&mut log_state.log)
                .iter()
                .skip(*last_observed)
                .take(MAX_QUERY_LENGTH)
                .map(Clone::clone)
                .collect();
            *last_observed = log_state.log.len();
            hanging_get_state.responder.send(&mut events.iter_mut(), false)?;
        }
        Ok(())
    });

    match fut.await {
        // Don't consider PEER_CLOSED to be an error.
        Err(fidl::Error::ServerResponseWrite(zx_status::Status::PEER_CLOSED)) => Ok(()),
        other => other,
    }
}

/// Handles requests to query the state of the mock.
async fn run_cobalt_query_service(
    stream: cobalt_test::LoggerQuerierRequestStream,
    loggers: LoggersHandle,
) -> Result<(), fidl::Error> {
    use cobalt_test::LogMethod::*;

    let _client_state: HashMap<_, _> = stream
        .try_fold(
            HashMap::new(),
            |mut client_state: HashMap<
                u32,
                HashMap<fidl_fuchsia_cobalt_test::LogMethod, Arc<Mutex<usize>>>,
            >,
             event| async {
                match event {
                    cobalt_test::LoggerQuerierRequest::WatchLogs2 {
                        project_id,
                        method,
                        responder,
                    } => {
                        let state = loggers
                            .lock()
                            .await
                            .entry(project_id)
                            .or_insert_with(Default::default)
                            .clone();

                        let mut state = state.lock().await;
                        let log_state = match method {
                            LogEvent => &mut state.log_event,
                            LogEventCount => &mut state.log_event_count,
                            LogElapsedTime => &mut state.log_elapsed_time,
                            LogFrameRate => &mut state.log_frame_rate,
                            LogMemoryUsage => &mut state.log_memory_usage,
                            LogIntHistogram => &mut state.log_int_histogram,
                            LogCobaltEvent => &mut state.log_cobalt_event,
                            LogCobaltEvents => &mut state.log_cobalt_events,
                        };
                        let last_observed = client_state
                            .entry(project_id)
                            .or_insert_with(Default::default)
                            .entry(method)
                            .or_insert_with(Default::default);
                        let mut last_observed_len = last_observed.lock().await;
                        let current_len = log_state.log.len();
                        if current_len != *last_observed_len {
                            let events = &mut log_state.log;
                            let more = events.len() > cobalt_test::MAX_QUERY_LENGTH as usize;
                            let mut events: Vec<_> = events
                                .iter()
                                .skip(*last_observed_len)
                                .take(MAX_QUERY_LENGTH)
                                .cloned()
                                .collect();
                            *last_observed_len = current_len;
                            let () = responder.send(&mut events.iter_mut(), more)?;
                        } else {
                            let () = log_state.hanging.push(HangingGetState {
                                responder: Box::new(responder),
                                last_observed: last_observed.clone(),
                            });
                        }
                    }
                    cobalt_test::LoggerQuerierRequest::WatchLogs {
                        project_id,
                        method,
                        responder,
                    } => {
                        if let Some(state) = loggers.lock().await.get(&project_id) {
                            let mut state = state.lock().await;
                            let log_state = match method {
                                LogEvent => &mut state.log_event,
                                LogEventCount => &mut state.log_event_count,
                                LogElapsedTime => &mut state.log_elapsed_time,
                                LogFrameRate => &mut state.log_frame_rate,
                                LogMemoryUsage => &mut state.log_memory_usage,
                                LogIntHistogram => &mut state.log_int_histogram,
                                LogCobaltEvent => &mut state.log_cobalt_event,
                                LogCobaltEvents => &mut state.log_cobalt_events,
                            };
                            let last_observed = client_state
                                .entry(project_id)
                                .or_insert_with(Default::default)
                                .entry(method)
                                .or_insert_with(Default::default);
                            let mut last_observed_len = last_observed.lock().await;
                            let current_len = log_state.log.len();
                            if current_len != *last_observed_len {
                                let events = &mut log_state.log;
                                let more = events.len() > cobalt_test::MAX_QUERY_LENGTH as usize;
                                let events = events
                                    .iter()
                                    .skip(*last_observed_len)
                                    .take(MAX_QUERY_LENGTH)
                                    .cloned()
                                    .collect();
                                *last_observed_len = current_len;
                                let () = responder.send(&mut Ok((events, more)))?;
                            } else {
                                let () = log_state.hanging.push(HangingGetState {
                                    responder: Box::new(responder),
                                    last_observed: last_observed.clone(),
                                });
                            }
                        } else {
                            let () = responder
                                .send(&mut Err(cobalt_test::QueryError::LoggerNotFound))?;
                        }
                    }
                    cobalt_test::LoggerQuerierRequest::ResetLogger {
                        project_id,
                        method,
                        control_handle: _,
                    } => {
                        if let Some(log) = loggers.lock().await.get(&project_id) {
                            let mut state = log.lock().await;
                            match method {
                                LogEvent => state.log_event.log.clear(),
                                LogEventCount => state.log_event_count.log.clear(),
                                LogElapsedTime => state.log_elapsed_time.log.clear(),
                                LogFrameRate => state.log_frame_rate.log.clear(),
                                LogMemoryUsage => state.log_memory_usage.log.clear(),
                                LogIntHistogram => state.log_int_histogram.log.clear(),
                                LogCobaltEvent => state.log_cobalt_event.log.clear(),
                                LogCobaltEvents => state.log_cobalt_events.log.clear(),
                            }
                        }
                    }
                }
                Ok(client_state)
            },
        )
        .await?;
    Ok(())
}

async fn run_metrics_query_service(
    stream: fidl_fuchsia_metrics_test::MetricEventLoggerQuerierRequestStream,
    loggers: LoggersHandle,
) -> Result<(), fidl::Error> {
    use fidl_fuchsia_metrics_test::LogMethod;

    let _client_state: HashMap<_, _> = stream
        .try_fold(
            HashMap::new(),
            |mut client_state: HashMap<u32, HashMap<LogMethod, Arc<Mutex<usize>>>>, event| async {
                match event {
                    fidl_fuchsia_metrics_test::MetricEventLoggerQuerierRequest::WatchLogs {
                        project_id,
                        method,
                        responder,
                    } => {
                        let state = loggers
                            .lock()
                            .await
                            .entry(project_id)
                            .or_insert_with(Default::default)
                            .clone();

                        let mut state = state.lock().await;
                        let log_state = match method {
                            LogMethod::LogOccurrence => &mut state.log_occurrence,
                            LogMethod::LogInteger => &mut state.log_integer,
                            LogMethod::LogIntegerHistogram => &mut state.log_integer_histogram,
                            LogMethod::LogString => &mut state.log_string,
                            LogMethod::LogMetricEvents => &mut state.log_metric_events,
                        };
                        let last_observed = client_state
                            .entry(project_id)
                            .or_insert_with(Default::default)
                            .entry(method)
                            .or_insert_with(Default::default);
                        let mut last_observed_len = last_observed.lock().await;
                        let current_len = log_state.log.len();
                        if current_len != *last_observed_len {
                            let events = &mut log_state.log;
                            let more = events.len() > cobalt_test::MAX_QUERY_LENGTH as usize;
                            let mut events: Vec<_> = events
                                .iter()
                                .skip(*last_observed_len)
                                .take(MAX_QUERY_LENGTH)
                                .cloned()
                                .collect();
                            *last_observed_len = current_len;
                            responder.send(&mut events.iter_mut(), more)?;
                        } else {
                            log_state.hanging.push(MetricHangingGetState {
                                responder: responder,
                                last_observed: last_observed.clone(),
                            });
                        }
                    }
                    fidl_fuchsia_metrics_test::MetricEventLoggerQuerierRequest::ResetLogger {
                        project_id,
                        method,
                        ..
                    } => {
                        if let Some(log) = loggers.lock().await.get(&project_id) {
                            let mut state = log.lock().await;
                            match method {
                                LogMethod::LogOccurrence => state.log_occurrence.log.clear(),
                                LogMethod::LogInteger => state.log_integer.log.clear(),
                                LogMethod::LogIntegerHistogram => {
                                    state.log_integer_histogram.log.clear()
                                }
                                LogMethod::LogString => state.log_string.log.clear(),
                                LogMethod::LogMetricEvents => state.log_metric_events.log.clear(),
                            }
                        }
                    }
                }
                Ok(client_state)
            },
        )
        .await?;
    Ok(())
}

enum IncomingService {
    Cobalt(fidl_fuchsia_cobalt::LoggerFactoryRequestStream),
    CobaltQuery(fidl_fuchsia_cobalt_test::LoggerQuerierRequestStream),
    Metrics(fidl_fuchsia_metrics::MetricEventLoggerFactoryRequestStream),
    MetricsQuery(fidl_fuchsia_metrics_test::MetricEventLoggerQuerierRequestStream),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    syslog::init_with_tags(&["mock-cobalt"])?;
    fx_log_info!("Starting mock cobalt service...");

    let loggers = LoggersHandle::default();

    let mut fs = fuchsia_component::server::ServiceFs::new_local();
    fs.dir("svc")
        .add_fidl_service(IncomingService::Cobalt)
        .add_fidl_service(IncomingService::CobaltQuery)
        .add_fidl_service(IncomingService::Metrics)
        .add_fidl_service(IncomingService::MetricsQuery);
    fs.take_and_serve_directory_handle()?;
    fs.then(futures::future::ok)
        .try_for_each_concurrent(None, |client_request| async {
            let loggers = loggers.clone();
            match client_request {
                IncomingService::Cobalt(stream) => run_cobalt_service(stream, loggers).await,
                IncomingService::CobaltQuery(stream) => {
                    run_cobalt_query_service(stream, loggers).await
                }

                IncomingService::Metrics(stream) => run_metrics_service(stream, loggers).await,
                IncomingService::MetricsQuery(stream) => {
                    run_metrics_query_service(stream, loggers).await
                }
            }
        })
        .await?;

    Ok(())
}

#[cfg(test)]
mod cobalt_tests {
    use super::*;
    use async_utils::PollExt;
    use fidl::endpoints::{create_proxy, create_proxy_and_stream};
    use fidl_fuchsia_cobalt::*;
    use fidl_fuchsia_cobalt_test::{LogMethod, LoggerQuerierMarker, QueryError};
    use fuchsia_async as fasync;
    use futures::FutureExt;

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_factory() {
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (_logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");

        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();

        assert!(loggers.lock().await.is_empty());

        factory_proxy
            .create_logger_from_project_id(1234, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        assert!(loggers.lock().await.get(&1234).is_some());
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_and_query_interface_single_event() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_logger_from_project_id(123, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log a single event.
        logger_proxy.log_event(1, 2).await.expect("log_event fidl call to succeed");
        assert_eq!(
            Ok((vec![CobaltEvent::builder(1).with_event_code(2).as_event()], false)),
            querier_proxy
                .watch_logs(123, LogMethod::LogEvent)
                .await
                .expect("log_event fidl call to succeed")
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_and_query_interface_multiple_events() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_logger_from_project_id(12, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log 1 more than the maximum number of events that can be stored and assert that
        // `more` flag is true on logger query request.
        for i in 0..(MAX_QUERY_LENGTH as u32 + 1) {
            logger_proxy
                .log_event(i, i + 1)
                .await
                .expect("repeated log_event fidl call to succeed");
        }
        let (events, more) = querier_proxy
            .watch_logs(12, LogMethod::LogEvent)
            .await
            .expect("watch_logs fidl call to succeed")
            .expect("logger to exist and have recorded events");
        assert_eq!(CobaltEvent::builder(0).with_event_code(1).as_event(), events[0]);
        assert_eq!(MAX_QUERY_LENGTH, events.len());
        assert!(more);
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_query_interface_no_logger_error() {
        let loggers = LoggersHandle::default();

        // Create channel.
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handler. Any failures in the service spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        // Assert on initial state.
        assert_eq!(
            Err(QueryError::LoggerNotFound),
            querier_proxy
                .watch_logs(1, LogMethod::LogEvent)
                .await
                .expect("watch_logs fidl call to succeed")
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_and_query_interface_single_event2() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_logger_from_project_id(123, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log a single event.
        logger_proxy.log_event(1, 2).await.expect("log_event fidl call to succeed");
        assert_eq!(
            (vec![CobaltEvent::builder(1).with_event_code(2).as_event()], false),
            querier_proxy
                .watch_logs2(123, LogMethod::LogEvent)
                .await
                .expect("log_event fidl call to succeed")
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_and_query_interface_multiple_events2() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_logger_from_project_id(12, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log 1 more than the maximum number of events that can be stored and assert that
        // `more` flag is true on logger query request.
        for i in 0..(MAX_QUERY_LENGTH as u32 + 1) {
            logger_proxy
                .log_event(i, i + 1)
                .await
                .expect("repeated log_event fidl call to succeed");
        }
        let (events, more) = querier_proxy
            .watch_logs2(12, LogMethod::LogEvent)
            .await
            .expect("watch_logs2 fidl call to succeed");
        assert_eq!(CobaltEvent::builder(0).with_event_code(1).as_event(), events[0]);
        assert_eq!(MAX_QUERY_LENGTH, events.len());
        assert!(more);
    }

    #[test]
    fn mock_query_interface_no_logger_never_completes() {
        let mut exec = fasync::TestExecutor::new().unwrap();

        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers).map(|_| ())).detach();

        // watch_logs2 query does not complete without a logger for the requested project id.
        let watch_logs_fut = querier_proxy.watch_logs2(123, LogMethod::LogEvent);
        futures::pin_mut!(watch_logs_fut);

        assert!(exec.run_until_stalled(&mut watch_logs_fut).is_pending());

        // Create a new logger for the requested project id
        let create_logger_fut = factory_proxy.create_logger_from_project_id(123, server);
        futures::pin_mut!(create_logger_fut);
        exec.run_until_stalled(&mut create_logger_fut)
            .expect("logger creation future to complete")
            .expect("create_logger_from_project_id fidl call to succeed");

        // watch_logs2 query still does not complete without a LogEvent for the requested project
        // id.
        assert!(exec.run_until_stalled(&mut watch_logs_fut).is_pending());

        // Log a single event
        let log_event_fut = logger_proxy.log_event(1, 2);
        futures::pin_mut!(log_event_fut);
        exec.run_until_stalled(&mut log_event_fut)
            .expect("log event future to complete")
            .expect("log_event fidl call to succeed");

        // finally, now that a logger and log event have been created, watch_logs2 query will
        // succeed.
        let result = exec
            .run_until_stalled(&mut watch_logs_fut)
            .expect("log_event future to complete")
            .expect("log_event fidl call to succeed");
        assert_eq!((vec![CobaltEvent::builder(1).with_event_code(2).as_event()], false), result);
    }

    #[test]
    fn mock_query_interface_no_events_never_completes() {
        let mut exec = fasync::TestExecutor::new().unwrap();

        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");
        let (_logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        let test = async move {
            factory_proxy
                .create_logger_from_project_id(123, server)
                .await
                .expect("create_logger_from_project_id fidl call to succeed");

            assert_eq!(
                (vec![CobaltEvent::builder(1).with_event_code(2).as_event()], false),
                querier_proxy
                    .watch_logs2(123, LogMethod::LogEvent)
                    .await
                    .expect("log_event fidl call to succeed")
            );
        };
        futures::pin_mut!(test);
        assert!(exec.run_until_stalled(&mut test).is_pending());
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_logger_logger_type_tracking() -> Result<(), fidl::Error> {
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");

        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        let project_id = 1;

        factory_proxy
            .create_logger_from_project_id(project_id, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        let metric_id = 1;
        let event_code = 2;
        let component_name = "component";
        let period_duration_micros = 0;
        let count = 3;
        let frame_rate: f32 = 59.9;

        logger_proxy.log_event(metric_id, event_code).await?;
        logger_proxy
            .log_event_count(metric_id, event_code, component_name, period_duration_micros, count)
            .await?;
        logger_proxy
            .log_elapsed_time(metric_id, event_code, component_name, period_duration_micros)
            .await?;
        logger_proxy.log_memory_usage(metric_id, event_code, component_name, count).await?;
        logger_proxy.log_frame_rate(metric_id, event_code, component_name, frame_rate).await?;
        logger_proxy
            .log_int_histogram(metric_id, event_code, component_name, &mut vec![].into_iter())
            .await?;
        logger_proxy
            .log_cobalt_event(&mut cobalt::CobaltEvent {
                metric_id,
                event_codes: vec![event_code],
                component: Some(component_name.to_string()),
                payload: cobalt::EventPayload::Event(cobalt::Event {}),
            })
            .await?;
        logger_proxy.log_cobalt_events(&mut vec![].into_iter()).await?;
        let log = loggers.lock().await;
        let log = log.get(&project_id).expect("project should have been created");
        let state = log.lock().await;
        assert_eq!(state.log_event.log.len(), 1);
        assert_eq!(state.log_event_count.log.len(), 1);
        assert_eq!(state.log_elapsed_time.log.len(), 1);
        assert_eq!(state.log_memory_usage.log.len(), 1);
        assert_eq!(state.log_frame_rate.log.len(), 1);
        assert_eq!(state.log_int_histogram.log.len(), 1);
        assert_eq!(state.log_cobalt_event.log.len(), 1);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_query_interface_reset_state() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_cobalt_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_cobalt_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_logger_from_project_id(987, server)
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log a single event.
        logger_proxy.log_event(1, 2).await.expect("log_event fidl call to succeed");
        assert_eq!(
            (vec![CobaltEvent::builder(1).with_event_code(2).as_event()], false),
            querier_proxy
                .watch_logs2(987, LogMethod::LogEvent)
                .await
                .expect("log_event fidl call to succeed")
        );

        // Clear logger state.
        querier_proxy
            .reset_logger(987, LogMethod::LogEvent)
            .expect("reset_logger fidl call to succeed");

        assert_eq!(
            (vec![], false),
            querier_proxy
                .watch_logs2(987, LogMethod::LogEvent)
                .await
                .expect("watch_logs2 fidl call to succeed")
        );
    }

    #[test]
    fn mock_query_interface_hanging_get() {
        let mut executor = fuchsia_async::TestExecutor::new().unwrap();
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) = create_proxy_and_stream::<LoggerFactoryMarker>()
            .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, logger_proxy_server_end) =
            create_proxy::<LoggerMarker>().expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) = create_proxy_and_stream::<LoggerQuerierMarker>()
            .expect("create logger querier proxy and stream to succeed");

        let cobalt_service = run_cobalt_service(factory_stream, loggers.clone());
        let cobalt_query_service = run_cobalt_query_service(query_stream, loggers.clone());

        futures::pin_mut!(cobalt_service);
        futures::pin_mut!(cobalt_query_service);

        // Neither of these futures should ever return if there no errors, so the joined future
        // will never return.
        let services = futures::future::join(cobalt_service, cobalt_query_service);

        let project_id = 765;
        let mut create_logger = futures::future::select(
            services,
            factory_proxy.create_logger_from_project_id(project_id, logger_proxy_server_end),
        );
        let create_logger_poll = executor.run_until_stalled(&mut create_logger);
        assert!(create_logger_poll.is_ready());

        let mut continuation = match create_logger_poll {
            core::task::Poll::Pending => {
                unreachable!("we asserted that create_logger_poll was ready")
            }
            core::task::Poll::Ready(either) => match either {
                futures::future::Either::Left(_services_future_returned) => unreachable!(
                    "unexpected services future return (cannot be formatted with default formatter)"
                ),
                futures::future::Either::Right((create_logger_status, services_continuation)) => {
                    assert_eq!(
                        create_logger_status.expect("fidl call failed"),
                        fidl_fuchsia_cobalt::Status::Ok
                    );
                    services_continuation
                }
            },
        };

        // Resolve two hanging gets and ensure that only the novel data (the same data both times)
        // is returned.
        for _ in 0..2 {
            let watch_logs_hanging_get = querier_proxy.watch_logs2(project_id, LogMethod::LogEvent);
            let mut watch_logs_hanging_get =
                futures::future::select(continuation, watch_logs_hanging_get);
            let watch_logs_poll = executor.run_until_stalled(&mut watch_logs_hanging_get);
            assert!(watch_logs_poll.is_pending());

            let event_metric_id = 1;
            let event_code = 2;
            let log_event = logger_proxy.log_event(event_metric_id, event_code);

            let mut resolved_hanging_get = futures::future::join(watch_logs_hanging_get, log_event);
            let resolved_hanging_get = executor.run_until_stalled(&mut resolved_hanging_get);
            assert!(resolved_hanging_get.is_ready());

            continuation = match resolved_hanging_get {
                core::task::Poll::Pending => {
                    unreachable!("we asserted that resolved_hanging_get was ready")
                }
                core::task::Poll::Ready((watch_logs_result, log_event_result)) => {
                    assert_eq!(
                        log_event_result.expect("expected log event to succeed"),
                        fidl_fuchsia_cobalt::Status::Ok
                    );

                    match watch_logs_result {
                        futures::future::Either::Left(_services_future_returned) => unreachable!(
                            "unexpected services future return (cannot be formatted with the \
                             default formatter)"
                        ),
                        futures::future::Either::Right((
                            cobalt_query_result,
                            services_continuation,
                        )) => {
                            let (mut logged_events, more) = cobalt_query_result
                                .expect("expect cobalt query FIDL call to succeed");
                            assert_eq!(logged_events.len(), 1);
                            let mut logged_event = logged_events.pop().unwrap();
                            assert_eq!(logged_event.metric_id, event_metric_id);
                            assert_eq!(logged_event.event_codes.len(), 1);
                            assert_eq!(logged_event.event_codes.pop().unwrap(), event_code);
                            assert_eq!(more, false);
                            services_continuation
                        }
                    }
                }
            };

            assert!(executor.run_until_stalled(&mut continuation).is_pending());
        }
    }
}

#[cfg(test)]
mod metrics_tests {
    use super::*;
    use async_utils::PollExt;
    use fidl::endpoints::{create_proxy, create_proxy_and_stream};
    use fidl_fuchsia_metrics::*;
    use fidl_fuchsia_metrics_test::{LogMethod, MetricEventLoggerQuerierMarker};
    use fuchsia_async as fasync;
    use futures::FutureExt;

    #[fasync::run_until_stalled(test)]
    async fn mock_metric_event_logger_factory() {
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (_logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");

        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();

        assert!(loggers.lock().await.is_empty());

        factory_proxy
            .create_metric_event_logger(
                ProjectSpec { project_id: Some(1234), ..ProjectSpec::EMPTY },
                server,
            )
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        assert!(loggers.lock().await.get(&1234).is_some());
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_metric_event_logger_and_query_interface_single_event() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_metrics_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_metric_event_logger(
                ProjectSpec { project_id: Some(123), ..ProjectSpec::EMPTY },
                server,
            )
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log a single event.
        logger_proxy.log_integer(12345, 123, &[]).await.expect("log_event fidl call to succeed");
        assert_eq!(
            (vec![MetricEvent::builder(12345).as_integer(123)], false),
            querier_proxy
                .watch_logs(123, LogMethod::LogInteger)
                .await
                .expect("log_event fidl call to succeed")
        );
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_metric_event_logger_and_query_interface_multiple_events() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_metrics_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_metric_event_logger(
                ProjectSpec { project_id: Some(12), ..ProjectSpec::EMPTY },
                server,
            )
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log 1 more than the maximum number of events that can be stored and assert that
        // `more` flag is true on logger query request.
        for i in 0..(MAX_QUERY_LENGTH as u32 + 1) {
            logger_proxy
                .log_integer(i, (i + 1) as i64, &[1])
                .await
                .expect("repeated log_event fidl call to succeed");
        }
        let (events, more) = querier_proxy
            .watch_logs(12, LogMethod::LogInteger)
            .await
            .expect("watch_logs fidl call to succeed");
        assert_eq!(MetricEvent::builder(0).with_event_code(1).as_integer(1), events[0]);
        assert_eq!(MAX_QUERY_LENGTH, events.len());
        assert!(more);
    }

    #[test]
    fn mock_query_interface_no_logger_never_completes() {
        let mut exec = fasync::TestExecutor::new().unwrap();

        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_metrics_query_service(query_stream, loggers).map(|_| ())).detach();

        // watch_logs2 query does not complete without a logger for the requested project id.
        let watch_logs_fut = querier_proxy.watch_logs(123, LogMethod::LogInteger);
        futures::pin_mut!(watch_logs_fut);

        assert!(exec.run_until_stalled(&mut watch_logs_fut).is_pending());

        // Create a new logger for the requested project id
        let create_logger_fut = factory_proxy.create_metric_event_logger(
            ProjectSpec { project_id: Some(123), ..ProjectSpec::EMPTY },
            server,
        );
        futures::pin_mut!(create_logger_fut);
        exec.run_until_stalled(&mut create_logger_fut)
            .expect("logger creation future to complete")
            .expect("create_logger_from_project_id fidl call to succeed");

        // watch_logs query still does not complete without a LogEvent for the requested project
        // id.
        assert!(exec.run_until_stalled(&mut watch_logs_fut).is_pending());

        // Log a single event
        let log_event_fut = logger_proxy.log_integer(1, 2, &[3]);
        futures::pin_mut!(log_event_fut);
        exec.run_until_stalled(&mut log_event_fut)
            .expect("log event future to complete")
            .expect("log_event fidl call to succeed");

        // finally, now that a logger and log event have been created, watch_logs2 query will
        // succeed.
        let result = exec
            .run_until_stalled(&mut watch_logs_fut)
            .expect("log_event future to complete")
            .expect("log_event fidl call to succeed");
        assert_eq!((vec![MetricEvent::builder(1).with_event_code(3).as_integer(2)], false), result);
    }

    #[test]
    fn mock_query_interface_no_events_never_completes() {
        let mut exec = fasync::TestExecutor::new().unwrap();

        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");
        let (_logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_metrics_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        let test = async move {
            factory_proxy
                .create_metric_event_logger(
                    ProjectSpec { project_id: Some(123), ..ProjectSpec::EMPTY },
                    server,
                )
                .await
                .expect("create_logger_from_project_id fidl call to succeed");

            assert_eq!(
                (vec![MetricEvent::builder(1).with_event_code(2).as_integer(3)], false),
                querier_proxy
                    .watch_logs(123, LogMethod::LogInteger)
                    .await
                    .expect("log_event fidl call to succeed")
            );
        };
        futures::pin_mut!(test);
        assert!(exec.run_until_stalled(&mut test).is_pending());
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_metric_event_logger_logger_type_tracking() -> Result<(), fidl::Error> {
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");

        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        let project_id = 1;

        factory_proxy
            .create_metric_event_logger(
                ProjectSpec { project_id: Some(project_id), ..ProjectSpec::EMPTY },
                server,
            )
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        let metric_id = 1;
        let event_code = 2;
        let count = 3;
        let value = 4;
        let string = "value";

        logger_proxy.log_occurrence(metric_id, count, &[event_code]).await?;
        logger_proxy.log_integer(metric_id, value, &[event_code]).await?;
        logger_proxy
            .log_integer_histogram(metric_id, &mut vec![].into_iter(), &[event_code])
            .await?;
        logger_proxy.log_string(metric_id, string, &[event_code]).await?;
        logger_proxy
            .log_metric_events(
                &mut vec![&mut MetricEvent::builder(metric_id)
                    .with_event_code(event_code)
                    .as_occurrence(count)]
                .into_iter(),
            )
            .await?;
        let log = loggers.lock().await;
        let log = log.get(&project_id).expect("project should have been created");
        let state = log.lock().await;
        assert_eq!(state.log_occurrence.log.len(), 1);
        assert_eq!(state.log_integer.log.len(), 1);
        assert_eq!(state.log_integer_histogram.log.len(), 1);
        assert_eq!(state.log_string.log.len(), 1);
        assert_eq!(state.log_metric_events.log.len(), 1);
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn mock_query_interface_reset_state() {
        let loggers = LoggersHandle::default();

        // Create channels.
        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, server) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");

        // Spawn service handlers. Any failures in the services spawned here will trigger panics
        // via expect method calls below.
        fasync::Task::local(run_metrics_service(factory_stream, loggers.clone()).map(|_| ()))
            .detach();
        fasync::Task::local(run_metrics_query_service(query_stream, loggers.clone()).map(|_| ()))
            .detach();

        factory_proxy
            .create_metric_event_logger(
                ProjectSpec { project_id: Some(987), ..ProjectSpec::EMPTY },
                server,
            )
            .await
            .expect("create_logger_from_project_id fidl call to succeed");

        // Log a single event.
        logger_proxy.log_integer(1, 2, &[3]).await.expect("log_event fidl call to succeed");
        assert_eq!(
            (vec![MetricEvent::builder(1).with_event_code(3).as_integer(2)], false),
            querier_proxy
                .watch_logs(987, LogMethod::LogInteger)
                .await
                .expect("log_event fidl call to succeed")
        );

        // Clear logger state.
        querier_proxy
            .reset_logger(987, LogMethod::LogInteger)
            .expect("reset_logger fidl call to succeed");

        assert_eq!(
            (vec![], false),
            querier_proxy
                .watch_logs(987, LogMethod::LogInteger)
                .await
                .expect("watch_logs fidl call to succeed")
        );
    }

    #[test]
    fn mock_query_interface_hanging_get() {
        let mut executor = fuchsia_async::TestExecutor::new().unwrap();
        let loggers = LoggersHandle::default();

        let (factory_proxy, factory_stream) =
            create_proxy_and_stream::<MetricEventLoggerFactoryMarker>()
                .expect("create logger factroy proxy and stream to succeed");
        let (logger_proxy, logger_proxy_server_end) = create_proxy::<MetricEventLoggerMarker>()
            .expect("create logger proxy and server end to succeed");
        let (querier_proxy, query_stream) =
            create_proxy_and_stream::<MetricEventLoggerQuerierMarker>()
                .expect("create logger querier proxy and stream to succeed");

        let cobalt_service = run_metrics_service(factory_stream, loggers.clone());
        let cobalt_query_service = run_metrics_query_service(query_stream, loggers.clone());

        futures::pin_mut!(cobalt_service);
        futures::pin_mut!(cobalt_query_service);

        // Neither of these futures should ever return if there no errors, so the joined future
        // will never return.
        let services = futures::future::join(cobalt_service, cobalt_query_service);

        let project_id = 765;
        let mut create_logger = futures::future::select(
            services,
            factory_proxy.create_metric_event_logger(
                ProjectSpec { project_id: Some(project_id), ..ProjectSpec::EMPTY },
                logger_proxy_server_end,
            ),
        );
        let create_logger_poll = executor.run_until_stalled(&mut create_logger);
        assert!(create_logger_poll.is_ready());

        let mut continuation = match create_logger_poll {
            core::task::Poll::Pending => {
                unreachable!("we asserted that create_logger_poll was ready")
            }
            core::task::Poll::Ready(either) => match either {
                futures::future::Either::Left(_services_future_returned) => unreachable!(
                    "unexpected services future return (cannot be formatted with default formatter)"
                ),
                futures::future::Either::Right((create_logger_status, services_continuation)) => {
                    assert_eq!(
                        create_logger_status.expect("fidl call failed"),
                        fidl_fuchsia_metrics::Status::Ok
                    );
                    services_continuation
                }
            },
        };

        // Resolve two hanging gets and ensure that only the novel data (the same data both times)
        // is returned.
        for _ in 0..2 {
            let watch_logs_hanging_get =
                querier_proxy.watch_logs(project_id, LogMethod::LogInteger);
            let mut watch_logs_hanging_get =
                futures::future::select(continuation, watch_logs_hanging_get);
            let watch_logs_poll = executor.run_until_stalled(&mut watch_logs_hanging_get);
            assert!(watch_logs_poll.is_pending());

            let event_metric_id = 1;
            let event_code = 2;
            let value = 3;
            let log_event = logger_proxy.log_integer(event_metric_id, value, &[event_code]);

            let mut resolved_hanging_get = futures::future::join(watch_logs_hanging_get, log_event);
            let resolved_hanging_get = executor.run_until_stalled(&mut resolved_hanging_get);
            assert!(resolved_hanging_get.is_ready());

            continuation = match resolved_hanging_get {
                core::task::Poll::Pending => {
                    unreachable!("we asserted that resolved_hanging_get was ready")
                }
                core::task::Poll::Ready((watch_logs_result, log_event_result)) => {
                    assert_eq!(
                        log_event_result.expect("expected log event to succeed"),
                        fidl_fuchsia_metrics::Status::Ok
                    );

                    match watch_logs_result {
                        futures::future::Either::Left(_services_future_returned) => unreachable!(
                            "unexpected services future return (cannot be formatted with the \
                             default formatter)"
                        ),
                        futures::future::Either::Right((
                            cobalt_query_result,
                            services_continuation,
                        )) => {
                            let (mut logged_events, more) = cobalt_query_result
                                .expect("expect cobalt query FIDL call to succeed");
                            assert_eq!(logged_events.len(), 1);
                            let mut logged_event = logged_events.pop().unwrap();
                            assert_eq!(logged_event.metric_id, event_metric_id);
                            assert_eq!(logged_event.event_codes.len(), 1);
                            assert_eq!(logged_event.event_codes.pop().unwrap(), event_code);
                            assert_eq!(more, false);
                            services_continuation
                        }
                    }
                }
            };

            assert!(executor.run_until_stalled(&mut continuation).is_pending());
        }
    }
}
