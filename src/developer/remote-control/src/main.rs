// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fidl::endpoints::{ClientEnd, ProtocolMarker, RequestStream},
    fidl_fuchsia_developer_remotecontrol as rcs,
    fidl_fuchsia_overnet::{ServiceProviderRequest, ServiceProviderRequestStream},
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    futures::join,
    futures::prelude::*,
    hoist::{hoist, OvernetInstance},
    remote_control::RemoteControlService,
    std::rc::Rc,
    tracing::{error, info},
};

mod args;

async fn exec_server() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["remote-control"])?;

    let (s, p) = fidl::Channel::create().context("creating ServiceProvider zx channel")?;
    let chan =
        fidl::AsyncChannel::from_channel(s).context("creating ServiceProvider async channel")?;
    let stream = ServiceProviderRequestStream::from_channel(chan);
    hoist().publish_service(rcs::RemoteControlMarker::NAME, ClientEnd::new(p))?;

    let service = Rc::new(RemoteControlService::new().await);

    let sc1 = service.clone();
    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(move |req| {
        fasync::Task::local(sc1.clone().serve_stream(req).map(|_| ())).detach();
    });

    fs.take_and_serve_directory_handle()?;
    let fidl_fut = fs.collect::<()>();

    let sc = service.clone();
    let onet_fut = stream.for_each_concurrent(None, move |svc| {
        let ServiceProviderRequest::ConnectToService {
            chan,
            info: _,
            control_handle: _control_handle,
        } = svc.unwrap();
        let chan =
            fidl::AsyncChannel::from_channel(chan).context("failed to make async channel").unwrap();

        sc.clone().serve_stream(rcs::RemoteControlRequestStream::from_channel(chan)).map(|_| ())
    });
    info!("published remote control service to overnet");

    join!(fidl_fut, onet_fut);
    Ok(())
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    let args::RemoteControl { cmd } = argh::from_env();

    let res = match cmd {
        args::Command::DiagnosticsBridge(_) => diagnostics_bridge::exec_server().await,
        args::Command::RemoteControl(_) => exec_server().await,
    };

    if let Err(err) = res {
        error!(%err, "Error running command");
        std::process::exit(1);
    }
    Ok(())
}
