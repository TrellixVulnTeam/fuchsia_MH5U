// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        legacy::{Iface, IfaceRef},
        mode_management::iface_manager_api::IfaceManagerApi,
        mode_management::phy_manager::PhyManagerApi,
    },
    anyhow::format_err,
    fidl::endpoints::create_proxy,
    fidl_fuchsia_wlan_common as wlan_common,
    fidl_fuchsia_wlan_device_service::{DeviceServiceProxy, DeviceWatcherEvent},
    fuchsia_zircon as zx,
    futures::lock::Mutex,
    log::{error, info},
    std::sync::Arc,
};

pub struct Listener {
    proxy: DeviceServiceProxy,
    legacy_shim: IfaceRef,
    phy_manager: Arc<Mutex<dyn PhyManagerApi + Send>>,
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
}

pub async fn handle_event(listener: &Listener, evt: DeviceWatcherEvent) {
    info!("got event: {:?}", evt);
    match evt {
        DeviceWatcherEvent::OnPhyAdded { phy_id } => {
            on_phy_added(listener, phy_id).await;
        }
        DeviceWatcherEvent::OnPhyRemoved { phy_id } => {
            info!("phy removed: {}", phy_id);
            let mut phy_manager = listener.phy_manager.lock().await;
            phy_manager.remove_phy(phy_id);
        }
        DeviceWatcherEvent::OnIfaceAdded { iface_id } => {
            // TODO(fxbug.dev/56539): When the legacy shim is removed, adding the interface to the PhyManager
            // should be handled inside of iface_manager.handle_added_iface.
            let mut phy_manager = listener.phy_manager.lock().await;
            match phy_manager.on_iface_added(iface_id).await {
                Ok(()) => match on_iface_added_legacy(listener, iface_id).await {
                    Ok(()) => {}
                    Err(e) => info!("error adding new iface {}: {}", iface_id, e),
                },
                Err(e) => info!("error adding new iface {}: {}", iface_id, e),
            }

            // Drop the PhyManager lock after using it.  The IfaceManager will need to lock the
            // resource as part of the connect operation.
            drop(phy_manager);

            let mut iface_manager = listener.iface_manager.lock().await;
            if let Err(e) = iface_manager.handle_added_iface(iface_id).await {
                error!("Failed to add interface to IfaceManager: {}", e);
            }
        }
        DeviceWatcherEvent::OnIfaceRemoved { iface_id } => {
            let mut iface_manager = listener.iface_manager.lock().await;
            match iface_manager.handle_removed_iface(iface_id).await {
                Ok(()) => {}
                Err(e) => info!("Unable to record idle interface {}: {:?}", iface_id, e),
            }

            listener.legacy_shim.remove_if_matching(iface_id);
            info!("iface removed: {}", iface_id);
        }
    }
}

async fn on_phy_added(listener: &Listener, phy_id: u16) {
    info!("phy {} added", phy_id);
    let mut phy_manager = listener.phy_manager.lock().await;
    if let Err(e) = phy_manager.add_phy(phy_id).await {
        info!("error adding new phy {}: {}", phy_id, e);
        phy_manager.log_phy_add_failure();
    }
}

/// Configured the interface that is used to service the legacy WLAN API.
async fn on_iface_added_legacy(listener: &Listener, iface_id: u16) -> Result<(), anyhow::Error> {
    let (status, response) = listener.proxy.query_iface(iface_id).await?;
    match status {
        fuchsia_zircon::sys::ZX_OK => {}
        fuchsia_zircon::sys::ZX_ERR_NOT_FOUND => {
            return Err(format_err!("Could not find iface: {}", iface_id));
        }
        ref status => return Err(format_err!("Could not query iface information: {}", status)),
    }

    let response =
        response.ok_or_else(|| format_err!("Iface information is missing: {}", iface_id))?;

    let service = listener.proxy.clone();

    match response.role {
        wlan_common::WlanMacRole::Client => {
            let legacy_shim = listener.legacy_shim.clone();
            let (sme, remote) = create_proxy()
                .map_err(|e| format_err!("Failed to create a FIDL channel: {}", e))?;

            let status = service
                .get_client_sme(iface_id, remote)
                .await
                .map_err(|e| format_err!("Failed to get client SME: {}", e))?;

            zx::Status::ok(status)
                .map_err(|e| format_err!("GetClientSme returned an error: {}", e))?;

            let lc = Iface { sme: sme.clone(), iface_id };
            legacy_shim.set_if_empty(lc);
        }
        // The AP service make direct use of the PhyManager to get interfaces.
        wlan_common::WlanMacRole::Ap => {}
        wlan_common::WlanMacRole::Mesh => {
            return Err(format_err!("Unexpectedly observed a mesh iface: {}", iface_id))
        }
    }

    info!("new iface {} added successfully", iface_id);
    Ok(())
}

impl Listener {
    pub fn new(
        proxy: DeviceServiceProxy,
        legacy_shim: IfaceRef,
        phy_manager: Arc<Mutex<dyn PhyManagerApi + Send>>,
        iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    ) -> Self {
        Listener { proxy, legacy_shim, phy_manager, iface_manager }
    }
}
