// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context, Error},
    component_events::{
        events::{Discovered, Event, EventMode, EventSource, EventSubscription, Started},
        matcher::EventMatcher,
        sequence::*,
    },
    fidl::endpoints::ServiceMarker,
    fidl_fuchsia_examples_services as fexamples, fidl_fuchsia_sys2 as fsys2,
    fuchsia_component::client,
    fuchsia_component_test::ScopedInstance,
    tracing::*,
};

/// Name of the collection that contains branch components.
const BRANCHES_COLLECTION: &str = "branches";

/// Component URL of the branch component.
const BRANCH_COMPONENT_URL: &str = "#meta/service-routing-branch.cm";

/// Name of the collection in the branch component that contains BankAccount service providers.
const ACCOUNT_PROVIDERS_COLLECTION: &str = "account_providers";

/// Name of the provider-a.cm child component in the branch.
const PROVIDER_A_NAME: &str = "a";

/// Name of the provider-b.cm child component in the branch.
const PROVIDER_B_NAME: &str = "b";

/// Path to the LifecycleController protocol in the hub.
const LIFECYCLE_CONTROLLER_HUB_PATH: &str = "/hub/debug/fuchsia.sys2.LifecycleController";

#[fuchsia::test]
async fn list_instances_test() {
    let branch = start_branch().await.expect("failed to start branch component");
    start_provider(&branch, PROVIDER_A_NAME).await.expect("failed to start provider a");
    start_provider(&branch, PROVIDER_B_NAME).await.expect("failed to start provider b");

    // List the instances in the BankAccount service.
    let service_dir = io_util::directory::open_directory(
        branch.get_exposed_dir(),
        fexamples::BankAccountMarker::SERVICE_NAME,
        io_util::OPEN_RIGHT_READABLE,
    )
    .await
    .expect("failed to open service dir");

    let instances = files_async::readdir(&service_dir)
        .await
        .expect("failed to read entries from service dir")
        .into_iter()
        .map(|dirent| dirent.name);

    assert_eq!(2, instances.len());
}

#[fuchsia::test]
async fn connect_to_instances_test() {
    let branch = start_branch().await.expect("failed to start branch component");
    start_provider(&branch, PROVIDER_A_NAME).await.expect("failed to start provider a");
    start_provider(&branch, PROVIDER_B_NAME).await.expect("failed to start provider b");

    // List the instances in the BankAccount service.
    let service_dir = io_util::directory::open_directory(
        branch.get_exposed_dir(),
        fexamples::BankAccountMarker::SERVICE_NAME,
        io_util::OPEN_RIGHT_READABLE,
    )
    .await
    .expect("failed to open service dir");
    let instances = files_async::readdir(&service_dir)
        .await
        .expect("failed to read entries from service dir")
        .into_iter()
        .map(|dirent| dirent.name);

    // Connect to every instance and ensure the protocols are functional.
    for instance in instances {
        let proxy = client::connect_to_service_instance_at_dir::<fexamples::BankAccountMarker>(
            branch.get_exposed_dir(),
            &instance,
        )
        .expect("failed to connect to service instance");
        let read_only_account = proxy.read_only().expect("read_only protocol");
        let owner = read_only_account.get_owner().await.expect("failed to get owner");
        let initial_balance = read_only_account.get_balance().await.expect("failed to get_balance");
        info!("retrieved account for owner '{}' with balance ${}", &owner, &initial_balance);

        let read_write_account = proxy.read_write().expect("read_write protocol");
        assert_eq!(read_write_account.get_owner().await.expect("failed to get_owner"), owner);
        assert_eq!(
            read_write_account.get_balance().await.expect("failed to get_balance"),
            initial_balance
        );
    }
}

/// Starts a branch child component.
async fn start_branch() -> Result<ScopedInstance, Error> {
    let event_source = EventSource::new()?;
    let event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Discovered::NAME], EventMode::Async)])
        .await
        .context("failed to subscribe to EventSource")?;

    let branch =
        ScopedInstance::new(BRANCHES_COLLECTION.to_string(), BRANCH_COMPONENT_URL.to_string())
            .await
            .context("failed to create branch component instance")?;
    branch.start_with_binder_sync().await?;

    // Wait for the providers to be discovered (created) to ensure that
    // subsequent calls to `start_provider` can start them.
    EventSequence::new()
        .has_subset(
            vec![
                EventMatcher::ok().r#type(Discovered::TYPE).moniker_regex(format!(
                    "./{}:{}/{}:{}",
                    BRANCHES_COLLECTION,
                    branch.child_name(),
                    ACCOUNT_PROVIDERS_COLLECTION,
                    PROVIDER_A_NAME,
                )),
                EventMatcher::ok().r#type(Discovered::TYPE).moniker_regex(format!(
                    "./{}:{}/{}:{}",
                    BRANCHES_COLLECTION,
                    branch.child_name(),
                    ACCOUNT_PROVIDERS_COLLECTION,
                    PROVIDER_B_NAME,
                )),
            ],
            Ordering::Unordered,
        )
        .expect(event_stream)
        .await
        .context("event sequence did not match expected")?;

    Ok(branch)
}

/// Starts the provider with the name `child_name` in the branch component.
async fn start_provider(branch: &ScopedInstance, child_name: &str) -> Result<(), Error> {
    let lifecycle_controller_proxy = client::connect_to_protocol_at_path::<
        fsys2::LifecycleControllerMarker,
    >(LIFECYCLE_CONTROLLER_HUB_PATH)
    .context("failed to connect to LifecycleController")?;

    let event_source = EventSource::new()?;
    let event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Started::NAME], EventMode::Async)])
        .await
        .context("failed to subscribe to EventSource")?;

    let provider_moniker = format!(
        "./{}:{}/{}:{}",
        BRANCHES_COLLECTION,
        branch.child_name(),
        ACCOUNT_PROVIDERS_COLLECTION,
        child_name,
    );

    // Start the provider child.
    lifecycle_controller_proxy
        .start(&provider_moniker)
        .await?
        .map_err(|err| format_err!("failed to start provider component: {:?}", err))?;

    // Wait for the provider to start.
    EventSequence::new()
        .has_subset(
            vec![EventMatcher::ok().r#type(Started::TYPE).moniker_regex(provider_moniker)],
            Ordering::Unordered,
        )
        .expect(event_stream)
        .await
        .context("event sequence did not match expected")?;

    Ok(())
}
