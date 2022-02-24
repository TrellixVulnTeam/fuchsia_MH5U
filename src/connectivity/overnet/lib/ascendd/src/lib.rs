// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod serial;

use crate::serial::run_serial_link_handlers;
use anyhow::{bail, format_err, Error};
use argh::FromArgs;
use async_net::unix::{UnixListener, UnixStream};
use fuchsia_async::Task;
use fuchsia_async::TimeoutExt;
use futures::prelude::*;
use hoist::hoist;
use std::sync::Arc;
use std::task::{Context, Poll};
use std::{io::ErrorKind::TimedOut, pin::Pin, time::Duration};
use stream_link::run_stream_link;

#[derive(FromArgs, Default)]
/// daemon to lift a non-Fuchsia device into Overnet.
pub struct Opt {
    #[argh(option, long = "sockpath")]
    /// path to the ascendd socket.
    /// If not provided, this will default to a new socket-file in /tmp.
    pub sockpath: Option<String>,

    #[argh(option, long = "serial")]
    /// selector for which serial devices to communicate over.
    /// Could be 'none' to not communcate over serial, 'all' to query all serial devices and try to
    /// communicate with them, or a path to a serial device to communicate over *that* device.
    /// If not provided, this will default to 'none'.
    pub serial: Option<String>,
}

#[derive(Debug)]
pub struct Ascendd {
    task: Task<Result<(), Error>>,
}

impl Ascendd {
    pub async fn new(
        opt: Opt,
        stdout: impl AsyncWrite + Unpin + Send + 'static,
    ) -> Result<Self, Error> {
        let (sockpath, serial, incoming) = bind_listener(opt).await?;
        Ok(Self { task: Task::spawn(run_ascendd(sockpath, serial, incoming, stdout)) })
    }
}

impl Future for Ascendd {
    type Output = Result<(), Error>;

    fn poll(mut self: Pin<&mut Self>, ctx: &mut Context<'_>) -> Poll<Result<(), Error>> {
        self.task.poll_unpin(ctx)
    }
}

/// Run an ascendd server on the given stream IOs identified by the given labels
/// and paths, to completion.
pub fn run_stream<'a>(
    node: Arc<overnet_core::Router>,
    rx: &'a mut (dyn AsyncRead + Unpin + Send),
    tx: &'a mut (dyn AsyncWrite + Unpin + Send),
    label: Option<String>,
    path: Option<String>,
) -> impl Future<Output = Result<(), Error>> + 'a {
    let config = Box::new(move || {
        Some(fidl_fuchsia_overnet_protocol::LinkConfig::AscenddServer(
            fidl_fuchsia_overnet_protocol::AscenddLinkConfig {
                path: path.clone(),
                connection_label: label.clone(),
                ..fidl_fuchsia_overnet_protocol::AscenddLinkConfig::EMPTY
            },
        ))
    });

    run_stream_link(node, rx, tx, Default::default(), config)
}

async fn bind_listener(opt: Opt) -> Result<(String, String, UnixListener), Error> {
    let Opt { sockpath, serial } = opt;

    let sockpath = sockpath.unwrap_or(hoist::DEFAULT_ASCENDD_PATH.to_string());
    let serial = serial.unwrap_or("none".to_string());

    log::info!("starting ascendd on {} with node id {:?}", sockpath, hoist().node().node_id());

    let incoming = loop {
        match UnixListener::bind(&sockpath) {
            Ok(listener) => {
                break listener;
            }
            Err(_) => match UnixStream::connect(&sockpath)
                .on_timeout(Duration::from_secs(1), || {
                    Err(std::io::Error::new(TimedOut, format_err!("connecting to ascendd socket")))
                })
                .await
            {
                Ok(_) => {
                    log::error!("another ascendd is already listening at {}", &sockpath);
                    bail!("another ascendd is aleady listening!");
                }
                Err(_) => {
                    log::info!("cleaning up stale ascendd socket at {}", &sockpath);
                    std::fs::remove_file(&sockpath)?;
                }
            },
        }
    };

    Ok((sockpath, serial, incoming))
}

async fn run_ascendd(
    sockpath: String,
    serial: String,
    incoming: UnixListener,
    stdout: impl AsyncWrite + Unpin + Send,
) -> Result<(), Error> {
    let node = hoist().node();
    node.set_implementation(fidl_fuchsia_overnet_protocol::Implementation::Ascendd);

    log::info!("ascendd listening to socket {}", sockpath);

    let sockpath = &sockpath;

    futures::future::try_join(
        run_serial_link_handlers(Arc::downgrade(&hoist().node()), &serial, stdout),
        async move {
            incoming
                .incoming()
                .for_each_concurrent(None, |stream| async move {
                    match stream {
                        Ok(stream) => {
                            let (mut rx, mut tx) = stream.split();
                            if let Err(e) = run_stream(
                                hoist().node(),
                                &mut rx,
                                &mut tx,
                                None,
                                Some(sockpath.to_string()),
                            )
                            .await
                            {
                                log::warn!("Failed serving socket: {:?}", e);
                            }
                        }
                        Err(e) => {
                            log::warn!("Failed starting socket: {:?}", e);
                        }
                    }
                })
                .await;
            Ok(())
        },
    )
    .await
    .map(drop)
}
