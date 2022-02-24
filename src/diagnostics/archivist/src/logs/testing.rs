// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::{
    events::types::{Event, EventPayload, LogSinkRequestedPayload},
    identity::ComponentIdentity,
    logs::budget::BudgetManager,
    repository::DataRepo,
};
use async_trait::async_trait;
use diagnostics_log_encoding::{encode::Encoder, Record};
use diagnostics_message::{fx_log_packet_t, MAX_DATAGRAM_LEN};
use fidl::endpoints::ProtocolMarker;
use fidl_fuchsia_io::DirectoryProxy;
use fidl_fuchsia_logger::{
    LogFilterOptions, LogLevelFilter, LogMarker, LogMessage, LogProxy, LogSinkMarker, LogSinkProxy,
};
use fidl_fuchsia_sys2 as fsys;
use fuchsia_async as fasync;
use fuchsia_async::Task;
use fuchsia_component::client::connect_to_protocol_at_dir_svc;
use fuchsia_inspect::Inspector;
use fuchsia_syslog_listener::{run_log_listener_with_proxy, LogProcessor};
use fuchsia_zircon as zx;
use futures::channel::mpsc;
use futures::prelude::*;
use std::{
    collections::VecDeque,
    convert::TryInto,
    io::Cursor,
    marker::PhantomData,
    sync::{Arc, Weak},
};
use validating_log_listener::{validate_log_dump, validate_log_stream};

pub struct TestHarness {
    inspector: Inspector,
    log_manager: DataRepo,
    log_proxy: LogProxy,
    /// weak pointers to "pending" TestStreams which haven't dropped yet
    pending_streams: Vec<Weak<()>>,
    /// LogSinks to retain for inspect attribution tests
    sinks: Option<Vec<LogSinkProxy>>,
}

pub fn create_log_sink_requested_event(
    target_moniker: String,
    target_url: String,
    capability: zx::Channel,
) -> fsys::Event {
    fsys::Event {
        header: Some(fsys::EventHeader {
            event_type: Some(fsys::EventType::CapabilityRequested),
            moniker: Some(target_moniker),
            component_url: Some(target_url),
            timestamp: Some(zx::Time::get_monotonic().into_nanos()),
            ..fsys::EventHeader::EMPTY
        }),
        event_result: Some(fsys::EventResult::Payload(fsys::EventPayload::CapabilityRequested(
            fsys::CapabilityRequestedPayload {
                name: Some(LogSinkMarker::NAME.into()),
                capability: Some(capability),
                ..fsys::CapabilityRequestedPayload::EMPTY
            },
        ))),
        ..fsys::Event::EMPTY
    }
}

impl TestHarness {
    pub fn new() -> Self {
        Self::make(false)
    }

    /// Create a new test harness which will keep its LogSinks alive as long as it itself is,
    /// useful for testing inspect hierarchies for attribution.
    // TODO(fxbug.dev/53932) this will be made unnecessary by historical retention of component stats
    pub fn with_retained_sinks() -> Self {
        Self::make(true)
    }

    fn make(hold_sinks: bool) -> Self {
        let inspector = Inspector::new();
        let budget = BudgetManager::new(1_000_000);
        let log_manager = DataRepo::new(&budget, inspector.root());

        let (listen_sender, listen_receiver) = mpsc::unbounded();
        let (log_proxy, log_stream) =
            fidl::endpoints::create_proxy_and_stream::<LogMarker>().unwrap();

        log_manager.clone().handle_log(log_stream, listen_sender);
        fasync::Task::spawn(
            listen_receiver.for_each_concurrent(None, |rx| async move { rx.await }),
        )
        .detach();

        Self {
            inspector,
            log_manager,
            log_proxy,
            pending_streams: vec![],
            sinks: if hold_sinks { Some(vec![]) } else { None },
        }
    }

    pub fn create_default_reader(&self, identity: ComponentIdentity) -> Arc<dyn LogReader> {
        DefaultLogReader::new(self.log_manager.clone(), Arc::new(identity))
    }

    pub fn create_event_stream_reader(
        &self,
        target_moniker: impl Into<String>,
        target_url: impl Into<String>,
    ) -> Arc<dyn LogReader> {
        EventStreamLogReader::new(self.log_manager.clone(), target_moniker, target_url)
    }

    /// Check to make sure all `TestStream`s have been dropped. This ensures that we repeatedly test
    /// the case where a socket's write half is dropped before the socket is drained.
    fn check_pending_streams(&mut self) {
        self.pending_streams.retain(|w| w.upgrade().is_some());
        assert_eq!(
            self.pending_streams.len(),
            0,
            "drop all test streams before filter_test() to test crashed writer behavior"
        );
    }

    /// Run a filter test, returning the Inspector to check Inspect output.
    pub async fn filter_test(
        mut self,
        expected: impl IntoIterator<Item = LogMessage>,
        filter_options: Option<LogFilterOptions>,
    ) -> Inspector {
        self.check_pending_streams();
        validate_log_stream(expected, self.log_proxy, filter_options).await;
        self.inspector
    }

    pub async fn manager_test(mut self, test_dump_logs: bool) {
        let mut p = setup_default_packet();
        let lm1 = LogMessage {
            time: p.metadata.time,
            pid: p.metadata.pid,
            tid: p.metadata.tid,
            dropped_logs: p.metadata.dropped_logs,
            severity: p.metadata.severity,
            msg: String::from("BBBBB"),
            tags: vec![String::from("AAAAA")],
        };
        let mut lm2 = copy_log_message(&lm1);
        let mut lm3 = copy_log_message(&lm1);
        let mut stream = self.create_stream(Arc::new(ComponentIdentity::unknown()));
        stream.write_packet(&mut p);

        p.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        lm2.severity = LogLevelFilter::Info.into_primitive().into();
        lm3.severity = LogLevelFilter::Info.into_primitive().into();
        stream.write_packet(&mut p);

        p.metadata.pid = 2;
        lm3.pid = 2;
        stream.write_packet(&mut p);
        drop(stream);
        self.check_pending_streams();
        if test_dump_logs {
            validate_log_dump(vec![lm1, lm2, lm3], self.log_proxy, None).await;
        } else {
            validate_log_stream(vec![lm1, lm2, lm3], self.log_proxy, None).await;
        }
    }

    /// Create a [`TestStream`] which should be dropped before calling `filter_test` or
    /// `manager_test`.
    pub fn create_stream(
        &mut self,
        identity: Arc<ComponentIdentity>,
    ) -> TestStream<LogPacketWriter> {
        self.make_stream(DefaultLogReader::new(self.log_manager.clone(), identity))
    }

    /// Create a [`TestStream`] which should be dropped before calling `filter_test` or
    /// `manager_test`.
    pub fn create_stream_from_log_reader(
        &mut self,
        log_reader: Arc<dyn LogReader>,
    ) -> TestStream<LogPacketWriter> {
        self.make_stream(log_reader)
    }

    /// Create a [`TestStream`] which should be dropped before calling `filter_test` or
    /// `manager_test`.
    pub fn create_structured_stream(
        &mut self,
        identity: Arc<ComponentIdentity>,
    ) -> TestStream<StructuredMessageWriter> {
        self.make_stream(DefaultLogReader::new(self.log_manager.clone(), identity))
    }

    fn make_stream<E, P>(&mut self, log_reader: Arc<dyn LogReader>) -> TestStream<E>
    where
        E: LogWriter<Packet = P>,
    {
        let (log_sender, log_receiver) = mpsc::unbounded();
        let _log_sink_proxy = log_reader.handle_request(log_sender);

        fasync::Task::spawn(log_receiver.for_each_concurrent(None, |rx| async move { rx.await }))
            .detach();

        let (sin, sout) = zx::Socket::create(zx::SocketOpts::DATAGRAM).unwrap();
        E::connect(&_log_sink_proxy, sout);

        let _alive = Arc::new(());
        self.pending_streams.push(Arc::downgrade(&_alive));

        if let Some(sinks) = self.sinks.as_mut() {
            sinks.push(_log_sink_proxy.clone());
        }

        TestStream { _alive, _log_sink_proxy, sin, _encoder: PhantomData }
    }
}

/// A `LogWriter` can connect to and send `Packets` to a LogSink over a socket.
pub trait LogWriter {
    type Packet;
    fn connect(log_sink: &LogSinkProxy, sout: zx::Socket);

    fn write(sout: &zx::Socket, packet: &Self::Packet);
}

/// A `LogWriter` that writes `fx_log_packet_t` to a LogSink in the syslog
/// format.
pub struct LogPacketWriter;

/// A `LogWriter` that writes `Record` to a LogSink in the structured
/// log format.
pub struct StructuredMessageWriter;

impl LogWriter for LogPacketWriter {
    type Packet = fx_log_packet_t;

    fn connect(log_sink: &LogSinkProxy, sout: zx::Socket) {
        log_sink.connect(sout).expect("unable to connect out socket to log sink");
    }

    fn write(sin: &zx::Socket, packet: &fx_log_packet_t) {
        sin.write(packet.as_bytes()).unwrap();
    }
}

impl LogWriter for StructuredMessageWriter {
    type Packet = Record;

    fn connect(log_sink: &LogSinkProxy, sin: zx::Socket) {
        log_sink.connect_structured(sin).expect("unable to connect out socket to log sink");
    }

    fn write(sin: &zx::Socket, record: &Record) {
        let mut buffer = Cursor::new(vec![0; MAX_DATAGRAM_LEN]);
        let mut encoder = Encoder::new(&mut buffer);
        encoder.write_record(record).unwrap();
        let slice = buffer.get_ref().as_slice();
        sin.write(slice).unwrap();
    }
}

/// A `LogReader` host a LogSink connection.
pub trait LogReader {
    fn handle_request(&self, sender: mpsc::UnboundedSender<Task<()>>) -> LogSinkProxy;
}

// A LogReader that exercises the handle_log_sink code path.
pub struct DefaultLogReader {
    log_manager: DataRepo,
    identity: Arc<ComponentIdentity>,
}

impl DefaultLogReader {
    fn new(log_manager: DataRepo, identity: Arc<ComponentIdentity>) -> Arc<dyn LogReader> {
        Arc::new(Self { log_manager, identity })
    }
}

impl LogReader for DefaultLogReader {
    fn handle_request(&self, log_sender: mpsc::UnboundedSender<Task<()>>) -> LogSinkProxy {
        let (log_sink_proxy, log_sink_stream) =
            fidl::endpoints::create_proxy_and_stream::<LogSinkMarker>().unwrap();
        let container = self.log_manager.write().get_log_container((*self.identity).clone());
        container.handle_log_sink(log_sink_stream, log_sender);
        log_sink_proxy
    }
}

// A LogReader that exercises the components v2 EventStream and CapabilityRequested event
// code path for log attribution.
pub struct EventStreamLogReader {
    log_manager: DataRepo,
    target_moniker: String,
    target_url: String,
}

impl EventStreamLogReader {
    fn new(
        log_manager: DataRepo,
        target_moniker: impl Into<String>,
        target_url: impl Into<String>,
    ) -> Arc<dyn LogReader> {
        Arc::new(Self {
            log_manager,
            target_moniker: target_moniker.into(),
            target_url: target_url.into(),
        })
    }

    async fn handle_event_stream(
        mut stream: fsys::EventStreamRequestStream,
        sender: mpsc::UnboundedSender<Task<()>>,
        log_manager: DataRepo,
    ) {
        while let Ok(Some(request)) = stream.try_next().await {
            match request {
                fsys::EventStreamRequest::OnEvent { event, .. } => {
                    Self::handle_event(event, sender.clone(), log_manager.clone())
                }
            }
        }
    }

    fn handle_event(
        event: fsys::Event,
        sender: mpsc::UnboundedSender<Task<()>>,
        log_manager: DataRepo,
    ) {
        let LogSinkRequestedPayload { component, request_stream } =
            match event.try_into().expect("into component event") {
                Event { payload: EventPayload::LogSinkRequested(payload), .. } => payload,
                other => unreachable!("should never see {:?} here", other),
            };
        let container = log_manager.write().get_log_container(component);
        container.handle_log_sink(request_stream.unwrap(), sender);
    }
}

impl LogReader for EventStreamLogReader {
    fn handle_request(&self, log_sender: mpsc::UnboundedSender<Task<()>>) -> LogSinkProxy {
        let (event_stream_proxy, event_stream) =
            fidl::endpoints::create_proxy_and_stream::<fsys::EventStreamMarker>().unwrap();
        let (log_sink_proxy, log_sink_server_end) =
            fidl::endpoints::create_proxy::<LogSinkMarker>().unwrap();
        event_stream_proxy
            .on_event(create_log_sink_requested_event(
                self.target_moniker.clone(),
                self.target_url.clone(),
                log_sink_server_end.into_channel(),
            ))
            .unwrap();
        let task = Task::spawn(Self::handle_event_stream(
            event_stream,
            log_sender.clone(),
            self.log_manager.clone(),
        ));
        log_sender.unbounded_send(task).unwrap();

        log_sink_proxy
    }
}

pub struct TestStream<E> {
    sin: zx::Socket,
    _alive: Arc<()>,
    _log_sink_proxy: LogSinkProxy,
    _encoder: PhantomData<E>,
}

impl<E, P> TestStream<E>
where
    E: LogWriter<Packet = P>,
{
    pub fn write_packets(&mut self, packets: Vec<P>) {
        for p in packets {
            self.write_packet(&p);
        }
    }

    pub fn write_packet(&mut self, packet: &P) {
        E::write(&self.sin, packet);
    }
}

/// Run a test on logs from klog, returning the inspector object.
pub async fn debuglog_test(
    expected: impl IntoIterator<Item = LogMessage>,
    debug_log: TestDebugLog,
) -> Inspector {
    let (log_sender, log_receiver) = mpsc::unbounded();
    fasync::Task::spawn(log_receiver.for_each_concurrent(None, |rx| async move { rx.await }))
        .detach();

    let inspector = Inspector::new();
    let budget = BudgetManager::new(1_000_000);
    let lm = DataRepo::new(&budget, inspector.root());
    let (log_proxy, log_stream) = fidl::endpoints::create_proxy_and_stream::<LogMarker>().unwrap();
    lm.clone().handle_log(log_stream, log_sender);
    fasync::Task::spawn(lm.drain_debuglog(debug_log)).detach();

    validate_log_stream(expected, log_proxy, None).await;
    inspector
}

pub fn setup_default_packet() -> fx_log_packet_t {
    let mut p: fx_log_packet_t = Default::default();
    p.metadata.pid = 1;
    p.metadata.tid = 1;
    p.metadata.severity = LogLevelFilter::Warn.into_primitive().into();
    p.metadata.dropped_logs = 2;
    p.data[0] = 5;
    p.fill_data(1..6, 65);
    p.fill_data(7..12, 66);
    return p;
}

pub fn copy_log_message(log_message: &LogMessage) -> LogMessage {
    LogMessage {
        pid: log_message.pid,
        tid: log_message.tid,
        time: log_message.time,
        severity: log_message.severity,
        dropped_logs: log_message.dropped_logs,
        tags: log_message.tags.clone(),
        msg: log_message.msg.clone(),
    }
}

/// A fake reader that returns enqueued responses on read.
pub struct TestDebugLog {
    read_responses: parking_lot::Mutex<VecDeque<ReadResponse>>,
}
type ReadResponse = Result<zx::sys::zx_log_record_t, zx::Status>;

#[async_trait]
impl crate::logs::debuglog::DebugLog for TestDebugLog {
    async fn read(&self) -> Result<zx::sys::zx_log_record_t, zx::Status> {
        self.read_responses.lock().pop_front().expect("Got more read requests than enqueued")
    }

    async fn ready_signal(&self) -> Result<(), zx::Status> {
        if self.read_responses.lock().is_empty() {
            // ready signal should never complete if we have no logs left.
            futures::future::pending().await
        }
        Ok(())
    }
}

impl TestDebugLog {
    pub fn new() -> Self {
        TestDebugLog { read_responses: parking_lot::Mutex::new(VecDeque::new()) }
    }

    pub fn enqueue_read(&self, response: zx::sys::zx_log_record_t) {
        self.read_responses.lock().push_back(Ok(response));
    }

    pub fn enqueue_read_entry(&self, entry: &TestDebugEntry) {
        self.enqueue_read(entry.record);
    }

    pub fn enqueue_read_fail(&self, error: zx::Status) {
        self.read_responses.lock().push_back(Err(error))
    }
}

pub struct TestDebugEntry {
    pub record: zx::sys::zx_log_record_t,
}

pub const TEST_KLOG_FLAGS: u8 = 47;
pub const TEST_KLOG_TIMESTAMP: i64 = 12345i64;
pub const TEST_KLOG_PID: u64 = 0xad01u64;
pub const TEST_KLOG_TID: u64 = 0xbe02u64;

impl TestDebugEntry {
    pub fn new(log_data: &[u8]) -> Self {
        let mut rec = zx::sys::zx_log_record_t::default();
        let len = rec.data.len().min(log_data.len());
        rec.sequence = 0;
        rec.datalen = len as u16;
        rec.flags = TEST_KLOG_FLAGS;
        rec.timestamp = TEST_KLOG_TIMESTAMP;
        rec.pid = TEST_KLOG_PID;
        rec.tid = TEST_KLOG_TID;
        rec.data[..len].copy_from_slice(&log_data[..len]);
        TestDebugEntry { record: rec }
    }
}

/// Helper to connect to log sink and make it easy to write logs to socket.
pub struct LogSinkHelper {
    log_sink: Option<LogSinkProxy>,
    sock: Option<zx::Socket>,
}

impl LogSinkHelper {
    pub fn new(directory: &DirectoryProxy) -> Self {
        let log_sink = connect_to_protocol_at_dir_svc::<LogSinkMarker>(&directory)
            .expect("cannot connect to log sink");
        let mut s = Self { log_sink: Some(log_sink), sock: None };
        s.sock = Some(s.connect());
        s
    }

    pub fn connect(&self) -> zx::Socket {
        let (sin, sout) =
            zx::Socket::create(zx::SocketOpts::DATAGRAM).expect("Cannot create socket");
        self.log_sink.as_ref().unwrap().connect(sin).expect("unable to send socket to log sink");
        sout
    }

    /// kills current sock and creates new connection.
    pub fn add_new_connection(&mut self) {
        self.kill_sock();
        self.sock = Some(self.connect());
    }

    pub fn kill_sock(&mut self) {
        self.sock.take();
    }

    pub fn write_log(&self, msg: &str) {
        Self::write_log_at(self.sock.as_ref().unwrap(), msg);
    }

    pub fn write_log_at(sock: &zx::Socket, msg: &str) {
        let mut p: fx_log_packet_t = Default::default();
        p.metadata.pid = 1;
        p.metadata.tid = 1;
        p.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        p.metadata.dropped_logs = 0;
        p.data[0] = 0;
        p.add_data(1, msg.as_bytes());

        sock.write(&mut p.as_bytes()).unwrap();
    }

    pub fn kill_log_sink(&mut self) {
        self.log_sink.take();
    }
}

struct Listener {
    send_logs: mpsc::UnboundedSender<String>,
}

impl LogProcessor for Listener {
    fn log(&mut self, message: LogMessage) {
        self.send_logs.unbounded_send(message.msg).unwrap();
    }

    fn done(&mut self) {
        panic!("this should not be called");
    }
}

pub fn start_listener(directory: &DirectoryProxy) -> mpsc::UnboundedReceiver<String> {
    let log_proxy = connect_to_protocol_at_dir_svc::<LogMarker>(&directory)
        .expect("cannot connect to log proxy");
    let (send_logs, recv_logs) = mpsc::unbounded();
    let mut options = LogFilterOptions {
        filter_by_pid: false,
        pid: 0,
        min_severity: LogLevelFilter::None,
        verbosity: 0,
        filter_by_tid: false,
        tid: 0,
        tags: vec![],
    };
    let l = Listener { send_logs };
    fasync::Task::spawn(async move {
        run_log_listener_with_proxy(&log_proxy, l, Some(&mut options), false, None).await.unwrap();
    })
    .detach();

    recv_logs
}
