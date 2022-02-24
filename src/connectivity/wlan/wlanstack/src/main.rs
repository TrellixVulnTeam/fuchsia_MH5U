// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! System service for wireless networking

#![deny(missing_docs)]
#![recursion_limit = "256"]

mod device;
mod future_util;
mod inspect;
mod service;
#[cfg(test)]
pub mod test_helper;

use anyhow::{Context, Error};
use argh::FromArgs;
use fidl_fuchsia_wlan_device_service::DeviceServiceRequestStream;
use fuchsia_async as fasync;
use fuchsia_component::server::{ServiceFs, ServiceObjLocal};
use fuchsia_inspect::Inspector;
use fuchsia_inspect_contrib::auto_persist;
use fuchsia_syslog as syslog;
use futures::future::try_join;
use futures::prelude::*;
use log::info;
use std::sync::Arc;
use wlan_sme;

use crate::device::IfaceMap;

const CONCURRENT_LIMIT: usize = 1000;

// Service name to persist Inspect data across boots
const PERSISTENCE_SERVICE_PATH: &str = "/svc/fuchsia.diagnostics.persist.DataPersistence-wlan";

/// Configuration for wlanstack service.
/// This configuration is a super set of individual component configurations such as SME.
#[derive(FromArgs, Clone, Debug, Default)]
pub struct ServiceCfg {
    /// if WEP should be supported by the service instance.
    #[argh(switch)]
    pub wep_supported: bool,
    /// if legacy WPA1 should be supported by the service instance.
    #[argh(switch)]
    pub wpa1_supported: bool,
}

impl From<ServiceCfg> for wlan_sme::Config {
    fn from(cfg: ServiceCfg) -> Self {
        Self { wep_supported: cfg.wep_supported, wpa1_supported: cfg.wpa1_supported }
    }
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    // Initialize logging with a tag that can be used to select these logs for forwarding to console
    syslog::init_with_tags(&["wlan"]).expect("Syslog init should not fail");

    info!("Starting");
    let cfg: ServiceCfg = argh::from_env();
    info!("{:?}", cfg);

    let mut fs = ServiceFs::new_local();
    let inspector = Inspector::new_with_size(inspect::VMO_SIZE_BYTES);
    inspect_runtime::serve(&inspector, &mut fs)?;

    let persistence_proxy = fuchsia_component::client::connect_to_protocol_at_path::<
        fidl_fuchsia_diagnostics_persist::DataPersistenceMarker,
    >(PERSISTENCE_SERVICE_PATH)
    .context("failed to connect to persistence service")?;
    let (persistence_req_sender, persistence_req_forwarder_fut) =
        auto_persist::create_persistence_req_sender(persistence_proxy);

    let inspect_tree =
        Arc::new(inspect::WlanstackTree::new(inspector, persistence_req_sender.clone()));
    fs.dir("svc").add_fidl_service(IncomingServices::Device);

    let ifaces = IfaceMap::new();
    let ifaces = Arc::new(ifaces);

    let dev_monitor = fuchsia_component::client::connect_to_protocol::<
        fidl_fuchsia_wlan_device_service::DeviceMonitorMarker,
    >()
    .context("Failed to connect to DeviceMonitor.")?;
    let serve_fidl_fut =
        serve_fidl(cfg, fs, ifaces, inspect_tree, dev_monitor, persistence_req_sender);

    let ((), ()) = try_join(serve_fidl_fut, persistence_req_forwarder_fut.map(Ok)).await?;
    info!("Exiting");
    Ok(())
}

enum IncomingServices {
    Device(DeviceServiceRequestStream),
}

async fn serve_fidl(
    cfg: ServiceCfg,
    mut fs: ServiceFs<ServiceObjLocal<'_, IncomingServices>>,
    ifaces: Arc<IfaceMap>,
    inspect_tree: Arc<inspect::WlanstackTree>,
    dev_monitor_proxy: fidl_fuchsia_wlan_device_service::DeviceMonitorProxy,
    persistence_req_sender: auto_persist::PersistenceReqSender,
) -> Result<(), Error> {
    fs.take_and_serve_directory_handle()?;
    let iface_counter = Arc::new(service::IfaceCounter::new());

    let fdio_server = fs.for_each_concurrent(CONCURRENT_LIMIT, move |s| {
        let ifaces = ifaces.clone();
        let cfg = cfg.clone();
        let inspect_tree = inspect_tree.clone();
        let iface_counter = iface_counter.clone();
        let dev_monitor_proxy = dev_monitor_proxy.clone();
        let persistence_req_sender = persistence_req_sender.clone();
        async move {
            match s {
                IncomingServices::Device(stream) => {
                    service::serve_device_requests(
                        iface_counter,
                        cfg,
                        ifaces,
                        stream,
                        inspect_tree,
                        dev_monitor_proxy,
                        persistence_req_sender,
                    )
                    .unwrap_or_else(|e| println!("{:?}", e))
                    .await
                }
            }
        }
    });
    fdio_server.await;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn parse_svc_cfg_wep() {
        let cfg = ServiceCfg::from_args(&["bin/app"], &["--wep-supported"]).unwrap();
        assert!(cfg.wep_supported);
    }

    #[test]
    fn parse_svc_cfg_default() {
        let cfg = ServiceCfg::from_args(&["bin/app"], &[]).unwrap();
        assert!(!cfg.wep_supported);
    }

    #[test]
    fn svc_to_sme_cfg() {
        let svc_cfg = ServiceCfg::from_args(&["bin/app"], &[]).unwrap();
        let sme_cfg: wlan_sme::Config = svc_cfg.into();
        assert!(!sme_cfg.wep_supported);

        let svc_cfg = ServiceCfg::from_args(&["bin/app"], &["--wep-supported"]).unwrap();
        let sme_cfg: wlan_sme::Config = svc_cfg.into();
        assert!(sme_cfg.wep_supported);
    }
}
