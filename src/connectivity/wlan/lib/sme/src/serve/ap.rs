// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::ap as ap_sme;
use anyhow::format_err;
use fidl_fuchsia_wlan_mlme::{self as fidl_mlme, MlmeEventStream, MlmeProxy};
use fidl_fuchsia_wlan_sme as fidl_sme;
use futures::channel::mpsc;
use futures::prelude::*;
use futures::select;
use futures::stream::FuturesUnordered;
use ieee80211::Ssid;
use log::error;
use pin_utils::pin_mut;
use std::sync::{Arc, Mutex};
use void::Void;
use wlan_common::RadioConfig;

pub type Endpoint = fidl::endpoints::ServerEnd<fidl_sme::ApSmeMarker>;
type Sme = ap_sme::ApSme;

pub async fn serve(
    proxy: MlmeProxy,
    device_info: fidl_mlme::DeviceInfo,
    event_stream: MlmeEventStream,
    new_fidl_clients: mpsc::UnboundedReceiver<Endpoint>,
) -> Result<(), anyhow::Error> {
    let (sme, mlme_stream, time_stream) = Sme::new(device_info);
    let sme = Arc::new(Mutex::new(sme));
    let mlme_sme =
        super::serve_mlme_sme(proxy, event_stream, Arc::clone(&sme), mlme_stream, time_stream);
    let sme_fidl = serve_fidl(&sme, new_fidl_clients);
    pin_mut!(mlme_sme);
    pin_mut!(sme_fidl);
    select! {
        mlme_sme = mlme_sme.fuse() => mlme_sme?,
        sme_fidl = sme_fidl.fuse() => match sme_fidl? {},
    }
    Ok(())
}

async fn serve_fidl(
    sme: &Mutex<Sme>,
    new_fidl_clients: mpsc::UnboundedReceiver<Endpoint>,
) -> Result<Void, anyhow::Error> {
    let mut new_fidl_clients = new_fidl_clients.fuse();
    let mut fidl_clients = FuturesUnordered::new();
    loop {
        select! {
            new_fidl_client = new_fidl_clients.next() => match new_fidl_client {
                Some(c) => fidl_clients.push(serve_fidl_endpoint(sme, c)),
                None => return Err(format_err!("New FIDL client stream unexpectedly ended")),
            },
            _ = fidl_clients.next() => {},
        }
    }
}

async fn serve_fidl_endpoint(sme: &Mutex<Sme>, endpoint: Endpoint) {
    const MAX_CONCURRENT_REQUESTS: usize = 1000;
    let stream = match endpoint.into_stream() {
        Ok(s) => s,
        Err(e) => {
            error!("Failed to create a stream from a zircon channel: {}", e);
            return;
        }
    };
    let r = stream
        .try_for_each_concurrent(MAX_CONCURRENT_REQUESTS, move |request| {
            handle_fidl_request(sme, request)
        })
        .await;
    if let Err(e) = r {
        error!("Error serving a FIDL client of AP SME: {}", e);
    }
}

async fn handle_fidl_request(
    sme: &Mutex<Sme>,
    request: fidl_sme::ApSmeRequest,
) -> Result<(), ::fidl::Error> {
    match request {
        fidl_sme::ApSmeRequest::Start { config, responder } => {
            let r = start(sme, config).await;
            responder.send(r)?;
        }
        fidl_sme::ApSmeRequest::Stop { responder } => {
            let r = stop(sme).await;
            responder.send(r)?;
        }
        fidl_sme::ApSmeRequest::Status { responder } => {
            let mut r = status(sme);
            responder.send(&mut r)?;
        }
    }
    Ok(())
}

async fn start(sme: &Mutex<Sme>, config: fidl_sme::ApConfig) -> fidl_sme::StartApResultCode {
    let sme_config = ap_sme::Config {
        ssid: Ssid::from_bytes_unchecked(config.ssid),
        password: config.password,
        radio_cfg: RadioConfig::from(config.radio_cfg),
    };

    let receiver = sme.lock().unwrap().on_start_command(sme_config);
    let r = receiver.await.unwrap_or_else(|_| {
        error!("Responder for AP Start command was dropped without sending a response");
        ap_sme::StartResult::InternalError
    });

    match r {
        ap_sme::StartResult::Success => fidl_sme::StartApResultCode::Success,
        ap_sme::StartResult::AlreadyStarted => fidl_sme::StartApResultCode::AlreadyStarted,
        ap_sme::StartResult::InternalError => fidl_sme::StartApResultCode::InternalError,
        ap_sme::StartResult::Canceled => fidl_sme::StartApResultCode::Canceled,
        ap_sme::StartResult::TimedOut => fidl_sme::StartApResultCode::TimedOut,
        ap_sme::StartResult::PreviousStartInProgress => {
            fidl_sme::StartApResultCode::PreviousStartInProgress
        }
        ap_sme::StartResult::InvalidArguments(e) => {
            error!("Invalid arguments for AP start: {}", e);
            fidl_sme::StartApResultCode::InvalidArguments
        }
    }
}

async fn stop(sme: &Mutex<Sme>) -> fidl_sme::StopApResultCode {
    let receiver = sme.lock().unwrap().on_stop_command();
    receiver.await.unwrap_or_else(|_| {
        error!("Responder for AP Stop command was dropped without sending a response");
        fidl_sme::StopApResultCode::InternalError
    })
}

fn status(sme: &Mutex<Sme>) -> fidl_sme::ApStatusResponse {
    fidl_sme::ApStatusResponse { running_ap: sme.lock().unwrap().get_running_ap().map(Box::new) }
}
