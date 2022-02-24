// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    component_events::{events::*, matcher::*},
    fidl_fuchsia_sys2 as fsys, fuchsia_async as fasync,
    fuchsia_component_test::new::{Capability, ChildOptions, RealmBuilder, Ref, Route},
};

async fn start_nested_cm_and_wait_for_clean_stop(root_url: &str, moniker_to_wait_on: &str) {
    let builder = RealmBuilder::new().await.unwrap();
    let root = builder.add_child("root", root_url, ChildOptions::new().eager()).await.unwrap();
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol_by_name("fuchsia.logger.LogSink"))
                .capability(Capability::protocol_by_name("fuchsia.sys2.EventSource"))
                .from(Ref::parent())
                .to(&root),
        )
        .await
        .unwrap();
    let instance =
        builder.build_in_nested_component_manager("#meta/component_manager.cm").await.unwrap();
    let proxy =
        instance.root.connect_to_protocol_at_exposed_dir::<fsys::EventSourceMarker>().unwrap();

    let event_source = EventSource::from_proxy(proxy);

    let mut event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Stopped::NAME], EventMode::Async)])
        .await
        .unwrap();

    event_source.start_component_tree().await;

    // Expect the component to stop
    EventMatcher::ok()
        .stop(Some(ExitStatusMatcher::Clean))
        .moniker_regex(moniker_to_wait_on)
        .wait::<Stopped>(&mut event_stream)
        .await
        .unwrap();
}

#[fasync::run_singlethreaded(test)]
async fn advanced_routing() {
    start_nested_cm_and_wait_for_clean_stop(
        "#meta/advanced_routing_echo_realm.cm",
        "./root/reporter",
    )
    .await;
}

#[fasync::run_singlethreaded(test)]
async fn dynamic_child() {
    start_nested_cm_and_wait_for_clean_stop("#meta/dynamic_child_reporter.cm", "./root").await;
}

#[fasync::run_singlethreaded(test)]
async fn visibility() {
    start_nested_cm_and_wait_for_clean_stop("#meta/visibility_reporter.cm", "./root").await;
}

#[fasync::run_singlethreaded(test)]
async fn resolver() {
    start_nested_cm_and_wait_for_clean_stop("#meta/resolver.cm", "./root").await;
}

#[fasync::run_singlethreaded(test)]
async fn structured_config() {
    start_nested_cm_and_wait_for_clean_stop("#meta/structured_config_reporter.cm", "./root").await;
}
