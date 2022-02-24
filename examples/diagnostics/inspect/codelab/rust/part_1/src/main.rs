// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::reverser::ReverserServerFactory,
    anyhow::{Context, Error},
    fidl_fuchsia_examples_inspect::FizzBuzzMarker,
    fuchsia_component::{client, server::ServiceFs},
    futures::{future::try_join, FutureExt, StreamExt},
    tracing::info,
    // CODELAB: Use inspect.
};

mod reverser;

// [START init_logger]
#[fuchsia::component(logging_tags = ["inspect_rust_codelab", "part1"])]
// [END init_logger]
async fn main() -> Result<(), Error> {
    // [START servicefs_init]
    let mut fs = ServiceFs::new();
    // [END servicefs_init]

    info!("starting up...");

    // CODELAB: Initialize Inspect here.

    // Create a new Reverser Server factory.
    let reverser_factory = ReverserServerFactory::new();

    // Serve the reverser service
    // [START serve_service]
    fs.dir("svc").add_fidl_service(move |stream| reverser_factory.spawn_new(stream));
    fs.take_and_serve_directory_handle()?;
    // [END serve_service]

    // Send a request to the FizzBuzz service and print the response when it arrives.
    // [START fizzbuzz_connect]
    let fizzbuzz_fut = async move {
        let fizzbuzz = client::connect_to_protocol::<FizzBuzzMarker>()
            .context("failed to connect to fizzbuzz")?;
        match fizzbuzz.execute(30u32).await {
            Ok(result) => info!(%result, "Got FizzBuzz"),
            Err(_) => {}
        };
        Ok(())
    };
    // [END fizzbuzz_connect]

    // [START servicefs_collect]
    let running_service_fs = fs.collect::<()>().map(Ok);
    // [END servicefs_collect]
    try_join(running_service_fs, fizzbuzz_fut).await.map(|((), ())| ())
}
