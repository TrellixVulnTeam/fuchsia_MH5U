// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! GoogleAuthProvider is an implementation of the `AuthProvider` FIDL protocol
//! that communicates with the Google identity system to perform authentication
//! for and issue tokens for Google accounts.

#![deny(missing_docs)]

mod constants;
mod error;
mod http;
mod oauth;
mod oauth_open_id_connect;
mod open_id_connect;
mod time;
mod web;

use crate::http::UrlLoaderHttpClient;
use crate::oauth::Oauth;
use crate::oauth_open_id_connect::OauthOpenIdConnect;
use crate::open_id_connect::OpenIdConnect;
use crate::time::UtcClock;
use crate::web::DefaultStandaloneWebFrame;
use anyhow::{Context as _, Error};
use fidl::endpoints::{create_proxy, ClientEnd};
use fidl_fuchsia_net_http::LoaderMarker;
use fidl_fuchsia_web::{ContextMarker, ContextProviderMarker, CreateContextParams, FrameMarker};
use fuchsia_async as fasync;
use fuchsia_component::client::connect_to_protocol;
use fuchsia_component::server::ServiceFs;
use fuchsia_zircon as zx;
use futures::StreamExt;
use log::{error, info};
use std::sync::Arc;

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["auth"]).expect("Can't init logger");
    info!("Starting Google Auth Provider");

    let mut executor = fasync::LocalExecutor::new().context("Error creating executor")?;

    let url_loader = connect_to_protocol::<LoaderMarker>()?;
    let frame_supplier = WebFrameSupplier::new();
    let http_client = UrlLoaderHttpClient::new(url_loader);

    let mut fs = ServiceFs::new();

    let oauth_open_id_connect =
        Arc::new(OauthOpenIdConnect::<_, UtcClock>::new(http_client.clone()));
    fs.dir("svc").add_fidl_service(move |stream| {
        let oauth_open_id_connect_clone = Arc::clone(&oauth_open_id_connect);
        fasync::Task::spawn(async move {
            oauth_open_id_connect_clone
                .handle_requests_from_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Error handling OauthOpenIdConnect channel: {:?}", e));
        })
        .detach();
    });

    let oauth = Arc::new(Oauth::<_, _, UtcClock>::new(frame_supplier, http_client));
    fs.dir("svc").add_fidl_service(move |stream| {
        let oauth_clone = Arc::clone(&oauth);
        fasync::Task::spawn(async move {
            oauth_clone
                .handle_requests_from_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Error handling Oauth channel: {:?}", e));
        })
        .detach();
    });

    let open_id_connect = Arc::new(OpenIdConnect::new());
    fs.dir("svc").add_fidl_service(move |stream| {
        let open_id_connect_clone = Arc::clone(&open_id_connect);
        fasync::Task::spawn(async move {
            open_id_connect_clone
                .handle_requests_from_stream(stream)
                .await
                .unwrap_or_else(|e| error!("Error handling OpenIdConnect channel: {:?}", e));
        })
        .detach();
    });

    fs.take_and_serve_directory_handle()?;

    executor.run_singlethreaded(fs.collect::<()>());
    Ok(())
}

/// Struct that provides new web frames by connecting to a ContextProvider service.
#[derive(Clone)]
struct WebFrameSupplier;

impl WebFrameSupplier {
    /// Create a new `WebFrameSupplier`
    fn new() -> Self {
        WebFrameSupplier {}
    }
}

impl web::WebFrameSupplier for WebFrameSupplier {
    type Frame = DefaultStandaloneWebFrame;
    fn new_standalone_frame(&self) -> Result<DefaultStandaloneWebFrame, anyhow::Error> {
        let context_provider = connect_to_protocol::<ContextProviderMarker>()?;
        let (context_proxy, context_server_end) = create_proxy::<ContextMarker>()?;

        // Get a handle to the incoming service directory to pass to the web browser.
        // TODO(satsukiu): create a method in fidl::endpoints to connect to ServiceFs instead.
        let (client, server) = zx::Channel::create()?;
        fdio::service_connect("/svc", server)?;
        let service_directory = ClientEnd::new(client);

        context_provider.create(
            CreateContextParams {
                service_directory: Some(service_directory),
                ..CreateContextParams::EMPTY
            },
            context_server_end,
        )?;

        let (frame_proxy, frame_server_end) = create_proxy::<FrameMarker>()?;
        context_proxy.create_frame(frame_server_end)?;
        Ok(DefaultStandaloneWebFrame::new(context_proxy, frame_proxy))
    }
}
