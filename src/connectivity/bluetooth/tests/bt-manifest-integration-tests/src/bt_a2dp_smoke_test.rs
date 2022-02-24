// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fidl::endpoints::DiscoverableProtocolMarker,
    fidl_fuchsia_bluetooth_a2dp as fidl_a2dp, fidl_fuchsia_bluetooth_avdtp as fidl_avdtp,
    fidl_fuchsia_bluetooth_avrcp as fidl_avrcp,
    fidl_fuchsia_bluetooth_bredr::{ProfileMarker, ProfileRequestStream},
    fidl_fuchsia_bluetooth_internal_a2dp::{ControllerMarker, ControllerProxy},
    fidl_fuchsia_cobalt::{LoggerFactoryMarker, LoggerFactoryRequestStream},
    fidl_fuchsia_media::{
        AudioDeviceEnumeratorMarker, AudioDeviceEnumeratorRequestStream,
        SessionAudioConsumerFactoryMarker, SessionAudioConsumerFactoryRequestStream,
    },
    fidl_fuchsia_media_sessions2::{
        DiscoveryMarker, DiscoveryRequestStream, PublisherMarker, PublisherRequestStream,
    },
    fidl_fuchsia_mediacodec::{CodecFactoryMarker, CodecFactoryRequestStream},
    fidl_fuchsia_power::{BatteryManagerMarker, BatteryManagerRequestStream},
    fidl_fuchsia_settings::{AudioMarker, AudioRequestStream},
    fidl_fuchsia_sysmem::{AllocatorMarker, AllocatorRequestStream},
    fidl_fuchsia_tracing_provider::{RegistryMarker, RegistryRequestStream},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_component_test::new::{
        Capability, ChildOptions, LocalComponentHandles, RealmBuilder, Ref, Route,
    },
    futures::{channel::mpsc, SinkExt, StreamExt},
    realmbuilder_mock_helpers::add_fidl_service_handler,
    std::{collections::HashSet, iter::FromIterator},
    tracing::info,
};

/// A2DP component URL.
const A2DP_URL: &str = "fuchsia-pkg://fuchsia.com/bt-a2dp-smoke-test#meta/bt-a2dp-topology-fake.cm";

/// The different events generated by this test.
/// Note: In order to prevent the component under test from terminating, any FIDL request or
/// Proxy is preserved.
enum Event {
    Profile(Option<ProfileRequestStream>),
    Avdtp(Option<fidl_avdtp::PeerManagerProxy>),
    AudioMode(Option<fidl_a2dp::AudioModeProxy>),
    A2dpMediaStream(Option<ControllerProxy>),
    Avrcp(Option<fidl_avrcp::PeerManagerRequestStream>),
    Codec(Option<CodecFactoryRequestStream>),
    Registry(Option<RegistryRequestStream>),
    Session(Option<SessionAudioConsumerFactoryRequestStream>),
    AudioSettings(Option<AudioRequestStream>),
    Cobalt(Option<LoggerFactoryRequestStream>),
    MediaSession(Option<DiscoveryRequestStream>),
    MediaPublisher(Option<PublisherRequestStream>),
    AudioDevice(Option<AudioDeviceEnumeratorRequestStream>),
    Allocator(Option<AllocatorRequestStream>),
    BatteryManager(Option<BatteryManagerRequestStream>),
}

impl From<ProfileRequestStream> for Event {
    fn from(src: ProfileRequestStream) -> Self {
        Self::Profile(Some(src))
    }
}

impl From<fidl_avrcp::PeerManagerRequestStream> for Event {
    fn from(src: fidl_avrcp::PeerManagerRequestStream) -> Self {
        Self::Avrcp(Some(src))
    }
}

impl From<CodecFactoryRequestStream> for Event {
    fn from(src: CodecFactoryRequestStream) -> Self {
        Self::Codec(Some(src))
    }
}

impl From<RegistryRequestStream> for Event {
    fn from(src: RegistryRequestStream) -> Self {
        Self::Registry(Some(src))
    }
}

impl From<SessionAudioConsumerFactoryRequestStream> for Event {
    fn from(src: SessionAudioConsumerFactoryRequestStream) -> Self {
        Self::Session(Some(src))
    }
}

impl From<AudioRequestStream> for Event {
    fn from(src: AudioRequestStream) -> Self {
        Self::AudioSettings(Some(src))
    }
}

impl From<LoggerFactoryRequestStream> for Event {
    fn from(src: LoggerFactoryRequestStream) -> Self {
        Self::Cobalt(Some(src))
    }
}

impl From<DiscoveryRequestStream> for Event {
    fn from(src: DiscoveryRequestStream) -> Self {
        Self::MediaSession(Some(src))
    }
}

impl From<PublisherRequestStream> for Event {
    fn from(src: PublisherRequestStream) -> Self {
        Self::MediaPublisher(Some(src))
    }
}

impl From<AudioDeviceEnumeratorRequestStream> for Event {
    fn from(src: AudioDeviceEnumeratorRequestStream) -> Self {
        Self::AudioDevice(Some(src))
    }
}

impl From<AllocatorRequestStream> for Event {
    fn from(src: AllocatorRequestStream) -> Self {
        Self::Allocator(Some(src))
    }
}

impl From<BatteryManagerRequestStream> for Event {
    fn from(src: BatteryManagerRequestStream) -> Self {
        Self::BatteryManager(Some(src))
    }
}

/// Represents a fake A2DP client that requests the `avdtp.PeerManager` and `a2dp.AudioMode` services.
async fn mock_a2dp_client(
    mut sender: mpsc::Sender<Event>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let peer_manager_svc = handles.connect_to_protocol::<fidl_avdtp::PeerManagerMarker>()?;
    sender.send(Event::Avdtp(Some(peer_manager_svc))).await.expect("failed sending ack to test");

    let audio_mode_svc = handles.connect_to_protocol::<fidl_a2dp::AudioModeMarker>()?;
    sender.send(Event::AudioMode(Some(audio_mode_svc))).await.expect("failed sending ack to test");

    let a2dp_media_stream_svc = handles.connect_to_protocol::<ControllerMarker>()?;
    sender
        .send(Event::A2dpMediaStream(Some(a2dp_media_stream_svc)))
        .await
        .expect("failed sending ack to test");
    Ok(())
}

/// The component mock that provides all the services that A2DP requires.
async fn mock_component(
    sender: mpsc::Sender<Event>,
    handles: LocalComponentHandles,
) -> Result<(), Error> {
    let mut fs = ServiceFs::new();

    add_fidl_service_handler::<fidl_avrcp::PeerManagerMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<ProfileMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<LoggerFactoryMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<AudioDeviceEnumeratorMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<SessionAudioConsumerFactoryMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<PublisherMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<CodecFactoryMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<AudioMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<AllocatorMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<RegistryMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<DiscoveryMarker, _>(&mut fs, sender.clone());
    add_fidl_service_handler::<BatteryManagerMarker, _>(&mut fs, sender);

    let _ = fs.serve_connection(handles.outgoing_dir.into_channel())?;
    fs.collect::<()>().await;

    Ok(())
}

/// Local name of the A2DP component in the Realm.
const A2DP_MONIKER: &str = "a2dp";
/// Local name of the A2DP client in the Realm.
const A2DP_CLIENT_MONIKER: &str = "fake-a2dp-client";
/// Local name of the component which provides services used by A2DP in the Realm.
const SERVICE_PROVIDER_MONIKER: &str = "fake-service-provider";

async fn add_a2dp_dependency_route<S: DiscoverableProtocolMarker>(builder: &RealmBuilder) {
    let _ = builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<S>())
                .from(Ref::child(SERVICE_PROVIDER_MONIKER))
                .to(Ref::child(A2DP_MONIKER)),
        )
        .await
        .expect("Failed adding route for service");
}

/// Tests that the v2 A2DP component has the correct topology and verifies that
/// it connects and provides the expected services.
#[fasync::run_singlethreaded(test)]
async fn a2dp_v2_component_topology() {
    fuchsia_syslog::init().unwrap();
    info!("Starting A2DP v2 smoke test...");

    let (sender, mut receiver) = mpsc::channel(0);
    let fake_client_tx = sender.clone();
    let service_tx = sender.clone();

    let builder = RealmBuilder::new().await.expect("Failed to create test realm builder");

    // The v2 component under test.
    let a2dp = builder
        .add_child(A2DP_MONIKER, A2DP_URL.to_string(), ChildOptions::new())
        .await
        .expect("Failed adding a2dp to topology");

    // Generic backend component that provides a slew of services that will be requested.
    let service_provider = builder
        .add_local_child(
            SERVICE_PROVIDER_MONIKER,
            move |handles: LocalComponentHandles| {
                let sender = service_tx.clone();
                Box::pin(mock_component(sender, handles))
            },
            ChildOptions::new(),
        )
        .await
        .expect("Failed adding profile mock to topology");

    // Mock A2DP client that will request the PeerManager and AudioMode services
    // which are provided by the A2DP component.
    let a2dp_client = builder
        .add_local_child(
            A2DP_CLIENT_MONIKER,
            move |handles: LocalComponentHandles| {
                let sender = fake_client_tx.clone();
                Box::pin(mock_a2dp_client(sender, handles))
            },
            ChildOptions::new().eager(),
        )
        .await
        .expect("Failed adding a2dp client mock to topology");

    // Capabilities provided by A2DP.
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fidl_avdtp::PeerManagerMarker>())
                .capability(Capability::protocol::<fidl_a2dp::AudioModeMarker>())
                .capability(Capability::protocol::<ControllerMarker>())
                .from(&a2dp)
                .to(&a2dp_client),
        )
        .await
        .expect("Failed adding route for A2DP services");

    // Capabilities provided by the generic service provider component, which are consumed
    // by the A2DP component.
    add_a2dp_dependency_route::<fidl_avrcp::PeerManagerMarker>(&builder).await;
    add_a2dp_dependency_route::<ProfileMarker>(&builder).await;
    add_a2dp_dependency_route::<LoggerFactoryMarker>(&builder).await;
    add_a2dp_dependency_route::<AudioDeviceEnumeratorMarker>(&builder).await;
    add_a2dp_dependency_route::<SessionAudioConsumerFactoryMarker>(&builder).await;
    add_a2dp_dependency_route::<PublisherMarker>(&builder).await;
    add_a2dp_dependency_route::<CodecFactoryMarker>(&builder).await;
    add_a2dp_dependency_route::<AudioMarker>(&builder).await;
    add_a2dp_dependency_route::<AllocatorMarker>(&builder).await;
    add_a2dp_dependency_route::<RegistryMarker>(&builder).await;
    add_a2dp_dependency_route::<BatteryManagerMarker>(&builder).await;
    // Capability used by AVRCP Target, a child of A2DP. Route this service to A2DP to be
    // transitively routed to it.
    add_a2dp_dependency_route::<DiscoveryMarker>(&builder).await;

    // Logging service, used by all children in this test.
    builder
        .add_route(
            Route::new()
                .capability(Capability::protocol::<fidl_fuchsia_logger::LogSinkMarker>())
                .from(Ref::parent())
                .to(&a2dp)
                .to(&a2dp_client)
                .to(&service_provider),
        )
        .await
        .expect("Failed adding LogSink route to test components");
    let _test_topology = builder.build().await.unwrap();

    // If the routing is correctly configured, we expect 17 events:
    //   - `bt-a2dp` connecting to the 11 services specified in its manifest.
    //   - `bt-avrcp-target` (a child of bt-a2dp) connecting to the 3 services specified in its
    //     manifest.
    //   - `fake-a2dp-client` connecting to the `avdtp.PeerManager`, `AudioMode`, & `Controller`
    //     services which are provided by `bt-a2dp`.
    let mut events = Vec::new();
    let expected_number_of_events = 17;
    for i in 0..expected_number_of_events {
        let msg = format!("Unexpected error waiting for {:?} event", i);
        events.push(receiver.next().await.expect(&msg));
    }
    assert_eq!(events.len(), expected_number_of_events);

    let discriminants: Vec<_> = events.iter().map(std::mem::discriminant).collect();

    // We expect two `avrcp.PeerManager` and `BatteryManager` events since both components use it.
    assert_eq!(
        discriminants.iter().filter(|&&d| d == std::mem::discriminant(&Event::Avrcp(None))).count(),
        2
    );
    assert_eq!(
        discriminants
            .iter()
            .filter(|&&d| d == std::mem::discriminant(&Event::BatteryManager(None)))
            .count(),
        2
    );
    // Total unique requests = Total requests minus the two duplicate requests.
    let discriminant_set: HashSet<_> = HashSet::from_iter(discriminants.iter());
    assert_eq!(discriminant_set.len(), 15);

    info!("Finished A2DP smoke test");
}