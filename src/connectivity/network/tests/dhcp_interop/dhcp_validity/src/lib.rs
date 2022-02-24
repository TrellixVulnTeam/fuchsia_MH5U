// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{format_err, Context as _, Error},
    fidl_fuchsia_net as fnet, fidl_fuchsia_net_dhcpv6 as fdhcpv6,
    fidl_fuchsia_net_ext::IpExt as _,
    fidl_fuchsia_net_interfaces as finterfaces, fidl_fuchsia_net_name as fnetname,
    fidl_fuchsia_netemul_guest::{
        CommandListenerMarker, GuestDiscoveryMarker, GuestInteractionMarker,
    },
    fuchsia_component::client,
    futures::TryFutureExt as _,
    netemul_guest_lib::wait_for_command_completion,
};

/// Run a command on a guest VM to configure its DHCP server.
///
/// # Arguments
///
/// * `guest_name` - String slice name of the guest to be configured.
/// * `command_to_run` - String slice command that will be executed on the guest VM.
pub async fn configure_dhcp_server(guest_name: &str, command_to_run: &str) -> Result<(), Error> {
    // Run a bash script to start the DHCP service on the Debian guest.
    let mut env = vec![];
    let guest_discovery_service = client::connect_to_protocol::<GuestDiscoveryMarker>()?;
    let (gis, gis_ch) = fidl::endpoints::create_proxy::<GuestInteractionMarker>()?;
    let () = guest_discovery_service.get_guest(None, guest_name, gis_ch)?;

    let (client_proxy, server_end) = fidl::endpoints::create_proxy::<CommandListenerMarker>()?;

    gis.execute_command(command_to_run, &mut env.iter_mut(), None, None, None, server_end)?;

    // Ensure that the process completes normally.
    wait_for_command_completion(client_proxy.take_event_stream(), None).await
}

/// Ensure that an address is added to a Netstack interface.
///
/// # Arguments
///
/// * `want_addr` - IpAddress that should appear on a Netstack interface.
/// * `timeout` - Duration to wait for the address to appear in Netstack.
pub async fn verify_v4_addr_present(want_addr: fnet::IpAddress) -> Result<(), Error> {
    let interface_state = client::connect_to_protocol::<finterfaces::StateMarker>()?;
    let mut if_map = std::collections::HashMap::new();
    fidl_fuchsia_net_interfaces_ext::wait_interface(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)?,
        &mut if_map,
        |if_map| {
            // TODO(https://github.com/rust-lang/rust/issues/80967): use bool::then_some.
            if_map
                .values()
                .any(|fidl_fuchsia_net_interfaces_ext::Properties { addresses, .. }| {
                    addresses.iter().any(
                        |fidl_fuchsia_net_interfaces_ext::Address {
                             addr: fnet::Subnet { addr, prefix_len: _ },
                             valid_until: _,
                         }| { *addr == want_addr },
                    )
                })
                .then(|| ())
        },
    )
    .map_err(anyhow::Error::from)
    .await
    .with_context(|| {
        format!(
            "failed to wait for address {:?}, final interfaces state: {}",
            want_addr,
            if_map.iter().fold(
                String::from("addresses present:"),
                |s, (id, fidl_fuchsia_net_interfaces_ext::Properties { addresses, .. })| {
                    s + &format!(" {:?}: {:?}", id, addresses)
                }
            ),
        )
    })
}

/// Verifies a DHCPv6 client can receive an expected list of DNS servers.
///
/// # Arguments
///
/// * `interface_id` - ID identifying the interface to start the DHCPv6 client on.
/// * `want_dns_servers` - Vector of DNS servers that the DHCPv6 client is expected to receive.
pub async fn verify_v6_dns_servers(
    interface_id: u64,
    want_dns_servers: Vec<fnetname::DnsServer_>,
) -> Result<(), Error> {
    let interface_state = client::connect_to_protocol::<finterfaces::StateMarker>()?;
    let mut state = fidl_fuchsia_net_interfaces_ext::InterfaceState::Unknown(interface_id.into());
    let addr = fidl_fuchsia_net_interfaces_ext::wait_interface_with_id(
        fidl_fuchsia_net_interfaces_ext::event_stream_from_state(&interface_state)?,
        &mut state,
        |fidl_fuchsia_net_interfaces_ext::Properties {
             id: _,
             name: _,
             device_class: _,
             online: _,
             addresses,
             has_default_ipv4_route: _,
             has_default_ipv6_route: _,
         }| {
            addresses.into_iter().find_map(
                |fidl_fuchsia_net_interfaces_ext::Address {
                     addr: fnet::Subnet { addr, prefix_len: _ },
                     valid_until: _,
                 }| match addr {
                    fnet::IpAddress::Ipv4(fnet::Ipv4Address { addr: _ }) => None,
                    fnet::IpAddress::Ipv6(addr) => addr.is_unicast_link_local().then(|| *addr),
                },
            )
        },
    )
    .await
    .context("getting interface state")?;

    let provider = client::connect_to_protocol::<fdhcpv6::ClientProviderMarker>()
        .context("connecting to DHCPv6 client")?;

    let (client_end, server_end) = fidl::endpoints::create_endpoints::<fdhcpv6::ClientMarker>()
        .context("creating DHCPv6 client channel")?;
    let () = provider.new_client(
        fdhcpv6::NewClientParams {
            interface_id: Some(interface_id),
            address: Some(fnet::Ipv6SocketAddress {
                address: addr,
                port: fdhcpv6::DEFAULT_CLIENT_PORT,
                zone_index: interface_id,
            }),
            config: Some(fdhcpv6::ClientConfig {
                information_config: Some(fdhcpv6::InformationConfig {
                    dns_servers: Some(true),
                    ..fdhcpv6::InformationConfig::EMPTY
                }),
                ..fdhcpv6::ClientConfig::EMPTY
            }),
            ..fdhcpv6::NewClientParams::EMPTY
        },
        server_end,
    )?;
    let client_proxy = client_end.into_proxy().context("getting client proxy from channel")?;

    let got_dns_servers = client_proxy.watch_servers().await.context("watching DNS servers")?;
    if got_dns_servers == want_dns_servers {
        Ok(())
    } else {
        Err(format_err!(
            "DHCPv6 client received unexpected DNS servers:\ngot dns servers: {:?}\n, want dns servers: {:?}\n",
            got_dns_servers,
            want_dns_servers
        ))
    }
}
