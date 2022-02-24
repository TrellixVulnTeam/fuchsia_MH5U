// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_wlan_common::WlanMacRole::Client,
    fidl_fuchsia_wlan_device_service::{
        CreateIfaceRequest, DeviceMonitorMarker, DeviceServiceMarker,
    },
    fuchsia_component::client::connect_to_protocol,
    fuchsia_zircon::sys::ZX_OK,
    fuchsia_zircon::DurationNum,
    ieee80211::NULL_MAC_ADDR,
    wlan_common::test_utils::ExpectWithin,
    wlan_hw_sim::*,
};

/// Spawn one wlantap PHY, which creates a first MAC interface by the order of wlancfg. And then
/// manually create another MAC interface via a new channel to wlanstack.
/// Verify both interfaces are created successfully.
#[fuchsia_async::run_singlethreaded(test)]
async fn multiple_interfaces_per_phy() {
    init_syslog();

    let client_helper = test_utils::TestHelper::begin_test(default_wlantap_config_client()).await;

    let wlanstack_svc =
        connect_to_protocol::<DeviceServiceMarker>().expect("connecting to wlanstack");
    let wlan_monitor_svc =
        connect_to_protocol::<DeviceMonitorMarker>().expect("connecting to wlandevicemonitor");
    let first_iface_id = get_first_matching_iface_id(&wlanstack_svc, |_iface| true)
        .expect_within(5.seconds(), "getting first iface")
        .await;
    let resp = wlanstack_svc.list_ifaces().await.expect("listing ifaces #1");
    assert_eq!(resp.ifaces.len(), 1);

    let resp = wlan_monitor_svc.list_phys().await.expect("listing phys");
    assert_eq!(resp.len(), 1);

    let phy_id = resp[0];
    let mut req = CreateIfaceRequest { phy_id, role: Client, sta_addr: NULL_MAC_ADDR };
    let (status, resp) =
        wlan_monitor_svc.create_iface(&mut req).await.expect("creating a new iface");
    assert_eq!(status, ZX_OK);

    let new_iface_id = resp.unwrap().iface_id;
    assert_ne!(new_iface_id, first_iface_id);

    let second_iface_id =
        get_first_matching_iface_id(&wlanstack_svc, |iface| iface.id != first_iface_id)
            .expect_within(5.seconds(), "getting next client iface")
            .await;
    assert_eq!(new_iface_id, second_iface_id);

    let resp = wlanstack_svc.list_ifaces().await.expect("listing ifaces #2");
    assert_eq!(resp.ifaces.len(), 2);
    client_helper.stop().await;
}
