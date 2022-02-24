// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        crypt::Crypt,
        object_store::{volume::root_volume, FxFilesystem},
    },
    anyhow::Error,
    std::sync::Arc,
    storage_device::DeviceHolder,
};

pub async fn mkfs(device: DeviceHolder, crypt: Arc<dyn Crypt>) -> Result<(), Error> {
    let fs = FxFilesystem::new_empty(device).await?;
    {
        // expect instead of propagating errors here, since otherwise we could drop |fs| before
        // close is called, which leads to confusing and unrelated error messages.
        let root_volume = root_volume(&fs).await.expect("Open root_volume failed");
        root_volume.new_volume("default", crypt).await.expect("Create volume failed");
    }
    fs.close().await?;
    Ok(())
}
