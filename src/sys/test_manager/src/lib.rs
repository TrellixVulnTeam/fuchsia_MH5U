// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        debug_data_server::{serve_debug_data, DebugDataFile},
        diagnostics::IsolatedLogsProvider,
        error::*,
    },
    anyhow::{anyhow, format_err, Context, Error},
    cm_rust,
    diagnostics_bridge::ArchiveReaderManager,
    fdiagnostics::ArchiveAccessorProxy,
    fidl::endpoints::{create_endpoints, create_proxy, ClientEnd},
    fidl::prelude::*,
    fidl_fuchsia_component_decl as fdecl,
    fidl_fuchsia_component_test::Capability2 as RBCapability,
    fidl_fuchsia_debugdata as fdebugdata, fidl_fuchsia_diagnostics as fdiagnostics,
    fidl_fuchsia_io as fio, fidl_fuchsia_sys as fv1sys, fidl_fuchsia_sys2 as fsys,
    fidl_fuchsia_test as ftest, fidl_fuchsia_test_internal as ftest_internal,
    fidl_fuchsia_test_manager as ftest_manager,
    fsys::ComponentResolverProxy,
    ftest::Invocation,
    ftest_manager::{
        CaseStatus, DebugDataIteratorMarker, LaunchError, RunControllerRequest,
        RunControllerRequestStream, RunEvent as FidlRunEvent,
        RunEventPayload as FidlRunEventPayload, SuiteControllerRequest,
        SuiteControllerRequestStream, SuiteEvent as FidlSuiteEvent,
        SuiteEventPayload as FidlSuiteEventPayload, SuiteStatus,
    },
    fuchsia_async::{self as fasync, TimeoutExt},
    fuchsia_component::client::{connect_channel_to_protocol, connect_to_protocol},
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::{
        error::Error as RealmBuilderError,
        new::{
            Capability, ChildOptions, Event, LocalComponentHandles, RealmBuilder, RealmInstance,
            Ref, Route, SubRealmBuilder,
        },
    },
    fuchsia_url::pkg_url::PkgUrl,
    fuchsia_zircon as zx,
    futures::{
        channel::{mpsc, oneshot},
        future::join_all,
        lock,
        prelude::*,
        StreamExt,
    },
    io_util,
    lazy_static::lazy_static,
    maplit::hashset,
    moniker::RelativeMonikerBase,
    std::{
        collections::{HashMap, HashSet},
        convert::{TryFrom, TryInto},
        sync::{
            atomic::{AtomicU32, AtomicU64, Ordering},
            Arc, Weak,
        },
    },
    thiserror::Error,
    tracing::{debug, error, info, warn},
};

mod debug_data_server;
mod diagnostics;
mod error;
mod facet;
mod resolver;
mod self_diagnostics;

pub use self_diagnostics::RootInspectNode;

pub const TEST_ROOT_REALM_NAME: &'static str = "test_root";
const WRAPPER_REALM_NAME: &'static str = "test_wrapper";
const ARCHIVIST_REALM_NAME: &'static str = "archivist";
const MOCKS_SERVER_REALM_NAME: &'static str = "mocks-server";
const ENCLOSING_ENV_REALM_NAME: &'static str = "enclosing_env";

const ARCHIVIST_FOR_EMBEDDING_URL: &'static str =
    "fuchsia-pkg://fuchsia.com/test_manager#meta/archivist-for-embedding.cm";
const HERMETIC_RESOLVER_REALM_NAME: &'static str = "hermetic_resolver";
const HERMETIC_RESOLVER_CAPABILITY_NAME: &'static str = "hermetic_resolver";
const HERMETIC_ENVIRONMENT_NAME: &'static str = "hermetic";
const HERMETIC_TESTS_COLLECTION: &'static str = "tests";
const HERMETIC_TIER_2_TESTS_COLLECTION: &'static str = "tier-2-tests";
const SYSTEM_TESTS_COLLECTION: &'static str = "system-tests";
const CTS_TESTS_COLLECTION: &'static str = "cts-tests";
const VULKAN_TESTS_COLLECTION: &'static str = "vulkan-tests";
const CHROMIUM_TESTS_COLLECTION: &'static str = "chromium-tests";

lazy_static! {
    static ref TEST_TYPE_REALM_MAP: HashMap<&'static str, &'static str> = [
        ("hermetic", HERMETIC_TESTS_COLLECTION),
        ("hermetic-tier-2", HERMETIC_TIER_2_TESTS_COLLECTION),
        ("system", SYSTEM_TESTS_COLLECTION),
        ("cts", CTS_TESTS_COLLECTION),
        ("vulkan", VULKAN_TESTS_COLLECTION),
        ("chromium", CHROMIUM_TESTS_COLLECTION)
    ]
    .iter()
    .copied()
    .collect();
}

struct Suite {
    test_url: String,
    options: ftest_manager::RunOptions,
    controller: SuiteControllerRequestStream,
    resolver: Arc<ComponentResolverProxy>,
    above_root_capabilities_for_test: Arc<AboveRootCapabilitiesForTest>,
}

struct TestRunBuilder {
    suites: Vec<Suite>,
}

impl TestRunBuilder {
    async fn run_controller(
        controller: &mut RunControllerRequestStream,
        run_task: futures::future::RemoteHandle<()>,
        stop_sender: oneshot::Sender<()>,
        event_recv: mpsc::Receiver<RunEvent>,
        inspect_node: &self_diagnostics::RunInspectNode,
    ) {
        let mut task = Some(run_task);
        let mut stop_sender = Some(stop_sender);
        let mut event_recv = event_recv.fuse();

        let mut stop_or_kill_called = false;
        let mut events_drained = false;
        let mut events_sent_successfully = true;

        // no need to check controller error.
        loop {
            inspect_node
                .set_controller_state(self_diagnostics::RunControllerState::AwaitingRequest);
            let request = match controller.try_next().await {
                Ok(Some(request)) => request,
                _ => break,
            };
            match request {
                RunControllerRequest::Stop { .. } => {
                    stop_or_kill_called = true;
                    if let Some(stop_sender) = stop_sender.take() {
                        // no need to check error.
                        let _ = stop_sender.send(());
                        // after this all `senders` go away and subsequent GetEvent call will
                        // return rest of events and eventually a empty array and will close the
                        // connection after that.
                    }
                }
                RunControllerRequest::Kill { .. } => {
                    stop_or_kill_called = true;
                    // dropping the remote handle cancels it.
                    drop(task.take());
                    // after this all `senders` go away and subsequent GetEvent call will
                    // return rest of events and eventually a empty array and will close the
                    // connection after that.
                }
                RunControllerRequest::GetEvents { responder } => {
                    let mut events = vec![];
                    // TODO(fxbug.dev/91553): This can block handling Stop and Kill requests if no
                    // events are available.
                    inspect_node
                        .set_controller_state(self_diagnostics::RunControllerState::AwaitingEvents);
                    if let Some(event) = event_recv.next().await {
                        events.push(event);
                        while events.len() < EVENTS_THRESHOLD {
                            if let Some(Some(event)) = event_recv.next().now_or_never() {
                                events.push(event);
                            } else {
                                break;
                            }
                        }
                    }
                    let no_events_left = events.is_empty();
                    let response_err =
                        responder.send(&mut events.into_iter().map(RunEvent::into)).is_err();

                    // Order setting these variables matters. Expected is for the client to receive at
                    // least one event response. Client might send more, which is okay but we suppress
                    // response errors after the first empty vec.
                    if !events_drained && response_err {
                        events_sent_successfully = false;
                    }
                    events_drained = no_events_left;
                }
            }
        }
        inspect_node.set_controller_state(self_diagnostics::RunControllerState::Done {
            stopped_or_killed: stop_or_kill_called,
            events_drained,
            events_sent_successfully,
        });
    }

    async fn run(
        self,
        mut controller: RunControllerRequestStream,
        debug_controller: ftest_internal::DebugDataSetControllerProxy,
        debug_iterator: ClientEnd<ftest_manager::DebugDataIteratorMarker>,
        inspect_node: self_diagnostics::RunInspectNode,
    ) {
        let (stop_sender, mut stop_recv) = oneshot::channel::<()>();
        let (event_sender, event_recv) = mpsc::channel::<RunEvent>(16);

        // TODO(fxbug.dev/93350) reenable sending debug data over RunController. This is disabled
        // for now as debug data collection relies on inspecting component lifecycle events to
        // drain connections to DebugData, but we are observing missing events. We'll need to send
        // this over RunController before coverage can be pulled from ffx test instead of over SCP.
        // The infrastructure to collect debug data is left in place so that SCP still works, and
        // to aid debugging missing events.
        let debug_event_task = send_debug_data_if_produced(
            debug_controller.take_event_stream(),
            debug_iterator,
            &inspect_node,
        );
        let inspect_node_ref = &inspect_node;

        // Generate a random number in an attempt to prevent realm name collisions between runs.
        let run_id: u32 = rand::random();
        let run_suites_fut = async move {
            inspect_node_ref.set_execution_state(self_diagnostics::RunExecutionState::Executing);
            // run test suites serially for now
            for (suite_idx, suite) in self.suites.into_iter().enumerate() {
                // only check before running the test. We should complete the test run for
                // running tests, if stop is called.
                if let Ok(Some(())) = stop_recv.try_recv() {
                    break;
                }
                let instance_name = format!("{:?}-{:?}", run_id, suite_idx);
                let suite_inspect = inspect_node_ref.new_suite(&instance_name, &suite.test_url);
                run_single_suite(suite, &debug_controller, &instance_name, suite_inspect).await;
            }
            debug_controller
                .finish()
                .unwrap_or_else(|e| warn!("Error finishing debug data set: {:?}", e));

            // Collect run artifacts
            let mut kernel_debug_tasks = vec![];
            kernel_debug_tasks.push(send_kernel_debug_data(event_sender.clone()));
            join_all(kernel_debug_tasks).await;
            inspect_node_ref.set_execution_state(self_diagnostics::RunExecutionState::Complete);
        };

        let (remote, remote_handle) = run_suites_fut.remote_handle();

        let ((), ()) = futures::future::join(
            remote,
            Self::run_controller(
                &mut controller,
                remote_handle,
                stop_sender,
                event_recv,
                inspect_node_ref,
            ),
        )
        .await;

        // Workaround to prevent zx_peer_closed error
        // TODO(fxbug.dev/87976) once fxbug.dev/87890 is fixed, the controller should be dropped
        // as soon as all events are drained.
        let (inner, _) = controller.into_inner();
        if let Err(e) =
            fasync::OnSignals::new(inner.channel(), zx::Signals::CHANNEL_PEER_CLOSED).await
        {
            warn!("Error waiting for the RunController channel to close: {:?}", e);
        }
        debug_event_task.await;
    }
}

async fn send_debug_data_if_produced(
    mut controller_events: ftest_internal::DebugDataSetControllerEventStream,
    _debug_iterator: ClientEnd<ftest_manager::DebugDataIteratorMarker>,
    inspect_node: &self_diagnostics::RunInspectNode,
) {
    inspect_node.set_debug_data_state(self_diagnostics::DebugDataState::PendingDebugDataProduced);
    match controller_events.next().await {
        Some(Ok(ftest_internal::DebugDataSetControllerEvent::OnDebugDataProduced {})) => {
            inspect_node.set_debug_data_state(self_diagnostics::DebugDataState::DebugDataProduced);
        }
        Some(Err(_)) | None => {
            inspect_node.set_debug_data_state(self_diagnostics::DebugDataState::NoDebugData);
        }
    }
}

async fn send_kernel_debug_data(mut event_sender: mpsc::Sender<RunEvent>) {
    let file = io_util::open_file_in_namespace(
        "/kernel_data/zircon.elf.profraw",
        io_util::OPEN_RIGHT_READABLE,
    );
    match file {
        Ok(file) => match io_util::read_file_bytes(&file).await {
            Ok(contents) => {
                let (client, task) = serve_debug_data(vec![DebugDataFile {
                    name: "zircon.elf.profraw".to_string(),
                    contents,
                }]);
                let _ = event_sender.send(RunEvent::debug_data(client).into()).await;
                task.await;
            }
            Err(e) => {
                warn!("Failed to read kernel profile contents {:?}", e);
            }
        },
        Err(e) => {
            warn!("Failed to open kernel profile contents {:?}", e);
        }
    }
}

fn suite_error(err: fidl::Error) -> anyhow::Error {
    match err {
        fidl::Error::ClientChannelClosed { .. } => anyhow::anyhow!(
            "The test protocol was closed. This may mean `fuchsia.test.Suite` was not \
            configured correctly. Refer to: \
            https://fuchsia.dev/fuchsia-src/development/components/v2/troubleshooting#troubleshoot-test"
        ),
        err => err.into(),
    }
}

/// Enumerates test cases and return invocations.
async fn enumerate_test_cases(
    suite: &ftest::SuiteProxy,
    matcher: Option<&CaseMatcher>,
) -> Result<Vec<Invocation>, anyhow::Error> {
    debug!("enumerating tests");
    let (case_iterator, server_end) =
        fidl::endpoints::create_proxy().expect("cannot create case iterator");
    suite.get_tests(server_end).map_err(suite_error)?;
    let mut invocations = vec![];

    loop {
        let cases = case_iterator.get_next().await.map_err(suite_error)?;
        if cases.is_empty() {
            break;
        }
        for case in cases {
            let case_name = case.name.ok_or(format_err!("invocation should contain a name."))?;
            if matcher.as_ref().map_or(true, |m| m.matches(&case_name)) {
                invocations.push(Invocation {
                    name: Some(case_name),
                    tag: None,
                    ..Invocation::EMPTY
                });
            }
        }
    }

    debug!("invocations: {:#?}", invocations);

    Ok(invocations)
}

#[derive(Debug)]
struct CaseMatcher {
    /// Patterns specifying cases to include.
    includes: Vec<glob::Pattern>,
    /// Patterns specifying cases to exclude.
    excludes: Vec<glob::Pattern>,
}

impl CaseMatcher {
    fn new<T: AsRef<str>>(filters: &Vec<T>) -> Result<Self, anyhow::Error> {
        let mut matcher = CaseMatcher { includes: vec![], excludes: vec![] };
        filters.iter().try_for_each(|filter| {
            match filter.as_ref().chars().next() {
                Some('-') => {
                    let pattern = glob::Pattern::new(&filter.as_ref()[1..])
                        .map_err(|e| format_err!("Bad negative test filter pattern: {}", e))?;
                    matcher.excludes.push(pattern);
                }
                Some(_) | None => {
                    let pattern = glob::Pattern::new(&filter.as_ref())
                        .map_err(|e| format_err!("Bad test filter pattern: {}", e))?;
                    matcher.includes.push(pattern);
                }
            }
            Ok::<(), anyhow::Error>(())
        })?;
        Ok(matcher)
    }

    /// Returns whether or not a case should be run.
    fn matches(&self, case: &str) -> bool {
        let matches_includes = match self.includes.is_empty() {
            true => true,
            false => self.includes.iter().any(|p| p.matches(case)),
        };
        matches_includes && !self.excludes.iter().any(|p| p.matches(case))
    }
}

fn get_invocation_options(options: ftest_manager::RunOptions) -> ftest::RunOptions {
    ftest::RunOptions {
        include_disabled_tests: options.run_disabled_tests,
        parallel: options.parallel,
        arguments: options.arguments,
        ..ftest::RunOptions::EMPTY
    }
}

fn concat_suite_status(initial: SuiteStatus, new: SuiteStatus) -> SuiteStatus {
    if initial.into_primitive() > new.into_primitive() {
        return initial;
    }
    return new;
}

enum RunEventPayload {
    DebugData(ClientEnd<DebugDataIteratorMarker>),
}

struct RunEvent {
    timestamp: i64,
    payload: RunEventPayload,
}

impl Into<FidlRunEvent> for RunEvent {
    fn into(self) -> FidlRunEvent {
        match self.payload {
            RunEventPayload::DebugData(client) => FidlRunEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlRunEventPayload::Artifact(ftest_manager::Artifact::DebugData(
                    client,
                ))),
                ..FidlRunEvent::EMPTY
            },
        }
    }
}

impl RunEvent {
    fn debug_data(client: ClientEnd<DebugDataIteratorMarker>) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: RunEventPayload::DebugData(client),
        }
    }
}

enum SuiteEventPayload {
    CaseFound(String, u32),
    CaseStarted(u32),
    CaseStopped(u32, CaseStatus),
    CaseFinished(u32),
    CaseStdout(u32, zx::Socket),
    CaseStderr(u32, zx::Socket),
    CustomArtifact(ftest_manager::CustomArtifact),
    SuiteSyslog(ftest_manager::Syslog),
    SuiteStarted,
    SuiteStopped(SuiteStatus),
}

struct SuiteEvents {
    timestamp: i64,
    payload: SuiteEventPayload,
}

impl Into<FidlSuiteEvent> for SuiteEvents {
    fn into(self) -> FidlSuiteEvent {
        match self.payload {
            SuiteEventPayload::CaseFound(name, identifier) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseFound(ftest_manager::CaseFound {
                    test_case_name: name,
                    identifier,
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CaseStarted(identifier) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseStarted(ftest_manager::CaseStarted {
                    identifier,
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CaseStopped(identifier, status) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseStopped(ftest_manager::CaseStopped {
                    identifier,
                    status,
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CaseFinished(identifier) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseFinished(ftest_manager::CaseFinished {
                    identifier,
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CaseStdout(identifier, socket) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseArtifact(ftest_manager::CaseArtifact {
                    identifier,
                    artifact: ftest_manager::Artifact::Stdout(socket),
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CaseStderr(identifier, socket) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::CaseArtifact(ftest_manager::CaseArtifact {
                    identifier,
                    artifact: ftest_manager::Artifact::Stderr(socket),
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::CustomArtifact(custom) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::SuiteArtifact(ftest_manager::SuiteArtifact {
                    artifact: ftest_manager::Artifact::Custom(custom),
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::SuiteSyslog(syslog) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::SuiteArtifact(ftest_manager::SuiteArtifact {
                    artifact: ftest_manager::Artifact::Log(syslog),
                })),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::SuiteStarted => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::SuiteStarted(ftest_manager::SuiteStarted {})),
                ..FidlSuiteEvent::EMPTY
            },
            SuiteEventPayload::SuiteStopped(status) => FidlSuiteEvent {
                timestamp: Some(self.timestamp),
                payload: Some(FidlSuiteEventPayload::SuiteStopped(ftest_manager::SuiteStopped {
                    status,
                })),
                ..FidlSuiteEvent::EMPTY
            },
        }
    }
}

impl SuiteEvents {
    fn case_found(identifier: u32, name: String) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseFound(name, identifier),
        }
    }

    fn case_started(identifier: u32) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseStarted(identifier),
        }
    }

    fn case_stopped(identifier: u32, status: CaseStatus) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseStopped(identifier, status),
        }
    }

    fn case_finished(identifier: u32) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseFinished(identifier),
        }
    }

    fn case_stdout(identifier: u32, socket: zx::Socket) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseStdout(identifier, socket),
        }
    }

    fn case_stderr(identifier: u32, socket: zx::Socket) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CaseStderr(identifier, socket),
        }
    }

    fn suite_syslog(syslog: ftest_manager::Syslog) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::SuiteSyslog(syslog),
        }
    }

    fn suite_custom_artifact(custom: ftest_manager::CustomArtifact) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::CustomArtifact(custom),
        }
    }

    fn suite_started() -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::SuiteStarted,
        }
    }

    fn suite_stopped(status: SuiteStatus) -> Self {
        Self {
            timestamp: zx::Time::get_monotonic().into_nanos(),
            payload: SuiteEventPayload::SuiteStopped(status),
        }
    }

    #[cfg(test)]
    fn into_suite_run_event(self) -> FidlSuiteEvent {
        self.into()
    }
}

/// Runs the test component using `suite` and collects stdout logs and results.
async fn run_invocations(
    suite: &ftest::SuiteProxy,
    invocations: Vec<Invocation>,
    run_options: fidl_fuchsia_test::RunOptions,
    counter: &AtomicU32,
    sender: &mut mpsc::Sender<Result<FidlSuiteEvent, LaunchError>>,
    timeout_fut: futures::future::Shared<fasync::Timer>,
) -> Result<SuiteStatus, anyhow::Error> {
    let (run_listener_client, mut run_listener) =
        fidl::endpoints::create_request_stream().expect("cannot create request stream");
    suite.run(&mut invocations.into_iter().map(|i| i.into()), run_options, run_listener_client)?;

    let tasks = Arc::new(lock::Mutex::new(vec![]));
    let running_test_cases = Arc::new(lock::Mutex::new(HashSet::new()));
    let tasks_clone = tasks.clone();
    let initial_suite_status: SuiteStatus;
    let mut sender_clone = sender.clone();
    let test_fut = async {
        let mut initial_suite_status = SuiteStatus::DidNotFinish;
        while let Some(result_event) =
            run_listener.try_next().await.context("error waiting for listener")?
        {
            match result_event {
                ftest::RunListenerRequest::OnTestCaseStarted {
                    invocation,
                    std_handles,
                    listener,
                    control_handle: _,
                } => {
                    let name =
                        invocation.name.ok_or(format_err!("cannot find name in invocation"))?;
                    let identifier = counter.fetch_add(1, Ordering::Relaxed);
                    let events = vec![
                        Ok(SuiteEvents::case_found(identifier, name).into()),
                        Ok(SuiteEvents::case_started(identifier).into()),
                    ];
                    for event in events {
                        sender_clone.send(event).await.unwrap();
                    }
                    let listener =
                        listener.into_stream().context("Cannot convert listener to stream")?;
                    running_test_cases.lock().await.insert(identifier);
                    let running_test_cases = running_test_cases.clone();
                    let mut sender = sender_clone.clone();
                    let task = fasync::Task::spawn(async move {
                        let mut sender_stdout = sender.clone();
                        let mut sender_stderr = sender.clone();
                        let completion_fut = async {
                            let status = listen_for_completion(listener).await;
                            sender
                                .send(Ok(
                                    SuiteEvents::case_stopped(identifier, status.clone()).into()
                                ))
                                .await
                                .unwrap();
                            status
                        };
                        let ftest::StdHandles { out, err, .. } = std_handles;
                        let stdout_fut = async move {
                            if let Some(out) = out {
                                match fasync::OnSignals::new(
                                    &out,
                                    zx::Signals::SOCKET_READABLE | zx::Signals::SOCKET_PEER_CLOSED,
                                )
                                .await
                                {
                                    Ok(signals)
                                        if signals.contains(zx::Signals::SOCKET_READABLE) =>
                                    {
                                        sender_stdout
                                            .send(Ok(
                                                SuiteEvents::case_stdout(identifier, out).into()
                                            ))
                                            .await
                                            .unwrap();
                                    }
                                    Ok(_) | Err(_) => (),
                                }
                            }
                        };
                        let stderr_fut = async move {
                            if let Some(err) = err {
                                match fasync::OnSignals::new(
                                    &err,
                                    zx::Signals::SOCKET_READABLE | zx::Signals::SOCKET_PEER_CLOSED,
                                )
                                .await
                                {
                                    Ok(signals)
                                        if signals.contains(zx::Signals::SOCKET_READABLE) =>
                                    {
                                        sender_stderr
                                            .send(Ok(
                                                SuiteEvents::case_stderr(identifier, err).into()
                                            ))
                                            .await
                                            .unwrap();
                                    }
                                    Ok(_) | Err(_) => (),
                                }
                            }
                        };
                        let (status, (), ()) =
                            futures::future::join3(completion_fut, stdout_fut, stderr_fut).await;

                        sender
                            .send(Ok(SuiteEvents::case_finished(identifier).into()))
                            .await
                            .unwrap();
                        running_test_cases.lock().await.remove(&identifier);
                        status
                    });
                    tasks_clone.lock().await.push(task);
                }
                ftest::RunListenerRequest::OnFinished { .. } => {
                    initial_suite_status = SuiteStatus::Passed;
                    break;
                }
            }
        }
        Ok(initial_suite_status)
    }
    .fuse();

    futures::pin_mut!(test_fut);
    let timeout_fut = timeout_fut.fuse();
    futures::pin_mut!(timeout_fut);

    futures::select! {
        () = timeout_fut => {
                let mut all_tasks = vec![];
                let mut tasks = tasks.lock().await;
                all_tasks.append(&mut tasks);
                drop(tasks);
                for t in all_tasks {
                    t.cancel().await;
                }
                let running_test_cases = running_test_cases.lock().await;
                for i in &*running_test_cases {
                    sender
                        .send(Ok(SuiteEvents::case_stopped(*i, CaseStatus::TimedOut).into()))
                        .await
                        .unwrap();
                    sender
                        .send(Ok(SuiteEvents::case_finished(*i).into()))
                        .await
                        .unwrap();
                }
                return Ok(SuiteStatus::TimedOut);
            }
        r = test_fut => {
            initial_suite_status = match r {
                Err(e) => {
                    return Err(e);
                }
                Ok(s) => s,
            };
        }
    }

    let mut tasks = tasks.lock().await;
    let mut all_tasks = vec![];
    all_tasks.append(&mut tasks);
    // await for all invocations to complete for which test case never completed.
    let suite_status = join_all(all_tasks)
        .await
        .into_iter()
        .fold(initial_suite_status, get_suite_status_from_case_status);
    Ok(suite_status)
}

fn get_suite_status_from_case_status(
    initial_suite_status: SuiteStatus,
    case_status: CaseStatus,
) -> SuiteStatus {
    let status = match case_status {
        CaseStatus::Passed => SuiteStatus::Passed,
        CaseStatus::Failed => SuiteStatus::Failed,
        CaseStatus::TimedOut => SuiteStatus::TimedOut,
        CaseStatus::Skipped => SuiteStatus::Passed,
        CaseStatus::Error => SuiteStatus::Failed,
        _ => {
            panic!("this should not happen");
        }
    };
    concat_suite_status(initial_suite_status, status)
}

/// Listen for test completion on the given |listener|. Returns None if the channel is closed
/// before a test completion event.
async fn listen_for_completion(mut listener: ftest::CaseListenerRequestStream) -> CaseStatus {
    match listener.try_next().await.context("waiting for listener") {
        Ok(Some(request)) => {
            let ftest::CaseListenerRequest::Finished { result, control_handle: _ } = request;
            let result = match result.status {
                Some(status) => match status {
                    fidl_fuchsia_test::Status::Passed => CaseStatus::Passed,
                    fidl_fuchsia_test::Status::Failed => CaseStatus::Failed,
                    fidl_fuchsia_test::Status::Skipped => CaseStatus::Skipped,
                },
                // This will happen when test protocol is not properly implemented
                // by the test and it forgets to set the result.
                None => CaseStatus::Error,
            };
            result
        }
        Err(e) => {
            warn!("listener failed: {:?}", e);
            CaseStatus::Error
        }
        Ok(None) => CaseStatus::Error,
    }
}

// max events to send so that we don't cross fidl limits.
// TODO(anmittal): Use tape measure to calculate limit.
const EVENTS_THRESHOLD: usize = 50;

impl Suite {
    async fn run_controller(
        controller: &mut SuiteControllerRequestStream,
        stop_sender: oneshot::Sender<()>,
        run_suite_remote_handle: futures::future::RemoteHandle<()>,
        event_recv: mpsc::Receiver<Result<FidlSuiteEvent, LaunchError>>,
    ) -> Result<(), Error> {
        let mut event_recv = event_recv.into_stream().fuse();
        let mut stop_sender = Some(stop_sender);
        let mut run_suite_remote_handle = Some(run_suite_remote_handle);
        'controller_loop: while let Some(event) =
            controller.try_next().await.context("error running controller")?
        {
            match event {
                SuiteControllerRequest::Stop { .. } => {
                    // no need to handle error as task might already have finished.
                    if let Some(stop) = stop_sender.take() {
                        let _ = stop.send(());
                        // after this all `senders` go away and subsequent GetEvent call will
                        // return rest of event. Eventually an empty array and will close the
                        // connection after that.
                    }
                }
                SuiteControllerRequest::GetEvents { responder } => {
                    let mut events = vec![];

                    // wait for first event
                    let mut e = event_recv.next().await;

                    while let Some(event) = e {
                        match event {
                            Ok(event) => {
                                events.push(event);
                            }
                            Err(err) => {
                                responder
                                    .send(&mut Err(err))
                                    .map_err(TestManagerError::Response)?;
                                break 'controller_loop;
                            }
                        }
                        if events.len() >= EVENTS_THRESHOLD {
                            responder.send(&mut Ok(events)).map_err(TestManagerError::Response)?;
                            continue 'controller_loop;
                        }
                        e = match event_recv.next().now_or_never() {
                            Some(e) => e,
                            None => break,
                        }
                    }

                    let len = events.len();
                    responder.send(&mut Ok(events)).map_err(TestManagerError::Response)?;
                    if len == 0 {
                        break;
                    }
                }
                SuiteControllerRequest::Kill { .. } => {
                    // Dropping the remote handle for the suite execution task cancels it.
                    drop(run_suite_remote_handle.take());
                    // after this all `senders` go away and subsequent GetEvent call will
                    // return rest of event. Eventually an empty array and will close the
                    // connection after that.
                }
            }
        }
        Ok(())
    }
}

async fn run_single_suite(
    suite: Suite,
    debug_controller: &ftest_internal::DebugDataSetControllerProxy,
    instance_name: &str,
    inspect_node: self_diagnostics::SuiteInspectNode,
) {
    let (mut sender, recv) = mpsc::channel(1024);
    let (stop_sender, stop_recv) = oneshot::channel::<()>();
    let mut maybe_instance = None;
    let mut realm_moniker = None;

    let Suite { test_url, options, mut controller, resolver, above_root_capabilities_for_test } =
        suite;

    let run_test_fut = async {
        inspect_node.set_execution_state(self_diagnostics::ExecutionState::GetFacets);
        let facets = match facet::get_suite_facets(&test_url, &resolver).await {
            Ok(facets) => facets,
            Err(e) => {
                sender.send(Err(e.into())).await.unwrap();
                return;
            }
        };
        let realm_moniker_ref =
            realm_moniker.insert(format!("./{}:{}", facets.collection, instance_name));
        if let Err(e) = debug_controller.add_realm(realm_moniker_ref.as_str(), &test_url).await {
            warn!("Failed to add realm {} to debug data: {:?}", realm_moniker_ref.as_str(), e);
        }
        inspect_node.set_execution_state(self_diagnostics::ExecutionState::Launch);
        match RunningSuite::launch(
            &test_url,
            facets,
            Some(instance_name),
            resolver,
            above_root_capabilities_for_test,
        )
        .await
        {
            Ok(instance) => {
                inspect_node.set_execution_state(self_diagnostics::ExecutionState::RunTests);
                let instance_ref = maybe_instance.insert(instance);
                instance_ref.run_tests(&test_url, options, sender, stop_recv).await;
                inspect_node.set_execution_state(self_diagnostics::ExecutionState::TestsDone);
            }
            Err(e) => {
                let _ = debug_controller.remove_realm(realm_moniker_ref.as_str());
                sender.send(Err(e.into())).await.unwrap();
            }
        }
    };
    let (run_test_remote, run_test_handle) = run_test_fut.remote_handle();

    let controller_fut = Suite::run_controller(&mut controller, stop_sender, run_test_handle, recv);
    let ((), controller_ret) = futures::future::join(run_test_remote, controller_fut).await;

    if let Err(e) = controller_ret {
        warn!("Ended test {}: {:?}", test_url, e);
    }

    if let Some(instance) = maybe_instance.take() {
        inspect_node.set_execution_state(self_diagnostics::ExecutionState::TearDown);
        if let Err(err) = instance.destroy().await {
            // Failure to destroy an instance could mean that some component events fail to send.
            // Missing events could cause debug data collection to hang as it relies on events to
            // understand when a realm has stopped.
            error!(?err, "Failed to destroy instance for {}. Debug data may be lost.", test_url);
            if let Some(moniker) = realm_moniker {
                let _ = debug_controller.remove_realm(&moniker);
            }
        }
    }
    inspect_node.set_execution_state(self_diagnostics::ExecutionState::Complete);
    // Workaround to prevent zx_peer_closed error
    // TODO(fxbug.dev/87976) once fxbug.dev/87890 is fixed, the controller should be dropped as soon as all
    // events are drained.
    let (inner, _) = controller.into_inner();
    if let Err(e) = fasync::OnSignals::new(inner.channel(), zx::Signals::CHANNEL_PEER_CLOSED).await
    {
        warn!("Error waiting for SuiteController channel to close: {:?}", e);
    }
}

/// Start test manager and serve it over `stream`.
pub async fn run_test_manager(
    mut stream: ftest_manager::RunBuilderRequestStream,
    resolver: Arc<ComponentResolverProxy>,
    debug_data_controller: Arc<ftest_internal::DebugDataControllerProxy>,
    above_root_capabilities_for_test: Arc<AboveRootCapabilitiesForTest>,
    inspect_root: &self_diagnostics::RootInspectNode,
) -> Result<(), TestManagerError> {
    let mut builder = TestRunBuilder { suites: vec![] };
    while let Some(event) = stream.try_next().await.map_err(TestManagerError::Stream)? {
        match event {
            ftest_manager::RunBuilderRequest::AddSuite {
                test_url,
                options,
                controller,
                control_handle,
            } => {
                let controller = match controller.into_stream() {
                    Ok(c) => c,
                    Err(e) => {
                        warn!(
                            "Cannot add suite {}, invalid controller. Closing connection. error: {}",
                            test_url,e
                        );
                        control_handle.shutdown_with_epitaph(zx::Status::BAD_HANDLE);
                        break;
                    }
                };

                builder.suites.push(Suite {
                    test_url,
                    options,
                    controller,
                    resolver: resolver.clone(),
                    above_root_capabilities_for_test: above_root_capabilities_for_test.clone(),
                });
            }
            ftest_manager::RunBuilderRequest::Build { controller, control_handle } => {
                let controller = match controller.into_stream() {
                    Ok(c) => c,
                    Err(e) => {
                        warn!("Invalid builder controller. Closing connection. error: {}", e);
                        control_handle.shutdown_with_epitaph(zx::Status::BAD_HANDLE);
                        break;
                    }
                };
                let (debug_set_controller, set_controller_server) =
                    create_proxy::<ftest_internal::DebugDataSetControllerMarker>().unwrap();
                let (debug_iterator, iterator_server) =
                    create_endpoints::<ftest_manager::DebugDataIteratorMarker>().unwrap();
                debug_data_controller.new_set(iterator_server, set_controller_server).unwrap();
                let run_inspect = inspect_root
                    .new_run(&format!("run_{:?}", zx::Time::get_monotonic().into_nanos()));
                builder.run(controller, debug_set_controller, debug_iterator, run_inspect).await;
                // clients should reconnect to run new tests.
                break;
            }
        }
    }
    Ok(())
}

/// Start test manager and serve it over `stream`.
pub async fn run_test_manager_query_server(
    mut stream: ftest_manager::QueryRequestStream,
    resolver: Arc<ComponentResolverProxy>,
    above_root_capabilities_for_test: Arc<AboveRootCapabilitiesForTest>,
) -> Result<(), TestManagerError> {
    while let Some(event) = stream.try_next().await.map_err(TestManagerError::Stream)? {
        match event {
            ftest_manager::QueryRequest::Enumerate { test_url, iterator, responder } => {
                let mut iterator = match iterator.into_stream() {
                    Ok(c) => c,
                    Err(e) => {
                        warn!("Cannot query test, invalid iterator {}: {}", test_url, e);
                        let _ = responder.send(&mut Err(LaunchError::InvalidArgs));
                        break;
                    }
                };
                let launch_fut = facet::get_suite_facets(&test_url, &resolver).and_then(|facets| {
                    RunningSuite::launch(
                        &test_url,
                        facets,
                        None,
                        resolver.clone(),
                        above_root_capabilities_for_test.clone(),
                    )
                });
                match launch_fut.await {
                    Ok(suite_instance) => {
                        let suite = match suite_instance.connect_to_suite() {
                            Ok(proxy) => proxy,
                            Err(e) => {
                                let _ = responder.send(&mut Err(e.into()));
                                continue;
                            }
                        };
                        let enumeration_result = enumerate_test_cases(&suite, None).await;
                        let t = fasync::Task::spawn(suite_instance.destroy());
                        match enumeration_result {
                            Ok(invocations) => {
                                const NAMES_CHUNK: usize = 50;
                                let mut names = Vec::with_capacity(invocations.len());
                                if let Ok(_) =
                                    invocations.into_iter().try_for_each(|i| match i.name {
                                        Some(name) => {
                                            names.push(name);
                                            Ok(())
                                        }
                                        None => {
                                            warn!("no name for a invocation in {}", test_url);
                                            Err(())
                                        }
                                    })
                                {
                                    let _ = responder.send(&mut Ok(()));
                                    let mut names = names.chunks(NAMES_CHUNK);
                                    while let Ok(Some(request)) = iterator.try_next().await {
                                        match request {
                                            ftest_manager::CaseIteratorRequest::GetNext {
                                                responder,
                                            } => match names.next() {
                                                Some(names) => {
                                                    let _ =
                                                        responder.send(&mut names.into_iter().map(
                                                            |s| ftest_manager::Case {
                                                                name: Some(s.into()),
                                                                ..ftest_manager::Case::EMPTY
                                                            },
                                                        ));
                                                }
                                                None => {
                                                    let _ = responder.send(&mut vec![].into_iter());
                                                }
                                            },
                                        }
                                    }
                                } else {
                                    let _ = responder.send(&mut Err(LaunchError::CaseEnumeration));
                                }
                            }
                            Err(e) => {
                                warn!("cannot enumerate tests for {}: {:?}", test_url, e);
                                let _ = responder.send(&mut Err(map_suite_error_epitaph(
                                    suite,
                                    LaunchError::CaseEnumeration,
                                )));
                            }
                        }
                        if let Err(err) = t.await {
                            warn!(?err, "Error destroying test realm for {}", test_url);
                        }
                    }
                    Err(e) => {
                        let _ = responder.send(&mut Err(e.into()));
                    }
                }
            }
        }
    }
    Ok(())
}

/// A |RunningSuite| represents a launched test component.
struct RunningSuite {
    instance: RealmInstance,
    logs_iterator_task: Option<fasync::Task<Result<(), Error>>>,
    /// Server ends of event pairs used to track if a client is accessing a component's
    /// custom storage. Used to defer destruction of the realm until clients have completed
    /// reading the storage.
    custom_artifact_tokens: Vec<zx::EventPair>,
    /// Keep archive accessor which tests might use through weak references.
    archive_accessor: Arc<ArchiveAccessorProxy>,

    /// The test collection in which this suite is running.
    test_collection: &'static str,
}

impl RunningSuite {
    /// Launch a suite component.
    async fn launch(
        test_url: &str,
        facets: facet::SuiteFacets,
        instance_name: Option<&str>,
        resolver: Arc<ComponentResolverProxy>,
        above_root_capabilities_for_test: Arc<AboveRootCapabilitiesForTest>,
    ) -> Result<Self, LaunchTestError> {
        info!("Starting '{}' in '{}' collection.", test_url, facets.collection);

        // This archive accessor will be served by the embedded archivist.
        let (archive_accessor, archive_accessor_server_end) =
            fidl::endpoints::create_proxy::<fdiagnostics::ArchiveAccessorMarker>()
                .map_err(LaunchTestError::CreateProxyForArchiveAccessor)?;

        let archive_accessor_arc = Arc::new(archive_accessor);
        let test_package = match PkgUrl::parse(test_url) {
            Ok(package_url) => package_url.name().to_string(),
            Err(_) => return Err(LaunchTestError::InvalidResolverData),
        };
        let builder = get_realm(
            Arc::downgrade(&archive_accessor_arc),
            test_url,
            test_package.as_ref(),
            facets.collection,
            above_root_capabilities_for_test,
            resolver,
        )
        .await
        .map_err(LaunchTestError::InitializeTestRealm)?;
        let instance = match instance_name {
            None => builder.build().await,
            Some(name) => builder.build_with_name(name).await,
        }
        .map_err(LaunchTestError::CreateTestRealm)?;
        let connect_to_instance_services = async move {
            instance
                .root
                .connect_request_to_protocol_at_exposed_dir::<fdiagnostics::ArchiveAccessorMarker>(
                    archive_accessor_server_end,
                )
                .map_err(LaunchTestError::ConnectToArchiveAccessor)?;
            Ok(RunningSuite {
                custom_artifact_tokens: vec![],
                logs_iterator_task: None,
                instance,
                archive_accessor: archive_accessor_arc,
                test_collection: facets.collection,
            })
        };

        connect_to_instance_services.await
    }

    async fn run_tests(
        &mut self,
        test_url: &str,
        options: ftest_manager::RunOptions,
        mut sender: mpsc::Sender<Result<FidlSuiteEvent, LaunchError>>,
        mut stop_recv: oneshot::Receiver<()>,
    ) {
        debug!("running test suite {}", test_url);

        let (log_iterator, syslog) = match options.log_iterator {
            Some(ftest_manager::LogsIteratorOption::ArchiveIterator) => {
                let (proxy, request) =
                    fidl::endpoints::create_endpoints().expect("cannot create suite");
                (
                    ftest_manager::LogsIterator::Archive(request),
                    ftest_manager::Syslog::Archive(proxy),
                )
            }
            _ => {
                let (proxy, request) =
                    fidl::endpoints::create_endpoints().expect("cannot create suite");
                (ftest_manager::LogsIterator::Batch(request), ftest_manager::Syslog::Batch(proxy))
            }
        };

        sender.send(Ok(SuiteEvents::suite_syslog(syslog).into())).await.unwrap();

        let logs_iterator_task_result = match log_iterator {
            ftest_manager::LogsIterator::Archive(iterator) => {
                IsolatedLogsProvider::new(self.archive_accessor.clone())
                    .spawn_iterator_server(iterator)
                    .map(Some)
            }
            ftest_manager::LogsIterator::Batch(iterator) => {
                IsolatedLogsProvider::new(self.archive_accessor.clone())
                    .start_streaming_logs(iterator)
                    .map(|()| None)
            }
            _ => Ok(None),
        };
        self.logs_iterator_task = match logs_iterator_task_result {
            Ok(task) => task,
            Err(e) => {
                warn!("Error spawning iterator server: {:?}", e);
                sender.send(Err(LaunchTestError::StreamIsolatedLogs(e).into())).await.unwrap();
                return;
            }
        };

        let fut = async {
            let matcher = match options.case_filters_to_run.as_ref() {
                Some(filters) => match CaseMatcher::new(filters) {
                    Ok(p) => Some(p),
                    Err(e) => {
                        sender.send(Err(LaunchError::InvalidArgs)).await.unwrap();
                        return Err(e);
                    }
                },
                None => None,
            };

            let suite = self.connect_to_suite()?;
            let invocations = match enumerate_test_cases(&suite, matcher.as_ref()).await {
                Ok(i) if i.is_empty() && matcher.is_some() => {
                    sender.send(Err(LaunchError::NoMatchingCases)).await.unwrap();
                    return Err(format_err!("Found no matching cases using {:?}", matcher));
                }
                Ok(i) => i,
                Err(e) => {
                    sender
                        .send(Err(map_suite_error_epitaph(suite, LaunchError::CaseEnumeration)))
                        .await
                        .unwrap();
                    return Err(e);
                }
            };
            if let Ok(Some(_)) = stop_recv.try_recv() {
                sender
                    .send(Ok(SuiteEvents::suite_stopped(SuiteStatus::Stopped).into()))
                    .await
                    .unwrap();
                return Ok(());
            }

            let mut suite_status = SuiteStatus::Passed;
            let mut invocations_iter = invocations.into_iter();
            let counter = AtomicU32::new(0);
            let timeout_time = match options.timeout {
                Some(t) => zx::Time::after(zx::Duration::from_nanos(t)),
                None => zx::Time::INFINITE,
            };
            let timeout_fut = fasync::Timer::new(timeout_time).shared();

            let run_options = get_invocation_options(options);

            sender.send(Ok(SuiteEvents::suite_started().into())).await.unwrap();

            loop {
                const INVOCATIONS_CHUNK: usize = 50;
                let chunk = invocations_iter.by_ref().take(INVOCATIONS_CHUNK).collect::<Vec<_>>();
                if chunk.is_empty() {
                    break;
                }
                let res = match run_invocations(
                    &suite,
                    chunk,
                    run_options.clone(),
                    &counter,
                    &mut sender,
                    timeout_fut.clone(),
                )
                .await
                .context("Error running test cases")
                {
                    Ok(success) => success,
                    Err(e) => {
                        return Err(e);
                    }
                };
                if res == SuiteStatus::TimedOut {
                    sender
                        .send(Ok(SuiteEvents::suite_stopped(SuiteStatus::TimedOut).into()))
                        .await
                        .unwrap();
                    return self.report_custom_artifacts(&mut sender).await;
                }
                suite_status = concat_suite_status(suite_status, res);
                if let Ok(Some(_)) = stop_recv.try_recv() {
                    sender
                        .send(Ok(SuiteEvents::suite_stopped(SuiteStatus::Stopped).into()))
                        .await
                        .unwrap();
                    return self.report_custom_artifacts(&mut sender).await;
                }
            }
            sender.send(Ok(SuiteEvents::suite_stopped(suite_status).into())).await.unwrap();
            self.report_custom_artifacts(&mut sender).await
        };
        if let Err(e) = fut.await {
            warn!("Error running test {}: {:?}", test_url, e);
        }
    }

    /// Find any custom artifact users under the test realm and report them via sender.
    async fn report_custom_artifacts(
        &mut self,
        sender: &mut mpsc::Sender<Result<FidlSuiteEvent, LaunchError>>,
    ) -> Result<(), Error> {
        let artifact_storage_admin = connect_to_protocol::<fsys::StorageAdminMarker>()?;

        let root_moniker =
            format!("./{}:{}", self.test_collection, self.instance.root.child_name());
        let (iterator, iter_server) = create_proxy::<fsys::StorageIteratorMarker>()?;
        artifact_storage_admin
            .list_storage_in_realm(&root_moniker, iter_server)
            .await?
            .map_err(|e| format_err!("Error listing storage users in test realm: {:?}", e))?;
        let stream = stream_fn(move || iterator.next());
        futures::pin_mut!(stream);
        while let Some(storage_moniker) = stream.try_next().await? {
            let (node, server) = fidl::endpoints::create_endpoints::<fio::NodeMarker>()?;
            let directory: ClientEnd<fio::DirectoryMarker> = node.into_channel().into();
            artifact_storage_admin.open_component_storage(
                &storage_moniker,
                fio::OPEN_RIGHT_READABLE,
                fio::MODE_TYPE_DIRECTORY,
                server,
            )?;
            let (event_client, event_server) = zx::EventPair::create()?;
            self.custom_artifact_tokens.push(event_server);

            // Monikers should be reported relative to the test root, so strip away the wrapping
            // components from the path.
            let moniker_parsed =
                cm_moniker::InstancedRelativeMoniker::try_from(storage_moniker.as_str()).unwrap();
            let down_path = moniker_parsed
                .down_path()
                .iter()
                .skip(3)
                .map(Clone::clone)
                .collect::<Vec<cm_moniker::InstancedChildMoniker>>();
            let instanced_moniker = cm_moniker::InstancedRelativeMoniker::new(vec![], down_path);
            let moniker_relative_to_test_root = instanced_moniker.to_relative_moniker();
            sender
                .send(Ok(SuiteEvents::suite_custom_artifact(ftest_manager::CustomArtifact {
                    directory_and_token: Some(ftest_manager::DirectoryAndToken {
                        directory,
                        token: event_client,
                    }),
                    component_moniker: Some(moniker_relative_to_test_root.to_string()),
                    ..ftest_manager::CustomArtifact::EMPTY
                })
                .into()))
                .await
                .unwrap();
        }
        Ok(())
    }

    fn connect_to_suite(&self) -> Result<ftest::SuiteProxy, LaunchTestError> {
        let (proxy, server_end) = fidl::endpoints::create_proxy::<ftest::SuiteMarker>().unwrap();
        self.instance
            .root
            .connect_request_to_protocol_at_exposed_dir::<ftest::SuiteMarker>(server_end)
            .map_err(LaunchTestError::ConnectToTestSuite)?;
        Ok(proxy)
    }

    /// Mark the resources associated with the suite for destruction, then wait for destruction to
    /// complete. Returns an error only if destruction fails.
    async fn destroy(mut self) -> Result<(), Error> {
        // TODO(fxbug.dev/92769) Remove timeout once component manager hangs are removed.
        const TEARDOWN_TIMEOUT: zx::Duration = zx::Duration::from_seconds(30);

        // before destroying the realm, wait for any clients to finish accessing storage.
        // TODO(fxbug.dev/84825): Separate realm destruction and destruction of custom
        // storage resources.
        // TODO(fxbug.dev/84882): Remove signal for USER_0, this is used while Overnet does not support
        // signalling ZX_EVENTPAIR_CLOSED when the eventpair is closed.
        let tokens_closed_signals = self.custom_artifact_tokens.iter().map(|token| {
            fasync::OnSignals::new(token, zx::Signals::EVENTPAIR_CLOSED | zx::Signals::USER_0)
                .unwrap_or_else(|_| zx::Signals::empty())
        });
        futures::future::join_all(tokens_closed_signals)
            .map(|_| Ok(()))
            .on_timeout(TEARDOWN_TIMEOUT, || {
                Err(anyhow!("Timeout waiting for clients to access storage"))
            })
            .await?;

        let destroy_waiter = self.instance.root.take_destroy_waiter();
        drop(self.instance);
        drop(self.archive_accessor);
        #[derive(Debug, Error)]
        enum TeardownError {
            #[error("timeout")]
            Timeout,
            #[error("{}", .0)]
            Other(Error),
        }
        // When serving logs over ArchiveIterator in the host, we should also wait for all logs to
        // be drained.
        let logs_iterator_task = self
            .logs_iterator_task
            .unwrap_or_else(|| fasync::Task::spawn(futures::future::ready(Ok(()))));
        let (logs_iterator_res, teardown_res) = futures::future::join(
            logs_iterator_task
                .map_err(|e| TeardownError::Other(e))
                .on_timeout(TEARDOWN_TIMEOUT, || Err(TeardownError::Timeout)),
            destroy_waiter
                .map_err(|e| TeardownError::Other(e))
                .on_timeout(TEARDOWN_TIMEOUT, || Err(TeardownError::Timeout)),
        )
        .await;
        let logs_iterator_res = match logs_iterator_res {
            Err(TeardownError::Other(e)) => {
                // Allow test teardown to proceed if failed to stream logs
                warn!("Error streaming logs: {}", e);
                Ok(())
            }
            r => r,
        };
        match (logs_iterator_res, teardown_res) {
            (Err(e1), Err(e2)) => {
                Err(anyhow!("Draining logs failed: {}. Realm teardown failed: {}", e1, e2))
            }
            (Err(e), Ok(())) => Err(anyhow!("Draining logs failed: {}", e)),
            (Ok(()), Err(e)) => Err(anyhow!("Realm teardown failed: {}", e)),
            (Ok(()), Ok(())) => Ok(()),
        }
    }
}

/// Convert iterator fidl method into stream of events.
/// ie convert
/// fidl_method() -> Future<Result<Vec<T>, E>>
/// to
/// Stream<Item=Result<T, E>>
fn stream_fn<F, T, E, Fut>(query_fn: F) -> impl Stream<Item = Result<T, E>>
where
    F: 'static + FnMut() -> Fut + Unpin + Send + Sync,
    Fut: Future<Output = Result<Vec<T>, E>> + Unpin + Send + Sync,
{
    futures::stream::try_unfold(query_fn, |mut query_fn| async move {
        Ok(Some((query_fn().await?, query_fn)))
    })
    .try_take_while(|vec| futures::future::ready(Ok(!vec.is_empty())))
    .map_ok(|vec| futures::stream::iter(vec).map(Ok))
    .try_flatten()
}

async fn get_realm(
    archive_accessor: Weak<fdiagnostics::ArchiveAccessorProxy>,
    test_url: &str,
    test_package: &str,
    collection: &str,
    above_root_capabilities_for_test: Arc<AboveRootCapabilitiesForTest>,
    resolver: Arc<ComponentResolverProxy>,
) -> Result<RealmBuilder, RealmBuilderError> {
    let builder = RealmBuilder::new_with_collection(collection.to_string()).await?;

    let mocks_server = builder
        .add_local_child(
            MOCKS_SERVER_REALM_NAME,
            move |handles| Box::pin(serve_mocks(archive_accessor.clone(), handles)),
            ChildOptions::new(),
        )
        .await?;

    let wrapper_realm =
        builder.add_child_realm(WRAPPER_REALM_NAME, ChildOptions::new().eager()).await?;

    // If this is realm is inside the hermetic tests collections, set up the
    // hermetic resolver local component.
    let mut test_root_child_opts = ChildOptions::new().eager();
    if collection.eq(HERMETIC_TESTS_COLLECTION) {
        let allowed_package_names = hashset! {test_package.to_string()};
        wrapper_realm
            .add_local_child(
                HERMETIC_RESOLVER_REALM_NAME,
                move |handles| {
                    Box::pin(resolver::serve_hermetic_resolver(
                        handles,
                        allowed_package_names.clone(),
                        resolver.clone(),
                        true,
                    ))
                },
                ChildOptions::new(),
            )
            .await?;

        // Provide and expose the resolver capability from the resolver to test_wrapper.
        let mut hermetic_resolver_decl =
            wrapper_realm.get_component_decl(HERMETIC_RESOLVER_REALM_NAME).await?;
        hermetic_resolver_decl.exposes.push(cm_rust::ExposeDecl::Resolver(
            cm_rust::ExposeResolverDecl {
                source: cm_rust::ExposeSource::Self_,
                source_name: cm_rust::CapabilityName(String::from(
                    HERMETIC_RESOLVER_CAPABILITY_NAME,
                )),
                target: cm_rust::ExposeTarget::Parent,
                target_name: cm_rust::CapabilityName(String::from(
                    HERMETIC_RESOLVER_CAPABILITY_NAME,
                )),
            },
        ));
        hermetic_resolver_decl.capabilities.push(cm_rust::CapabilityDecl::Resolver(
            cm_rust::ResolverDecl {
                name: cm_rust::CapabilityName(String::from(HERMETIC_RESOLVER_CAPABILITY_NAME)),
                source_path: Some(cm_rust::CapabilityPath {
                    dirname: String::from("/svc"),
                    basename: String::from("fuchsia.sys2.ComponentResolver"),
                }),
            },
        ));
        wrapper_realm
            .replace_component_decl(HERMETIC_RESOLVER_REALM_NAME, hermetic_resolver_decl)
            .await?;

        // Create the hermetic environment in the test_wrapper.
        let mut test_wrapper_decl = wrapper_realm.get_realm_decl().await?;
        test_wrapper_decl.environments.push(cm_rust::EnvironmentDecl {
            name: String::from(HERMETIC_ENVIRONMENT_NAME),
            extends: fdecl::EnvironmentExtends::Realm,
            resolvers: vec![cm_rust::ResolverRegistration {
                resolver: cm_rust::CapabilityName(String::from(HERMETIC_RESOLVER_CAPABILITY_NAME)),
                source: cm_rust::RegistrationSource::Child(String::from(
                    HERMETIC_RESOLVER_CAPABILITY_NAME,
                )),
                scheme: String::from("fuchsia-pkg"),
            }],
            runners: vec![],
            debug_capabilities: vec![],
            stop_timeout_ms: None,
        });
        wrapper_realm.replace_realm_decl(test_wrapper_decl).await?;
        test_root_child_opts = ChildOptions::new().environment(HERMETIC_ENVIRONMENT_NAME).eager();
    }

    let test_root =
        wrapper_realm.add_child(TEST_ROOT_REALM_NAME, test_url, test_root_child_opts).await?;
    let archivist = wrapper_realm
        .add_child(ARCHIVIST_REALM_NAME, ARCHIVIST_FOR_EMBEDDING_URL, ChildOptions::new().eager())
        .await?;
    let enclosing_env = wrapper_realm
        .add_local_child(
            ENCLOSING_ENV_REALM_NAME,
            move |handles| Box::pin(gen_enclosing_env(handles)),
            ChildOptions::new(),
        )
        .await?;

    // Parent to archivist
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fsys::EventSourceMarker>())
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .from(Ref::parent())
                .to(&wrapper_realm),
        )
        .await?;
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fsys::EventSourceMarker>())
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .from(Ref::parent())
                .to(&archivist),
        )
        .await?;

    // archivist to test root and enclosing env
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .from(&archivist)
                .to(&test_root)
                .to(&enclosing_env),
        )
        .await?;

    // archivist to test root
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.logger.Log"))
                .from(&archivist)
                .to(&test_root),
        )
        .await?;

    // Mocks server to test root
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fdiagnostics::ArchiveAccessorMarker>())
                .from(&mocks_server)
                .to(&wrapper_realm),
        )
        .await?;
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fdiagnostics::ArchiveAccessorMarker>())
                .from(Ref::parent())
                .to(&test_root),
        )
        .await?;

    // archivist to parent
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fdiagnostics::ArchiveAccessorMarker>())
                .from(&archivist)
                .to(Ref::parent()),
        )
        .await?;

    // test root to parent
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol::<ftest::SuiteMarker>())
                .from(&test_root)
                .to(Ref::parent()),
        )
        .await?;

    // enclosing env to test root
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fv1sys::EnvironmentMarker>())
                .capability(Capability::protocol::<fv1sys::LauncherMarker>())
                .capability(Capability::protocol::<fv1sys::LoaderMarker>())
                .from(&enclosing_env)
                .to(&test_root),
        )
        .await?;

    // debug to enclosing env
    // This must be done manually, as there is currently no way to add a "use from debug"
    // declaration using `add_route`.
    let mut enclosing_env_decl = wrapper_realm.get_component_decl(&enclosing_env).await?;
    enclosing_env_decl.uses.push(cm_rust::UseDecl::Protocol(cm_rust::UseProtocolDecl {
        source: cm_rust::UseSource::Debug,
        source_name: fdebugdata::DebugDataMarker::PROTOCOL_NAME.into(),
        target_path: format!("/svc/{}", fdebugdata::DebugDataMarker::PROTOCOL_NAME)
            .as_str()
            .try_into()
            .unwrap(),
        dependency_type: cm_rust::DependencyType::Strong,
    }));
    wrapper_realm.replace_component_decl(&enclosing_env, enclosing_env_decl).await?;

    // wrapper realm to archivist
    wrapper_realm
        .add_route(
            Route::new()
                .capability(Capability::event(Event::Started, cm_rust::EventMode::Async))
                .capability(Capability::event(Event::Stopped, cm_rust::EventMode::Async))
                .capability(Capability::event(Event::Running, cm_rust::EventMode::Async))
                .capability(Capability::event(
                    Event::directory_ready("diagnostics"),
                    cm_rust::EventMode::Async,
                ))
                .capability(Capability::event(
                    Event::capability_requested("fuchsia.logger.LogSink"),
                    cm_rust::EventMode::Async,
                ))
                .from(Ref::framework())
                .to(&archivist),
        )
        .await?;

    builder
        .add_route(
            Route::new()
                // from archivist
                .capability(Capability::protocol::<fdiagnostics::ArchiveAccessorMarker>())
                // from test root
                .capability(Capability::protocol::<ftest::SuiteMarker>())
                .from(&wrapper_realm)
                .to(Ref::parent()),
        )
        .await?;

    above_root_capabilities_for_test.apply(collection, &builder, &wrapper_realm).await?;

    Ok(builder)
}

async fn serve_mocks(
    archive_accessor: Weak<fdiagnostics::ArchiveAccessorProxy>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();
    fs.dir("svc").add_fidl_service(move |stream| {
        let archive_accessor_clone = archive_accessor.clone();
        fasync::Task::spawn(async move {
            diagnostics::run_intermediary_archive_accessor(archive_accessor_clone, stream)
                .await
                .unwrap_or_else(|e| {
                    warn!("Couldn't run proxied ArchiveAccessor: {:?}", e);
                })
        })
        .detach()
    });
    fs.serve_connection(handles.outgoing_dir.into_channel())?;
    fs.collect::<()>().await;
    Ok(())
}

// Maps failures to call to fuchsia.test.Suite to a LaunchError value. This
// function should only be called iff an error was encountered when invoking a
// method on the `suite` object. Otherwise, `default_value` is returned.
fn map_suite_error_epitaph(suite: ftest::SuiteProxy, default_value: LaunchError) -> LaunchError {
    // Call now_or_never() so that test_manager isn't blocked on event not being ready.
    let next_evt_peek = suite.take_event_stream().next().now_or_never();
    match next_evt_peek {
        Some(Some(Err(fidl::Error::ClientChannelClosed {
            status: zx::Status::NOT_FOUND, ..
        }))) => LaunchError::InstanceCannotResolve,
        Some(Some(Err(fidl::Error::ClientChannelClosed { .. }))) => default_value,
        _ => {
            warn!("empty epitaph read from Suite: {:?}", next_evt_peek);
            LaunchError::InternalError
        }
    }
}

pub struct AboveRootCapabilitiesForTest {
    capabilities: HashMap<&'static str, Vec<RBCapability>>,
}

impl AboveRootCapabilitiesForTest {
    pub async fn new(manifest_name: &str) -> Result<Self, Error> {
        let path = format!("/pkg/meta/{}", manifest_name);
        let file_proxy = io_util::open_file_in_namespace(&path, io_util::OPEN_RIGHT_READABLE)?;
        let component_decl = io_util::read_file_fidl::<fdecl::Component>(&file_proxy).await?;
        let capabilities = Self::load(component_decl);
        Ok(Self { capabilities })
    }

    async fn apply(
        &self,
        collection: &str,
        builder: &RealmBuilder,
        wrapper_realm: &SubRealmBuilder,
    ) -> Result<(), RealmBuilderError> {
        if self.capabilities.contains_key(collection) {
            for capability in &self.capabilities[collection] {
                builder
                    .add_route(
                        Route::new()
                            .capability(capability.clone())
                            .from(Ref::parent())
                            .to(wrapper_realm),
                    )
                    .await?;
                wrapper_realm
                    .add_route(
                        Route::new()
                            .capability(capability.clone())
                            .from(Ref::parent())
                            .to(Ref::child(TEST_ROOT_REALM_NAME)),
                    )
                    .await?;
            }
        }
        Ok(())
    }

    fn load(decl: fdecl::Component) -> HashMap<&'static str, Vec<RBCapability>> {
        let mut capabilities: HashMap<_, _> =
            TEST_TYPE_REALM_MAP.values().map(|v| (*v, vec![])).collect();
        for offer_decl in decl.offers.unwrap_or(vec![]) {
            match offer_decl {
                fdecl::Offer::Protocol(fdecl::OfferProtocol {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    target_name: Some(target_name),
                    ..
                }) if capabilities.contains_key(name.as_str())
                    && target_name != "fuchsia.logger.LogSink" =>
                {
                    capabilities
                        .get_mut(name.as_str())
                        .unwrap()
                        .push(Capability::protocol_by_name(target_name).into());
                }
                fdecl::Offer::Directory(fdecl::OfferDirectory {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    target_name: Some(target_name),
                    ..
                }) if capabilities.contains_key(name.as_str()) => {
                    capabilities
                        .get_mut(name.as_str())
                        .unwrap()
                        .push(Capability::directory(target_name).into());
                }
                fdecl::Offer::Storage(fdecl::OfferStorage {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    target_name: Some(target_name),
                    ..
                }) if capabilities.contains_key(name.as_str()) => {
                    let use_path = format!("/{}", target_name);
                    capabilities
                        .get_mut(name.as_str())
                        .unwrap()
                        .push(Capability::storage(target_name).path(use_path).into());
                }
                fdecl::Offer::Service(fdecl::OfferService {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    ..
                })
                | fdecl::Offer::Runner(fdecl::OfferRunner {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    ..
                })
                | fdecl::Offer::Resolver(fdecl::OfferResolver {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    ..
                }) if capabilities.contains_key(name.as_str()) => {
                    unimplemented!(
                        "Services, runners and resolvers are not supported by realm builder"
                    );
                }
                fdecl::Offer::Event(fdecl::OfferEvent {
                    target: Some(fdecl::Ref::Collection(fdecl::CollectionRef { name })),
                    ..
                }) if capabilities.contains_key(name.as_str()) => {
                    unreachable!("No events should be routed from above root to a test.");
                }
                _ => {
                    // Ignore anything else that is not routed to test collections
                }
            }
        }
        capabilities
    }
}

lazy_static! {
    static ref ENCLOSING_ENV_ID: AtomicU64 = AtomicU64::new(1);
}

/// Represents a single CFv1 environment.
/// Consumer of this protocol have no access to system services.
/// The logger provided to clients comes from isolated archivist.
/// TODO(82072): Support collection of inspect by isolated archivist.
struct EnclosingEnvironment {
    svc_task: Option<fasync::Task<()>>,
    env_controller_proxy: Option<fv1sys::EnvironmentControllerProxy>,
    env_proxy: fv1sys::EnvironmentProxy,
    service_directory: zx::Channel,
}

impl Drop for EnclosingEnvironment {
    fn drop(&mut self) {
        let svc_task = self.svc_task.take();
        let env_controller_proxy = self.env_controller_proxy.take();
        fasync::Task::spawn(async move {
            if let Some(svc_task) = svc_task {
                svc_task.cancel().await;
            }
            if let Some(env_controller_proxy) = env_controller_proxy {
                let _ = env_controller_proxy.kill().await;
            }
        })
        .detach();
    }
}

impl EnclosingEnvironment {
    fn new(incoming_svc: fio::DirectoryProxy) -> Result<Arc<Self>, Error> {
        let sys_env = connect_to_protocol::<fv1sys::EnvironmentMarker>()?;
        let (additional_svc, additional_directory_request) = zx::Channel::create()?;
        let incoming_svc = Arc::new(incoming_svc);
        let incoming_svc_clone = incoming_svc.clone();
        let mut fs = ServiceFs::new();
        fs.add_service_at(fv1sys::LoaderMarker::NAME, move |chan: fuchsia_zircon::Channel| {
            if let Err(e) = connect_channel_to_protocol::<fv1sys::LoaderMarker>(chan) {
                warn!("Cannot connect to loader: {}", e);
            }
            None
        })
        .add_service_at(fdebugdata::DebugDataMarker::NAME, move |chan: fuchsia_zircon::Channel| {
            if let Err(e) = fdio::service_connect_at(
                incoming_svc_clone.as_channel().as_ref(),
                fdebugdata::DebugDataMarker::NAME,
                chan,
            ) {
                warn!("cannot connect to DebugData: {}", e);
            }
            None
        })
        .add_service_at("fuchsia.logger.LogSink", move |chan: fuchsia_zircon::Channel| {
            if let Err(e) = fdio::service_connect_at(
                incoming_svc.as_channel().as_ref(),
                "fuchsia.logger.LogSink",
                chan,
            ) {
                warn!("cannot connect to LogSink: {}", e);
            }
            None
        });

        fs.serve_connection(additional_svc)?;
        let svc_task = fasync::Task::spawn(async move {
            fs.collect::<()>().await;
        });

        let mut service_list = fv1sys::ServiceList {
            names: vec![
                fv1sys::LoaderMarker::NAME.to_string(),
                fdebugdata::DebugDataMarker::NAME.to_string(),
                "fuchsia.logger.LogSink".into(),
            ],
            provider: None,
            host_directory: Some(additional_directory_request),
        };

        let mut opts = fv1sys::EnvironmentOptions {
            inherit_parent_services: false,
            use_parent_runners: false,
            kill_on_oom: true,
            delete_storage_on_death: true,
        };

        let (env_proxy, env_server_end) = fidl::endpoints::create_proxy()?;
        let (service_directory, directory_request) = zx::Channel::create()?;

        let (env_controller_proxy, env_controller_server_end) = fidl::endpoints::create_proxy()?;
        let name = format!("env-{}", ENCLOSING_ENV_ID.fetch_add(1, Ordering::SeqCst));
        sys_env
            .create_nested_environment(
                env_server_end,
                env_controller_server_end,
                &name,
                Some(&mut service_list),
                &mut opts,
            )
            .context("Cannot create nested env")?;
        env_proxy.get_directory(directory_request).context("cannot get env directory")?;
        Ok(Self {
            svc_task: svc_task.into(),
            env_controller_proxy: env_controller_proxy.into(),
            env_proxy,
            service_directory,
        }
        .into())
    }

    fn get_launcher(&self, launcher: fidl::endpoints::ServerEnd<fv1sys::LauncherMarker>) {
        if let Err(e) = self.env_proxy.get_launcher(launcher) {
            warn!("GetLauncher failed: {}", e);
        }
    }

    fn connect_to_protocol(&self, protocol_name: &str, chan: zx::Channel) {
        if let Err(e) = fdio::service_connect_at(&self.service_directory, protocol_name, chan) {
            warn!("service_connect_at failed for {}: {}", protocol_name, e);
        }
    }

    async fn serve(&self, mut req_stream: fv1sys::EnvironmentRequestStream) {
        while let Some(req) = req_stream
            .try_next()
            .await
            .context("serving V1 stream failed")
            .map_err(|e| {
                warn!("{}", e);
            })
            .unwrap_or(None)
        {
            match req {
                fv1sys::EnvironmentRequest::GetLauncher { launcher, control_handle } => {
                    if let Err(e) = self.env_proxy.get_launcher(launcher) {
                        warn!("GetLauncher failed: {}", e);
                        control_handle.shutdown();
                    }
                }
                fv1sys::EnvironmentRequest::GetServices { services, control_handle } => {
                    if let Err(e) = self.env_proxy.get_services(services) {
                        warn!("GetServices failed: {}", e);

                        control_handle.shutdown();
                    }
                }
                fv1sys::EnvironmentRequest::GetDirectory { directory_request, control_handle } => {
                    if let Err(e) = self.env_proxy.get_directory(directory_request) {
                        warn!("GetDirectory failed: {}", e);
                        control_handle.shutdown();
                    }
                }
                fv1sys::EnvironmentRequest::CreateNestedEnvironment {
                    environment,
                    controller,
                    label,
                    mut additional_services,
                    mut options,
                    control_handle,
                } => {
                    let services = match &mut additional_services {
                        Some(s) => s.as_mut().into(),
                        None => None,
                    };
                    if let Err(e) = self.env_proxy.create_nested_environment(
                        environment,
                        controller,
                        &label,
                        services,
                        &mut options,
                    ) {
                        warn!("CreateNestedEnvironment failed: {}", e);
                        control_handle.shutdown();
                    }
                }
            }
        }
    }
}

/// Create a new and single enclosing env for every test. Each test only gets a single enclosing env
/// no matter how many times it connects to Environment service.
async fn gen_enclosing_env(handles: LocalComponentHandles) -> Result<(), Error> {
    // This function should only be called when test tries to connect to Environment or Launcher.
    let mut fs = ServiceFs::new();
    let incoming_svc = handles.clone_from_namespace("svc")?;
    let enclosing_env =
        EnclosingEnvironment::new(incoming_svc).context("Cannot create enclosing env")?;
    let enclosing_env_clone = enclosing_env.clone();
    let enclosing_env_clone2 = enclosing_env.clone();

    fs.dir("svc")
        .add_fidl_service(move |req_stream: fv1sys::EnvironmentRequestStream| {
            debug!("Received Env connection request");
            let enclosing_env = enclosing_env.clone();
            fasync::Task::spawn(async move {
                enclosing_env.serve(req_stream).await;
            })
            .detach();
        })
        .add_service_at(fv1sys::LauncherMarker::NAME, move |chan: fuchsia_zircon::Channel| {
            enclosing_env_clone.get_launcher(chan.into());
            None
        })
        .add_service_at(fv1sys::LoaderMarker::NAME, move |chan: fuchsia_zircon::Channel| {
            enclosing_env_clone2.connect_to_protocol(fv1sys::LoaderMarker::NAME, chan);
            None
        });

    fs.serve_connection(handles.outgoing_dir.into_channel())?;
    fs.collect::<()>().await;

    // TODO(fxbug.dev/82021): kill and clean environment
    Ok(())
}

#[cfg(test)]
mod tests {
    use {
        super::*, assert_matches::assert_matches, fidl::endpoints::create_proxy_and_stream,
        maplit::hashset,
    };

    #[test]
    fn case_matcher_tests() {
        let all_test_case_names = hashset! {
            "Foo.Test1", "Foo.Test2", "Foo.Test3", "Bar.Test1", "Bar.Test2", "Bar.Test3"
        };

        let cases = vec![
            (vec![], all_test_case_names.clone()),
            (vec!["Foo.Test1"], hashset! {"Foo.Test1"}),
            (vec!["Foo.*"], hashset! {"Foo.Test1", "Foo.Test2", "Foo.Test3"}),
            (vec!["-Foo.*"], hashset! {"Bar.Test1", "Bar.Test2", "Bar.Test3"}),
            (vec!["Foo.*", "-*.Test2"], hashset! {"Foo.Test1", "Foo.Test3"}),
        ];

        for (filters, expected_matching_cases) in cases.into_iter() {
            let case_matcher = CaseMatcher::new(&filters).expect("Create case matcher");
            for test_case in all_test_case_names.iter() {
                match expected_matching_cases.contains(test_case) {
                    true => assert!(
                        case_matcher.matches(test_case),
                        "Expected filters {:?} to match test case name {}",
                        filters,
                        test_case
                    ),
                    false => assert!(
                        !case_matcher.matches(test_case),
                        "Expected filters {:?} to not match test case name {}",
                        filters,
                        test_case
                    ),
                }
            }
        }
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn suite_controller_stop_test() {
        let (sender, recv) = mpsc::channel(1024);
        let (stop_sender, stop_recv) = oneshot::channel::<()>();
        let (task, remote_handle) = async move {
            stop_recv.await.unwrap();
            // drop event sender so that fake test can end.
            drop(sender);
        }
        .remote_handle();
        let _task = fasync::Task::spawn(task);
        let (proxy, mut controller) =
            create_proxy_and_stream::<ftest_manager::SuiteControllerMarker>().unwrap();
        let run_controller = fasync::Task::spawn(async move {
            Suite::run_controller(&mut controller, stop_sender, remote_handle, recv).await
        });
        proxy.stop().unwrap();

        assert_eq!(proxy.get_events().await.unwrap(), Ok(vec![]));
        // this should end
        run_controller.await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn suite_controller_abort_remote_when_controller_closed() {
        let (_sender, recv) = mpsc::channel(1024);
        let (stop_sender, _stop_recv) = oneshot::channel::<()>();
        // Create a future that normally never resolves.
        let (task, remote_handle) = futures::future::pending().remote_handle();
        let pending_task = fasync::Task::spawn(task);
        let (proxy, mut controller) =
            create_proxy_and_stream::<ftest_manager::SuiteControllerMarker>().unwrap();
        let run_controller = fasync::Task::spawn(async move {
            Suite::run_controller(&mut controller, stop_sender, remote_handle, recv).await
        });
        drop(proxy);
        // After controller is dropped, both the controller future and the task it was
        // controlling should terminate.
        pending_task.await;
        run_controller.await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn suite_controller_get_events() {
        let (mut sender, recv) = mpsc::channel(1024);
        let (stop_sender, stop_recv) = oneshot::channel::<()>();
        let (task, remote_handle) = async {}.remote_handle();
        let _task = fasync::Task::spawn(task);
        let (proxy, mut controller) =
            create_proxy_and_stream::<ftest_manager::SuiteControllerMarker>().unwrap();
        let run_controller = fasync::Task::spawn(async move {
            Suite::run_controller(&mut controller, stop_sender, remote_handle, recv).await
        });
        sender.send(Ok(SuiteEvents::case_found(1, "case1".to_string()).into())).await.unwrap();
        sender.send(Ok(SuiteEvents::case_found(2, "case2".to_string()).into())).await.unwrap();

        let events = proxy.get_events().await.unwrap().unwrap();
        assert_eq!(events.len(), 2);
        assert_eq!(
            events[0].payload,
            SuiteEvents::case_found(1, "case1".to_string()).into_suite_run_event().payload,
        );
        assert_eq!(
            events[1].payload,
            SuiteEvents::case_found(2, "case2".to_string()).into_suite_run_event().payload,
        );
        sender.send(Ok(SuiteEvents::case_started(2).into())).await.unwrap();
        proxy.stop().unwrap();

        // test that controller collects event after stop is called.
        sender.send(Ok(SuiteEvents::case_started(1).into())).await.unwrap();
        sender.send(Ok(SuiteEvents::case_found(3, "case3".to_string()).into())).await.unwrap();

        stop_recv.await.unwrap();
        // drop event sender so that fake test can end.
        drop(sender);
        let events = proxy.get_events().await.unwrap().unwrap();
        assert_eq!(events.len(), 3);

        assert_eq!(events[0].payload, SuiteEvents::case_started(2).into_suite_run_event().payload,);
        assert_eq!(events[1].payload, SuiteEvents::case_started(1).into_suite_run_event().payload,);
        assert_eq!(
            events[2].payload,
            SuiteEvents::case_found(3, "case3".to_string()).into_suite_run_event().payload,
        );

        let events = proxy.get_events().await.unwrap().unwrap();
        assert_eq!(events, vec![]);
        // this should end
        run_controller.await.unwrap();
    }

    #[test]
    fn suite_controller_hanging_get_events() {
        let mut executor = fasync::TestExecutor::new().unwrap();
        let (mut sender, recv) = mpsc::channel(1024);
        let (stop_sender, _stop_recv) = oneshot::channel::<()>();
        let (task, remote_handle) = async {}.remote_handle();
        let _task = fasync::Task::spawn(task);
        let (proxy, mut controller) =
            create_proxy_and_stream::<ftest_manager::SuiteControllerMarker>().unwrap();
        let _run_controller = fasync::Task::spawn(async move {
            Suite::run_controller(&mut controller, stop_sender, remote_handle, recv).await
        });

        // send get event call which would hang as there are no events.
        let mut get_events =
            fasync::Task::spawn(async move { proxy.get_events().await.unwrap().unwrap() });
        assert_eq!(executor.run_until_stalled(&mut get_events), std::task::Poll::Pending);
        executor.run_singlethreaded(async {
            sender.send(Ok(SuiteEvents::case_found(1, "case1".to_string()).into())).await.unwrap();
            sender.send(Ok(SuiteEvents::case_found(2, "case2".to_string()).into())).await.unwrap();
        });
        let events = executor.run_singlethreaded(get_events);
        assert_eq!(events.len(), 2);
        assert_eq!(
            events[0].payload,
            SuiteEvents::case_found(1, "case1".to_string()).into_suite_run_event().payload,
        );
        assert_eq!(
            events[1].payload,
            SuiteEvents::case_found(2, "case2".to_string()).into_suite_run_event().payload,
        );
    }

    #[test]
    fn suite_events() {
        let event = SuiteEvents::case_found(1, "case1".to_string()).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_eq!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseFound(ftest_manager::CaseFound {
                test_case_name: "case1".into(),
                identifier: 1
            }))
        );

        let event = SuiteEvents::case_started(2).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_eq!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseStarted(ftest_manager::CaseStarted { identifier: 2 }))
        );

        let event = SuiteEvents::case_stopped(2, CaseStatus::Failed).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_eq!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseStopped(ftest_manager::CaseStopped {
                identifier: 2,
                status: CaseStatus::Failed
            }))
        );

        let event = SuiteEvents::case_finished(2).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_eq!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseFinished(ftest_manager::CaseFinished {
                identifier: 2
            }))
        );

        let (sock1, _sock2) = zx::Socket::create(zx::SocketOpts::empty()).unwrap();
        let event = SuiteEvents::case_stdout(2, sock1).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_matches!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseArtifact(ftest_manager::CaseArtifact {
                identifier: 2,
                artifact: ftest_manager::Artifact::Stdout(_)
            }))
        );

        let (sock1, _sock2) = zx::Socket::create(zx::SocketOpts::empty()).unwrap();
        let event = SuiteEvents::case_stderr(2, sock1).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_matches!(
            event.payload,
            Some(FidlSuiteEventPayload::CaseArtifact(ftest_manager::CaseArtifact {
                identifier: 2,
                artifact: ftest_manager::Artifact::Stderr(_)
            }))
        );

        let event = SuiteEvents::suite_stopped(SuiteStatus::Failed).into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_eq!(
            event.payload,
            Some(FidlSuiteEventPayload::SuiteStopped(ftest_manager::SuiteStopped {
                status: SuiteStatus::Failed,
            }))
        );

        let (client_end, _server_end) = fidl::endpoints::create_endpoints().unwrap();
        let event = SuiteEvents::suite_syslog(ftest_manager::Syslog::Archive(client_end))
            .into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_matches!(
            event.payload,
            Some(FidlSuiteEventPayload::SuiteArtifact(ftest_manager::SuiteArtifact {
                artifact: ftest_manager::Artifact::Log(ftest_manager::Syslog::Archive(_)),
            }))
        );

        let (client_end, _server_end) = fidl::endpoints::create_endpoints().unwrap();
        let event = SuiteEvents::suite_syslog(ftest_manager::Syslog::Batch(client_end))
            .into_suite_run_event();
        assert_matches!(event.timestamp, Some(_));
        assert_matches!(
            event.payload,
            Some(FidlSuiteEventPayload::SuiteArtifact(ftest_manager::SuiteArtifact {
                artifact: ftest_manager::Artifact::Log(ftest_manager::Syslog::Batch(_)),
            }))
        );
    }

    #[test]
    fn suite_status() {
        let all_case_status = vec![
            CaseStatus::Error,
            CaseStatus::TimedOut,
            CaseStatus::Failed,
            CaseStatus::Skipped,
            CaseStatus::Passed,
        ];
        for status in &all_case_status {
            assert_eq!(
                get_suite_status_from_case_status(SuiteStatus::InternalError, *status),
                SuiteStatus::InternalError
            );
        }

        for status in &all_case_status {
            let s = get_suite_status_from_case_status(SuiteStatus::TimedOut, *status);
            assert_eq!(s, SuiteStatus::TimedOut);
        }

        for status in &all_case_status {
            let s = get_suite_status_from_case_status(SuiteStatus::DidNotFinish, *status);
            let mut expected = SuiteStatus::DidNotFinish;
            if status == &CaseStatus::TimedOut {
                expected = SuiteStatus::TimedOut;
            }
            assert_eq!(s, expected);
        }

        for status in &all_case_status {
            let s = get_suite_status_from_case_status(SuiteStatus::Failed, *status);
            let mut expected = SuiteStatus::Failed;
            if status == &CaseStatus::TimedOut {
                expected = SuiteStatus::TimedOut;
            }
            assert_eq!(s, expected);
        }

        for status in &all_case_status {
            let s = get_suite_status_from_case_status(SuiteStatus::Passed, *status);
            let mut expected = SuiteStatus::Passed;
            if status == &CaseStatus::Error {
                expected = SuiteStatus::Failed;
            }
            if status == &CaseStatus::TimedOut {
                expected = SuiteStatus::TimedOut;
            }
            if status == &CaseStatus::Failed {
                expected = SuiteStatus::Failed;
            }
            assert_eq!(s, expected);
        }

        let all_suite_status = vec![
            SuiteStatus::Passed,
            SuiteStatus::Failed,
            SuiteStatus::TimedOut,
            SuiteStatus::Stopped,
            SuiteStatus::InternalError,
        ];

        for initial_status in &all_suite_status {
            for status in &all_suite_status {
                let s = concat_suite_status(*initial_status, *status);
                let expected: SuiteStatus;
                if initial_status.into_primitive() > status.into_primitive() {
                    expected = *initial_status;
                } else {
                    expected = *status;
                }

                assert_eq!(s, expected);
            }
        }
    }
}
