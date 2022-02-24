// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::{Context as _, Error},
    fuchsia_async as fasync,
    futures::lock::Mutex,
    std::sync::Arc,
};

use crate::{
    registry::RegistryStore,
    router::Router,
    service::{ManagerService, RegistryService},
};

mod registry;
mod router;
mod service;

const SERVER_THREADS: usize = 2;

/// Environment consists of FIDL services handlers and shortcut storage.
pub struct Environment {
    store: RegistryStore,
    registry_service: RegistryService,
    manager_service: Arc<Mutex<ManagerService>>,
}

impl Environment {
    fn new() -> Self {
        let store = RegistryStore::new();
        Environment {
            store: store.clone(),
            registry_service: RegistryService::new(),
            manager_service: Arc::new(Mutex::new(ManagerService::new(store))),
        }
    }
}

async fn run() -> Result<(), Error> {
    let environment = Environment::new();
    let router = Router::new(environment);
    router.setup_and_serve_fs().await
}

fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["shortcut"]).expect("shortcut syslog init should not fail");
    let mut executor = fasync::SendExecutor::new(SERVER_THREADS)
        .context("Creating fuchsia_async executor for Shortcut failed")?;

    executor.run(run())
}
