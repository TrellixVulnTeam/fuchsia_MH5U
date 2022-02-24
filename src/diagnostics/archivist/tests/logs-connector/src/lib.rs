// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#![warn(clippy::all)]

use diagnostics_message::fx_log_packet_t;
use fidl::{
    endpoints::{ClientEnd, ProtocolMarker, ServerEnd},
    Socket, SocketOpts,
};
use fidl_fuchsia_diagnostics_test::ControllerMarker;
use fidl_fuchsia_logger::{LogFilterOptions, LogLevelFilter, LogMarker, LogMessage, LogSinkMarker};
use fidl_fuchsia_sys::LauncherMarker;
use fidl_fuchsia_sys_internal::{
    LogConnection, LogConnectionListenerMarker, LogConnectorMarker, LogConnectorRequest,
    LogConnectorRequestStream, SourceIdentity,
};
use fuchsia_async as fasync;
use fuchsia_component::{
    client::{connect_to_protocol, launch_with_options, LaunchOptions},
    server::ServiceFs,
};
use fuchsia_syslog::levels::INFO;
use fuchsia_syslog_listener::{run_log_listener_with_proxy, LogProcessor};
use fuchsia_zircon as zx;
use futures::{channel::mpsc, SinkExt, StreamExt, TryStreamExt};

#[fuchsia::test]
async fn same_log_sink_simultaneously_via_connector() {
    let (client, server) = zx::Channel::create().unwrap();
    let mut serverend = Some(ServerEnd::new(server));
    // connect multiple identical log sinks
    let mut sockets = Vec::new();
    for _ in 0..50 {
        let (message_client, message_server) = Socket::create(SocketOpts::DATAGRAM).unwrap();
        sockets.push(message_server);

        // each with the same message repeated multiple times
        let mut packet = fx_log_packet_t::default();
        packet.metadata.pid = 1000;
        packet.metadata.tid = 2000;
        packet.metadata.severity = LogLevelFilter::Info.into_primitive().into();
        packet.data[0] = 0;
        packet.add_data(1, "repeated log".as_bytes());
        for _ in 0..5 {
            message_client.write(packet.as_bytes()).unwrap();
        }
    }
    {
        let listener = ClientEnd::<LogConnectionListenerMarker>::new(client).into_proxy().unwrap();
        for socket in sockets {
            let (client, server) = zx::Channel::create().unwrap();
            let log_request = ServerEnd::<LogSinkMarker>::new(server);
            let source_identity = SourceIdentity {
                realm_path: Some(vec![]),
                component_name: Some("testing123".to_string()),
                instance_id: Some("0".to_string()),
                component_url: Some(
                    "fuchsia-pkg://fuchsia.com/test-logs-connector#meta/test-logs-connector.cmx"
                        .to_string(),
                ),
                ..SourceIdentity::EMPTY
            };
            listener
                .on_new_connection(&mut LogConnection { log_request, source_identity })
                .unwrap();
            let log_sink = ClientEnd::<LogSinkMarker>::new(client).into_proxy().unwrap();
            log_sink.connect(socket).unwrap();
        }
    }
    let (dir_client, dir_server) = zx::Channel::create().unwrap();
    let mut fs = ServiceFs::new();
    let (take_log_listener_response_snd, mut take_log_listener_response_rcv) =
        futures::channel::mpsc::channel(1);
    fs.add_fidl_service_at(
        LogConnectorMarker::NAME,
        move |mut stream: LogConnectorRequestStream| {
            let mut serverend = serverend.take();
            let mut sender_mut = Some(take_log_listener_response_snd.clone());
            fasync::Task::spawn(async move {
                while let Some(LogConnectorRequest::TakeLogConnectionListener { responder }) =
                    stream.try_next().await.unwrap()
                {
                    responder.send(serverend.take()).unwrap();
                    if let Some(mut sender) = sender_mut.take() {
                        sender.send(()).await.unwrap();
                    }
                }
            })
            .detach()
        },
    )
    .serve_connection(dir_server)
    .unwrap();
    fasync::Task::spawn(fs.collect()).detach();

    // launch archivist-for-embedding.cmx
    let launcher = connect_to_protocol::<LauncherMarker>().unwrap();
    let mut options = LaunchOptions::new();
    options.set_additional_services(vec![LogConnectorMarker::NAME.to_string()], dir_client);
    let mut archivist = launch_with_options(
        &launcher,
        "fuchsia-pkg://fuchsia.com/archivist-for-embedding#meta/archivist-for-embedding.cmx"
            .to_owned(),
        None,
        options,
    )
    .unwrap();
    take_log_listener_response_rcv.next().await.unwrap();
    // run log listener
    let log_proxy = archivist.connect_to_protocol::<LogMarker>().unwrap();
    let (send_logs, recv_logs) = mpsc::unbounded();
    fasync::Task::spawn(async move {
        let listen = Listener { send_logs };
        let mut options = LogFilterOptions {
            filter_by_pid: true,
            pid: 1000,
            filter_by_tid: true,
            tid: 2000,
            verbosity: 0,
            min_severity: LogLevelFilter::None,
            tags: Vec::new(),
        };
        run_log_listener_with_proxy(&log_proxy, listen, Some(&mut options), false, None)
            .await
            .unwrap();
    })
    .detach();

    // connect to controller and call stop
    let controller = archivist.connect_to_protocol::<ControllerMarker>().unwrap();
    controller.stop().unwrap();

    // collect all logs
    let logs = recv_logs.map(|message| (message.severity, message.msg)).collect::<Vec<_>>().await;

    // recv_logs returned, means archivist_for_test must be dead. check.
    assert!(archivist.wait().await.unwrap().success());
    assert_eq!(
        logs,
        std::iter::repeat((INFO, "repeated log".to_owned())).take(250).collect::<Vec<_>>()
    );
}

struct Listener {
    send_logs: mpsc::UnboundedSender<LogMessage>,
}

impl LogProcessor for Listener {
    fn log(&mut self, message: LogMessage) {
        self.send_logs.unbounded_send(message).unwrap();
    }

    fn done(&mut self) {
        panic!("this should not be called");
    }
}
