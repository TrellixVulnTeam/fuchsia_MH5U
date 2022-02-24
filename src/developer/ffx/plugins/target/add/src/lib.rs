// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Result,
    errors::ffx_error,
    ffx_core::ffx_plugin,
    ffx_target_add_args::AddCommand,
    fidl_fuchsia_developer_bridge::{self as bridge, TargetCollectionProxy},
    fidl_fuchsia_net as net,
    netext::parse_address_parts,
    std::net::IpAddr,
};

#[cfg(not(test))]
use std::ffi::CString;

#[ffx_plugin(TargetCollectionProxy = "daemon::protocol")]
pub async fn add(target_collection_proxy: TargetCollectionProxy, cmd: AddCommand) -> Result<()> {
    let (addr, scope, port) =
        parse_address_parts(cmd.addr.as_str()).map_err(|e| ffx_error!("{}", e))?;
    // TODO(fxbug.dev/93511): Check if the scope ID is an index.
    #[cfg(not(test))]
    let scope_id = if let Some(scope) = scope {
        unsafe {
            let scope = CString::new(scope).unwrap();
            libc::if_nametoindex(scope.as_ptr())
        }
    } else {
        0
    };

    #[cfg(test)]
    let scope_id = if let Some(scope) = scope { scope.parse()? } else { 0 };

    let ip = match addr {
        IpAddr::V6(i) => net::IpAddress::Ipv6(net::Ipv6Address { addr: i.octets().into() }),
        IpAddr::V4(i) => net::IpAddress::Ipv4(net::Ipv4Address { addr: i.octets().into() }),
    };
    let mut addr = if let Some(port) = port {
        bridge::TargetAddrInfo::IpPort(bridge::TargetIpPort { ip, port, scope_id })
    } else {
        bridge::TargetAddrInfo::Ip(bridge::TargetIp { ip, scope_id })
    };

    target_collection_proxy.add_target(&mut addr).await?;

    Ok(())
}

////////////////////////////////////////////////////////////////////////////////
// tests

#[cfg(test)]
mod test {
    use super::*;

    fn setup_fake_target_collection<T: 'static + Fn(bridge::TargetAddrInfo) + Send>(
        test: T,
    ) -> TargetCollectionProxy {
        setup_fake_target_collection_proxy(move |req| match req {
            bridge::TargetCollectionRequest::AddTarget { ip, responder } => {
                test(ip);
                responder.send().unwrap();
            }
            _ => assert!(false),
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::Ip(bridge::TargetIp {
                    ip: net::IpAddress::Ipv4(net::Ipv4Address {
                        addr: "123.210.123.210"
                            .parse::<std::net::Ipv4Addr>()
                            .unwrap()
                            .octets()
                            .into()
                    }),
                    scope_id: 0,
                })
            )
        });
        add(server, AddCommand { addr: "123.210.123.210".to_owned() }).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add_port() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::IpPort(bridge::TargetIpPort {
                    ip: net::IpAddress::Ipv4(net::Ipv4Address {
                        addr: "123.210.123.210"
                            .parse::<std::net::Ipv4Addr>()
                            .unwrap()
                            .octets()
                            .into()
                    }),
                    scope_id: 0,
                    port: 2310,
                })
            )
        });
        add(server, AddCommand { addr: "123.210.123.210:2310".to_owned() }).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add_v6() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::Ip(bridge::TargetIp {
                    ip: net::IpAddress::Ipv6(net::Ipv6Address {
                        addr: "f000::1".parse::<std::net::Ipv6Addr>().unwrap().octets().into()
                    }),
                    scope_id: 0,
                })
            )
        });
        add(server, AddCommand { addr: "f000::1".to_owned() }).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add_v6_port() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::IpPort(bridge::TargetIpPort {
                    ip: net::IpAddress::Ipv6(net::Ipv6Address {
                        addr: "f000::1".parse::<std::net::Ipv6Addr>().unwrap().octets().into()
                    }),
                    scope_id: 0,
                    port: 65,
                })
            )
        });
        add(server, AddCommand { addr: "[f000::1]:65".to_owned() }).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add_v6_scope_id() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::Ip(bridge::TargetIp {
                    ip: net::IpAddress::Ipv6(net::Ipv6Address {
                        addr: "f000::1".parse::<std::net::Ipv6Addr>().unwrap().octets().into()
                    }),
                    scope_id: 1,
                })
            )
        });
        add(server, AddCommand { addr: "f000::1%1".to_owned() }).await.unwrap();
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_add_v6_scope_id_port() {
        let server = setup_fake_target_collection(|addr| {
            assert_eq!(
                addr,
                bridge::TargetAddrInfo::IpPort(bridge::TargetIpPort {
                    ip: net::IpAddress::Ipv6(net::Ipv6Address {
                        addr: "f000::1".parse::<std::net::Ipv6Addr>().unwrap().octets().into()
                    }),
                    scope_id: 1,
                    port: 640,
                })
            )
        });
        add(server, AddCommand { addr: "[f000::1%1]:640".to_owned() }).await.unwrap();
    }
}
