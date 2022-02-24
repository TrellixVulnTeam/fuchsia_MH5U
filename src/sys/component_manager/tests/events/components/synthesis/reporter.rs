// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    assert_matches::assert_matches,
    component_events::events::{
        Destroyed, DirectoryReady, Event, EventMode, EventSource, EventSubscription, Running,
        Started,
    },
    fidl_fuchsia_sys2 as fsys,
    fuchsia_component_test::ScopedInstance,
    regex::Regex,
    std::{collections::BTreeSet, convert::TryFrom, iter::FromIterator},
    tracing::*,
};

#[fuchsia::component]
async fn main() {
    let event_source = EventSource::new().unwrap();
    let mut event_stream = event_source
        .subscribe(vec![EventSubscription::new(vec![Started::NAME], EventMode::Async)])
        .await
        .unwrap();

    // Make 4 components: 1 directory ready child and 3 stub children
    let mut instances = vec![];
    let url =
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/stub_component.cm".to_string();
    let url_cap_ready =
        "fuchsia-pkg://fuchsia.com/events_integration_test#meta/directory_ready_child.cm"
            .to_string();
    let scoped_instance =
        ScopedInstance::new("coll".to_string(), url_cap_ready.clone()).await.unwrap();
    let _ = scoped_instance.connect_to_binder().unwrap();
    instances.push(scoped_instance);
    assert_matches!(
        event_stream.next().await.unwrap(),
        fsys::Event { header: Some(fsys::EventHeader { event_type: Some(Started::TYPE), .. }), .. }
    );

    for _ in 0..3 {
        let scoped_instance = ScopedInstance::new("coll".to_string(), url.clone()).await.unwrap();
        let _ = scoped_instance.connect_to_binder().unwrap();
        instances.push(scoped_instance);
        assert_matches!(
            event_stream.next().await.unwrap(),
            fsys::Event {
                header: Some(fsys::EventHeader { event_type: Some(Started::TYPE), .. }),
                ..
            }
        );
    }

    // Destroy one stub child, this shouldn't appear anywhere in the events.
    let mut instance = instances.pop().unwrap();
    let destroy_waiter = instance.take_destroy_waiter();
    drop(instance);
    destroy_waiter.await.unwrap();

    // Subscribe to events.
    let event_source = EventSource::new().unwrap();
    let mut event_stream = event_source
        .subscribe(vec![EventSubscription::new(
            vec![Running::NAME, Destroyed::NAME, DirectoryReady::NAME],
            EventMode::Async,
        )])
        .await
        .unwrap();

    // There were 4 running instances when the stream was created: this instance itself and three
    // more. We are also expecting directory ready for one of them.
    let mut running = vec![];
    let mut directory_ready = vec![];

    while running.len() < 4 || directory_ready.len() < 1 {
        let event = event_stream.next().await.unwrap();
        if let Some(header) = &event.header {
            match header.event_type {
                Some(Running::TYPE) => {
                    let event = Running::try_from(event).expect("convert to running");
                    info!("Got running event");
                    running.push(event.target_moniker().to_string());
                }
                Some(DirectoryReady::TYPE) => {
                    let event =
                        DirectoryReady::try_from(event).expect("convert to directory ready");
                    info!("Got directory ready event");
                    directory_ready.push(event.target_moniker().to_string());
                }
                other => panic!("unexpected event type: {:?}", other),
            }
        }
    }

    // There must be exactly 4 unique running events, 1 directory ready event.
    // The first running event must be for this component itself.
    // The others must be from the dynamic collection.
    assert_eq!(running.len(), 4);
    assert_eq!(directory_ready.len(), 1);
    assert_eq!(running[0], ".");

    let re = Regex::new(r"./coll:auto-[[:xdigit:]]+").unwrap();
    assert!(running[1..].iter().all(|m| re.is_match(m)));

    assert_eq!(BTreeSet::from_iter::<Vec<String>>(running).len(), 4);

    // Dropping instances stops and destroys the children.
    drop(instances);

    // The three instances were destroyed.
    let mut seen_destroyed = 0;
    while seen_destroyed != 3 {
        let event = event_stream.next().await.unwrap();
        if let Some(header) = event.header {
            match header.event_type {
                Some(DirectoryReady::TYPE) => {
                    // ignore. we could get a duplicate here.
                }
                Some(Destroyed::TYPE) => {
                    seen_destroyed += 1;
                }
                event => {
                    panic!("Got unexpected event type: {:?}", event);
                }
            }
        }
    }
}
