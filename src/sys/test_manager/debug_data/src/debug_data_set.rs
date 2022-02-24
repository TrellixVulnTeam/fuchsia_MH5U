// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::message::DebugDataRequestMessage;
use anyhow::{anyhow, Error};
use async_trait::async_trait;
use fidl::endpoints::{RequestStream, ServerEnd};
use fidl_fuchsia_debugdata as fdebug;
use fidl_fuchsia_sys2 as fsys;
use fidl_fuchsia_test_internal as ftest_internal;
use fidl_fuchsia_test_manager as ftest_manager;
use fuchsia_inspect::types::Node;
use fuchsia_zircon as zx;
use futures::{channel::mpsc, lock::Mutex, SinkExt, Stream, StreamExt, TryFutureExt, TryStreamExt};
use log::{error, warn};
use moniker::{ChildMoniker, RelativeMoniker, RelativeMonikerBase};
use std::{
    collections::{HashMap, HashSet},
    sync::{Arc, Weak},
};

#[async_trait(?Send)]
pub trait DebugRequestHandler {
    /// Handle a stream of |DebugData| connection requests. The final processed data
    /// is reported over the |iterator| channel.
    async fn handle_debug_requests(
        &self,
        debug_request_recv: mpsc::Receiver<DebugDataRequestMessage>,
        iterator: ftest_manager::DebugDataIteratorRequestStream,
    ) -> Result<(), Error>;
}

/// Processes |DebugDataController| requests and component lifecycle events.
/// This collects the debug data sets that are defined over the |DebugDataController|
/// and |DebugDataSetController| processes, and demultiplixes the events into streams
/// per data set.
///
/// The output is a terminated stream of |DebugData| connection requests and an |DebugDataIterator|
/// request for each set. This output is passed to |debug_request_handler| for processing.
///
/// The stream is terminated by observing the passed lifecycle events and determining when all
/// realms in a set have stopped. To do this, a number of assumptions about ordering of events:
///  * A new realm is reported via DebugDataSetController::AddRealm before any events for that
///    realm are sent
///  * After a realm is stopped, no child components will start under it
/// If these assumptions are broken, events may be lost.
pub async fn handle_debug_data_controller_and_events<CS, D>(
    controller_requests: CS,
    events: fsys::EventStreamRequestStream,
    debug_request_handler: D,
    inspect_node: &Node,
) where
    CS: Stream<Item = Result<ftest_internal::DebugDataControllerRequest, fidl::Error>>
        + std::marker::Unpin,
    D: DebugRequestHandler,
{
    let debug_data_sets: Mutex<Vec<Weak<Mutex<inner::DebugDataSet>>>> = Mutex::new(vec![]);
    let debug_data_sets_ref = &debug_data_sets;
    let debug_request_handler_ref = &debug_request_handler;

    let inspect_node_count = std::sync::atomic::AtomicU32::new(0);
    let inspect_node_count_ref = &inspect_node_count;

    let controller_fut = controller_requests.for_each_concurrent(None, |request_result| {
        async move {
            let ftest_internal::DebugDataControllerRequest::NewSet { iter, controller, .. } =
                request_result?;
            let iter = iter.into_stream()?;
            let controller = controller.into_stream()?;
            let controller_handle = controller.control_handle();
            let (debug_request_send, debug_request_recv) = mpsc::channel(5);
            let debug_data_set =
                inner::DebugDataSet::new(debug_request_send, move |debug_data_requested| {
                    if debug_data_requested {
                        let _ = controller_handle.send_on_debug_data_produced();
                    }
                })
                .with_inspect(
                    inspect_node,
                    &format!(
                        "{:?}",
                        inspect_node_count_ref.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
                    ),
                );
            debug_data_sets_ref.lock().await.push(Arc::downgrade(&debug_data_set));
            futures::future::try_join(
                serve_debug_data_set_controller(&*debug_data_set, controller),
                debug_request_handler_ref.handle_debug_requests(debug_request_recv, iter),
            )
            .await?;
            Result::<(), Error>::Ok(())
        }
        .unwrap_or_else(|e| warn!("Error serving debug data set: {:?}", e))
    });
    let event_fut = route_events(events, debug_data_sets_ref)
        .unwrap_or_else(|e| error!("Error routing debug data events: {:?}", e));
    futures::future::join(controller_fut, event_fut).await;
}

async fn route_events(
    mut event_stream: fsys::EventStreamRequestStream,
    sets: &Mutex<Vec<Weak<Mutex<inner::DebugDataSet>>>>,
) -> Result<(), Error> {
    while let Some(req_res) = event_stream.next().await {
        let fsys::EventStreamRequest::OnEvent { event, .. } = match req_res {
            Ok(req) => req,
            Err(e) => {
                warn!("Error getting event: {:?}", e);
                continue;
            }
        };
        let header = event.header.as_ref().unwrap();
        let moniker = RelativeMoniker::parse(&header.moniker.as_ref().unwrap()).unwrap();
        let mut locked_sets = sets.lock().await;
        let mut active_sets = vec![];
        locked_sets.retain(|weak_set| match Weak::upgrade(weak_set) {
            Some(set) => {
                active_sets.push(set);
                true
            }
            None => false,
        });
        drop(locked_sets);
        let mut matched = false;
        for active_set in active_sets {
            let mut locked_set = active_set.lock().await;
            if locked_set.includes_moniker(&moniker) {
                locked_set
                    .handle_event(event)
                    .await
                    .unwrap_or_else(|e| warn!("Error handling event: {:?}", e));
                matched = true;
                break;
            }
        }
        if !matched {
            match moniker.down_path().get(0) {
                Some(child_moniker) if child_moniker.collection.is_some() => {
                    warn!("Unhandled event moniker {}", moniker)
                }
                // suppress warning if the moniker isn't in a collection (and thus isn't a test).
                None | Some(_) => (),
            }
        }
    }
    Ok(())
}

async fn serve_debug_data_set_controller(
    set: &Mutex<inner::DebugDataSet>,
    mut controller: ftest_internal::DebugDataSetControllerRequestStream,
) -> Result<(), Error> {
    let mut finish_called = false;
    let control_fut = async {
        while let Some(request) = controller.try_next().await? {
            match request {
                ftest_internal::DebugDataSetControllerRequest::AddRealm {
                    realm_moniker,
                    url,
                    responder,
                } => {
                    let mut result = match set.lock().await.add_realm(realm_moniker, url) {
                        Ok(()) => Ok(()),
                        Err(e) => {
                            warn!("Error while adding realm: {:?}", e);
                            Err(zx::Status::INVALID_ARGS.into_raw())
                        }
                    };
                    let _ = responder.send(&mut result)?;
                }
                ftest_internal::DebugDataSetControllerRequest::RemoveRealm {
                    realm_moniker,
                    ..
                } => match set.lock().await.remove_realm(realm_moniker) {
                    Ok(()) => (),
                    Err(e) => warn!("Error removing a realm: {:?}", e),
                },
                ftest_internal::DebugDataSetControllerRequest::Finish { .. } => {
                    finish_called = true;
                    break;
                }
            }
        }
        Ok::<_, fidl::Error>(())
    };
    let result = control_fut.await;
    set.lock().await.complete_set();
    result?;
    match finish_called {
        true => Ok(()),
        false => Err(anyhow!("Controller client did not call finish!")),
    }
}

mod inner {
    use {
        super::*,
        fuchsia_inspect::{ArrayProperty, LazyNode},
        futures::FutureExt,
        moniker::ChildMonikerBase,
    };

    /// Callback invoked when the DataSet determines that there is or is not any debug data
    /// produced. The parameter is true iff debug data was produced.
    type Callback = Box<dyn 'static + Fn(bool) + Send + Sync>;

    /// A container that tracks the current known state of realms in a debug data set.
    pub(super) struct DebugDataSet {
        realms: HashMap<ChildMoniker, String>,
        running_components: HashSet<RelativeMoniker>,
        destroyed_before_start: HashSet<RelativeMoniker>,
        seen_realms: HashSet<ChildMoniker>,
        done_adding_realms: bool,
        sender: mpsc::Sender<DebugDataRequestMessage>,
        on_capability_event: Option<Callback>,
        inspect_node: Option<LazyNode>,
    }

    impl DebugDataSet {
        /// Create a new DebugDataSet.
        pub fn new<F: 'static + Fn(bool) + Send + Sync>(
            sender: mpsc::Sender<DebugDataRequestMessage>,
            on_capability_event: F,
        ) -> Self {
            Self {
                realms: HashMap::new(),
                running_components: HashSet::new(),
                destroyed_before_start: HashSet::new(),
                seen_realms: HashSet::new(),
                done_adding_realms: false,
                sender,
                on_capability_event: Some(Box::new(on_capability_event)),
                inspect_node: None,
            }
        }

        // TODO(fxbug.dev/93280): array creation panics if a slot size larger than
        // 255 is specified. Remove this maximum once this is fixed in fuchsia_inspect.
        const MAX_INSPECT_ARRAY_SIZE: usize = u8::MAX as usize;

        /// Attach an inspect node to the DebugDataSet under |parent_node|.
        pub fn with_inspect(self, parent_node: &Node, name: &str) -> Arc<Mutex<Self>> {
            let arc_self = Arc::new(Mutex::new(self));
            let weak_self = Arc::downgrade(&arc_self);
            let lazy_node_fn = move || {
                let weak_clone = weak_self.clone();
                async move {
                    let inspector = fuchsia_inspect::Inspector::new();
                    let root = inspector.root();
                    if let Some(this_mutex) = weak_clone.upgrade() {
                        let this_lock = this_mutex.lock().await;
                        let this = &*this_lock;
                        root.record_child("realms", |realm_node| {
                            for (realm, url) in this.realms.iter() {
                                realm_node.record_string(realm.as_str(), &url);
                            }
                        });
                        root.record_int(
                            "num_running_components",
                            this.running_components.len() as i64,
                        );
                        let running_components = root.create_string_array(
                            "running_components",
                            std::cmp::min(
                                this.running_components.len(),
                                Self::MAX_INSPECT_ARRAY_SIZE,
                            ),
                        );
                        for (idx, cmp) in this
                            .running_components
                            .iter()
                            .take(Self::MAX_INSPECT_ARRAY_SIZE)
                            .enumerate()
                        {
                            running_components.set(idx, format!("{}", cmp))
                        }
                        root.record(running_components);
                        root.record_int(
                            "num_destroyed_before_start",
                            this.destroyed_before_start.len() as i64,
                        );
                        let destroyed_before_start = root.create_string_array(
                            "destroyed_before_start",
                            std::cmp::min(
                                this.destroyed_before_start.len(),
                                Self::MAX_INSPECT_ARRAY_SIZE,
                            ),
                        );
                        for (idx, cmp) in this
                            .destroyed_before_start
                            .iter()
                            .take(Self::MAX_INSPECT_ARRAY_SIZE)
                            .enumerate()
                        {
                            destroyed_before_start.set(idx, format!("{}", cmp))
                        }
                        root.record(destroyed_before_start);
                        root.record_int("num_seen_realms", this.seen_realms.len() as i64);
                        let seen_realms = root.create_string_array(
                            "seen_realms",
                            std::cmp::min(this.seen_realms.len(), Self::MAX_INSPECT_ARRAY_SIZE),
                        );
                        for (idx, cmp) in
                            this.seen_realms.iter().take(Self::MAX_INSPECT_ARRAY_SIZE).enumerate()
                        {
                            seen_realms.set(idx, format!("{}", cmp))
                        }
                        root.record(seen_realms);
                        root.record_bool("done_adding_realms", this.done_adding_realms);
                    }
                    Ok(inspector)
                }
                .boxed()
            };

            // Lock can't fail since we created the mutex above and control all the handles.
            arc_self
                .try_lock()
                .unwrap()
                .inspect_node
                .replace(parent_node.create_lazy_child(name, lazy_node_fn));
            arc_self
        }

        pub fn includes_moniker(&self, moniker: &RelativeMoniker) -> bool {
            // Assumes that the test realm is contained in a collection under test_manager.
            if !moniker.up_path().is_empty() {
                return false;
            }
            match moniker.down_path().iter().next() {
                None => false,
                Some(child_moniker) => self.realms.contains_key(child_moniker),
            }
        }

        pub fn add_realm(&mut self, moniker: String, url: String) -> Result<(), Error> {
            let moniker_child = realm_moniker_child(moniker)?;
            self.realms.insert(moniker_child, url);
            Ok(())
        }

        pub fn remove_realm(&mut self, moniker: String) -> Result<(), Error> {
            let moniker_child = realm_moniker_child(moniker)?;
            self.realms.remove(&moniker_child);
            self.running_components
                .retain(|component_moniker| component_moniker.down_path()[0] != moniker_child);
            self.destroyed_before_start
                .retain(|component_moniker| component_moniker.down_path()[0] != moniker_child);
            self.seen_realms.remove(&moniker_child);
            self.close_sink_if_done();
            Ok(())
        }

        pub fn complete_set(&mut self) {
            self.done_adding_realms = true;
            self.close_sink_if_done();
        }

        pub async fn handle_event(&mut self, event: fsys::Event) -> Result<(), Error> {
            let header = event.header.as_ref().ok_or(anyhow!("Event contained no header"))?;
            let unparsed_moniker =
                header.moniker.as_ref().ok_or(anyhow!("Event contained no moniker"))?;
            let moniker = RelativeMoniker::parse(unparsed_moniker)?;
            let realm_id = moniker
                .down_path()
                .iter()
                .next()
                .ok_or(anyhow!("Event moniker contains empty down path"))?
                .clone();

            match header.event_type.ok_or(anyhow!("Event contained no event type"))? {
                fsys::EventType::CapabilityRequested => {
                    let test_url = self.realms.get(&realm_id).unwrap().clone();
                    let request = debug_data_request_from_event(event);
                    self.sender.send(DebugDataRequestMessage { test_url, request }).await?;
                    self.on_capability_event.take().map(|callback| callback(true));
                }
                fsys::EventType::Started => {
                    if self.destroyed_before_start.remove(&moniker) {
                        warn!("Got a destroy event before start event for {}", moniker);
                    } else {
                        self.seen_realms.insert(realm_id);
                        self.running_components.insert(moniker);
                    }
                }
                // TODO(fxbug.dev/86503): Sometimes an instance may be destroyed before it is
                // started or stopped. So we listen for destroyed instead of stopped, and record
                // instances for which we destroyed but never got a start event for.
                fsys::EventType::Destroyed => {
                    if !self.running_components.remove(&moniker) {
                        self.seen_realms.insert(realm_id);
                        self.destroyed_before_start.insert(moniker);
                    }
                    self.close_sink_if_done();
                }
                other => warn!("Got unhandled event type: {:?}", other),
            }
            Ok(())
        }

        fn close_sink_if_done(&mut self) {
            if self.done_adding_realms
                && self.running_components.is_empty()
                && self.seen_realms.len() == self.realms.len()
            {
                self.sender.close_channel();
                self.on_capability_event.take().map(|callback| callback(false));
            }
        }
    }

    fn realm_moniker_child(realm_moniker: String) -> Result<ChildMoniker, Error> {
        let moniker = RelativeMoniker::parse(&realm_moniker)?;
        let moniker_is_valid = moniker.up_path().is_empty() && moniker.down_path().len() == 1;
        match moniker_is_valid {
            true => Ok(moniker.down_path()[0].clone()),
            false => Err(anyhow!("Moniker {:?} invalidates assumptions about test topology")),
        }
    }

    fn debug_data_request_from_event(event: fsys::Event) -> ServerEnd<fdebug::DebugDataMarker> {
        let result = event.event_result.unwrap();
        match result {
            fsys::EventResult::Payload(fsys::EventPayload::CapabilityRequested(
                fsys::CapabilityRequestedPayload { capability, .. },
            )) => {
                // todo check capability name and other stuff
                ServerEnd::new(capability.unwrap())
            }
            _ => panic!("unexpected payload"),
        }
    }

    #[cfg(test)]
    mod test {
        use super::super::testing::*;
        use super::*;
        use fuchsia_inspect::assert_data_tree;
        use maplit::hashmap;

        #[fuchsia::test]
        fn includes_moniker() {
            let added_realms = vec![
                ("./test:child-1", "test-url-1"),
                ("./test:child-2", "test-url-2"),
                ("./system-test:child-1", "test-url-3"),
                ("./system-test:child-2", "test-url-4"),
            ];
            let included_monikers = vec![
                "./test:child-1",
                "./test:child-1/sub1",
                "./test:child-1/sub1/sub2:child",
                "./system-test:child-1",
                "./system-test:child-2/sub1:child/sub2",
            ];
            let excluded_monikers = vec![
                ".",
                ".\\super/test:child-1",
                "./test:child-3",
                "./test:child-3/sub1",
                "./realm",
                "./realm/sub1",
                "./system-test:child3",
            ];

            let (send, _) = mpsc::channel(1);
            let mut set = DebugDataSet::new(send, |_| ());
            for (realm, url) in added_realms {
                set.add_realm(realm.to_string(), url.to_string()).expect("add realm");
            }
            for moniker in included_monikers {
                let parsed = RelativeMoniker::parse(moniker).unwrap();
                assert!(set.includes_moniker(&parsed), "Expected {} to be in the set", moniker);
            }
            for moniker in excluded_monikers {
                let parsed = RelativeMoniker::parse(moniker).unwrap();
                assert!(
                    !set.includes_moniker(&parsed),
                    "Expected {} to not be in the set",
                    moniker
                );
            }
        }

        fn common_test_realms() -> Vec<(&'static str, &'static str)> {
            vec![("./test:child-1", "test-url-1"), ("./system-test:child-2", "test-url-2")]
        }

        fn common_test_events() -> Vec<fsys::Event> {
            vec![
                start_event("./test:child-1"),
                capability_event("./test:child-1"),
                destroy_event("./test:child-1"),
                start_event("./system-test:child-2"),
                capability_event("./system-test:child-2"),
                start_event("./system-test:child-2/sub1"),
                capability_event("./system-test:child-2/sub1"),
                destroy_event("./system-test:child-2/sub1"),
                destroy_event("./system-test:child-2"),
            ]
        }

        /// Collect the requests sent on the receiver and count by test url.
        async fn collect_requests_to_count(
            recv: mpsc::Receiver<DebugDataRequestMessage>,
        ) -> HashMap<String, u32> {
            let mut occurrences = HashMap::new();
            recv.for_each(|message| {
                occurrences.entry(message.test_url).and_modify(|count| *count += 1).or_insert(1);
                futures::future::ready(())
            })
            .await;
            occurrences
        }

        #[fuchsia::test]
        async fn no_debug_data() {
            let (send, recv) = mpsc::channel(10);
            let mut set = DebugDataSet::new(send, |got_data| assert!(!got_data));
            for (realm, url) in common_test_realms() {
                set.add_realm(realm.to_string(), url.to_string()).expect("add realm");
                set.handle_event(start_event(realm)).await.expect("handle event");
                set.handle_event(destroy_event(realm)).await.expect("handle event");
            }
            set.complete_set();
            assert_eq!(collect_requests_to_count(recv).await, hashmap! {});
        }

        #[fuchsia::test]
        async fn complete_set_before_realm_events_complete() {
            let (send, recv) = mpsc::channel(10);
            let mut set = DebugDataSet::new(send, |got_data| assert!(got_data));
            for (realm, url) in common_test_realms() {
                set.add_realm(realm.to_string(), url.to_string()).expect("add realm");
            }
            // If the set is marked complete, then events finish, stream should terminate.
            set.complete_set();
            for event in common_test_events() {
                set.handle_event(event).await.expect("handle event");
            }

            assert_eq!(
                collect_requests_to_count(recv).await,
                hashmap! {
                    "test-url-1".to_string() => 1,
                    "test-url-2".to_string() => 2
                }
            );
        }

        #[fuchsia::test]
        async fn complete_set_after_realm_events_complete() {
            let (send, recv) = mpsc::channel(10);
            let mut set = DebugDataSet::new(send, |got_data| assert!(got_data));
            for (realm, url) in common_test_realms() {
                set.add_realm(realm.to_string(), url.to_string()).expect("add realm");
            }
            // If events finish, then set is marked complete, the stream should terminate.
            for event in common_test_events() {
                set.handle_event(event).await.expect("handle event");
            }
            set.complete_set();

            assert_eq!(
                collect_requests_to_count(recv).await,
                hashmap! {
                    "test-url-1".to_string() => 1,
                    "test-url-2".to_string() => 2
                }
            );
        }

        #[fuchsia::test]
        async fn add_realm_after_initial_realm_completes() {
            let (send, recv) = mpsc::channel(10);
            let mut set = DebugDataSet::new(send, |got_data| assert!(got_data));
            set.add_realm("./test:realm-1".to_string(), "test-url-1".to_string())
                .expect("add realm");
            set.handle_event(start_event("./test:realm-1")).await.expect("handle event");
            set.handle_event(capability_event("./test:realm-1")).await.expect("handle event");
            set.handle_event(destroy_event("./test:realm-1")).await.expect("handle event");
            // At this point, realm-1 has stopped, but the set should remain open as we may still
            // add additional realms.
            set.add_realm("./test:realm-2".to_string(), "test-url-2".to_string())
                .expect("add realm");
            set.handle_event(start_event("./test:realm-2")).await.expect("handle event");
            set.handle_event(capability_event("./test:realm-2")).await.expect("handle event");
            set.handle_event(destroy_event("./test:realm-2")).await.expect("handle event");
            set.complete_set();
            // Requests for both realms should be present.
            assert_eq!(
                collect_requests_to_count(recv).await,
                hashmap! {
                    "test-url-1".to_string() => 1,
                    "test-url-2".to_string() => 1
                }
            );
        }

        #[fuchsia::test]
        async fn remove_realm_before_it_produces_events() {
            let (send, recv) = mpsc::channel(10);
            let mut set = DebugDataSet::new(send, |got_data| assert!(got_data));
            set.add_realm("./test:realm-1".to_string(), "test-url-1".to_string())
                .expect("add realm");
            set.handle_event(start_event("./test:realm-1")).await.expect("handle event");
            set.handle_event(capability_event("./test:realm-1")).await.expect("handle event");
            set.handle_event(destroy_event("./test:realm-1")).await.expect("handle event");
            // At this point, realm-1 has stopped, but the set should remain open as we may still
            // add additional realms.
            set.add_realm("./test:realm-2".to_string(), "test-url-2".to_string())
                .expect("add realm");
            set.remove_realm("./test:realm-2".to_string()).expect("remove realm");
            set.complete_set();
            // Requests for only the realm that wasn't removed should be present.
            assert_eq!(
                collect_requests_to_count(recv).await,
                hashmap! {
                    "test-url-1".to_string() => 1,
                }
            );
        }

        #[fuchsia::test]
        async fn export_inspect() {
            let inspector = fuchsia_inspect::Inspector::new();
            let (send, recv) = mpsc::channel(10);
            let set = DebugDataSet::new(send, |got_data| assert!(!got_data))
                .with_inspect(inspector.root(), "set");
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {},
                        running_components: Vec::<String>::new(),
                        seen_realms: Vec::<String>::new(),
                        done_adding_realms: false,
                        destroyed_before_start: Vec::<String>::new()
                    }
                }
            );

            set.lock()
                .await
                .add_realm("./test:realm-1".to_string(), "test-url-1".to_string())
                .expect("add realm");
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {
                            "test:realm-1": "test-url-1"
                        },
                        done_adding_realms: false,
                        num_running_components: 0i64,
                        running_components: Vec::<String>::new(),
                        num_seen_realms: 0i64,
                        seen_realms: Vec::<String>::new(),
                    }
                }
            );

            set.lock()
                .await
                .handle_event(start_event("./test:realm-1"))
                .await
                .expect("handle event");
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {
                            "test:realm-1": "test-url-1"
                        },
                        done_adding_realms: false,
                        num_running_components: 1i64,
                        running_components: vec!["./test:realm-1"],
                        num_seen_realms: 1i64,
                        seen_realms: vec!["test:realm-1"]
                    }
                }
            );

            set.lock()
                .await
                .handle_event(destroy_event("./test:realm-1"))
                .await
                .expect("handle event");
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {
                            "test:realm-1": "test-url-1"
                        },
                        done_adding_realms: false,
                        running_components: Vec::<String>::new(),
                        seen_realms: vec!["test:realm-1"]
                    }
                }
            );

            set.lock().await.complete_set();
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {
                            "test:realm-1": "test-url-1"
                        },
                        done_adding_realms: true,
                        running_components: Vec::<String>::new(),
                        seen_realms: vec!["test:realm-1"]
                    }
                }
            );
            // Requests for only the realm that wasn't removed should be present.
            assert!(collect_requests_to_count(recv).await.is_empty());

            drop(set);
            assert_data_tree!(
                inspector,
                root: {}
            );
        }

        /// A PropertyAssertion impl that expects an array property to contain a subset of some
        /// string values.
        struct SubsetProperty {
            expected_superset: HashSet<String>,
            expected_size: usize,
        }

        impl SubsetProperty {
            fn new(expected_superset: HashSet<String>, expected_size: usize) -> Self {
                Self { expected_superset, expected_size }
            }
        }

        impl<K> fuchsia_inspect::testing::PropertyAssertion<K> for SubsetProperty {
            fn run(&self, actual: &fuchsia_inspect::hierarchy::Property<K>) -> Result<(), Error> {
                match actual {
                    fuchsia_inspect::hierarchy::Property::StringList(_, ref string_list) => {
                        let set: HashSet<String> = string_list.iter().cloned().collect();
                        match set.is_subset(&self.expected_superset) {
                            true => match set.len() == self.expected_size {
                                true => Ok(()),
                                false => Err(anyhow!(
                                    "Expected a set of size {:?} but got set {:?}",
                                    self.expected_size,
                                    set
                                )),
                            },
                            false => Err(anyhow!(
                                "Expected a subset of {:?} but got {:?}",
                                self.expected_superset,
                                set
                            )),
                        }
                    }
                    _ => Err(anyhow!("Expected a string list")),
                }
            }
        }

        #[fuchsia::test]
        async fn export_inspect_truncate_on_overflow() {
            let inspector = fuchsia_inspect::Inspector::new();
            let (send, _) = mpsc::channel(10);
            let set = DebugDataSet::new(send, |got_data| assert!(!got_data))
                .with_inspect(inspector.root(), "set");
            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        realms: {},
                        running_components: Vec::<String>::new(),
                        seen_realms: Vec::<String>::new(),
                        done_adding_realms: false,
                        destroyed_before_start: Vec::<String>::new()
                    }
                }
            );

            const OVERFLOW_COUNT: usize = DebugDataSet::MAX_INSPECT_ARRAY_SIZE + 1;
            for idx in 0..OVERFLOW_COUNT {
                let realm = format!("./test:{:?}", idx);
                let url = format!("url-{:?}", idx);
                set.lock().await.add_realm(realm, url).expect("add realm");
            }

            for idx in 0..OVERFLOW_COUNT {
                // add destroyed child realms first to test destroyed_before_start
                let realm = format!("./test:{:?}", idx);
                let moniker = format!("./test:{:?}/child", idx);
                set.lock().await.handle_event(destroy_event(&moniker)).await.expect("handle event");
                set.lock().await.handle_event(start_event(&realm)).await.expect("handle event");
            }

            assert_data_tree!(
                inspector,
                root: {
                    set: contains {
                        num_destroyed_before_start: OVERFLOW_COUNT as i64,
                        destroyed_before_start: SubsetProperty::new(
                            (0..OVERFLOW_COUNT).map(|idx| format!("./test:{:?}/child", idx)).collect(),
                            DebugDataSet::MAX_INSPECT_ARRAY_SIZE
                        ),
                        num_running_components: OVERFLOW_COUNT as i64,
                        running_components: SubsetProperty::new(
                            (0..OVERFLOW_COUNT).map(|idx| format!("./test:{:?}", idx)).collect(),
                            DebugDataSet::MAX_INSPECT_ARRAY_SIZE
                        ),
                        num_seen_realms: OVERFLOW_COUNT as i64,
                        seen_realms: SubsetProperty::new(
                            (0..OVERFLOW_COUNT).map(|idx| format!("test:{:?}", idx)).collect(),
                            DebugDataSet::MAX_INSPECT_ARRAY_SIZE
                        ),
                    }
                }
            );
        }
    }
}

#[cfg(test)]
mod testing {
    use super::*;
    use fidl::endpoints::ProtocolMarker;

    pub(super) fn start_event(moniker: &str) -> fsys::Event {
        fsys::Event {
            header: fsys::EventHeader {
                event_type: fsys::EventType::Started.into(),
                moniker: moniker.to_string().into(),
                ..fsys::EventHeader::EMPTY
            }
            .into(),
            event_result: Some(fsys::EventResult::Payload(fsys::EventPayload::Started(
                fsys::StartedPayload::EMPTY,
            ))),
            ..fsys::Event::EMPTY
        }
    }

    pub(super) fn destroy_event(moniker: &str) -> fsys::Event {
        fsys::Event {
            header: fsys::EventHeader {
                event_type: fsys::EventType::Destroyed.into(),
                moniker: moniker.to_string().into(),
                ..fsys::EventHeader::EMPTY
            }
            .into(),
            event_result: Some(fsys::EventResult::Payload(fsys::EventPayload::Destroyed(
                fsys::DestroyedPayload::EMPTY,
            ))),
            ..fsys::Event::EMPTY
        }
    }

    pub(super) fn capability_event(moniker: &str) -> fsys::Event {
        let (_client, server) = zx::Channel::create().unwrap();
        fsys::Event {
            header: fsys::EventHeader {
                event_type: fsys::EventType::CapabilityRequested.into(),
                moniker: moniker.to_string().into(),
                ..fsys::EventHeader::EMPTY
            }
            .into(),
            event_result: Some(fsys::EventResult::Payload(
                fsys::EventPayload::CapabilityRequested(fsys::CapabilityRequestedPayload {
                    name: fdebug::DebugDataMarker::NAME.to_string().into(),
                    capability: Some(server),
                    ..fsys::CapabilityRequestedPayload::EMPTY
                }),
            )),
            ..fsys::Event::EMPTY
        }
    }
}

#[cfg(test)]
mod test {
    use super::testing::*;
    use super::*;
    use assert_matches::assert_matches;
    use fidl::endpoints::{create_proxy, create_proxy_and_stream};
    use futures::Future;
    use maplit::hashmap;
    use std::sync::atomic::{AtomicU32, Ordering};

    /// A |DebugRequestHandler| implementation that counts the number of requests per state, and
    /// sends them on completion.
    struct TestDebugRequestHandler(AtomicU32, mpsc::Sender<DebugSetState>);

    #[derive(PartialEq, Debug)]
    struct DebugSetState {
        /// Number of requests received for each test URL.
        requests_for_url: HashMap<String, u32>,
        /// Index of the set (in order they are created.)
        index: u32,
    }

    impl TestDebugRequestHandler {
        fn new() -> (Self, mpsc::Receiver<DebugSetState>) {
            let (send, recv) = mpsc::channel(10);
            (Self(AtomicU32::new(0), send), recv)
        }
    }

    #[async_trait(?Send)]
    impl DebugRequestHandler for TestDebugRequestHandler {
        async fn handle_debug_requests(
            &self,
            mut debug_request_recv: mpsc::Receiver<DebugDataRequestMessage>,
            _iterator: ftest_manager::DebugDataIteratorRequestStream,
        ) -> Result<(), Error> {
            let index = self.0.fetch_add(1, Ordering::Relaxed);
            let mut requests_for_url = HashMap::new();

            while let Some(debug_request) = debug_request_recv.next().await {
                requests_for_url
                    .entry(debug_request.test_url)
                    .and_modify(|count| *count += 1)
                    .or_insert(1);
            }
            let _ = self.1.clone().send(DebugSetState { requests_for_url, index }).await;
            Ok(())
        }
    }

    async fn controller_and_event_test<F, Fut>(test_fn: F)
    where
        F: Fn(
            ftest_internal::DebugDataControllerProxy,
            fsys::EventStreamProxy,
            mpsc::Receiver<DebugSetState>,
        ) -> Fut,
        Fut: Future<Output = ()>,
    {
        let (controller_request_proxy, controller_request_stream) =
            create_proxy_and_stream::<ftest_internal::DebugDataControllerMarker>().unwrap();
        let (event_proxy, event_request_stream) =
            create_proxy_and_stream::<fsys::EventStreamMarker>().unwrap();
        let (request_handler, request_recv) = TestDebugRequestHandler::new();
        let ((), ()) = futures::future::join(
            handle_debug_data_controller_and_events(
                controller_request_stream,
                event_request_stream,
                request_handler,
                fuchsia_inspect::Inspector::new().root(),
            ),
            test_fn(controller_request_proxy, event_proxy, request_recv),
        )
        .await;
    }

    fn create_set_contoller_proxy() -> (
        ftest_internal::DebugDataSetControllerProxy,
        ServerEnd<ftest_internal::DebugDataSetControllerMarker>,
    ) {
        create_proxy::<ftest_internal::DebugDataSetControllerMarker>().unwrap()
    }

    fn create_iterator_proxy(
    ) -> (ftest_manager::DebugDataIteratorProxy, ServerEnd<ftest_manager::DebugDataIteratorMarker>)
    {
        create_proxy::<ftest_manager::DebugDataIteratorMarker>().unwrap()
    }

    const TEST_REALM: &str = "./test:child-1";
    const TEST_URL: &str = "test-url-1";

    #[fuchsia::test]
    async fn route_single_set_no_debug_data() {
        controller_and_event_test(|controller_proxy, event_proxy, mut request_recv| async move {
            let (set_controller, set_server) = create_set_contoller_proxy();
            let (_iterator, iterator_server) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server, set_server).expect("create new set");

            // Add a realm and send events showing it started and stopped.
            set_controller
                .add_realm(TEST_REALM, TEST_URL)
                .await
                .expect("add realm")
                .expect("add realm returned error");
            set_controller.finish().expect("finish set");
            event_proxy.on_event(start_event(TEST_REALM)).expect("start event");
            event_proxy.on_event(destroy_event(TEST_REALM)).expect("start event");

            // Since no debug data was produced, no OnDebugData event is produced.
            assert!(set_controller.take_event_stream().next().await.is_none());
            let set_state = request_recv.next().await.unwrap();
            assert_eq!(set_state, DebugSetState { requests_for_url: hashmap! {}, index: 0 });
        })
        .await;
    }

    #[fuchsia::test]
    async fn route_single_set_with_debug_data() {
        controller_and_event_test(|controller_proxy, event_proxy, mut request_recv| async move {
            let (set_controller, set_server) = create_set_contoller_proxy();
            let (_iterator, iterator_server) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server, set_server).expect("create new set");

            // Add a realm and send events showing it started and stopped.
            set_controller
                .add_realm(TEST_REALM, TEST_URL)
                .await
                .expect("add realm")
                .expect("add realm returned error");
            set_controller.finish().expect("finish set");
            event_proxy.on_event(start_event(TEST_REALM)).expect("start event");
            event_proxy.on_event(capability_event(TEST_REALM)).expect("capability event");
            event_proxy
                .on_event(capability_event(&format!("{}/child1", TEST_REALM)))
                .expect("capability event");
            event_proxy.on_event(destroy_event(TEST_REALM)).expect("start event");

            // OnDebugData event is produced.
            assert_matches!(
                set_controller.take_event_stream().next().await.unwrap().unwrap(),
                ftest_internal::DebugDataSetControllerEvent::OnDebugDataProduced { .. }
            );
            let set_state = request_recv.next().await.unwrap();
            assert_eq!(
                set_state,
                DebugSetState {
                    requests_for_url: hashmap! {
                        TEST_URL.to_string() => 2,
                    },
                    index: 0,
                }
            );
        })
        .await;
    }

    #[fuchsia::test]
    async fn destroy_before_start_okay() {
        // This test verifies that the stream won't hang if a Destroyed event comes before Started
        controller_and_event_test(|controller_proxy, event_proxy, mut request_recv| async move {
            let (set_controller, set_server) = create_set_contoller_proxy();
            let (_iterator, iterator_server) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server, set_server).expect("create new set");

            // Add a realm and send events showing it started and stopped.
            set_controller
                .add_realm(TEST_REALM, TEST_URL)
                .await
                .expect("add realm")
                .expect("add realm returned error");
            set_controller.finish().expect("finish set");
            event_proxy.on_event(destroy_event(TEST_REALM)).expect("destroy event");
            event_proxy.on_event(start_event(TEST_REALM)).expect("start event");

            // Since no debug data was produced, no OnDebugData event is produced.
            assert!(set_controller.take_event_stream().next().await.is_none());
            let set_state = request_recv.next().await.unwrap();
            assert_eq!(set_state, DebugSetState { requests_for_url: hashmap! {}, index: 0 });
        })
        .await;
    }

    #[fuchsia::test]
    async fn route_multiple_sets_with_debug_data() {
        controller_and_event_test(|controller_proxy, event_proxy, mut request_recv| async move {
            let set_1_realms =
                vec![("./test:child-1", "test-url-1-1"), ("./system-test:child-1", "test-url-1-2")];
            let set_2_realms =
                vec![("./test:child-2", "test-url-2-1"), ("./system-test:child-2", "test-url-2-2")];

            let (set_controller_1, set_server_1) = create_set_contoller_proxy();
            let (_iterator_1, iterator_server_1) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server_1, set_server_1).expect("create new set");

            let (set_controller_2, set_server_2) = create_set_contoller_proxy();
            let (_iterator_2, iterator_server_2) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server_2, set_server_2).expect("create new set");

            for (realm, url) in set_1_realms.iter() {
                set_controller_1
                    .add_realm(realm, url)
                    .await
                    .expect("add realm")
                    .expect("add_realm returned error");
                event_proxy.on_event(start_event(realm)).expect("send event");
                event_proxy.on_event(capability_event(realm)).expect("send event");
                event_proxy.on_event(destroy_event(realm)).expect("send event");
            }
            for (realm, url) in set_2_realms.iter() {
                set_controller_2
                    .add_realm(realm, url)
                    .await
                    .expect("add realm")
                    .expect("add_realm returned error");
                event_proxy.on_event(start_event(realm)).expect("send event");
                event_proxy.on_event(capability_event(realm)).expect("send event");
                event_proxy.on_event(destroy_event(realm)).expect("send event");
            }
            set_controller_1.finish().expect("finish set 1");
            set_controller_2.finish().expect("finish set 2");

            assert_matches!(
                set_controller_1.take_event_stream().next().await.unwrap().unwrap(),
                ftest_internal::DebugDataSetControllerEvent::OnDebugDataProduced { .. }
            );
            assert_matches!(
                set_controller_2.take_event_stream().next().await.unwrap().unwrap(),
                ftest_internal::DebugDataSetControllerEvent::OnDebugDataProduced { .. }
            );

            let set_state_1 = request_recv.next().await.unwrap();
            assert_eq!(
                set_state_1,
                DebugSetState {
                    requests_for_url: hashmap! {
                        "test-url-1-1".to_string() => 1,
                        "test-url-1-2".to_string() => 1,
                    },
                    index: 0,
                }
            );

            let set_state_2 = request_recv.next().await.unwrap();
            assert_eq!(
                set_state_2,
                DebugSetState {
                    requests_for_url: hashmap! {
                        "test-url-2-1".to_string() => 1,
                        "test-url-2-2".to_string() => 1,
                    },
                    index: 1,
                }
            );
        })
        .await;
    }

    #[fuchsia::test]
    async fn terminate_set_if_controller_terminates_without_finish() {
        controller_and_event_test(|controller_proxy, event_proxy, mut request_recv| async move {
            let (set_controller, set_server) = create_set_contoller_proxy();
            let (_iterator, iterator_server) = create_iterator_proxy();
            controller_proxy.new_set(iterator_server, set_server).expect("create new set");

            set_controller
                .add_realm(TEST_REALM, TEST_URL)
                .await
                .expect("add realm")
                .expect("add realm returned error");
            drop(set_controller);
            // drop event and controller proxy too so that the input streams terminate.
            drop(controller_proxy);
            drop(event_proxy);
            assert!(request_recv.next().await.is_none());
        })
        .await;
    }
}
