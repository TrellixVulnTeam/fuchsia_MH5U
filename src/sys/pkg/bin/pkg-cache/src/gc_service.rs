// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::base_packages::BasePackages,
    crate::index::PackageIndex,
    anyhow::{anyhow, Context, Error},
    fidl_fuchsia_space::{
        ErrorCode as SpaceErrorCode, ManagerRequest as SpaceManagerRequest,
        ManagerRequestStream as SpaceManagerRequestStream,
    },
    fidl_fuchsia_update::CommitStatusProviderProxy,
    fuchsia_syslog::{fx_log_err, fx_log_info},
    fuchsia_zircon::{self as zx, AsHandleRef},
    futures::{lock::Mutex, prelude::*},
    std::sync::Arc,
};

pub async fn serve(
    blobfs: blobfs::Client,
    base_packages: Arc<BasePackages>,
    package_index: Arc<Mutex<PackageIndex>>,
    commit_status_provider: CommitStatusProviderProxy,
    mut stream: SpaceManagerRequestStream,
) -> Result<(), Error> {
    let event_pair = commit_status_provider
        .is_current_system_committed()
        .await
        .context("while getting event pair")?;

    while let Some(event) = stream.try_next().await? {
        let SpaceManagerRequest::Gc { responder } = event;
        responder
            .send(&mut gc(&blobfs, base_packages.as_ref(), &package_index, &event_pair).await)?;
    }
    Ok(())
}

async fn gc(
    blobfs: &blobfs::Client,
    base_packages: &BasePackages,
    package_index: &Arc<Mutex<PackageIndex>>,
    event_pair: &zx::EventPair,
) -> Result<(), SpaceErrorCode> {
    fx_log_info!("performing gc");

    event_pair.wait_handle(zx::Signals::USER_0, zx::Time::INFINITE_PAST).map_err(|e| {
        match e {
            zx::Status::TIMED_OUT => {
                fx_log_info!("GC is blocked pending update.");
            }
            zx::Status::CANCELED => {
                fx_log_info!("Commit handle is closed, likely because we are rebooting.");
            }
            other => {
                fx_log_err!("Got unexpected status {:?} while waiting on handle.", other);
            }
        }
        SpaceErrorCode::PendingCommit
    })?;

    async move {
        // First, read out all blobs currently known in the system. Anything resolve after this is
        // implicitly protected, because only blobs that appear in this list can be collected.
        let mut eligible_blobs = blobfs.list_known_blobs().await?;

        // Any blobs protected by the package index are ineligible for collection.
        let package_index = package_index.lock().await;
        package_index.all_blobs().iter().for_each(|blob| {
            eligible_blobs.remove(blob);
        });

        // Blobs in base are immutable and ineligible for collection.
        base_packages.list_blobs().iter().for_each(|blob| {
            eligible_blobs.remove(blob);
        });

        // Evict all eligible blobs from blobfs.
        fx_log_info!("Garbage collecting {} blobs...", eligible_blobs.len());
        for (i, blob) in eligible_blobs.iter().enumerate() {
            blobfs.delete_blob(&blob).await?;
            if (i + 1) % 100 == 0 {
                fx_log_info!("{} blobs collected...", i + 1);
            }
        }
        Ok(())
    }
    .await
    .map_err(|e: Error| {
        fx_log_err!("Failed to perform GC operation: {:#}", anyhow!(e));
        SpaceErrorCode::Internal
    })?;

    Ok(())
}
