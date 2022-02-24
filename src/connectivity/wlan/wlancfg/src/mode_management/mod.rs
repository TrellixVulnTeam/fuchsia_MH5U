// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{
        client::network_selection::NetworkSelector, config_management::SavedNetworksManagerApi,
        telemetry::TelemetrySender, util::listener,
    },
    anyhow::Error,
    fuchsia_cobalt::CobaltSender,
    futures::{channel::mpsc, lock::Mutex, Future},
    std::sync::Arc,
    void::Void,
};

mod iface_manager;
pub mod iface_manager_api;
mod iface_manager_types;
pub mod phy_manager;

pub fn create_iface_manager(
    phy_manager: Arc<Mutex<dyn phy_manager::PhyManagerApi + Send>>,
    client_update_sender: listener::ClientListenerMessageSender,
    ap_update_sender: listener::ApListenerMessageSender,
    dev_svc_proxy: fidl_fuchsia_wlan_device_service::DeviceServiceProxy,
    saved_networks: Arc<dyn SavedNetworksManagerApi>,
    network_selector: Arc<NetworkSelector>,
    cobalt_api: CobaltSender,
    telemetry_sender: TelemetrySender,
) -> (Arc<Mutex<iface_manager_api::IfaceManager>>, impl Future<Output = Result<Void, Error>>) {
    let (sender, receiver) = mpsc::channel(0);
    let iface_manager_sender = Arc::new(Mutex::new(iface_manager_api::IfaceManager { sender }));
    let (stats_sender, stats_receiver) = mpsc::unbounded();
    let iface_manager = iface_manager::IfaceManagerService::new(
        phy_manager,
        client_update_sender,
        ap_update_sender,
        dev_svc_proxy,
        saved_networks,
        network_selector.clone(),
        cobalt_api,
        telemetry_sender,
        stats_sender,
    );
    let iface_manager_service = iface_manager::serve_iface_manager_requests(
        iface_manager,
        iface_manager_sender.clone(),
        network_selector,
        receiver,
        stats_receiver,
    );

    (iface_manager_sender, iface_manager_service)
}
