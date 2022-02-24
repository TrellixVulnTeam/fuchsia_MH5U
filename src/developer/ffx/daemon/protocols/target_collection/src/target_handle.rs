// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::reboot,
    anyhow::{anyhow, Context as _, Result},
    diagnostics::{get_streaming_min_timestamp, run_diagnostics_streaming},
    ffx_daemon_events::TargetEvent,
    ffx_daemon_target::logger::streamer::GenericDiagnosticsStreamer,
    ffx_daemon_target::target::Target,
    ffx_stream_util::TryStreamUtilExt,
    fidl::endpoints::ServerEnd,
    fidl_fuchsia_developer_bridge::{self as bridge},
    fuchsia_async::{
        futures::{FutureExt, TryStreamExt},
        TimeoutExt,
    },
    protocols::Context,
    std::future::Future,
    std::pin::Pin,
    std::rc::Rc,
    std::time::Duration,
    tasks::TaskManager,
};

// TODO(awdavies): Abstract this to use similar utilities to an actual protocol.
// This functionally behaves the same with the only caveat being that some
// initial state is set by the caller (the target Rc).
#[derive(Debug)]
pub(crate) struct TargetHandle {}

impl TargetHandle {
    pub(crate) fn new(
        target: Rc<Target>,
        cx: Context,
        handle: ServerEnd<bridge::TargetMarker>,
    ) -> Result<Pin<Box<dyn Future<Output = ()>>>> {
        let reboot_controller = reboot::RebootController::new(target.clone());
        let inner = TargetHandleInner { target, reboot_controller, tasks: TaskManager::default() };
        let stream = handle.into_stream()?;
        let fut = Box::pin(async move {
            let _ = stream
                .map_err(|err| anyhow!("{}", err))
                .try_for_each_concurrent_while_connected(None, |req| inner.handle(&cx, req))
                .await;
        });
        Ok(fut)
    }
}

struct TargetHandleInner {
    tasks: TaskManager,
    target: Rc<Target>,
    reboot_controller: reboot::RebootController,
}

impl TargetHandleInner {
    async fn handle(&self, _cx: &Context, req: bridge::TargetRequest) -> Result<()> {
        match req {
            bridge::TargetRequest::GetSshAddress { responder } => {
                // Product state and manual state are the two states where an
                // address is guaranteed. If the target is not in that state,
                // then wait for its state to change.
                let connection_state = self.target.get_connection_state();
                if !connection_state.is_product() && !connection_state.is_manual() {
                    self.target
                        .events
                        .wait_for(None, |e| {
                            if let TargetEvent::ConnectionStateChanged(_, state) = e {
                                // It's not clear if it is possible to change
                                // the state to `Manual`, but check for it just
                                // in case.
                                state.is_product() || state.is_manual()
                            } else {
                                false
                            }
                        })
                        .await
                        .context("waiting for connection state changes")?;
                }
                // After the event fires it should be guaranteed that the
                // SSH address is written to the target.
                let poll_duration = Duration::from_millis(15);
                loop {
                    if let Some(mut addr) = self.target.ssh_address_info() {
                        return responder.send(&mut addr).map_err(Into::into);
                    }
                    fuchsia_async::Timer::new(poll_duration).await;
                }
            }
            bridge::TargetRequest::OpenRemoteControl { remote_control, responder } => {
                self.target.run_host_pipe();
                let mut rcs = loop {
                    self.target
                        .events
                        .wait_for(None, |e| e == TargetEvent::RcsActivated)
                        .await
                        .context("waiting for RCS")?;
                    if let Some(rcs) = self.target.rcs() {
                        break rcs;
                    } else {
                        log::trace!("RCS dropped after event fired. Waiting again.");
                    }
                };
                // TODO(awdavies): Return this as a specific error to
                // the client with map_err.
                rcs.copy_to_channel(remote_control.into_channel())?;
                responder.send().map_err(Into::into)
            }
            bridge::TargetRequest::OpenFastboot { fastboot, .. } => {
                self.reboot_controller.spawn_fastboot(fastboot).await.map_err(Into::into)
            }
            bridge::TargetRequest::Reboot { state, responder } => {
                self.reboot_controller.reboot(state, responder).await
            }
            bridge::TargetRequest::Identity { responder } => {
                let target_info = bridge::TargetInfo::from(&*self.target);
                responder.send(target_info).map_err(Into::into)
            }
            bridge::TargetRequest::StreamActiveDiagnostics { parameters, iterator, responder } => {
                let target_identifier = self.target.nodename();
                let stream = self.target.stream_info();
                match stream
                    .wait_for_setup()
                    .map(|_| Ok(()))
                    .on_timeout(Duration::from_secs(3), || {
                        Err(bridge::DiagnosticsStreamError::NoStreamForTarget)
                    })
                    .await
                {
                    Ok(_) => {}
                    Err(e) => {
                        return responder.send(&mut Err(e)).context("sending error response");
                    }
                }
                let min_timestamp = match get_streaming_min_timestamp(&parameters, &stream).await {
                    Ok(n) => n,
                    Err(e) => {
                        responder.send(&mut Err(e))?;
                        return Ok(());
                    }
                };
                let log_iterator =
                    stream.stream_entries(parameters.stream_mode.unwrap(), min_timestamp).await?;
                self.tasks.spawn(async move {
                    let _ = run_diagnostics_streaming(log_iterator, iterator).await.map_err(|e| {
                        log::warn!("failure running diagnostics streaming: {:?}", e);
                    });
                });
                responder
                    .send(&mut Ok(bridge::LogSession {
                        target_identifier,
                        session_timestamp_nanos: stream
                            .session_timestamp_nanos()
                            .await
                            .map(|t| t as u64),
                        ..bridge::LogSession::EMPTY
                    }))
                    .map_err(Into::into)
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use chrono::Utc;
    use ffx_daemon_events::TargetConnectionState;
    use ffx_daemon_target::target::{TargetAddrEntry, TargetAddrType};
    use fidl::endpoints::ProtocolMarker;
    use fidl_fuchsia_developer_remotecontrol as fidl_rcs;
    use fuchsia_async::Task;
    use hoist::OvernetInstance;
    use protocols::testing::FakeDaemonBuilder;
    use rcs::RcsConnection;
    use std::net::{IpAddr, SocketAddr};
    use std::str::FromStr;

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_valid_target_state() {
        const TEST_SOCKETADDR: &'static str = "[fe80::1%1]:22";
        let daemon = FakeDaemonBuilder::new().build();
        let cx = Context::new(daemon);
        let target = Target::new_with_addr_entries(
            Some("pride-and-prejudice"),
            vec![TargetAddrEntry::new(
                SocketAddr::from_str(TEST_SOCKETADDR).unwrap().into(),
                Utc::now(),
                TargetAddrType::Ssh,
            )]
            .into_iter(),
        );
        target.update_connection_state(|_| TargetConnectionState::Mdns(std::time::Instant::now()));
        let (proxy, server) = fidl::endpoints::create_proxy::<bridge::TargetMarker>().unwrap();
        let _handle = Task::local(TargetHandle::new(target, cx, server).unwrap());
        let result = proxy.get_ssh_address().await.unwrap();
        if let bridge::TargetAddrInfo::IpPort(bridge::TargetIpPort {
            ip: fidl_fuchsia_net::IpAddress::Ipv6(fidl_fuchsia_net::Ipv6Address { addr }),
            ..
        }) = result
        {
            assert_eq!(IpAddr::from(addr), SocketAddr::from_str(TEST_SOCKETADDR).unwrap().ip());
        } else {
            panic!("incorrect address received: {:?}", result);
        }
    }

    fn spawn_protocol_provider(
        nodename: String,
        server: fidl::endpoints::ServerEnd<fidl_fuchsia_overnet::ServiceProviderMarker>,
    ) -> Task<()> {
        Task::local(async move {
            let mut stream = server.into_stream().unwrap();
            while let Ok(Some(req)) = stream.try_next().await {
                match req {
                    fidl_fuchsia_overnet::ServiceProviderRequest::ConnectToService {
                        chan, ..
                    } => {
                        let server_end =
                            fidl::endpoints::ServerEnd::<fidl_rcs::RemoteControlMarker>::new(chan);
                        let mut stream = server_end.into_stream().unwrap();
                        let nodename = nodename.clone();
                        Task::local(async move {
                            while let Ok(Some(req)) = stream.try_next().await {
                                match req {
                                    fidl_rcs::RemoteControlRequest::IdentifyHost { responder } => {
                                        let addrs = vec![fidl_fuchsia_net::Subnet {
                                            addr: fidl_fuchsia_net::IpAddress::Ipv4(
                                                fidl_fuchsia_net::Ipv4Address {
                                                    addr: [192, 168, 1, 2],
                                                },
                                            ),
                                            prefix_len: 24,
                                        }];
                                        let nodename = Some(nodename.clone());
                                        responder
                                            .send(&mut Ok(fidl_rcs::IdentifyHostResponse {
                                                nodename,
                                                addresses: Some(addrs),
                                                ..fidl_rcs::IdentifyHostResponse::EMPTY
                                            }))
                                            .unwrap();
                                    }
                                    _ => panic!("unsupported for this test"),
                                }
                            }
                        })
                        .detach();
                    }
                }
            }
        })
    }

    #[fuchsia_async::run_singlethreaded(test)]
    async fn test_open_rcs_valid() {
        const TEST_NODE_NAME: &'static str = "villete";
        let hoist2 = hoist::Hoist::new().unwrap();
        let (rx2, tx2) = fidl::Socket::create(fidl::SocketOpts::STREAM).unwrap();
        let (mut rx2, mut tx2) = (
            fidl::AsyncSocket::from_socket(rx2).unwrap(),
            fidl::AsyncSocket::from_socket(tx2).unwrap(),
        );
        let (rx1, tx1) = fidl::Socket::create(fidl::SocketOpts::STREAM).unwrap();
        let (mut rx1, mut tx1) = (
            fidl::AsyncSocket::from_socket(rx1).unwrap(),
            fidl::AsyncSocket::from_socket(tx1).unwrap(),
        );
        let _h1_task = Task::local(async move {
            let config = Box::new(move || {
                Some(fidl_fuchsia_overnet_protocol::LinkConfig::Socket(
                    fidl_fuchsia_overnet_protocol::Empty {},
                ))
            });
            stream_link::run_stream_link(
                hoist::hoist().node(),
                &mut rx1,
                &mut tx2,
                Default::default(),
                config,
            )
            .await
        });
        let hoist2_node = hoist2.node();
        let _h2_task = Task::local(async move {
            let config = Box::new(move || {
                Some(fidl_fuchsia_overnet_protocol::LinkConfig::Socket(
                    fidl_fuchsia_overnet_protocol::Empty {},
                ))
            });
            stream_link::run_stream_link(
                hoist2_node,
                &mut rx2,
                &mut tx1,
                Default::default(),
                config,
            )
            .await
        });
        let (client, server) =
            fidl::endpoints::create_endpoints::<fidl_fuchsia_overnet::ServiceProviderMarker>()
                .unwrap();
        let _svc_task = spawn_protocol_provider(TEST_NODE_NAME.to_owned(), server);
        hoist2
            .connect_as_service_publisher()
            .unwrap()
            .publish_service(fidl_rcs::RemoteControlMarker::NAME, client)
            .unwrap();
        let daemon = FakeDaemonBuilder::new().build();
        let cx = Context::new(daemon);
        let (client, server) = fidl::Channel::create().unwrap();
        hoist::hoist()
            .connect_as_service_consumer()
            .unwrap()
            .connect_to_service(
                &mut hoist2.node().node_id().into(),
                fidl_rcs::RemoteControlMarker::NAME,
                server,
            )
            .unwrap();
        let rcs_proxy =
            fidl_rcs::RemoteControlProxy::new(fidl::AsyncChannel::from_channel(client).unwrap());
        let target = Target::from_rcs_connection(RcsConnection::new_with_proxy(
            rcs_proxy.clone(),
            &hoist2.node().node_id().into(),
        ))
        .await
        .unwrap();
        let (target_proxy, server) =
            fidl::endpoints::create_proxy::<bridge::TargetMarker>().unwrap();
        let _handle = Task::local(TargetHandle::new(target, cx, server).unwrap());
        let (rcs, rcs_server) =
            fidl::endpoints::create_proxy::<fidl_rcs::RemoteControlMarker>().unwrap();
        target_proxy.open_remote_control(rcs_server).await.unwrap();
        assert_eq!(TEST_NODE_NAME, rcs.identify_host().await.unwrap().unwrap().nodename.unwrap());
    }
}
