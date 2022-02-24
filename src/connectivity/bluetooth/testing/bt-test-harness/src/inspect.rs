// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context, Error},
    bt_device_watcher::DeviceWatcher,
    diagnostics_reader::{ArchiveReader, ComponentSelector, DiagnosticsHierarchy, Inspect},
    fidl_fuchsia_bluetooth_sys::{AccessMarker, AccessProxy},
    fuchsia_async::DurationExt,
    fuchsia_bluetooth::{
        constants::HOST_DEVICE_DIR,
        expectation::{
            asynchronous::{
                expectable, Expectable, ExpectableExt, ExpectableState, ExpectableStateExt,
            },
            Predicate,
        },
    },
    fuchsia_zircon::DurationNum,
    futures::{future::BoxFuture, FutureExt},
    hci_emulator_client::Emulator,
    std::{
        ops::{Deref, DerefMut},
        path::PathBuf,
        sync::Arc,
    },
    test_harness::{SharedState, TestHarness},
};

use crate::timeout_duration;

const RETRY_TIMEOUT_SECONDS: i64 = 1;

#[derive(Default, Clone)]
pub struct InspectState {
    pub moniker: Vec<String>,
    pub hierarchies: Vec<DiagnosticsHierarchy>,
}

#[derive(Clone)]
pub struct InspectHarness(Expectable<InspectState, AccessProxy>);

impl InspectHarness {
    // Check if there are at least `min_num` hierarchies in our Inspect State. If so, return the
    // inspect state, otherwise return Error.
    pub async fn expect_n_hierarchies(&self, min_num: usize) -> Result<InspectState, Error> {
        self.when_satisfied(
            Predicate::<InspectState>::predicate(
                move |state| state.hierarchies.len() >= min_num,
                "Expected number of hierarchies received",
            ),
            timeout_duration(),
        )
        .await
    }
}

impl Deref for InspectHarness {
    type Target = Expectable<InspectState, AccessProxy>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

impl DerefMut for InspectHarness {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.0
    }
}

pub async fn handle_inspect_updates(harness: InspectHarness) -> Result<(), Error> {
    loop {
        if harness.read().moniker.len() > 0 {
            let mut reader = ArchiveReader::new();
            let _ = reader.add_selector(ComponentSelector::new(harness.read().moniker.clone()));
            harness.write_state().hierarchies = reader
                .snapshot::<Inspect>()
                .await?
                .into_iter()
                .flat_map(|result| result.payload)
                .collect();
            harness.notify_state_changed();
        }
        fuchsia_async::Timer::new(RETRY_TIMEOUT_SECONDS.seconds().after_now()).await;
    }
}

pub async fn new_inspect_harness() -> Result<(InspectHarness, Emulator, PathBuf), Error> {
    let emulator: Emulator = Emulator::create(None).await?;
    let host_dev = emulator.publish_and_wait_for_host(Emulator::default_settings()).await?;
    let host_path = host_dev.relative_path().to_path_buf();

    let proxy = fuchsia_component::client::connect_to_protocol::<AccessMarker>()
        .context("Failed to connect to Access service")?;

    let inspect_harness = InspectHarness(expectable(Default::default(), proxy));
    Ok((inspect_harness, emulator, host_path))
}

impl TestHarness for InspectHarness {
    type Env = (PathBuf, Emulator);
    type Runner = BoxFuture<'static, Result<(), Error>>;

    fn init(
        _shared_state: &Arc<SharedState>,
    ) -> BoxFuture<'static, Result<(Self, Self::Env, Self::Runner), Error>> {
        async {
            let (harness, emulator, host_path) = new_inspect_harness().await?;
            let run_inspect = handle_inspect_updates(harness.clone()).boxed();
            Ok((harness, (host_path, emulator), run_inspect))
        }
        .boxed()
    }

    fn terminate(env: Self::Env) -> BoxFuture<'static, Result<(), Error>> {
        let (host_path, mut emulator) = env;
        async move {
            // Shut down the fake bt-hci device and make sure the bt-host device gets removed.
            let mut watcher =
                DeviceWatcher::new_in_namespace(HOST_DEVICE_DIR, timeout_duration()).await?;
            emulator.destroy_and_wait().await?;
            watcher.watch_removed(&host_path).await
        }
        .boxed()
    }
}
