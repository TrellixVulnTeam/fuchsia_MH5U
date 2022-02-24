// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{test_utils, wlancfg_helper},
    fidl_fuchsia_wlan_device_service::{DeviceServiceProxy, QueryIfaceResponse},
    fidl_fuchsia_wlan_sme::{ApSmeProxy, ClientSmeProxy},
    fidl_fuchsia_wlan_tap::WlantapPhyConfig,
    fuchsia_zircon::{sys::ZX_OK, DurationNum},
    log::info,
};

pub struct CreateDeviceHelper<'a> {
    wlanstack_svc: &'a DeviceServiceProxy,
    iface_ids: Vec<u16>,
}

impl<'a> CreateDeviceHelper<'a> {
    pub fn new(wlanstack_svc: &'a DeviceServiceProxy) -> CreateDeviceHelper<'a> {
        return CreateDeviceHelper { wlanstack_svc, iface_ids: vec![] };
    }

    pub async fn create_device(
        &mut self,
        config: WlantapPhyConfig,
        network_config: Option<wlancfg_helper::NetworkConfigBuilder>,
    ) -> Result<(test_utils::TestHelper, u16), anyhow::Error> {
        let helper = match network_config {
            Some(network_config) => {
                test_utils::TestHelper::begin_ap_test(config, network_config).await
            }
            None => test_utils::TestHelper::begin_test(config).await,
        };

        let iface_id = get_first_matching_iface_id(self.wlanstack_svc, |iface| {
            !self.iface_ids.contains(&iface.id)
        })
        .await;
        self.iface_ids.push(iface_id);

        Ok((helper, iface_id))
    }
}

/// Queries wlanstack service and return the first iface id that makes |filter(iface)| true.
/// Panics after timeout expires.
pub async fn get_first_matching_iface_id<F: Fn(&QueryIfaceResponse) -> bool>(
    svc: &DeviceServiceProxy,
    filter: F,
) -> u16 {
    // Sleep between queries to make main future yield.
    let mut infinite_timeout =
        super::test_utils::RetryWithBackoff::infinite_with_max_interval(10.seconds());
    // Verbose logging of DeviceServiceProxy calls inserted to assist debugging
    // flakes such as https://fxbug.dev/85468.
    let mut attempt = 1;
    loop {
        info!("Calling list_ifaces(): attempt {}", attempt);
        let ifaces = svc.list_ifaces().await.expect("getting iface list").ifaces;
        {
            for iface in ifaces {
                info!("Calling query_iface({})", iface.iface_id);
                let (status, resp) =
                    svc.query_iface(iface.iface_id).await.expect("querying iface info");
                assert_eq!(status, ZX_OK, "query_iface {} failed: {}", iface.iface_id, status);
                if filter(&resp.unwrap()) {
                    return iface.iface_id;
                }
            }
        }
        info!("Failed to find a suitable iface.");
        // unwrap() will never fail since there is an infinite deadline.
        infinite_timeout.sleep_unless_after_deadline_verbose().await.unwrap();
        attempt += 1;
    }
}

/// Wrapper function to get an ApSmeProxy from wlanstack with an |iface_id| assumed to be valid.
pub async fn get_ap_sme(wlan_service: &DeviceServiceProxy, iface_id: u16) -> ApSmeProxy {
    let (proxy, remote) = fidl::endpoints::create_proxy().expect("fail to create fidl endpoints");
    let status = wlan_service.get_ap_sme(iface_id, remote).await.expect("fail get_ap_sme");
    assert_eq!(status, ZX_OK, "fail getting ap sme status: {}", status);
    proxy
}

/// Wrapper function to get a ClientSmeProxy from wlanstack with an |iface_id| assumed to be valid.
pub async fn get_client_sme(wlan_service: &DeviceServiceProxy, iface_id: u16) -> ClientSmeProxy {
    let (proxy, remote) = fidl::endpoints::create_proxy().expect("fail to create fidl endpoints");
    let status = wlan_service.get_client_sme(iface_id, remote).await.expect("fail get_client_sme");
    assert_eq!(status, ZX_OK, "fail getting client sme status: {}", status);
    proxy
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        fidl_fuchsia_wlan_common::WlanMacRole::*,
        fidl_fuchsia_wlan_device_service::{
            DeviceServiceMarker, DeviceServiceRequest::*, IfaceListItem, ListIfacesResponse,
            QueryIfaceResponse,
        },
        futures::{pin_mut, StreamExt},
        std::task::Poll,
        wlan_common::assert_variant,
    };

    fn fake_iface_item(iface_id: u16) -> IfaceListItem {
        IfaceListItem { iface_id }
    }

    fn fake_query_iface_response() -> QueryIfaceResponse {
        QueryIfaceResponse {
            role: Client,
            id: 0,
            phy_id: 0,
            phy_assigned_id: 0,
            sta_addr: [0; 6],
            driver_features: Vec::new(),
        }
    }

    fn test_matching_iface_id<F: Fn(&QueryIfaceResponse) -> bool>(
        filter: F,
        mut list_response: ListIfacesResponse,
        query_responses: Vec<QueryIfaceResponse>,
        expected_id: Option<u16>,
    ) {
        let mut exec = fuchsia_async::TestExecutor::new().expect("creating executor");
        let (proxy, remote) =
            fidl::endpoints::create_proxy::<DeviceServiceMarker>().expect("creating proxy");
        let mut request_stream = remote.into_stream().expect("getting request stream");

        let iface_id_fut = get_first_matching_iface_id(&proxy, filter);
        pin_mut!(iface_id_fut);

        // This line advances get_first_matching_iface_id to the point where it calls list_ifaces()
        // and waits for a response.
        assert_variant!(exec.run_until_stalled(&mut iface_id_fut), Poll::Pending);
        // The fake server receives the call as a request.
        let responder = assert_variant!(exec.run_singlethreaded(&mut request_stream.next()),
                                         Some(Ok(ListIfaces{responder})) => responder);
        // The fake response is sent.
        responder.send(&mut list_response).expect("sending list ifaces response");

        for mut query_resp in query_responses {
            // This line advances the future to the point where it calls query_iface(id) and waits
            // for a response.
            assert_variant!(exec.run_until_stalled(&mut iface_id_fut), Poll::Pending);
            // The fake server receives the call as a request.
            let (id, responder) = assert_variant!(
                exec.run_singlethreaded(&mut request_stream.next()),
                Some(Ok(QueryIface{iface_id, responder})) => (iface_id, responder));
            assert_eq!(id, query_resp.id);
            // The fake response is sent.
            responder.send(ZX_OK, Some(&mut query_resp)).expect("sending query iface response");
        }

        match expected_id {
            Some(id) => {
                let got_id = assert_variant!(exec.run_until_stalled(&mut iface_id_fut),
                                            Poll::Ready(id) => id);
                assert_eq!(got_id, id);
            }
            None => assert_variant!(exec.run_until_stalled(&mut iface_id_fut), Poll::Pending),
        }
    }

    #[test]
    fn no_iface() {
        test_matching_iface_id(|_iface| true, ListIfacesResponse { ifaces: vec![] }, vec![], None);
    }

    #[test]
    fn found_ap_iface() {
        test_matching_iface_id(
            |iface| iface.role == Ap,
            ListIfacesResponse { ifaces: vec![fake_iface_item(0), fake_iface_item(3)] },
            vec![
                fake_query_iface_response(),
                QueryIfaceResponse { role: Ap, id: 3, ..fake_query_iface_response() },
            ],
            Some(3),
        );
    }

    #[test]
    fn ifaces_exist_but_no_match() {
        test_matching_iface_id(
            |iface| iface.role == Client,
            ListIfacesResponse { ifaces: vec![fake_iface_item(0)] },
            vec![QueryIfaceResponse { role: Ap, ..fake_query_iface_response() }],
            None,
        )
    }
}
