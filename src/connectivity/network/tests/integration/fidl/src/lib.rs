// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![cfg(test)]

use std::collections::HashMap;

use fidl_fuchsia_net_ext::{IntoExt as _, NetTypesIpAddressExt};
use fidl_fuchsia_net_stack as fnet_stack;
use fidl_fuchsia_net_stack_ext::{exec_fidl, FidlReturn as _};
use fidl_fuchsia_netemul as fnetemul;
use fuchsia_async::{DurationExt as _, TimeoutExt as _};
use fuchsia_zircon as zx;

use anyhow::Context as _;
use futures::{FutureExt as _, StreamExt as _, TryStreamExt as _};
use net_declare::{fidl_mac, fidl_subnet, std_ip_v4, std_ip_v6, std_socket_addr};
use netemul::RealmUdpSocket as _;
use netstack_testing_common::{
    get_component_moniker,
    realms::{
        constants, KnownServiceProvider, Netstack, Netstack2, NetstackVersion, TestSandboxExt as _,
    },
    ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT, ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT,
};
use netstack_testing_macros::variants_test;
use packet::{serialize::Serializer as _, ParsablePacket as _};
use packet_formats::{
    error::ParseError,
    ethernet::{EthernetFrame, EthernetFrameBuilder, EthernetFrameLengthCheck, EthernetIpExt as _},
    icmp::{
        IcmpEchoRequest, IcmpIpExt, IcmpMessage, IcmpPacket, IcmpPacketBuilder, IcmpParseArgs,
        IcmpUnusedCode, MessageBody as _, OriginalPacket,
    },
    ip::IpPacketBuilder as _,
};
use test_case::test_case;

#[fuchsia_async::run_singlethreaded(test)]
async fn add_ethernet_device() {
    let name = "add_ethernet_device";
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<Netstack2, _>(name).expect("create realm");
    let netstack = realm
        .connect_to_protocol::<fidl_fuchsia_netstack::NetstackMarker>()
        .expect("connect to protocol");
    let device =
        sandbox.create_endpoint::<netemul::Ethernet, _>(name).await.expect("create endpoint");

    // We're testing add_ethernet_device (netstack.fidl), which
    // does not have a network device entry point.
    let eth = device.get_ethernet().await.expect("connet to ethernet device");
    let id = netstack
        .add_ethernet_device(
            name,
            &mut fidl_fuchsia_netstack::InterfaceConfig {
                name: name[..fidl_fuchsia_net_interfaces::INTERFACE_NAME_LENGTH.into()].to_string(),
                filepath: "/fake/filepath/for_test".to_string(),
                metric: 0,
            },
            eth,
        )
        .await
        .expect("add_ethernet_device FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw)
        .expect("add_ethernet_device failed");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let mut state = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id.into());
    let (device_class, online) = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
            .expect("create event stream"),
        &mut state,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id: _,
             name: _,
             device_class,
             online,
             addresses: _,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
         }| Some((*device_class, *online)),
    )
    .await
    .expect("observe interface addition");

    assert_eq!(
        device_class,
        fidl_fuchsia_net_interfaces::DeviceClass::Device(
            fidl_fuchsia_hardware_network::DeviceClass::Ethernet
        )
    );
    assert!(!online);
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_no_duplicate_interface_names() {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox
        .create_netstack_realm::<Netstack2, _>("no_duplicate_interface_names")
        .expect("create realm");
    let netstack = realm
        .connect_to_protocol::<fidl_fuchsia_netstack::NetstackMarker>()
        .expect("connect to protocol");
    // Create one endpoint of each type so we can use all the APIs that add an
    // interface. Note that fuchsia.net.stack/Stack.AddEthernetInterface does
    // not support setting the interface name.
    let eth_ep = sandbox
        .create_endpoint::<netemul::Ethernet, _>("eth-ep")
        .await
        .expect("create ethernet endpoint");

    const IFNAME: &'static str = "testif";
    const TOPOPATH: &'static str = "/fake/topopath";
    const FILEPATH: &'static str = "/fake/filepath";

    // Add the first ep to the stack so it takes over the name.
    let eth = eth_ep.get_ethernet().await.expect("connect to ethernet device");
    let _id: u32 = netstack
        .add_ethernet_device(
            TOPOPATH,
            &mut fidl_fuchsia_netstack::InterfaceConfig {
                name: IFNAME.to_string(),
                filepath: FILEPATH.to_string(),
                metric: 0,
            },
            eth,
        )
        .await
        .expect("add_ethernet_device FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw)
        .expect("add_ethernet_device error");

    // Now try to add again with the same parameters and expect an error.
    let eth = eth_ep.get_ethernet().await.expect("connect to ethernet device");
    let result = netstack
        .add_ethernet_device(
            TOPOPATH,
            &mut fidl_fuchsia_netstack::InterfaceConfig {
                name: IFNAME.to_string(),
                filepath: FILEPATH.to_string(),
                metric: 0,
            },
            eth,
        )
        .await
        .expect("add_ethernet_device FIDL error")
        .map_err(fuchsia_zircon::Status::from_raw);
    assert_eq!(result, Err(fuchsia_zircon::Status::ALREADY_EXISTS));
}

// TODO(https://fxbug.dev/75553): Remove this test when fuchsia.net.interfaces is supported in N3
// and test_add_remove_interface can be parameterized on Netstack.
#[variants_test]
async fn add_ethernet_interface<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let device =
        sandbox.create_endpoint::<netemul::Ethernet, _>(name).await.expect("create endpoint");

    let iface = device.into_interface_in_realm(&realm).await.expect("add device");
    let id = iface.id();

    let interfaces = fidl_fuchsia_net_interfaces_ext::existing(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
            .expect("create event stream"),
        HashMap::new(),
    )
    .await
    .expect("fetch existing interfaces");
    let fidl_fuchsia_net_interfaces_ext::Properties {
        id: _,
        name: _,
        device_class,
        online,
        addresses: _,
        has_default_ipv4_route: _,
        has_default_ipv6_route: _,
    } = interfaces.get(&id).expect("find added ethernet interface");
    assert_eq!(
        *device_class,
        fidl_fuchsia_net_interfaces::DeviceClass::Device(
            fidl_fuchsia_hardware_network::DeviceClass::Ethernet
        )
    );
    assert!(!online);
}

#[variants_test]
async fn add_del_interface_address_deprecated<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let stack =
        realm.connect_to_protocol::<fnet_stack::StackMarker>().expect("connect to protocol");
    let interfaces_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let device =
        sandbox.create_endpoint::<netemul::Ethernet, _>(name).await.expect("create endpoint");

    let iface = device.into_interface_in_realm(&realm).await.expect("add device");
    let id = iface.id();

    // TODO(https://fxbug.dev/20989#c5): netstack3 doesn't allow addresses to be added while
    // link is down.
    let () = stack.enable_interface_deprecated(id).await.squash_result().expect("enable interface");
    let () = iface.set_link_up(true).await.expect("bring device up");
    // TODO(https://fxbug.dev/60923): N3 doesn't implement watcher events past idle.
    loop {
        let interfaces = fidl_fuchsia_net_interfaces_ext::existing(
            fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
                .expect("create event stream"),
            HashMap::new(),
        )
        .await
        .expect("fetch existing interfaces");

        let fidl_fuchsia_net_interfaces_ext::Properties {
            id: _,
            name: _,
            device_class: _,
            online,
            addresses: _,
            has_default_ipv4_route: _,
            has_default_ipv6_route: _,
        } = interfaces.get(&id).expect("find added ethernet interface");
        if *online {
            break;
        }
    }

    let mut interface_address = fidl_subnet!("1.1.1.1/32");
    let res = stack
        .add_interface_address_deprecated(id, &mut interface_address)
        .await
        .expect("add_interface_address_deprecated");
    assert_eq!(res, Ok(()));

    // Should be an error the second time.
    let res = stack
        .add_interface_address_deprecated(id, &mut interface_address)
        .await
        .expect("add_interface_address_deprecated");
    assert_eq!(res, Err(fnet_stack::Error::AlreadyExists));

    let res = stack
        .add_interface_address_deprecated(id + 1, &mut interface_address)
        .await
        .expect("add_interface_address_deprecated");
    assert_eq!(res, Err(fnet_stack::Error::NotFound));

    let error = stack
        .add_interface_address_deprecated(
            id,
            &mut fidl_fuchsia_net::Subnet { prefix_len: 43, ..interface_address },
        )
        .await
        .expect("add_interface_address_deprecated")
        .unwrap_err();
    assert_eq!(error, fnet_stack::Error::InvalidArgs);

    let interface = fidl_fuchsia_net_interfaces_ext::existing(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
            .expect("create event stream"),
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id),
    )
    .await
    .expect("retrieve existing interface");
    // We use contains here because netstack can generate link-local addresses
    // that can't be predicted.
    assert_matches::assert_matches!(
        interface,
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Known(p)
            if p.addresses.contains(&fidl_fuchsia_net_interfaces_ext::Address {
                addr: interface_address,
                valid_until: zx::sys::ZX_TIME_INFINITE,
            })
    );

    let res = stack
        .del_interface_address_deprecated(id, &mut interface_address)
        .await
        .expect("del_interface_address_deprecated");
    assert_eq!(res, Ok(()));

    let interface = fidl_fuchsia_net_interfaces_ext::existing(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interfaces_state)
            .expect("create watcher event stream"),
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(id),
    )
    .await
    .expect("retrieve existing interface");
    // We use contains here because netstack can generate link-local addresses
    // that can't be predicted.
    assert_matches::assert_matches!(
        interface,
        fidl_fuchsia_net_interfaces_ext::InterfaceState::Known(p)
            if !p.addresses.contains(&fidl_fuchsia_net_interfaces_ext::Address {
                addr: interface_address,
                valid_until: zx::sys::ZX_TIME_INFINITE,
            })
    );
}

// Regression test which asserts that racing an address removal and interface
// removal doesn't cause a Netstack panic.
#[variants_test]
async fn remove_interface_and_address<E: netemul::Endpoint>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<Netstack2, _>(name).expect("create realm");
    let stack =
        realm.connect_to_protocol::<fnet_stack::StackMarker>().expect("connect to protocol");
    // NB: A second channel is needed in order for the address removal
    // requests and the interface removal request to be served concurrently.
    let stack2 =
        realm.connect_to_protocol::<fnet_stack::StackMarker>().expect("connect to protocol");

    let mut addresses = (0..32)
        .map(|i| fidl_fuchsia_net::Subnet {
            addr: fidl_fuchsia_net::IpAddress::Ipv4(fidl_fuchsia_net::Ipv4Address {
                addr: [1, 2, 3, i],
            }),
            prefix_len: 16,
        })
        .collect::<Vec<_>>();

    for i in 0..8 {
        let ep =
            sandbox.create_endpoint::<E, _>(format!("ep{}", i)).await.expect("create endpoint");
        let iface = ep.into_interface_in_realm(&realm).await.expect("add device");

        futures::stream::iter(addresses.iter_mut())
            .for_each_concurrent(None, |addr| {
                stack
                    .add_interface_address_deprecated(iface.id(), addr)
                    .map(|r| r.expect("call add_interface_address"))
                    .map(|r| r.expect("add interface address"))
            })
            .await;

        // Removing many addresses increases the chances that address removal
        // will be handled concurrently with interface removal.
        let remove_addr_fut =
            futures::stream::iter(addresses.iter_mut()).for_each_concurrent(None, |addr| {
                stack.del_interface_address_deprecated(iface.id(), addr).map(|r| {
                    match r.expect("call del_interface_address_deprecated") {
                        Ok(()) | Err(fnet_stack::Error::NotFound) => {}
                        Err(e) => panic!("delete interface address error: {:?}", e),
                    }
                })
            });

        // NB: The async block is necessary because calls on FIDL proxy
        // types make the request immediately and return a future which
        // resolves when the response is returned. Without the async block,
        // interface removal will be handled by Netstack immediately rather
        // concurrently with address removal, which is not the desired
        // behavior.
        let remove_interface_fut = async {
            stack2
                .del_ethernet_interface(iface.id())
                .await
                .expect("call del_ethernet_interface")
                .expect("delete interface")
        };

        futures::future::join(remove_addr_fut, remove_interface_fut).await;
    }
}

#[fuchsia_async::run_singlethreaded(test)]
async fn test_log_packets() {
    let name = "test_log_packets";
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    // Modify debug netstack args so that it does not log packets.
    let (realm, stack_log) = {
        let mut netstack =
            fnetemul::ChildDef::from(&KnownServiceProvider::Netstack(Netstack2::VERSION));
        let fnetemul::ChildDef { program_args, .. } = &mut netstack;
        assert_eq!(
            std::mem::replace(program_args, Some(vec!["--verbosity=debug".to_string()])),
            None,
        );
        let realm = sandbox.create_realm(name, [netstack]).expect("create realm");

        let netstack_proxy =
            realm.connect_to_protocol::<fnet_stack::LogMarker>().expect("connect to netstack");
        (realm, netstack_proxy)
    };
    let () = stack_log.set_log_packets(true).await.expect("enable packet logging");

    let sock =
        fuchsia_async::net::UdpSocket::bind_in_realm(&realm, std_socket_addr!("127.0.0.1:0"))
            .await
            .expect("create socket");
    let addr = sock.local_addr().expect("get bound socket address");
    const PAYLOAD: [u8; 4] = [1u8, 2, 3, 4];
    let sent = sock.send_to(&PAYLOAD[..], addr).await.expect("send_to failed");
    assert_eq!(sent, PAYLOAD.len());

    let patterns = ["send", "recv"]
        .iter()
        .map(|t| format!("{} udp {} -> {} len:{}", t, addr, addr, PAYLOAD.len()))
        .collect::<Vec<_>>();

    let netstack_moniker = get_component_moniker(&realm, constants::netstack::COMPONENT_NAME)
        .await
        .expect("get netstack moniker");
    let stream = diagnostics_reader::ArchiveReader::new()
        .select_all_for_moniker(&netstack_moniker)
        .snapshot_then_subscribe()
        .expect("subscribe to snapshot");

    let () = async_utils::fold::try_fold_while(stream, patterns, |mut patterns, data| {
        let () = patterns
            .retain(|pattern| !data.msg().map(|msg| msg.contains(pattern)).unwrap_or(false));
        futures::future::ok(if patterns.is_empty() {
            async_utils::fold::FoldWhile::Done(())
        } else {
            async_utils::fold::FoldWhile::Continue(patterns)
        })
    })
    .await
    .expect("observe expected patterns")
    .short_circuited()
    .unwrap_or_else(|patterns| {
        panic!("log stream ended while still waiting for patterns {:?}", patterns)
    });
}

const IPV4_LOOPBACK: fidl_fuchsia_net::Subnet = fidl_subnet!("127.0.0.1/8");
const IPV6_LOOPBACK: fidl_fuchsia_net::Subnet = fidl_subnet!("::1/128");

fn extract_v4_and_v6(
    addrs: impl IntoIterator<Item = fidl_fuchsia_net::Subnet>,
) -> (Option<fidl_fuchsia_net::Subnet>, Option<fidl_fuchsia_net::Subnet>) {
    let (v4, v6) = addrs.into_iter().fold((None, None), |(v4, v6), subnet| {
        let fidl_fuchsia_net::Subnet { addr, prefix_len: _ } = subnet;

        match addr {
            fidl_fuchsia_net::IpAddress::Ipv4(addr) => {
                if let Some(v4) = v4 {
                    panic!("IPv4 address already set; already have {:?}, got {:?}", v4, addr);
                } else {
                    (Some(subnet), v6)
                }
            }
            fidl_fuchsia_net::IpAddress::Ipv6(addr) => {
                if let Some(v6) = v6 {
                    panic!("IPv6 address already set; already have {:?}, got {:?}", v6, addr);
                } else {
                    (v4, Some(subnet))
                }
            }
        }
    });

    (v4, v6)
}

#[variants_test]
async fn add_remove_address_on_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");
    let stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
        .expect("get interface event stream");
    futures::pin_mut!(stream);

    let (loopback_id, addresses) = assert_matches::assert_matches!(
        stream.try_next().await,
        Ok(Some(fidl_fuchsia_net_interfaces::Event::Existing(
            fidl_fuchsia_net_interfaces::Properties {
                id: Some(id),
                device_class:
                    Some(fidl_fuchsia_net_interfaces::DeviceClass::Loopback(
                        fidl_fuchsia_net_interfaces::Empty {},
                    )),
                online: Some(true),
                addresses: Some(addresses),
                ..
            },
        ))) => (id, addresses)
    );
    let addresses =
        addresses.into_iter().map(|fidl_fuchsia_net_interfaces::Address { addr, .. }| {
            addr.expect("expected address to be set")
        });
    assert_eq!(extract_v4_and_v6(addresses), (Some(IPV4_LOOPBACK), Some(IPV6_LOOPBACK)));

    let stack = realm
        .connect_to_protocol::<fidl_fuchsia_net_stack::StackMarker>()
        .expect("connect to protocol");
    let stack = &stack;

    let del_addr = |mut addr| async move {
        stack
            .del_interface_address_deprecated(loopback_id, &mut addr)
            .await
            .expect("del_interface_address")
            .expect("expected to remove address")
    };
    del_addr(IPV4_LOOPBACK).await;
    del_addr(IPV6_LOOPBACK).await;

    const NEW_IPV4_ADDRESS: fidl_fuchsia_net::Subnet = fidl_subnet!("1.1.1.1/24");
    const NEW_IPV6_ADDRESS: fidl_fuchsia_net::Subnet = fidl_subnet!("a::1/64");
    let add_addr = |mut addr| async move {
        stack
            .add_interface_address_deprecated(loopback_id, &mut addr)
            .await
            .expect("add_interface_address")
            .expect("expected to add address")
    };
    add_addr(NEW_IPV4_ADDRESS).await;
    add_addr(NEW_IPV6_ADDRESS).await;

    // Wait for the addresses to be set.
    //
    // On Netstack2, the DAD resolution event (which the interface watcher
    // depends on to update its view of interface properties) is handled
    // asynchronously w.r.t. the add address operation so the new IPv6 address
    // may not be observed in the initial "Existing" event; it may be observed
    // in a "Changed" event.
    //
    // This is not an issue on Netstack3 as we will observe the address in an
    // "Existing" event right away as we do not perform DAD and Netstack3 is
    // queried for all interface states on watcher creation (no events happen
    // asynchronously).
    //
    // TODO(https://fxbug.dev/75553): Wait for changed event instead of creating
    // a new watcher.
    let mut state = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(loopback_id);
    fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
            .expect("get interface event stream"),
        &mut state,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id,
             name: _,
             device_class: _,
             online: _,
             addresses,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
         }| {
            assert_eq!(loopback_id, *id, "Don't expect to see other interfaces");
            let addresses = addresses.into_iter().map(
                |fidl_fuchsia_net_interfaces_ext::Address { addr, valid_until: _ }| {
                    addr
                },
            ).cloned();
            match extract_v4_and_v6(addresses) {
                (Some(v4), Some(v6)) => {
                    assert_eq!((v4, v6), (NEW_IPV4_ADDRESS, NEW_IPV6_ADDRESS));
                    Some(())
                }
                (v4, v6) => {
                    println!("waiting for both IPv4 and IPv6 addresses to be set; got (v4, v6) = ({:?}, {:?})", v4, v6);
                    None
                }
            }
        },
    ).await.expect("new addresses should be observed");
}

#[variants_test]
async fn disable_interface_loopback<N: Netstack>(name: &str) {
    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let realm = sandbox.create_netstack_realm::<N, _>(name).expect("create realm");
    let stack =
        realm.connect_to_protocol::<fnet_stack::StackMarker>().expect("connect to protocol");

    let interface_state = realm
        .connect_to_protocol::<fidl_fuchsia_net_interfaces::StateMarker>()
        .expect("connect to protocol");

    let stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
        .expect("get interface event stream");
    futures::pin_mut!(stream);

    let loopback_id = assert_matches::assert_matches!(
        stream.try_next().await,
        Ok(Some(fidl_fuchsia_net_interfaces::Event::Existing(
            fidl_fuchsia_net_interfaces::Properties {
                id: Some(id),
                device_class:
                    Some(fidl_fuchsia_net_interfaces::DeviceClass::Loopback(
                        fidl_fuchsia_net_interfaces::Empty {},
                    )),
                online: Some(true),
                ..
            },
        ))) => id
    );

    let () = assert_matches::assert_matches!(
        stream.try_next().await,
        Ok(Some(fidl_fuchsia_net_interfaces::Event::Idle(
            fidl_fuchsia_net_interfaces::Empty {},
        ))) => ()
    );

    match N::VERSION {
        NetstackVersion::Netstack2 => {
            let debug = realm
                .connect_to_protocol::<fidl_fuchsia_net_debug::InterfacesMarker>()
                .expect("connect to protocol");

            let (control, server_end) =
                fidl_fuchsia_net_interfaces_ext::admin::Control::create_endpoints()
                    .expect("create proxy");
            let () = debug.get_admin(loopback_id, server_end).expect("get admin");

            let did_disable = control.disable().await.expect("send disable").expect("disable");
            assert!(did_disable);

            let () = assert_matches::assert_matches!(stream.try_next().await,
                Ok(Some(fidl_fuchsia_net_interfaces::Event::Changed(
                    fidl_fuchsia_net_interfaces::Properties {
                        id: Some(id),
                        online: Some(false),
                        ..
                    },
                ))) if id == loopback_id => ()
            );
        }
        NetstackVersion::Netstack3 => {
            // TODO(https://fxbug.dev/92767): Remove this when N3 implements Control.
            let () =
                exec_fidl!(stack.disable_interface_deprecated(loopback_id), "disable interface")
                    .unwrap();

            // TODO(https://fxbug.dev/75553): Wait for changed event instead of
            // creating a new watcher.
            let stream = fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)
                .expect("get interface event stream");
            futures::pin_mut!(stream);
            let new_loopback_id = assert_matches::assert_matches!(
                stream.try_next().await,
                Ok(Some(fidl_fuchsia_net_interfaces::Event::Existing(
                    fidl_fuchsia_net_interfaces::Properties {
                        id: Some(id),
                        device_class:
                        Some(fidl_fuchsia_net_interfaces::DeviceClass::Loopback(
                            fidl_fuchsia_net_interfaces::Empty {},
                        )),
                        online: Some(false),
                        ..
                    },
                ))) => id
            );

            assert_eq!(loopback_id, new_loopback_id);
        }
        NetstackVersion::ProdNetstack2 => panic!("unexpectedly got ProdNetstack2 variant"),
    }
}

enum ForwardingConfiguration {
    All,
    Iface1Only(fidl_fuchsia_net::IpVersion),
    Iface2Only(fidl_fuchsia_net::IpVersion),
}

struct ForwardingTestCase<I: IcmpIpExt> {
    iface1_addr: fidl_fuchsia_net::Subnet,
    iface2_addr: fidl_fuchsia_net::Subnet,
    forwarding_config: Option<ForwardingConfiguration>,
    src_ip: I::Addr,
    dst_ip: I::Addr,
    expect_forward: bool,
}

fn test_forwarding_v4(
    forwarding_config: Option<ForwardingConfiguration>,
    expect_forward: bool,
) -> ForwardingTestCase<net_types::ip::Ipv4> {
    ForwardingTestCase {
        iface1_addr: fidl_subnet!("192.168.1.1/24"),
        iface2_addr: fidl_subnet!("192.168.2.1/24"),
        forwarding_config,
        // TODO(https://fxbug.dev/77901): Use `std_ip_v4!(..).into()`.
        // TODO(https://fxbug.dev/77965): Use `net_declare` macros to create
        // `net_types` addresses.
        src_ip: net_types::ip::Ipv4Addr::new(std_ip_v4!("192.168.1.2").octets()),
        dst_ip: net_types::ip::Ipv4Addr::new(std_ip_v4!("192.168.2.2").octets()),
        expect_forward,
    }
}

fn test_forwarding_v6(
    forwarding_config: Option<ForwardingConfiguration>,
    expect_forward: bool,
) -> ForwardingTestCase<net_types::ip::Ipv6> {
    ForwardingTestCase {
        iface1_addr: fidl_subnet!("a::1/64"),
        iface2_addr: fidl_subnet!("b::1/64"),
        forwarding_config,
        // TODO(https://fxbug.dev/77901): Use `std_ip_v6!(..).into()`.
        // TODO(https://fxbug.dev/77965): Use `net_declare` macros to create
        // `net_types` addresses.
        src_ip: net_types::ip::Ipv6Addr::from_bytes(std_ip_v6!("a::2").octets()),
        dst_ip: net_types::ip::Ipv6Addr::from_bytes(std_ip_v6!("b::2").octets()),
        expect_forward,
    }
}

#[variants_test]
#[test_case(
    "v4_none_forward_icmp_v4",
    test_forwarding_v4(
        None,
        false,
    ); "v4_none_forward_icmp_v4")]
#[test_case(
    "v4_all_forward_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::All),
        true,
    ); "v4_all_forward_icmp_v4")]
#[test_case(
    "v4_iface1_forward_v4_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V4)),
        true,
    ); "v4_iface1_forward_v4_icmp_v4")]
#[test_case(
    "v4_iface1_forward_v6_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V6)),
        false,
    ); "v4_iface1_forward_v6_icmp_v4")]
#[test_case(
    "v4_iface2_forward_v4_icmp_v4",
    test_forwarding_v4(
        Some(ForwardingConfiguration::Iface2Only(fidl_fuchsia_net::IpVersion::V4)),
        false,
    ); "v4_iface2_forward_v4_icmp_v4")]
#[test_case(
    "v6_none_forward_icmp_v6",
    test_forwarding_v6(
        None,
        false,
    ); "v6_none_forward_icmp_v6")]
#[test_case(
    "v6_all_forward_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::All),
        true,
    ); "v6_all_forward_icmp_v6")]
#[test_case(
    "v6_iface1_forward_v6_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V6)),
        true,
    ); "v6_iface1_forward_v6_icmp_v6")]
#[test_case(
    "v6_iface1_forward_v4_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface1Only(fidl_fuchsia_net::IpVersion::V4)),
        false,
    ); "v6_iface1_forward_v4_icmp_v6")]
#[test_case(
    "v6_iface2_forward_v6_icmp_v6",
    test_forwarding_v6(
        Some(ForwardingConfiguration::Iface2Only(fidl_fuchsia_net::IpVersion::V6)),
        false,
    ); "v6_iface2_forward_v6_icmp_v6")]
async fn test_forwarding<E: netemul::Endpoint, I: IcmpIpExt>(
    test_name: &str,
    sub_test_name: &str,
    test_case: ForwardingTestCase<I>,
) where
    IcmpEchoRequest:
        for<'a> IcmpMessage<I, &'a [u8], Code = IcmpUnusedCode, Body = OriginalPacket<&'a [u8]>>,
    I::Addr: NetTypesIpAddressExt,
{
    const TTL: u8 = 64;
    const ECHO_ID: u16 = 1;
    const ECHO_SEQ: u16 = 2;
    const MAC: fidl_fuchsia_net::MacAddress = fidl_mac!("02:0A:0B:0C:0D:0E");

    let ForwardingTestCase {
        iface1_addr,
        iface2_addr,
        forwarding_config,
        src_ip,
        dst_ip,
        expect_forward,
    } = test_case;

    let name = format!("{}_{}", test_name, sub_test_name);
    let name = name.as_str();

    let sandbox = netemul::TestSandbox::new().expect("create sandbox");
    let sandbox = &sandbox;
    let realm = sandbox.create_netstack_realm::<Netstack2, _>(name).expect("create netstack realm");
    let realm = &realm;

    let net_ep_iface = |net_num: u8, addr: fidl_fuchsia_net::Subnet| async move {
        let net = sandbox.create_network(format!("net{}", net_num)).await.expect("create network");
        let fake_ep = net.create_fake_endpoint().expect("create fake endpoint");
        let iface = realm
            .join_network::<E, _>(
                &net,
                format!("iface{}", net_num),
                &netemul::InterfaceConfig::StaticIp(addr),
            )
            .await
            .expect("configure networking");

        (net, fake_ep, iface)
    };

    let (_net1, fake_ep1, iface1) = net_ep_iface(1, iface1_addr).await;
    let (_net2, fake_ep2, iface2) = net_ep_iface(2, iface2_addr).await;

    if let Some(config) = forwarding_config {
        let stack = realm
            .connect_to_protocol::<fnet_stack::StackMarker>()
            .expect("error connecting to stack");

        match config {
            ForwardingConfiguration::All => {
                let () = stack
                    .enable_ip_forwarding()
                    .await
                    .expect("error enabling IP forwarding request");
            }
            ForwardingConfiguration::Iface1Only(ip_version) => {
                let () = stack
                    .set_interface_ip_forwarding(iface1.id(), ip_version, true)
                    .await
                    .expect("set_interface_ip_forwarding FIDL error for iface1")
                    .expect("error enabling IP forwarding on iface1");
            }
            ForwardingConfiguration::Iface2Only(ip_version) => {
                let () = stack
                    .set_interface_ip_forwarding(iface2.id(), ip_version, true)
                    .await
                    .expect("set_interface_ip_forwarding FIDL error for iface2")
                    .expect("error enabling IP forwarding on iface2");
            }
        }
    }

    let neighbor_controller = realm
        .connect_to_protocol::<fidl_fuchsia_net_neighbor::ControllerMarker>()
        .expect("connect to protocol");
    let dst_ip_fidl: <I::Addr as NetTypesIpAddressExt>::Fidl = dst_ip.into_ext();
    let () = neighbor_controller
        .add_entry(iface2.id(), &mut dst_ip_fidl.into_ext(), &mut MAC.clone())
        .await
        .expect("add_entry FIDL error")
        .expect("error adding static entry");

    let mut icmp_body = [1, 2, 3, 4, 5, 6, 7, 8];

    let ser = packet::Buf::new(&mut icmp_body, ..)
        .encapsulate(IcmpPacketBuilder::<I, _, _>::new(
            src_ip,
            dst_ip,
            IcmpUnusedCode,
            IcmpEchoRequest::new(ECHO_ID, ECHO_SEQ),
        ))
        .encapsulate(<I as packet_formats::ip::IpExt>::PacketBuilder::new(
            src_ip,
            dst_ip,
            TTL,
            I::ICMP_IP_PROTO,
        ))
        .encapsulate(EthernetFrameBuilder::new(
            net_types::ethernet::Mac::new([1, 2, 3, 4, 5, 6]),
            net_types::ethernet::Mac::BROADCAST,
            I::ETHER_TYPE,
        ))
        .serialize_vec_outer()
        .expect("serialize ICMP packet")
        .unwrap_b();

    let duration = if expect_forward {
        ASYNC_EVENT_POSITIVE_CHECK_TIMEOUT
    } else {
        ASYNC_EVENT_NEGATIVE_CHECK_TIMEOUT
    };

    let ((), forwarded) = futures::future::join(
        fake_ep1.write(ser.as_ref()).map(|r| r.expect("write to fake endpoint #1")),
        fake_ep2
            .frame_stream()
            .map(|r| r.expect("error getting OnData event"))
            .filter_map(|(data, dropped)| {
                assert_eq!(dropped, 0);

                let mut data = &data[..];

                let eth = EthernetFrame::parse(&mut data, EthernetFrameLengthCheck::NoCheck)
                    .expect("error parsing ethernet frame");

                if eth.ethertype() != Some(I::ETHER_TYPE) {
                    // Ignore other IP packets.
                    return futures::future::ready(None);
                }

                let (mut payload, src_ip, dst_ip, proto, got_ttl) =
                    packet_formats::testutil::parse_ip_packet::<I>(&data)
                        .expect("error parsing IP packet");

                if proto != I::ICMP_IP_PROTO {
                    // Ignore non-ICMP packets.
                    return futures::future::ready(None);
                }

                let icmp = match IcmpPacket::<I, _, IcmpEchoRequest>::parse(
                    &mut payload,
                    IcmpParseArgs::new(src_ip, dst_ip),
                ) {
                    Ok(o) => o,
                    Err(ParseError::NotExpected) => {
                        // Ignore non-echo request packets.
                        return futures::future::ready(None);
                    }
                    Err(e) => {
                        panic!("error parsing ICMP echo request packet: {}", e)
                    }
                };

                let echo_request = icmp.message();
                assert_eq!(echo_request.id(), ECHO_ID);
                assert_eq!(echo_request.seq(), ECHO_SEQ);
                assert_eq!(icmp.body().bytes(), icmp_body);
                assert_eq!(got_ttl, TTL - 1);

                // Our packet was forwarded.
                futures::future::ready(Some(true))
            })
            .next()
            .map(|r| r.expect("stream unexpectedly ended"))
            .on_timeout(duration.after_now(), || {
                // The packet was not forwarded.
                false
            }),
    )
    .await;

    assert_eq!(expect_forward, forwarded);
}
