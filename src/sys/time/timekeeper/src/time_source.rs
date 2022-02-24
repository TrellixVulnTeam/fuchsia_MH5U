// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{anyhow, Context as _, Error},
    fidl_fuchsia_time_external::{self as ftexternal, PushSourceProxy, Status},
    fuchsia_async::futures::Stream,
    fuchsia_component::client::{launch, launcher, App},
    fuchsia_zircon as zx,
    futures::{stream::Select, FutureExt, StreamExt, TryFutureExt},
    log::info,
    std::{fmt::Debug, pin::Pin, sync::Arc},
};

/// A time sample received from a source of time.
#[derive(Debug, PartialEq, Clone, Copy)]
pub struct Sample {
    /// The UTC time.
    pub utc: zx::Time,
    /// The monotonic time at which the UTC was most valid.
    pub monotonic: zx::Time,
    /// The standard deviation of the UTC error.
    pub std_dev: zx::Duration,
}

#[cfg(test)]
impl Sample {
    /// Constructs a new `Sample`.
    pub fn new(utc: zx::Time, monotonic: zx::Time, std_dev: zx::Duration) -> Sample {
        Sample { utc, monotonic, std_dev }
    }
}

/// An event that may be observed from a source of time.
#[derive(Debug, PartialEq, Clone, Copy)]
pub enum Event {
    /// The status of the time source changed.
    StatusChange {
        /// The current status of the time source.
        status: Status,
    },
    /// The time source produced a new time sample.
    Sample(Sample),
}

impl From<Sample> for Event {
    fn from(sample: Sample) -> Event {
        Event::Sample(sample)
    }
}

/// A definition of a time source that may subsequently be launched to create a stream of update
/// events.
pub trait TimeSource: Send + Sync + Debug {
    /// The type of `Stream` produced when launching the `TimeSource`.
    type EventStream: Stream<Item = Result<Event, Error>> + Unpin + Send;

    /// Attempts to launch the time source and return a stream of its time output and status
    /// change events.
    fn launch(&self) -> Result<Self::EventStream, Error>;
}

/// A time source that communicates using the `fuchsia.time.external.PushSource` protocol.
#[derive(Debug)]
pub struct PushTimeSource {
    /// The fully qualified name of the component to launch.
    component: String,
}

/// The `Stream` of events produced by a `PushTimeSource`
type PushTimeSourceEventStream = Select<
    Pin<Box<dyn Stream<Item = Result<Event, Error>> + Send>>,
    Pin<Box<dyn Stream<Item = Result<Event, Error>> + Send>>,
>;

impl PushTimeSource {
    /// Creates a new `PushTimeSource` using the supplied component name.
    pub fn new(component: String) -> Self {
        PushTimeSource { component }
    }

    /// Returns a stream of time output and status change events received using the supplied
    /// `PushSourceProxy`, retaining the optional `App` for the same lifetime.
    fn events_from_proxy(app: Option<App>, proxy: PushSourceProxy) -> PushTimeSourceEventStream {
        // Store the App in a tuple with the PushSourceProxy to ensure it remains in scope.
        let app_and_proxy = Arc::new((app, proxy));
        let app_and_proxy_clone = Arc::clone(&app_and_proxy);

        let status_stream = futures::stream::try_unfold(app_and_proxy, |app_and_proxy| {
            app_and_proxy
                .1
                .watch_status()
                .map_ok(move |status| Some((Event::StatusChange { status }, app_and_proxy)))
                .err_into()
        });

        let sample_stream = futures::stream::try_unfold(app_and_proxy_clone, |app_and_proxy| {
            app_and_proxy.1.watch_sample().map(move |result| match result {
                Ok(sample) => match (sample.utc, sample.monotonic, sample.standard_deviation) {
                    (None, _, _) => Err(anyhow!("sample missing utc")),
                    (_, None, _) => Err(anyhow!("sample missing monotonic")),
                    (_, _, None) => Err(anyhow!("sample missing standard deviation")),
                    (Some(utc), Some(monotonic), Some(std_dev)) => Ok(Some((
                        Event::Sample(Sample {
                            utc: zx::Time::from_nanos(utc),
                            monotonic: zx::Time::from_nanos(monotonic),
                            std_dev: zx::Duration::from_nanos(std_dev),
                        }),
                        app_and_proxy,
                    ))),
                },
                Err(err) => Err(err.into()),
            })
        });

        futures::stream::select(status_stream.boxed(), sample_stream.boxed())
    }
}

impl TimeSource for PushTimeSource {
    type EventStream = PushTimeSourceEventStream;

    fn launch(&self) -> Result<Self::EventStream, Error> {
        let launcher = launcher().context("starting launcher")?;
        info!("Launching PushTimeSource at {}", self.component);
        let app = launch(&launcher, self.component.clone(), None)
            .context(format!("launching push source {}", self.component))?;
        let proxy = app.connect_to_protocol::<ftexternal::PushSourceMarker>()?;
        Ok(PushTimeSource::events_from_proxy(Some(app), proxy))
    }
}

#[cfg(test)]
use {futures::stream, parking_lot::Mutex};

/// A time source that immediately produces a collections of events supplied at construction.
/// The time source may be launched multiple times and will return a different collection of events
/// on each launch. It will return pending after the last event in the last collection, and will
/// terminate the stream after the last event in all other collections. The time source will return
/// an error if asked to launch after the last collection of events has been returned.
#[cfg(test)]
pub struct FakeTimeSource {
    /// The collections of events to return. The TimeSource will return pending after the last
    /// event in the last collection, and will terminate the stream after the last event in all
    /// other collections.
    collections: Mutex<Vec<Vec<Result<Event, Error>>>>,
}

#[cfg(test)]
impl FakeTimeSource {
    /// Creates a new `FakeTimeSource` that produces the supplied single collection of successful
    /// events.
    pub fn events(events: Vec<Event>) -> Self {
        FakeTimeSource {
            collections: Mutex::new(vec![events.into_iter().map(|evt| Ok(evt)).collect()]),
        }
    }

    /// Creates a new `FakeTimeSource` that produces the supplied collections of successful events.
    pub fn event_collections(event_collections: Vec<Vec<Event>>) -> Self {
        FakeTimeSource {
            collections: Mutex::new(
                event_collections
                    .into_iter()
                    .map(|collection| collection.into_iter().map(|evt| Ok(evt)).collect())
                    .collect(),
            ),
        }
    }

    /// Creates a new `FakeTimeSource` that produces the supplied collections of results.
    pub fn result_collections(result_collections: Vec<Vec<Result<Event, Error>>>) -> Self {
        FakeTimeSource { collections: Mutex::new(result_collections) }
    }

    /// Creates a new `FakeTimeSource` that always fails to launch.
    pub fn failing() -> Self {
        FakeTimeSource { collections: Mutex::new(vec![]) }
    }
}

#[cfg(test)]
impl Debug for FakeTimeSource {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter.write_str("FakeTimeSource")
    }
}

#[cfg(test)]
impl TimeSource for FakeTimeSource {
    type EventStream = Pin<Box<dyn Stream<Item = Result<Event, Error>> + Send>>;

    fn launch(&self) -> Result<Self::EventStream, Error> {
        let mut lock = self.collections.lock();
        if lock.is_empty() {
            return Err(anyhow!("FakeTimeSource sent all supplied event collections"));
        }
        let events = lock.remove(0);
        // Return a pending after the last event if this was the last collection.
        if lock.is_empty() {
            Ok(stream::iter(events).chain(stream::pending()).boxed())
        } else {
            Ok(stream::iter(events).boxed())
        }
    }
}

#[cfg(test)]
mod test {
    use {super::*, fidl::prelude::*, fuchsia_async as fasync, lazy_static::lazy_static};

    const STATUS_1: Status = Status::Initializing;
    const SAMPLE_1_UTC_NANOS: i64 = 1234567;
    const SAMPLE_1_MONO_NANOS: i64 = 222;
    const SAMPLE_1_STD_DEV_NANOS: i64 = 8888;

    lazy_static! {
        static ref STATUS_EVENT_1: Event = Event::StatusChange { status: STATUS_1 };
        static ref SAMPLE_EVENT_1: Event = Event::from(Sample {
            utc: zx::Time::from_nanos(SAMPLE_1_UTC_NANOS),
            monotonic: zx::Time::from_nanos(SAMPLE_1_MONO_NANOS),
            std_dev: zx::Duration::from_nanos(SAMPLE_1_STD_DEV_NANOS),
        });
        static ref SAMPLE_EVENT_2: Event = Event::from(Sample {
            utc: zx::Time::from_nanos(12345678),
            monotonic: zx::Time::from_nanos(333),
            std_dev: zx::Duration::from_nanos(9999),
        });
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn single_event_set() -> Result<(), Error> {
        let fake = FakeTimeSource::events(vec![*STATUS_EVENT_1, *SAMPLE_EVENT_1, *SAMPLE_EVENT_2]);
        let mut events = fake.launch().context("Fake should launch without error")?;
        assert_eq!(events.next().await.unwrap().unwrap(), *STATUS_EVENT_1);
        assert_eq!(events.next().await.unwrap().unwrap(), *SAMPLE_EVENT_1);
        assert_eq!(events.next().await.unwrap().unwrap(), *SAMPLE_EVENT_2);
        // Making another call should lead to a stall and hence panic. We don't test this to
        // avoid a degenerate test, but do in fake_no_events_then_pending.
        assert!(fake.launch().is_err());
        Ok(())
    }

    #[fuchsia::test(allow_stalls = false)]
    async fn double_event_set() -> Result<(), Error> {
        let fake = FakeTimeSource::event_collections(vec![
            vec![*STATUS_EVENT_1, *SAMPLE_EVENT_1],
            vec![*SAMPLE_EVENT_2],
        ]);
        let mut events = fake.launch().context("Fake should launch without error")?;
        assert_eq!(events.next().await.unwrap().unwrap(), *STATUS_EVENT_1);
        assert_eq!(events.next().await.unwrap().unwrap(), *SAMPLE_EVENT_1);
        assert!(events.next().await.is_none());
        let mut events = fake.launch().context("Fake should relaunch without error")?;
        assert_eq!(events.next().await.unwrap().unwrap(), *SAMPLE_EVENT_2);
        // Making another call should lead to a stall and hence panic. We don't test this to
        // avoid a degenerate test, but do in fake_no_events_then_pending.
        assert!(fake.launch().is_err());
        Ok(())
    }

    #[fuchsia::test(allow_stalls = false)]
    #[should_panic]
    async fn fake_no_events_then_pending() {
        let fake = FakeTimeSource::events(vec![]);
        let mut events = fake.launch().unwrap();
        // Getting an event from the last collection should never complete, leading to a stall.
        events.next().await;
    }

    #[fuchsia::test]
    fn fake_failing() {
        let fake = FakeTimeSource::failing();
        assert!(fake.launch().is_err());
    }

    #[fuchsia::test]
    fn new_push_time_source() {
        const COMPONENT_NAME: &str = "alfred";
        let time_source = PushTimeSource::new(COMPONENT_NAME.to_string());
        assert_eq!(time_source.component, COMPONENT_NAME);
    }

    #[fuchsia::test]
    async fn push_time_source_events() {
        let (proxy, mut requests) =
            fidl::endpoints::create_proxy_and_stream::<ftexternal::PushSourceMarker>().unwrap();

        let _task = fasync::Task::spawn(async move {
            while let Some(Ok(request)) = requests.next().await {
                match request {
                    ftexternal::PushSourceRequest::WatchStatus { responder, .. } => {
                        responder.send(STATUS_1).unwrap();
                    }
                    ftexternal::PushSourceRequest::WatchSample { responder, .. } => {
                        let sample = ftexternal::TimeSample {
                            utc: Some(SAMPLE_1_UTC_NANOS),
                            monotonic: Some(SAMPLE_1_MONO_NANOS),
                            standard_deviation: Some(SAMPLE_1_STD_DEV_NANOS),
                            ..ftexternal::TimeSample::EMPTY
                        };
                        responder.send(sample).unwrap();
                    }
                    _ => {}
                };
            }
        });

        let mut events = PushTimeSource::events_from_proxy(None, proxy);
        // We expect to receive both events but the ordering is not deterministic.
        let event1 = events.next().await.unwrap().unwrap();
        let event2 = events.next().await.unwrap().unwrap();
        match event1 {
            Event::StatusChange { status: _ } => {
                assert_eq!(event1, *STATUS_EVENT_1);
                assert_eq!(event2, *SAMPLE_EVENT_1);
            }
            Event::Sample(_) => {
                assert_eq!(event1, *SAMPLE_EVENT_1);
                assert_eq!(event2, *STATUS_EVENT_1);
            }
        }
    }

    #[fuchsia::test]
    async fn push_time_source_failure() {
        let (proxy, mut requests) =
            fidl::endpoints::create_proxy_and_stream::<ftexternal::PushSourceMarker>().unwrap();

        let _task = fasync::Task::spawn(async move {
            while let Some(Ok(request)) = requests.next().await {
                // Close the channel on the first watch status request.
                match request {
                    ftexternal::PushSourceRequest::WatchStatus { responder, .. } => {
                        responder.control_handle().shutdown_with_epitaph(zx::Status::BAD_STATE);
                    }
                    _ => {}
                };
            }
        });

        let mut events = PushTimeSource::events_from_proxy(None, proxy);
        assert!(events.next().await.unwrap().is_err());
    }
}
