// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Manages Scan requests for the Client Policy API.
use {
    crate::{
        client::types,
        config_management::{
            select_subset_potentially_hidden_networks, SavedNetworksManagerApi, ScanResultType,
        },
        mode_management::iface_manager_api::IfaceManagerApi,
    },
    anyhow::{format_err, Error},
    async_trait::async_trait,
    fidl::prelude::*,
    fidl_fuchsia_location_sensor as fidl_location_sensor, fidl_fuchsia_wlan_policy as fidl_policy,
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_cobalt::CobaltSender,
    fuchsia_component::client::connect_to_protocol,
    fuchsia_zircon as zx,
    futures::{lock::Mutex, prelude::*},
    log::{debug, error, info, warn},
    measure_tape_for_scan_result::Measurable as _,
    std::{collections::HashMap, convert::TryFrom, sync::Arc},
    stream::FuturesUnordered,
    wlan_common,
    wlan_metrics_registry::{
        ActiveScanRequestedForNetworkSelectionMetricDimensionActiveScanSsidsRequested as ActiveScanSsidsRequested,
        ACTIVE_SCAN_REQUESTED_FOR_NETWORK_SELECTION_METRIC_ID,
    },
};

// TODO(fxbug.dev/80422): Remove this.
// Size of FIDL message header and FIDL error-wrapped vector header
const FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE: usize = 56;
// Delay between scanning retries when the firmware returns "ShouldWait" error code
const SCAN_RETRY_DELAY_MS: i64 = 100;

// Inidication of the scan caller, for use in logging caller specific metrics
pub enum ScanReason {
    ClientRequest,
    NetworkSelection,
}

#[derive(Debug, Clone, Eq, PartialEq, Ord, PartialOrd, Hash)]
struct SmeNetworkIdentifier {
    ssid: types::Ssid,
    protection: types::SecurityTypeDetailed,
}

/// Allows for consumption of updated scan results.
#[async_trait]
pub trait ScanResultUpdate: Sync + Send {
    async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>);
}

/// Requests a new SME scan and returns the results.
async fn sme_scan(
    sme_proxy: &fidl_sme::ClientSmeProxy,
    scan_request: fidl_sme::ScanRequest,
) -> Result<Vec<wlan_common::scan::ScanResult>, types::ScanError> {
    #[derive(PartialEq, Eq)]
    enum SmeScanError {
        ShouldRetryLater,
        Other,
    }
    async fn sme_scan_internal(
        sme_proxy: &fidl_sme::ClientSmeProxy,
        mut scan_request: fidl_sme::ScanRequest,
    ) -> Result<Vec<wlan_common::scan::ScanResult>, SmeScanError> {
        let (local, remote) = fidl::endpoints::create_proxy().map_err(|e| {
            error!("Failed to create FIDL proxy for scan: {:?}", e);
            SmeScanError::Other
        })?;
        let txn = {
            match sme_proxy.scan(&mut scan_request, remote) {
                Ok(()) => local,
                Err(error) => {
                    error!("Scan initiation error: {:?}", error);
                    return Err(SmeScanError::Other);
                }
            }
        };
        debug!("Sent scan request to SME successfully");
        let mut stream = txn.take_event_stream();
        let mut scanned_networks: Vec<wlan_common::scan::ScanResult> = vec![];
        while let Some(Ok(event)) = stream.next().await {
            match event {
                fidl_sme::ScanTransactionEvent::OnResult { aps } => {
                    debug!("Received scan results from SME");
                    scanned_networks.extend(
                        aps.iter()
                            .filter_map(|scan_result| {
                                wlan_common::scan::ScanResult::try_from(scan_result.clone())
                                    .map(|scan_result| Some(scan_result))
                                    .unwrap_or_else(|e| {
                                        // TODO(fxbug.dev/83708): Report details about which
                                        // scan result failed to convert if possible.
                                        error!("ScanResult conversion failed: {:?}", e);
                                        None
                                    })
                            })
                            .collect::<Vec<_>>(),
                    );
                }
                fidl_sme::ScanTransactionEvent::OnFinished {} => {
                    debug!("Finished getting scan results from SME");
                    return Ok(scanned_networks);
                }
                fidl_sme::ScanTransactionEvent::OnError { error } => {
                    return match error.code {
                        fidl_sme::ScanErrorCode::ShouldWait
                        | fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware => {
                            info!("Scan cancelled by SME, retry indicated: {:?}", error);
                            Err(SmeScanError::ShouldRetryLater)
                        }
                        _ => {
                            error!("Scan error from SME: {:?}", error);
                            Err(SmeScanError::Other)
                        }
                    }
                }
            };
        }
        // This case can happen when an interface is removed mid-scan. Any other cases are
        // unexpected.
        warn!("SME closed scan result channel without sending OnFinished");
        Err(SmeScanError::Other)
    }

    match sme_scan_internal(sme_proxy, scan_request.clone()).await {
        Ok(scan_results) => Ok(scan_results),
        // This can be in an .or_else() once async closures are supported in stable.
        // See issue #62290 <https://github.com/rust-lang/rust/issues/62290>.
        Err(SmeScanError::ShouldRetryLater) => {
            info!("Driver requested a delay before retrying scan");
            fasync::Timer::new(zx::Duration::from_millis(SCAN_RETRY_DELAY_MS).after_now()).await;
            // Retry once before returning an error.
            sme_scan_internal(sme_proxy, scan_request.clone()).await.or_else(|e| {
                if e == SmeScanError::ShouldRetryLater {
                    Err(types::ScanError::Cancelled)
                } else {
                    Err(types::ScanError::GeneralError)
                }
            })
        }
        Err(_) => Err(types::ScanError::GeneralError),
    }
}

/// Handles incoming scan requests by creating a new SME scan request.
/// For the output_iterator, returns scan results and/or errors.
/// On successful scan, also provides scan results to:
/// - Emergency Location Provider
/// - Network Selection Module
pub(crate) async fn perform_scan(
    iface_manager: Arc<Mutex<dyn IfaceManagerApi + Send>>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
    output_iterator: Option<fidl::endpoints::ServerEnd<fidl_policy::ScanResultIteratorMarker>>,
    mut network_selector: impl ScanResultUpdate,
    mut location_sensor_updater: impl ScanResultUpdate,
    scan_reason: ScanReason,
    // TODO(fxbug.dev/73821): This should be removed when ScanManager struct is implemented,
    // in favor of a field in the struct itself.
    cobalt_api: Option<Arc<Mutex<CobaltSender>>>,
) {
    let mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>> = HashMap::new();
    let mut non_fatal_scan_error_for_fidl_consumers = None;

    let sme_proxy = match iface_manager.lock().await.get_sme_proxy_for_scan().await {
        Ok(proxy) => proxy,
        Err(e) => {
            // The attempt to get an SME proxy failed. Send an error to the requester, return early.
            warn!("Failed to get an SME proxy for scan: {:?}", e);
            if let Some(output_iterator) = output_iterator {
                send_scan_results_over_fidl(
                    output_iterator,
                    &vec![],
                    Some(fidl_policy::ScanErrorCode::GeneralError),
                )
                .await
                .unwrap_or_else(|e| error!("Failed to send scan error: {}", e));
            }
            return;
        }
    };

    // Perform an initial passive scan
    let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
    let sme_result = sme_scan(&sme_proxy, scan_request).await;
    match sme_result {
        Ok(results) => {
            record_undirected_scan_results(&results, saved_networks_manager.clone()).await;
            insert_bss_to_network_bss_map(&mut bss_by_network, results, true);
        }
        Err(scan_err) => {
            // The passive scan failed. Send an error to the requester and return early.
            if let Some(output_iterator) = output_iterator {
                send_scan_results_over_fidl(output_iterator, &vec![], Some(scan_err))
                    .await
                    .unwrap_or_else(|e| error!("Failed to send scan error: {}", e));
            }
            return;
        }
    };

    let requested_active_scan_ids: Vec<types::NetworkIdentifier> =
        select_subset_potentially_hidden_networks(saved_networks_manager.get_networks().await);

    // Record active scan decisions to metrics. This is optional, based on
    // the scan reason and if the caller would like metrics logged.
    if let Some(cobalt_sender) = cobalt_api {
        match scan_reason {
            ScanReason::NetworkSelection => {
                let active_scan_request_count_metric = match requested_active_scan_ids.len() {
                    0 => ActiveScanSsidsRequested::Zero,
                    1 => ActiveScanSsidsRequested::One,
                    2..=4 => ActiveScanSsidsRequested::TwoToFour,
                    5..=10 => ActiveScanSsidsRequested::FiveToTen,
                    11..=20 => ActiveScanSsidsRequested::ElevenToTwenty,
                    21..=50 => ActiveScanSsidsRequested::TwentyOneToFifty,
                    51..=100 => ActiveScanSsidsRequested::FiftyOneToOneHundred,
                    101..=usize::MAX => ActiveScanSsidsRequested::OneHundredAndOneOrMore,
                    _ => unreachable!(),
                };
                let mut cobalt_sender_guard = cobalt_sender.lock().await;
                let cobalt_lock = &mut *cobalt_sender_guard;
                cobalt_lock.log_event(
                    ACTIVE_SCAN_REQUESTED_FOR_NETWORK_SELECTION_METRIC_ID,
                    active_scan_request_count_metric,
                );
                drop(cobalt_sender_guard);
            }
            _ => {}
        }
    }

    if requested_active_scan_ids.len() > 0 {
        let requested_active_scan_ssids =
            requested_active_scan_ids.iter().map(|id| id.ssid.to_vec()).collect();
        let scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: requested_active_scan_ssids,
            channels: vec![],
        });
        let sme_result = sme_scan(&sme_proxy, scan_request).await;
        match sme_result {
            Ok(results) => {
                record_directed_scan_results(
                    requested_active_scan_ids,
                    &results,
                    saved_networks_manager,
                )
                .await;
                insert_bss_to_network_bss_map(&mut bss_by_network, results, false);
            }
            Err(scan_err) => {
                non_fatal_scan_error_for_fidl_consumers = Some(scan_err);
                info!("Proceeding with passive scan results.");
            }
        }
    };

    let scan_results = network_bss_map_to_scan_result(bss_by_network);
    // TODO(b/182569380): use actual wpa3 support in this conversion rather than hardcoding 'false'
    let fidl_scan_results = scan_result_to_policy_scan_result(&scan_results, false);
    let mut scan_result_consumers = FuturesUnordered::new();

    // Send scan results to the location sensor
    scan_result_consumers.push(location_sensor_updater.update_scan_results(&scan_results));
    // Send scan results to the network selection module
    scan_result_consumers.push(network_selector.update_scan_results(&scan_results));
    // If the requester provided a channel, send the results to them
    if let Some(output_iterator) = output_iterator {
        let requester_fut = send_scan_results_over_fidl(
            output_iterator,
            &fidl_scan_results,
            non_fatal_scan_error_for_fidl_consumers,
        )
        .unwrap_or_else(|e| {
            error!("Failed to send scan results to requester: {:?}", e);
        });
        scan_result_consumers.push(Box::pin(requester_fut));
    }

    while let Some(_) = scan_result_consumers.next().await {}
}

/// Update the hidden network probabilties of saved networks seen in a
/// passive scan.
async fn record_undirected_scan_results(
    scan_results: &Vec<wlan_common::scan::ScanResult>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
) {
    let ids = scan_results
        .iter()
        .map(|result| types::NetworkIdentifierDetailed {
            ssid: result.bss_description.ssid.clone(),
            security_type: result.bss_description.protection().into(),
        })
        .collect();
    saved_networks_manager.record_scan_result(ScanResultType::Undirected, ids).await;
}

/// Perform a directed active scan for a given network on given channels.
pub(crate) async fn perform_directed_active_scan(
    sme_proxy: &fidl_sme::ClientSmeProxy,
    ssid: &types::Ssid,
    channels: Option<Vec<u8>>,
) -> Result<Vec<types::ScanResult>, types::ScanError> {
    let scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
        ssids: vec![ssid.to_vec()],
        channels: channels.unwrap_or(vec![]),
    });

    let sme_result = sme_scan(sme_proxy, scan_request).await;
    sme_result.map(|results| {
        let mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>> = HashMap::new();
        insert_bss_to_network_bss_map(&mut bss_by_network, results, false);

        // The active scan targets a specific SSID, ensure only that SSID is present in results
        bss_by_network.retain(|network_id, _| network_id.ssid == *ssid);

        network_bss_map_to_scan_result(bss_by_network)
    })
}

/// Figure out which saved networks we actively scanned for and did not get results for, and update
/// their configs to update the rate at which we would actively scan for these networks.
async fn record_directed_scan_results(
    target_ids: Vec<types::NetworkIdentifier>,
    scan_result_list: &Vec<wlan_common::scan::ScanResult>,
    saved_networks_manager: Arc<dyn SavedNetworksManagerApi>,
) {
    let ids = scan_result_list
        .iter()
        .map(|scan_result| types::NetworkIdentifierDetailed {
            ssid: scan_result.bss_description.ssid.clone(),
            security_type: scan_result.bss_description.protection().into(),
        })
        .collect();
    saved_networks_manager.record_scan_result(ScanResultType::Directed(target_ids), ids).await;
}

/// The location sensor module uses scan results to help determine the
/// device's location, for use by the Emergency Location Provider.
pub struct LocationSensorUpdater {
    pub wpa3_supported: bool,
}
#[async_trait]
impl ScanResultUpdate for LocationSensorUpdater {
    async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>) {
        async fn send_results(scan_results: &Vec<fidl_policy::ScanResult>) -> Result<(), Error> {
            // Get an output iterator
            let (iter, server) =
                fidl::endpoints::create_endpoints::<fidl_policy::ScanResultIteratorMarker>()
                    .map_err(|err| format_err!("failed to create iterator: {:?}", err))?;
            let location_watcher_proxy =
                connect_to_protocol::<fidl_location_sensor::WlanBaseStationWatcherMarker>()
                    .map_err(|err| {
                        format_err!("failed to connect to location sensor service: {:?}", err)
                    })?;
            location_watcher_proxy
                .report_current_stations(iter)
                .map_err(|err| format_err!("failed to call location sensor service: {:?}", err))?;

            // Send results to the iterator
            send_scan_results_over_fidl(server, &scan_results, None).await
        }

        let scan_results = scan_result_to_policy_scan_result(scan_results, self.wpa3_supported);
        // Filter out any errors and just log a message.
        // No error recovery, we'll just try again next time a scan result comes in.
        if let Err(e) = send_results(&scan_results).await {
            // TODO(fxbug.dev/52700) Upgrade this to a "warn!" once the location sensor works.
            debug!("Failed to send scan results to location sensor: {:?}", e)
        } else {
            debug!("Updated location sensor")
        };
    }
}

/// Converts sme::ScanResult to our internal BSS type, then adds it to the provided bss_by_network map.
/// Only keeps the first unique instance of a BSSID
fn insert_bss_to_network_bss_map(
    bss_by_network: &mut HashMap<SmeNetworkIdentifier, Vec<types::Bss>>,
    scan_result_list: Vec<wlan_common::scan::ScanResult>,
    observed_in_passive_scan: bool,
) {
    for scan_result in scan_result_list.into_iter() {
        let entry = bss_by_network
            .entry(SmeNetworkIdentifier {
                ssid: scan_result.bss_description.ssid.clone(),
                protection: scan_result.bss_description.protection().into(),
            })
            .or_insert(vec![]);
        // Check if this BSSID is already in the hashmap
        if !entry.iter().any(|existing_bss| existing_bss.bssid == scan_result.bss_description.bssid)
        {
            entry.push(types::Bss {
                bssid: scan_result.bss_description.bssid,
                rssi: scan_result.bss_description.rssi_dbm,
                snr_db: scan_result.bss_description.snr_db,
                channel: scan_result.bss_description.channel.into(),
                timestamp: scan_result.timestamp,
                observed_in_passive_scan,
                compatible: scan_result.compatible,
                bss_description: scan_result.bss_description.into(),
            });
        };
    }
}

fn network_bss_map_to_scan_result(
    mut bss_by_network: HashMap<SmeNetworkIdentifier, Vec<types::Bss>>,
) -> Vec<types::ScanResult> {
    let mut scan_results: Vec<types::ScanResult> = bss_by_network
        .drain()
        .map(|(SmeNetworkIdentifier { ssid, protection }, bss_entries)| {
            let compatibility = if bss_entries.iter().any(|bss| bss.compatible) {
                fidl_policy::Compatibility::Supported
            } else {
                fidl_policy::Compatibility::DisallowedNotSupported
            };
            types::ScanResult {
                ssid: ssid,
                security_type_detailed: protection,
                entries: bss_entries,
                compatibility: compatibility,
            }
        })
        .collect();

    scan_results.sort_by(|a, b| a.ssid.cmp(&b.ssid));
    return scan_results;
}

/// Convert the protection type we receive from the SME in scan results to the Policy layer
/// security type. This function should only be used when converting to results for the public
/// FIDL API, and not for internal use within Policy, where we should prefer the detailed SME
/// security types.
fn fidl_security_from_sme_protection(
    protection: fidl_sme::Protection,
    wpa3_supported: bool,
) -> Option<fidl_policy::SecurityType> {
    use fidl_policy::SecurityType;
    use fidl_sme::Protection::*;
    match protection {
        Wpa3Enterprise | Wpa3Personal | Wpa2Wpa3Personal => {
            Some(if wpa3_supported { SecurityType::Wpa3 } else { SecurityType::Wpa2 })
        }
        Wpa2Enterprise
        | Wpa2Personal
        | Wpa1Wpa2Personal
        | Wpa2PersonalTkipOnly
        | Wpa1Wpa2PersonalTkipOnly => Some(SecurityType::Wpa2),
        Wpa1 => Some(SecurityType::Wpa),
        Wep => Some(SecurityType::Wep),
        Open => Some(SecurityType::None),
        Unknown => None,
    }
}

fn scan_result_to_policy_scan_result(
    internal_results: &Vec<types::ScanResult>,
    wpa3_supported: bool,
) -> Vec<fidl_policy::ScanResult> {
    let scan_results: Vec<fidl_policy::ScanResult> = internal_results
        .iter()
        .filter_map(|internal| {
            if let Some(security) =
                fidl_security_from_sme_protection(internal.security_type_detailed, wpa3_supported)
            {
                Some(fidl_policy::ScanResult {
                    id: Some(fidl_policy::NetworkIdentifier {
                        ssid: internal.ssid.to_vec(),
                        type_: security,
                    }),
                    entries: Some(
                        internal
                            .entries
                            .iter()
                            .map(|input| {
                                // Get the frequency. On error, default to Some(0) rather than None
                                // to protect against consumer code that expects this field to
                                // always be set.
                                let frequency = types::WlanChan::from(input.channel)
                                    .get_center_freq()
                                    .unwrap_or(0);
                                fidl_policy::Bss {
                                    bssid: Some(input.bssid.0),
                                    rssi: Some(input.rssi),
                                    frequency: Some(frequency.into()), // u16.into() -> u32
                                    timestamp_nanos: Some(input.timestamp.into_nanos()),
                                    ..fidl_policy::Bss::EMPTY
                                }
                            })
                            .collect(),
                    ),
                    compatibility: if internal.entries.iter().any(|bss| bss.compatible) {
                        Some(fidl_policy::Compatibility::Supported)
                    } else {
                        Some(fidl_policy::Compatibility::DisallowedNotSupported)
                    },
                    ..fidl_policy::ScanResult::EMPTY
                })
            } else {
                debug!(
                    "Unknown security type present in scan results: {:?}",
                    internal.security_type_detailed
                );
                None
            }
        })
        .collect();

    return scan_results;
}

/// Send batches of results to the output iterator when getNext() is called on it.
/// Send empty batch and close the channel when no results are remaining.
async fn send_scan_results_over_fidl(
    output_iterator: fidl::endpoints::ServerEnd<fidl_policy::ScanResultIteratorMarker>,
    scan_results: &Vec<fidl_policy::ScanResult>,
    error_code: Option<fidl_policy::ScanErrorCode>,
) -> Result<(), Error> {
    // Wait to get a request for a chunk of scan results
    let (mut stream, ctrl) = output_iterator.into_stream_and_control_handle()?;
    let max_batch_size = zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize;
    let mut sent_some_results = false;
    let mut remaining_results = scan_results.iter().peekable();
    let mut batch_size: usize;

    // Verify consumer is expecting results before each batch
    loop {
        if let Some(fidl_policy::ScanResultIteratorRequest::GetNext { responder }) =
            stream.try_next().await?
        {
            sent_some_results = true;
            let mut batch = vec![];
            batch_size = FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE;
            // Peek if next element exists and will fit in current batch
            while let Some(peeked_result) = remaining_results.peek() {
                let result_size = peeked_result.measure().num_bytes;
                if result_size + FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE > max_batch_size {
                    return Err(format_err!("Single scan result too large to send via FIDL"));
                }
                // Peeked result will not fit. Send batch and continue.
                if result_size + batch_size > max_batch_size {
                    break;
                }
                // Actually remove result and push to batch
                if let Some(result) = remaining_results.next() {
                    batch.push(result.clone());
                    batch_size += result_size;
                }
            }

            if batch.is_empty() {
                // No more results to send
                if let Some(error_code) = error_code {
                    // Finish with the error
                    let mut err: fidl_policy::ScanResultIteratorGetNextResult = Err(error_code);
                    responder.send(&mut err)?;
                } else {
                    // Finish with the empty vec, indicating no more results
                    responder.send(&mut Ok(batch))?;
                }
                ctrl.shutdown();
                return Ok(());
            } else {
                responder.send(&mut Ok(batch))?;
            }
        } else {
            // This will happen if the iterator request stream was closed and we expected to send
            // another response.
            if sent_some_results {
                // Some consumers may not care about all scan results, e.g. if they find the
                // particular network they were looking for. This is not an error.
                debug!("Scan result consumer closed channel before consuming all scan results");
                return Ok(());
            } else {
                return Err(format_err!("Peer closed channel before receiving any scan results"));
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*,
        crate::{
            access_point::state_machine as ap_fsm,
            config_management::network_config::Credential,
            util::testing::{
                fakes::FakeSavedNetworksManager, generate_random_sme_scan_result,
                validate_sme_scan_request_and_send_results,
            },
        },
        anyhow::Error,
        fidl::endpoints::{create_proxy, Proxy},
        fuchsia_async as fasync, fuchsia_zircon as zx,
        futures::{channel::oneshot, lock::Mutex, task::Poll},
        pin_utils::pin_mut,
        std::{convert::TryInto, sync::Arc},
        test_case::test_case,
        wlan_common::{assert_variant, random_fidl_bss_description},
    };

    const CENTER_FREQ_CHAN_1: u32 = 2412;
    const CENTER_FREQ_CHAN_8: u32 = 2447;
    const CENTER_FREQ_CHAN_11: u32 = 2462;

    struct FakeIfaceManager {
        pub sme_proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy,
        pub wpa3_capable: bool,
    }

    impl FakeIfaceManager {
        pub fn new(proxy: fidl_fuchsia_wlan_sme::ClientSmeProxy) -> Self {
            FakeIfaceManager { sme_proxy: proxy, wpa3_capable: true }
        }
    }

    #[async_trait]
    impl IfaceManagerApi for FakeIfaceManager {
        async fn disconnect(
            &mut self,
            _network_id: types::NetworkIdentifier,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn connect(
            &mut self,
            _connect_req: types::ConnectRequest,
        ) -> Result<oneshot::Receiver<()>, Error> {
            unimplemented!()
        }

        async fn record_idle_client(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn has_idle_client(&mut self) -> Result<bool, Error> {
            unimplemented!()
        }

        async fn handle_added_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn handle_removed_iface(&mut self, _iface_id: u16) -> Result<(), Error> {
            unimplemented!()
        }

        async fn scan(
            &mut self,
            mut scan_request: fidl_sme::ScanRequest,
        ) -> Result<fidl_fuchsia_wlan_sme::ScanTransactionProxy, Error> {
            let (local, remote) = fidl::endpoints::create_proxy()?;
            let _ = self.sme_proxy.scan(&mut scan_request, remote);
            Ok(local)
        }

        async fn get_sme_proxy_for_scan(
            &mut self,
        ) -> Result<fidl_fuchsia_wlan_sme::ClientSmeProxy, Error> {
            Ok(self.sme_proxy.clone())
        }

        async fn stop_client_connections(
            &mut self,
            _reason: types::DisconnectReason,
        ) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_client_connections(&mut self) -> Result<(), Error> {
            unimplemented!()
        }

        async fn start_ap(
            &mut self,
            _config: ap_fsm::ApConfig,
        ) -> Result<oneshot::Receiver<()>, Error> {
            unimplemented!()
        }

        async fn stop_ap(&mut self, _ssid: types::Ssid, _password: Vec<u8>) -> Result<(), Error> {
            unimplemented!()
        }

        async fn stop_all_aps(&mut self) -> Result<(), Error> {
            unimplemented!()
        }

        async fn has_wpa3_capable_client(&mut self) -> Result<bool, Error> {
            Ok(self.wpa3_capable)
        }

        async fn set_country(
            &mut self,
            _country_code: Option<[u8; types::REGION_CODE_LEN]>,
        ) -> Result<(), Error> {
            unimplemented!()
        }
    }

    /// Creates a Client wrapper.
    async fn create_iface_manager(
    ) -> (Arc<Mutex<FakeIfaceManager>>, fidl_sme::ClientSmeRequestStream) {
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        let iface_manager = Arc::new(Mutex::new(FakeIfaceManager::new(client_sme)));
        (iface_manager, remote.into_stream().expect("failed to create stream"))
    }

    /// Creates an SME proxy for tests.
    async fn create_sme_proxy() -> (fidl_sme::ClientSmeProxy, fidl_sme::ClientSmeRequestStream) {
        let (client_sme, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        (client_sme, remote.into_stream().expect("failed to create stream"))
    }

    struct MockScanResultConsumer {
        scan_results: Arc<Mutex<Option<Vec<types::ScanResult>>>>,
    }
    impl MockScanResultConsumer {
        fn new() -> (Self, Arc<Mutex<Option<Vec<types::ScanResult>>>>) {
            let scan_results = Arc::new(Mutex::new(None));
            (Self { scan_results: Arc::clone(&scan_results) }, scan_results)
        }
    }
    #[async_trait]
    impl ScanResultUpdate for MockScanResultConsumer {
        async fn update_scan_results(&mut self, scan_results: &Vec<types::ScanResult>) {
            let mut guard = self.scan_results.lock().await;
            *guard = Some(scan_results.clone());
        }
    }

    // Creates test data for the scan functions.
    struct MockScanData {
        passive_input_aps: Vec<fidl_sme::ScanResult>,
        passive_internal_aps: Vec<types::ScanResult>,
        passive_fidl_aps: Vec<fidl_policy::ScanResult>,
        active_input_aps: Vec<fidl_sme::ScanResult>,
        combined_internal_aps: Vec<types::ScanResult>,
        combined_fidl_aps: Vec<fidl_policy::ScanResult>,
    }
    fn create_scan_ap_data() -> MockScanData {
        let passive_result_1 = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3Enterprise,
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 0,
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            ),
        };
        let passive_result_2 = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa2,
                bssid: [1, 2, 3, 4, 5, 6],
                ssid: types::Ssid::try_from("unique ssid").unwrap(),
                rssi_dbm: 7,
                snr_db: 2,
                channel: types::WlanChan::new(8, types::Cbw::Cbw20),
            ),
        };
        let passive_result_3 = fidl_sme::ScanResult {
            compatible: false,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3Enterprise,
                bssid: [7, 8, 9, 10, 11, 12],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 13,
                snr_db: 3,
                channel: types::WlanChan::new(11, types::Cbw::Cbw20),
            ),
        };

        let passive_input_aps =
            vec![passive_result_1.clone(), passive_result_2.clone(), passive_result_3.clone()];
        // input_aps contains some duplicate SSIDs, which should be
        // grouped in the output.
        let passive_internal_aps = vec![
            types::ScanResult {
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa3Enterprise,
                entries: vec![
                    types::Bss {
                        bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                        rssi: 0,
                        timestamp: zx::Time::from_nanos(passive_result_1.timestamp_nanos),
                        snr_db: 1,
                        channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                        observed_in_passive_scan: true,
                        compatible: true,
                        bss_description: passive_result_1.bss_description.clone(),
                    },
                    types::Bss {
                        bssid: types::Bssid([7, 8, 9, 10, 11, 12]),
                        rssi: 13,
                        timestamp: zx::Time::from_nanos(passive_result_3.timestamp_nanos),
                        snr_db: 3,
                        channel: types::WlanChan::new(11, types::Cbw::Cbw20),
                        observed_in_passive_scan: true,
                        compatible: false,
                        bss_description: passive_result_3.bss_description.clone(),
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                ssid: types::Ssid::try_from("unique ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa2Personal,
                entries: vec![types::Bss {
                    bssid: types::Bssid([1, 2, 3, 4, 5, 6]),
                    rssi: 7,
                    timestamp: zx::Time::from_nanos(passive_result_2.timestamp_nanos),
                    snr_db: 2,
                    channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    observed_in_passive_scan: true,
                    compatible: true,
                    bss_description: passive_result_2.bss_description.clone(),
                }],
                compatibility: types::Compatibility::Supported,
            },
        ];
        let passive_fidl_aps = vec![
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![
                    fidl_policy::Bss {
                        bssid: Some([0, 0, 0, 0, 0, 0]),
                        rssi: Some(0),
                        frequency: Some(CENTER_FREQ_CHAN_1),
                        timestamp_nanos: Some(passive_result_1.timestamp_nanos),
                        ..fidl_policy::Bss::EMPTY
                    },
                    fidl_policy::Bss {
                        bssid: Some([7, 8, 9, 10, 11, 12]),
                        rssi: Some(13),
                        frequency: Some(CENTER_FREQ_CHAN_11),
                        timestamp_nanos: Some(passive_result_3.timestamp_nanos),
                        ..fidl_policy::Bss::EMPTY
                    },
                ]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("unique ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![fidl_policy::Bss {
                    bssid: Some([1, 2, 3, 4, 5, 6]),
                    rssi: Some(7),
                    frequency: Some(CENTER_FREQ_CHAN_8),
                    timestamp_nanos: Some(passive_result_2.timestamp_nanos),
                    ..fidl_policy::Bss::EMPTY
                }]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
        ];

        let active_result_1 = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3Enterprise,
                bssid: [9, 9, 9, 9, 9, 9],
                ssid: types::Ssid::try_from("foo active ssid").unwrap(),
                rssi_dbm: 0,
                snr_db: 8,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            ),
        };
        let active_result_2 = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa2,
                bssid: [8, 8, 8, 8, 8, 8],
                ssid: types::Ssid::try_from("misc ssid").unwrap(),
                rssi_dbm: 7,
                snr_db: 9,
                channel: types::WlanChan::new(8, types::Cbw::Cbw20),
            ),
        };
        let active_input_aps = vec![active_result_1.clone(), active_result_2.clone()];
        let combined_internal_aps = vec![
            types::ScanResult {
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa3Enterprise,
                entries: vec![
                    types::Bss {
                        bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                        rssi: 0,
                        timestamp: zx::Time::from_nanos(passive_result_1.timestamp_nanos),
                        snr_db: 1,
                        channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                        observed_in_passive_scan: true,
                        compatible: true,
                        bss_description: passive_result_1.bss_description.clone(),
                    },
                    types::Bss {
                        bssid: types::Bssid([7, 8, 9, 10, 11, 12]),
                        rssi: 13,
                        timestamp: zx::Time::from_nanos(passive_result_3.timestamp_nanos),
                        snr_db: 3,
                        channel: types::WlanChan::new(11, types::Cbw::Cbw20),
                        observed_in_passive_scan: true,
                        compatible: false,
                        bss_description: passive_result_3.bss_description.clone(),
                    },
                ],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                ssid: types::Ssid::try_from("foo active ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa3Enterprise,
                entries: vec![types::Bss {
                    bssid: types::Bssid([9, 9, 9, 9, 9, 9]),
                    rssi: 0,
                    timestamp: zx::Time::from_nanos(active_result_1.timestamp_nanos),
                    snr_db: 8,
                    channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                    observed_in_passive_scan: false,
                    compatible: true,
                    bss_description: active_result_1.bss_description.clone(),
                }],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                ssid: types::Ssid::try_from("misc ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa2Personal,
                entries: vec![types::Bss {
                    bssid: types::Bssid([8, 8, 8, 8, 8, 8]),
                    rssi: 7,
                    timestamp: zx::Time::from_nanos(active_result_2.timestamp_nanos),
                    snr_db: 9,
                    channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    observed_in_passive_scan: false,
                    compatible: true,
                    bss_description: active_result_2.bss_description.clone(),
                }],
                compatibility: types::Compatibility::Supported,
            },
            types::ScanResult {
                ssid: types::Ssid::try_from("unique ssid").unwrap(),
                security_type_detailed: types::SecurityTypeDetailed::Wpa2Personal,
                entries: vec![types::Bss {
                    bssid: types::Bssid([1, 2, 3, 4, 5, 6]),
                    rssi: 7,
                    timestamp: zx::Time::from_nanos(passive_result_2.timestamp_nanos),
                    snr_db: 2,
                    channel: types::WlanChan::new(8, types::Cbw::Cbw20),
                    observed_in_passive_scan: true,
                    compatible: true,
                    bss_description: passive_result_2.bss_description.clone(),
                }],
                compatibility: types::Compatibility::Supported,
            },
        ];
        let combined_fidl_aps = vec![
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![
                    fidl_policy::Bss {
                        bssid: Some([0, 0, 0, 0, 0, 0]),
                        rssi: Some(0),
                        frequency: Some(CENTER_FREQ_CHAN_1),
                        timestamp_nanos: Some(passive_result_1.timestamp_nanos),
                        ..fidl_policy::Bss::EMPTY
                    },
                    fidl_policy::Bss {
                        bssid: Some([7, 8, 9, 10, 11, 12]),
                        rssi: Some(13),
                        frequency: Some(CENTER_FREQ_CHAN_11),
                        timestamp_nanos: Some(passive_result_3.timestamp_nanos),
                        ..fidl_policy::Bss::EMPTY
                    },
                ]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("foo active ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![fidl_policy::Bss {
                    bssid: Some([9, 9, 9, 9, 9, 9]),
                    rssi: Some(0),
                    frequency: Some(CENTER_FREQ_CHAN_1),
                    timestamp_nanos: Some(active_result_1.timestamp_nanos),
                    ..fidl_policy::Bss::EMPTY
                }]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("misc ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![fidl_policy::Bss {
                    bssid: Some([8, 8, 8, 8, 8, 8]),
                    rssi: Some(7),
                    frequency: Some(CENTER_FREQ_CHAN_8),
                    timestamp_nanos: Some(active_result_2.timestamp_nanos),
                    ..fidl_policy::Bss::EMPTY
                }]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
            fidl_policy::ScanResult {
                id: Some(fidl_policy::NetworkIdentifier {
                    ssid: types::Ssid::try_from("unique ssid").unwrap().into(),
                    type_: fidl_policy::SecurityType::Wpa2,
                }),
                entries: Some(vec![fidl_policy::Bss {
                    bssid: Some([1, 2, 3, 4, 5, 6]),
                    rssi: Some(7),
                    frequency: Some(CENTER_FREQ_CHAN_8),
                    timestamp_nanos: Some(passive_result_2.timestamp_nanos),
                    ..fidl_policy::Bss::EMPTY
                }]),
                compatibility: Some(fidl_policy::Compatibility::Supported),
                ..fidl_policy::ScanResult::EMPTY
            },
        ];

        MockScanData {
            passive_input_aps,
            passive_internal_aps,
            passive_fidl_aps,
            active_input_aps,
            combined_internal_aps,
            combined_fidl_aps,
        }
    }

    /// Generate a vector of FIDL scan results, each sized based on the input
    /// vector parameter. Size, in bytes, must be greater than the baseline scan
    /// result's size, measure below, and divisible into octets (by 8).
    fn create_fidl_scan_results_from_size(
        result_sizes: Vec<usize>,
    ) -> Vec<fidl_policy::ScanResult> {
        // Create a baseline result
        let minimal_scan_result = fidl_policy::ScanResult {
            id: Some(fidl_policy::NetworkIdentifier {
                ssid: types::Ssid::empty().into(),
                type_: fidl_policy::SecurityType::None,
            }),
            entries: Some(vec![]),
            ..fidl_policy::ScanResult::EMPTY
        };
        let minimal_result_size: usize = minimal_scan_result.measure().num_bytes;

        // Create result with single entry
        let mut scan_result_with_one_bss = minimal_scan_result.clone();
        scan_result_with_one_bss.entries = Some(vec![fidl_policy::Bss::EMPTY]);

        // Size of each additional BSS entry to FIDL ScanResult
        let empty_bss_entry_size: usize =
            scan_result_with_one_bss.measure().num_bytes - minimal_result_size;

        // Validate size is possible
        if result_sizes.iter().any(|size| size < &minimal_result_size || size % 8 != 0) {
            panic!("Invalid size. Requested size must be larger than {} minimum bytes and divisible into octets (by 8)", minimal_result_size);
        }

        let mut fidl_scan_results = vec![];
        for size in result_sizes {
            let mut scan_result = minimal_scan_result.clone();

            let num_bss_for_ap = (size - minimal_result_size) / empty_bss_entry_size;
            // Every 8 characters for SSID adds 8 bytes (1 octet).
            let ssid_length =
                (size - minimal_result_size) - (num_bss_for_ap * empty_bss_entry_size);

            scan_result.id = Some(fidl_policy::NetworkIdentifier {
                ssid: (0..ssid_length).map(|_| rand::random::<u8>()).collect(),
                type_: fidl_policy::SecurityType::None,
            });
            scan_result.entries = Some(vec![fidl_policy::Bss::EMPTY; num_bss_for_ap]);

            // Validate result measures to expected size.
            assert_eq!(scan_result.measure().num_bytes, size);

            fidl_scan_results.push(scan_result);
        }
        fidl_scan_results
    }

    #[fuchsia::test]
    fn sme_scan_with_passive_request() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone());
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: _,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        // Validate the SME received the scan_request and send back mock data
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &scan_request,
            input_aps.clone(),
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let scan_results: Vec<fidl_sme::ScanResult> = result.expect("failed to get scan results")
                .iter().map(|r| r.clone().into()).collect();
            assert_eq!(scan_results, input_aps);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
    }

    #[fuchsia::test]
    fn sme_scan_with_active_request() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![
                types::Ssid::try_from("foo_ssid").unwrap().into(),
                types::Ssid::try_from("bar_ssid").unwrap().into(),
            ],
            channels: vec![1, 20],
        });
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone());
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: _,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        // Validate the SME received the scan_request and send back mock data
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &scan_request,
            input_aps.clone(),
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let scan_results: Vec<fidl_sme::ScanResult> = result.expect("failed to get scan results")
                .iter().map(|r| r.clone().into()).collect();
            assert_eq!(scan_results, input_aps);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
    }

    #[test_case(fidl_sme::ScanErrorCode::InternalError; "SME scan error InternalError")]
    #[test_case(fidl_sme::ScanErrorCode::NotSupported; "SME scan error NotSupported")]
    #[fuchsia::test(add_test_attr = false)]
    fn sme_scan_error(error_code: fidl_sme::ScanErrorCode) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone());
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back an error
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send failed scan response.
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_error(&mut fidl_sme::ScanError {
                    code: error_code,
                    message: "Failed to scan".to_string()
                })
                    .expect("failed to send scan error");
            }
        );

        // No retry expected, check for results on the scan request
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let error = result.expect_err("did not expect to recieve scan results");
            assert_eq!(error, types::ScanError::GeneralError);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
    }

    #[test_case(fidl_sme::ScanErrorCode::ShouldWait, false; "SME scan error ShouldWait with failed retry")]
    #[test_case(fidl_sme::ScanErrorCode::ShouldWait, true; "SME scan error ShouldWait with successful retry")]
    #[test_case(fidl_sme::ScanErrorCode::CanceledByDriverOrFirmware, true; "SME scan error CanceledByDriverOrFirmware with successful retry")]
    #[fuchsia::test(add_test_attr = false)]
    fn sme_scan_error_with_retry(error_code: fidl_sme::ScanErrorCode, retry_succeeds: bool) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request.clone());
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back an error
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send failed scan response.
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_error(&mut fidl_sme::ScanError {
                    code: error_code,
                    message: "Failed to scan".to_string()
                })
                    .expect("failed to send scan error");
            }
        );

        // Advance the scanning future
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // There shouldn't yet be a request in the SME stream, since there should be
        // some delay before the scan is retried
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);

        // Wake up the timer and advance the scanning future
        assert!(exec.wake_next_timer().is_some());
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Expect a retry: we should now see a new request in the SME stream
        if retry_succeeds {
            // Validate the SME received the scan_request and send back mock data
            let aps = vec![];
            validate_sme_scan_request_and_send_results(
                &mut exec,
                &mut sme_stream,
                &scan_request,
                aps,
            );

            // Check for results
            assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
                assert_eq!(result, Ok(vec![]));
            });
        } else {
            // Check that a scan request was sent to the sme and send back an error
            assert_variant!(
                exec.run_until_stalled(&mut sme_stream.next()),
                Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                    txn, ..
                }))) => {
                    // Send failed scan response.
                    let (_stream, ctrl) = txn
                        .into_stream_and_control_handle().expect("error accessing control handle");
                    ctrl.send_on_error(&mut fidl_sme::ScanError {
                        code: error_code,
                        message: "Failed to scan".to_string()
                    })
                        .expect("failed to send scan error");
                }
            );

            // Check for results
            assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
                let error = result.expect_err("did not expect scan results");
                assert_eq!(error, types::ScanError::Cancelled);
            });
        }

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
    }

    #[fuchsia::test]
    fn sme_scan_channel_closed() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let scan_fut = sme_scan(&sme_proxy, scan_request);
        pin_mut!(scan_fut);

        // Request scan data from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back an error
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send failed scan response.
                txn.close_with_epitaph(zx::Status::OK).expect("Failed to close channel");
            }
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let error = result.expect_err("did not expect scan results");
            assert_eq!(error, types::ScanError::GeneralError);
        });

        // No further requests to the sme
        assert_variant!(exec.run_until_stalled(&mut sme_stream.next()), Poll::Pending);
    }

    #[fuchsia::test]
    fn basic_scan() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, network_selector_results) = MockScanResultConsumer::new();
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: internal_aps,
            passive_fidl_aps: fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_aps);
        });

        // Request the next chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();

        // Process scan handler
        // Note: this will be Poll::Ready because the scan handler will exit after sending the final
        // scan results.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(()));

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, vec![]);
        });

        // Check both successful scan consumers got results
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results.lock()),
            Some(internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results.lock()),
            Some(internal_aps.clone())
        );
    }

    /// Verify that only a passive scan occurs if all saved networks have a 0
    /// hidden network probability.
    #[fuchsia::test]
    fn passive_scan_only_with_zero_hidden_network_probabilities() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, _) = MockScanResultConsumer::new();
        let (location_sensor, _) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create passive scan info
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: _,
            passive_fidl_aps: fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();

        // Save a network and set its hidden probability to 0
        let network_id = types::NetworkIdentifier {
            ssid: types::Ssid::try_from("some ssid").unwrap(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager
                    .store(network_id.clone().into(), Credential::Password(b"randompass".to_vec()))
            )
            .expect("failed to store network")
            .is_none());
        exec.run_singlethreaded(
            saved_networks_manager.update_hidden_prob(network_id.clone().into(), 0.0),
        );
        let config = exec
            .run_singlethreaded(saved_networks_manager.lookup(network_id.clone().into()))
            .pop()
            .expect("failed to lookup");
        assert_eq!(config.hidden_probability, 0.0);

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager.clone(),
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check for results, verifying no active scan was requested.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_aps);
        });
    }

    /// Verify that saved networks seen in passive scans have their hidden network probabilities updated.
    #[fuchsia::test]
    fn passive_scan_updates_hidden_network_probabilities() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, _) = MockScanResultConsumer::new();
        let (location_sensor, _) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create the passive scan info
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: _,
            passive_fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();

        // Save a network that WILL be seen in the passive scan.
        let seen_in_passive_network =
            passive_fidl_aps[0].id.as_ref().expect("failed to get net id");
        assert!(exec
            .run_singlethreaded(saved_networks_manager.store(
                seen_in_passive_network.clone().into(),
                Credential::Password(b"randompass".to_vec())
            ))
            .expect("failed to store network")
            .is_none());

        // Save a network that will NOT be seen in the passive scan.
        let not_seen_net_id = types::NetworkIdentifier {
            ssid: types::Ssid::try_from("not_seen_net_id").unwrap(),
            security_type: types::SecurityType::Wpa,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager.store(
                    not_seen_net_id.clone().into(),
                    Credential::Password(b"foobarbaz".to_vec())
                )
            )
            .expect("failed to store network")
            .is_none());

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager.clone(),
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Verify that the passive scan results were recorded.
        assert!(
            *exec.run_singlethreaded(saved_networks_manager.passive_scan_result_recorded.lock())
        );

        // Note: the decision to active scan is non-deterministic (using the hidden network probabilities),
        // no need to continue and verify the results in this test case.
    }

    /// Verify that an active scan is occurs when there is a saved network with a 1.0
    /// hidden network probability, and that the probability is decreased when it
    /// goes unseen.
    #[fuchsia::test]
    fn active_scan_due_to_high_hidden_network_probabilities() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, _) = MockScanResultConsumer::new();
        let (location_sensor, _) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create the passive scan info
        let MockScanData {
            passive_input_aps,
            passive_internal_aps: _,
            passive_fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();

        // Save a network that will NOT be seen in the either scan and set
        // its initial hidden network probability to 1.0
        let unseen_ssid = types::Ssid::try_from("some ssid").unwrap();
        let unseen_network = types::NetworkIdentifier {
            ssid: unseen_ssid.clone(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager.store(
                    unseen_network.clone().into(),
                    Credential::Password(b"randompass".to_vec())
                )
            )
            .expect("failed to store network")
            .is_none());

        exec.run_singlethreaded(
            saved_networks_manager.update_hidden_prob(unseen_network.clone().into(), 1.0),
        );
        let config = exec
            .run_singlethreaded(saved_networks_manager.lookup(unseen_network.clone().into()))
            .pop()
            .expect("failed to lookup");
        assert_eq!(config.hidden_probability, 1.0);

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager.clone(),
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            passive_input_aps.clone(),
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check iterator is still waiting results, since there should be an
        // active scan still to come.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);

        // Verify record passive scan result was called.
        assert!(
            *exec.run_singlethreaded(saved_networks_manager.passive_scan_result_recorded.lock())
        );

        // Create mock active scan data. This should verify that an active scan was
        // issues based on the hidden network probabilties.
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![unseen_ssid.to_vec()],
            channels: vec![],
        });
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            vec![],
        );

        // Process SME result
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Get results from scans. Results should just be the passive results.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("failed to get scan results").unwrap();
            assert_eq!(results, passive_fidl_aps);
        });

        // Verify that active scan results were recorded.
        assert!(*exec.run_singlethreaded(saved_networks_manager.active_scan_result_recorded.lock()));
    }

    #[fuchsia::test]
    fn insert_bss_to_network_bss_map_duplicated_bss() {
        let mut bss_by_network = HashMap::new();

        // Create some input data with duplicated BSSID and Network Identifiers
        let passive_result = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3Enterprise,
                bssid: [0, 0, 0, 0, 0, 0],
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                rssi_dbm: 0,
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            ),
        };

        let passive_input_aps = vec![
            passive_result.clone(),
            fidl_sme::ScanResult {
                compatible: true,
                timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                bss_description: random_fidl_bss_description!(
                    Wpa3Enterprise,
                    bssid: [0, 0, 0, 0, 0, 0],
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                        rssi_dbm: 13,
                        snr_db: 3,
                        channel: types::WlanChan::new(14, types::Cbw::Cbw20),
                ),
            },
        ];

        let expected_id = SmeNetworkIdentifier {
            ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
            protection: fidl_sme::Protection::Wpa3Enterprise,
        };

        // We should only see one entry for the duplicated BSSs in the passive scan results
        let expected_bss = vec![types::Bss {
            bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
            rssi: 0,
            timestamp: zx::Time::from_nanos(passive_result.timestamp_nanos),
            snr_db: 1,
            channel: types::WlanChan::new(1, types::Cbw::Cbw20),
            observed_in_passive_scan: true,
            compatible: true,
            bss_description: passive_result.bss_description.clone(),
        }];

        insert_bss_to_network_bss_map(
            &mut bss_by_network,
            passive_input_aps
                .iter()
                .map(|scan_result| {
                    scan_result.clone().try_into().expect("Failed to convert ScanResult")
                })
                .collect::<Vec<wlan_common::scan::ScanResult>>(),
            true,
        );
        assert_eq!(bss_by_network.len(), 1);
        assert_eq!(bss_by_network[&expected_id], expected_bss);

        // Create some input data with one duplicate BSSID and one new BSSID
        let active_result = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa3Enterprise,
                ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                bssid: [1, 2, 3, 4, 5, 6],
                rssi_dbm: 101,
                snr_db: 101,
                channel: types::WlanChan::new(101, types::Cbw::Cbw40),
            ),
        };
        let active_input_aps = vec![
            fidl_sme::ScanResult {
                compatible: true,
                timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
                bss_description: random_fidl_bss_description!(
                    Wpa3Enterprise,
                    bssid: [0, 0, 0, 0, 0, 0],
                    ssid: types::Ssid::try_from("duplicated ssid").unwrap(),
                    rssi_dbm: 100,
                    snr_db: 100,
                    channel: types::WlanChan::new(100, types::Cbw::Cbw40),
                ),
            },
            active_result.clone(),
        ];

        // After the active scan, there should be a second bss included in the results
        let expected_bss = vec![
            types::Bss {
                bssid: types::Bssid([0, 0, 0, 0, 0, 0]),
                rssi: 0,
                timestamp: zx::Time::from_nanos(passive_result.timestamp_nanos),
                snr_db: 1,
                channel: types::WlanChan::new(1, types::Cbw::Cbw20),
                observed_in_passive_scan: true,
                compatible: true,
                bss_description: passive_result.bss_description.clone(),
            },
            types::Bss {
                bssid: types::Bssid([1, 2, 3, 4, 5, 6]),
                rssi: 101,
                timestamp: zx::Time::from_nanos(active_result.timestamp_nanos),
                snr_db: 101,
                channel: types::WlanChan::new(101, types::Cbw::Cbw40),
                observed_in_passive_scan: false,
                compatible: true,
                bss_description: active_result.bss_description.clone(),
            },
        ];

        insert_bss_to_network_bss_map(
            &mut bss_by_network,
            active_input_aps
                .iter()
                .map(|scan_result| {
                    scan_result.clone().try_into().expect("Failed to convert ScanResult")
                })
                .collect::<Vec<wlan_common::scan::ScanResult>>(),
            false,
        );
        assert_eq!(bss_by_network.len(), 1);
        assert_eq!(bss_by_network[&expected_id], expected_bss);
    }

    #[fuchsia::test]
    fn scan_with_active_scan_failure() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, network_selector_results) = MockScanResultConsumer::new();
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create the passive and active scan info
        let MockScanData {
            passive_input_aps,
            passive_internal_aps,
            passive_fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();

        // Save a network with hidden probability 1.0, which will guarantee an
        // active scan takes place
        let unseen_ssid = types::Ssid::try_from("foobarbaz ssid").unwrap();
        let unseen_network = types::NetworkIdentifier {
            ssid: unseen_ssid.clone(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager.store(
                    unseen_network.clone().into(),
                    Credential::Password(b"randompass".to_vec())
                )
            )
            .expect("failed to store network")
            .is_none());

        exec.run_singlethreaded(
            saved_networks_manager.update_hidden_prob(unseen_network.clone().into(), 1.0),
        );

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Respond to the first (passive) scan request
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            passive_input_aps.clone(),
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back an error
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![unseen_ssid.to_vec()],
            channels: vec![],
        });
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, req, ..
            }))) => {
                assert_eq!(req, expected_scan_request);
                // Send failed scan response.
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_error(&mut fidl_sme::ScanError {
                    code: fidl_sme::ScanErrorCode::InternalError,
                    message: "Failed to scan".to_string()
                })
                    .expect("failed to send scan error");
            }
        );

        // Process scan handler
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);
        // Check the FIDL result. We should first get all the available scan results, then an error.
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let result = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(result, passive_fidl_aps);
        });

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress scan handler forward so that it will respond to the iterator get next request.
        // Note: this will be Poll::Ready because the scan handler will exit after sending the final
        // scan results.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(()));
        // Now we should have the error available
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let result = result.expect("Failed to get next scan results").unwrap_err();
            assert_eq!(result, fidl_policy::ScanErrorCode::GeneralError);
        });

        // Check both scan consumers got just the passive scan results, since the active scan failed
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results.lock()),
            Some(passive_internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results.lock()),
            Some(passive_internal_aps.clone())
        );
    }

    #[fuchsia::test]
    fn scan_iterator_never_polled() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector1, network_selector_results1) = MockScanResultConsumer::new();
        let (location_sensor1, location_sensor_results1) = MockScanResultConsumer::new();
        let (network_selector2, network_selector_results2) = MockScanResultConsumer::new();
        let (location_sensor2, location_sensor_results2) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let (_iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client.clone(),
            saved_networks_manager.clone(),
            Some(iter_server),
            network_selector1,
            location_sensor1,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Progress scan side forward without ever calling getNext() on the scan result iterator
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: internal_aps,
            passive_fidl_aps: fidl_aps,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Progress scan side forward without progressing the scan result iterator
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Issue a second request to scan, to make sure that everything is still
        // moving along even though the first scan result iterator was never progressed.
        let (iter2, iter_server2) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut2 = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server2),
            network_selector2,
            location_sensor2,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut2);

        // Progress scan side forward
        assert_variant!(exec.run_until_stalled(&mut scan_fut2), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Request the results on the second iterator
        let mut output_iter_fut2 = iter2.get_next();

        // Progress scan side forward
        assert_variant!(exec.run_until_stalled(&mut scan_fut2), Poll::Pending);

        // Ensure results are present on the iterator
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut2), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_aps);
        });

        // Check all successful scan consumers got results
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results1.lock()),
            Some(internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results1.lock()),
            Some(internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results2.lock()),
            Some(internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results2.lock()),
            Some(internal_aps.clone())
        );
    }

    #[fuchsia::test]
    fn scan_iterator_shut_down() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, network_selector_results) = MockScanResultConsumer::new();
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        let MockScanData {
            passive_input_aps: input_aps,
            passive_internal_aps: internal_aps,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            input_aps.clone(),
        );

        // Close the channel
        drop(iter.into_channel());

        // Process scan handler
        // Note: this will be Poll::Ready because the scan handler will exit since all the consumers are done
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(()));

        // Check both successful scan consumers got results
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results.lock()),
            Some(internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results.lock()),
            Some(internal_aps.clone())
        );
    }

    #[fuchsia::test]
    fn scan_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector, network_selector_results) = MockScanResultConsumer::new();
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check that a scan request was sent to the sme and send back an error
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send failed scan response.
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                ctrl.send_on_error(&mut fidl_sme::ScanError {
                    code: fidl_sme::ScanErrorCode::InternalError,
                    message: "Failed to scan".to_string()
                })
                    .expect("failed to send scan error");
            }
        );

        // Process SME result.
        // Note: this will be Poll::Ready, since the scan handler will quit after sending the error
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(()));

        // the iterator should have an error on it
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results");
            assert_eq!(results, Err(fidl_policy::ScanErrorCode::GeneralError));
        });

        // Check both successful scan consumers have no results
        assert_eq!(*exec.run_singlethreaded(network_selector_results.lock()), None);
        assert_eq!(*exec.run_singlethreaded(location_sensor_results.lock()), None);
    }

    #[fuchsia::test]
    fn overlapping_scans() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (client, mut sme_stream) = exec.run_singlethreaded(create_iface_manager());
        let (network_selector1, network_selector_results1) = MockScanResultConsumer::new();
        let (location_sensor1, location_sensor_results1) = MockScanResultConsumer::new();
        let (network_selector2, network_selector_results2) = MockScanResultConsumer::new();
        let (location_sensor2, location_sensor_results2) = MockScanResultConsumer::new();

        // Use separate saved network managers so only one expects an active scan
        // based on saved networks.
        let saved_networks_manager1 = Arc::new(FakeSavedNetworksManager::new());
        let saved_networks_manager2 = Arc::new(FakeSavedNetworksManager::new());

        // Save a network with 1.0 hidden network probability to guarantee an active scan for scan_fut1.
        let active_ssid = types::Ssid::try_from("foo active ssid").unwrap();
        let active_id = types::NetworkIdentifier {
            ssid: active_ssid.clone(),
            security_type: types::SecurityType::Wpa3,
        };
        assert!(exec
            .run_singlethreaded(
                saved_networks_manager2
                    .store(active_id.clone().into(), Credential::Password(b"randompass".to_vec()))
            )
            .expect("failed to store network")
            .is_none());
        exec.run_singlethreaded(
            saved_networks_manager2.update_hidden_prob(active_id.clone().into(), 1.0),
        );

        let MockScanData {
            passive_input_aps,
            passive_internal_aps,
            passive_fidl_aps,
            active_input_aps,
            combined_internal_aps,
            combined_fidl_aps,
        } = create_scan_ap_data();

        // Create two sets of endpoints
        let (iter0, iter_server0) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let (iter1, iter_server1) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Issue request to scan on both iterator.
        let scan_fut0 = perform_scan(
            client.clone(),
            saved_networks_manager1.clone(),
            Some(iter_server0),
            network_selector1,
            location_sensor1,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut0);

        let scan_fut1 = perform_scan(
            client.clone(),
            saved_networks_manager2,
            Some(iter_server1),
            network_selector2,
            location_sensor2,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut1);

        // Request a chunk of scan results on both iterators. Progress until waiting on
        // response from server side of the iterator.
        let mut output_iter_fut0 = iter0.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut0), Poll::Pending);
        let mut output_iter_fut1 = iter1.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut1), Poll::Pending);

        // Progress first scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Pending);

        // Check that a scan request was sent to the sme and send back results
        assert_variant!(
            exec.run_until_stalled(&mut sme_stream.next()),
            Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
                txn, ..
            }))) => {
                // Send the first AP
                let (_stream, ctrl) = txn
                    .into_stream_and_control_handle().expect("error accessing control handle");
                let mut aps = [passive_input_aps[0].clone()];
                ctrl.send_on_result(&mut aps.iter_mut())
                    .expect("failed to send scan data");
                // Process SME result.
                assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Pending);
                // The iterator should not have any data yet, until the sme is done
                assert_variant!(exec.run_until_stalled(&mut output_iter_fut0), Poll::Pending);

                // Progress second scan handler forward so that it will respond to the iterator get next request.
                assert_variant!(exec.run_until_stalled(&mut scan_fut1), Poll::Pending);
                // Check that the second scan request was sent to the sme and send back results
                let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
                validate_sme_scan_request_and_send_results(&mut exec, &mut sme_stream, &expected_scan_request, passive_input_aps.clone()); // for output_iter_fut1
                // Process SME result.
                assert_variant!(exec.run_until_stalled(&mut scan_fut1), Poll::Pending);
                // The second request should now result in an active scan
                let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
                    channels: vec![],
                    ssids: vec![active_ssid.to_vec()],
                });
                validate_sme_scan_request_and_send_results(&mut exec, &mut sme_stream, &expected_scan_request, active_input_aps.clone()); // for output_iter_fut1
                // Process SME result.
                assert_variant!(exec.run_until_stalled(&mut scan_fut1), Poll::Pending);// The second iterator should have all its data

                assert_variant!(exec.run_until_stalled(&mut output_iter_fut1), Poll::Ready(result) => {
                    let results = result.expect("Failed to get next scan results").unwrap();
                    assert_eq!(results.len(), combined_fidl_aps.len());
                    assert_eq!(results, combined_fidl_aps);
                });

                // Send the remaining APs for the first iterator
                let mut aps = passive_input_aps[1..].iter().map(|a| a.clone()).collect::<Vec<_>>();
                ctrl.send_on_result(&mut aps.iter_mut())
                    .expect("failed to send scan data");
                // Process SME result.
                assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Pending);
                // Send the end of data
                ctrl.send_on_finished()
                    .expect("failed to send scan data");
            }
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut0), Poll::Pending);

        // The first iterator should have all its data
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut0), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), passive_fidl_aps.len());
            assert_eq!(results, passive_fidl_aps);
        });

        // Check both successful scan consumers got results
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results1.lock()),
            Some(passive_internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results1.lock()),
            Some(passive_internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results2.lock()),
            Some(combined_internal_aps.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results2.lock()),
            Some(combined_internal_aps.clone())
        );
    }

    #[test_case(true)]
    #[test_case(false)]
    #[fuchsia::test(add_test_attr = false)]
    fn perform_scan_wpa3_supported(wpa3_capable: bool) {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (network_selector, network_selector_results) = MockScanResultConsumer::new();
        let (location_sensor, location_sensor_results) = MockScanResultConsumer::new();
        let saved_networks_manager = Arc::new(FakeSavedNetworksManager::new());

        // Create a FakeIfaceManager that returns has_wpa3_iface() based on test value
        let (sme_proxy, remote) =
            create_proxy::<fidl_sme::ClientSmeMarker>().expect("error creating proxy");
        let mut sme_stream = remote.into_stream().expect("failed to create stream");
        let client = Arc::new(Mutex::new(FakeIfaceManager { sme_proxy, wpa3_capable }));

        // Issue request to scan.
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let scan_fut = perform_scan(
            client,
            saved_networks_manager,
            Some(iter_server),
            network_selector,
            location_sensor,
            ScanReason::NetworkSelection,
            None,
        );
        pin_mut!(scan_fut);

        // Request a chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Pending);
        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Generate scan results
        let ssid = types::Ssid::try_from("some_ssid").unwrap();
        let sme_scan_result = fidl_sme::ScanResult {
            compatible: true,
            timestamp_nanos: zx::Time::get_monotonic().into_nanos(),
            bss_description: random_fidl_bss_description!(
                Wpa2Wpa3,
                ssid: ssid.clone(),
                channel: types::WlanChan::new(8, types::Cbw::Cbw20),
            ),
        };
        let common_scan_result: wlan_common::scan::ScanResult = sme_scan_result
            .clone()
            .try_into()
            .expect("Failed to convert fidl_sme::ScanResult into wlan_common::scan::ScanResult");
        let _type_ =
            if wpa3_capable { types::SecurityType::Wpa3 } else { types::SecurityType::Wpa2 };
        let expected_scan_results = vec![fidl_policy::ScanResult {
            id: Some(fidl_policy::NetworkIdentifier {
                ssid: ssid.to_vec(),
                // Note: for now, we must always present WPA2/3 networks as WPA2 over our external
                // interfaces (i.e. to FIDL consumers of scan results). See b/182209070 for more
                // information.
                // TODO(b/182569380): change this back to a variable `type_` based on WPA3 support.
                type_: fidl_policy::SecurityType::Wpa2,
            }),
            entries: Some(vec![fidl_policy::Bss {
                bssid: Some(common_scan_result.bss_description.bssid.0),
                rssi: Some(common_scan_result.bss_description.rssi_dbm),
                // For now frequency and timestamp are set to 0 when converting types.
                frequency: Some(CENTER_FREQ_CHAN_8),
                timestamp_nanos: Some(common_scan_result.timestamp.into_nanos()),
                ..fidl_policy::Bss::EMPTY
            }]),
            compatibility: Some(types::Compatibility::Supported),
            ..fidl_policy::ScanResult::EMPTY
        }];
        let expected_internal_scans = vec![types::ScanResult {
            ssid: ssid.clone(),
            security_type_detailed: types::SecurityTypeDetailed::Wpa2Wpa3Personal,
            compatibility: fidl_policy::Compatibility::Supported,
            entries: vec![types::Bss {
                bssid: common_scan_result.bss_description.bssid,
                rssi: common_scan_result.bss_description.rssi_dbm,
                snr_db: common_scan_result.bss_description.snr_db,
                channel: common_scan_result.bss_description.channel.into(),
                timestamp: common_scan_result.timestamp,
                observed_in_passive_scan: true,
                compatible: common_scan_result.compatible,
                bss_description: common_scan_result.bss_description.into(),
            }],
        }];
        // Create mock scan data and send it via the SME
        let expected_scan_request = fidl_sme::ScanRequest::Passive(fidl_sme::PassiveScanRequest {});
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            vec![sme_scan_result],
        );

        // Process response from SME
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, expected_scan_results);
        });

        // Request the next chunk of scan results. Progress until waiting on response from server side of
        // the iterator.
        let mut output_iter_fut = iter.get_next();

        // Process scan handler
        // Note: this will be Poll::Ready because the scan handler will exit after sending the final
        // scan results.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(()));

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, vec![]);
        });

        // Check both successful scan consumers got results
        assert_eq!(
            *exec.run_singlethreaded(network_selector_results.lock()),
            Some(expected_internal_scans.clone())
        );
        assert_eq!(
            *exec.run_singlethreaded(location_sensor_results.lock()),
            Some(expected_internal_scans.clone())
        );
    }

    // TODO(fxbug.dev/54255): Separate test case for "empty final vector not consumed" vs "partial ap list"
    // consumed.
    #[fuchsia::test]
    fn partial_scan_result_consumption_has_no_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let MockScanData {
            passive_input_aps: _,
            passive_internal_aps: _,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps,
        } = create_scan_ap_data();

        // Create an iterator and send scan results
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let send_fut = send_scan_results_over_fidl(iter_server, &combined_fidl_aps, None);
        pin_mut!(send_fut);

        // Request a chunk of scan results.
        let mut output_iter_fut = iter.get_next();

        // Send first chunk of scan results
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        // Make sure the first chunk of results were delivered
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, combined_fidl_aps);
        });

        // Close the channel without getting remaining results
        // Note: as of the writing of this test, the "remaining results" are just the final message
        // with an empty vector of networks that signify the end of results. That final empty vector
        // is still considered part of the results, so this test successfully exercises the
        // "partial results read" path.
        drop(output_iter_fut);
        drop(iter);

        // This should not result in error, since some results were consumed
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Ok(())));
    }

    #[fuchsia::test]
    fn no_scan_result_consumption_has_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let MockScanData {
            passive_input_aps: _,
            passive_internal_aps: _,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps,
        } = create_scan_ap_data();

        // Create an iterator and send scan results
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");
        let send_fut = send_scan_results_over_fidl(iter_server, &combined_fidl_aps, None);
        pin_mut!(send_fut);

        // Close the channel without getting results
        drop(iter);

        // This should result in error, since no results were consumed
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Err(_)));
    }

    #[fuchsia::test]
    fn directed_active_scan_filters_desired_network() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let (sme_proxy, mut sme_stream) = exec.run_singlethreaded(create_sme_proxy());

        // Issue request to scan.
        let desired_ssid = types::Ssid::try_from("test_ssid").unwrap();
        let desired_channels = vec![1, 36];
        let scan_fut_desired_ssid = desired_ssid.clone();
        let scan_fut = perform_directed_active_scan(
            &sme_proxy,
            &scan_fut_desired_ssid,
            Some(desired_channels.clone()),
        );
        pin_mut!(scan_fut);

        // Generate scan results
        let scan_result_aps = vec![
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa3Enterprise, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2Wpa3, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2Wpa3, ssid: desired_ssid.clone()),
                ..generate_random_sme_scan_result()
            },
            fidl_sme::ScanResult {
                bss_description: random_fidl_bss_description!(Wpa2, ssid: types::Ssid::try_from("other ssid").unwrap()),
                ..generate_random_sme_scan_result()
            },
        ];

        // Progress scan handler forward so that it will respond to the iterator get next request.
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Pending);

        // Respond to the scan request
        let expected_scan_request = fidl_sme::ScanRequest::Active(fidl_sme::ActiveScanRequest {
            ssids: vec![desired_ssid.to_vec()],
            channels: desired_channels,
        });
        validate_sme_scan_request_and_send_results(
            &mut exec,
            &mut sme_stream,
            &expected_scan_request,
            scan_result_aps.clone(),
        );

        // Check for results
        assert_variant!(exec.run_until_stalled(&mut scan_fut), Poll::Ready(result) => {
            let mut result = result.unwrap();
            // Two networks with the desired SSID are present
            assert_eq!(result.len(), 2);
            result.sort_by_key(|r| r.security_type_detailed.clone());
            // One network is WPA2WPA3
            assert_eq!(result[0].ssid, desired_ssid.clone());
            assert_eq!(result[0].security_type_detailed, types::SecurityTypeDetailed::Wpa2Wpa3Personal);
            // Two BSSs for this network
            assert_eq!(result[0].entries.len(), 2);
            // Other network is WPA3
            assert_eq!(result[1].ssid, desired_ssid.clone());
            assert_eq!(result[1].security_type_detailed, types::SecurityTypeDetailed::Wpa3Enterprise);
            // One BSS for this network
            assert_eq!(result[1].entries.len(), 1);
        });
    }

    // TODO(fxbug.dev/52700) Ignore this test until the location sensor module exists.
    #[ignore]
    #[fuchsia::test]
    fn scan_observer_sends_to_location_sensor() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create an executor");
        let mut location_sensor_updater = LocationSensorUpdater { wpa3_supported: true };
        let MockScanData {
            passive_input_aps: _,
            passive_internal_aps: internal_aps,
            passive_fidl_aps: _,
            active_input_aps: _,
            combined_internal_aps: _,
            combined_fidl_aps: _,
        } = create_scan_ap_data();
        let fut = location_sensor_updater.update_scan_results(&internal_aps);
        exec.run_singlethreaded(fut);
        panic!("Need to reach into location sensor and check it got data")
    }

    #[fuchsia::test]
    fn sme_protection_converts_to_policy_security() {
        use {super::fidl_policy::SecurityType, super::fidl_sme::Protection};
        let wpa3_supported = true;
        let wpa3_not_supported = false;
        let test_pairs = vec![
            // Below are pairs when WPA3 is supported.
            (Protection::Wpa3Enterprise, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa3Personal, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa2Wpa3Personal, wpa3_supported, Some(SecurityType::Wpa3)),
            (Protection::Wpa2Enterprise, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Personal, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2Personal, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2PersonalTkipOnly, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2PersonalTkipOnly, wpa3_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1, wpa3_supported, Some(SecurityType::Wpa)),
            (Protection::Wep, wpa3_supported, Some(SecurityType::Wep)),
            (Protection::Open, wpa3_supported, Some(SecurityType::None)),
            (Protection::Unknown, wpa3_supported, None),
            // Below are pairs when WPA3 is not supported.
            (Protection::Wpa3Enterprise, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa3Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Wpa3Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Enterprise, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2Personal, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa2PersonalTkipOnly, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1Wpa2PersonalTkipOnly, wpa3_not_supported, Some(SecurityType::Wpa2)),
            (Protection::Wpa1, wpa3_not_supported, Some(SecurityType::Wpa)),
            (Protection::Wep, wpa3_not_supported, Some(SecurityType::Wep)),
            (Protection::Open, wpa3_not_supported, Some(SecurityType::None)),
            (Protection::Unknown, wpa3_not_supported, None),
        ];
        for (input, wpa3_capable, output) in test_pairs {
            assert_eq!(fidl_security_from_sme_protection(input, wpa3_capable), output);
        }
    }

    #[fuchsia::test]
    fn scan_result_generate_from_size() {
        let scan_results = create_fidl_scan_results_from_size(vec![112; 4]);
        assert_eq!(scan_results.len(), 4);
        assert!(scan_results.iter().all(|scan_result| scan_result.measure().num_bytes == 112));
    }

    #[fuchsia::test]
    fn scan_result_sends_max_message_size() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create and executor");
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a single scan result at the max allowed size to send in single
        // FIDL message.
        let fidl_scan_results = create_fidl_scan_results_from_size(vec![
            zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize
                - FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE,
        ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results, None);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_scan_results);
        })
    }

    #[fuchsia::test]
    fn scan_result_exceeding_max_size_throws_error() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create and executor");
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a single scan result exceeding the  max allowed size to send in single
        // FIDL message.
        let fidl_scan_results = create_fidl_scan_results_from_size(vec![
            (zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize
                - FIDL_HEADER_AND_ERR_WRAPPED_VEC_HEADER_SIZE)
                + 8,
        ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results, None);
        pin_mut!(send_fut);

        let _ = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Ready(Err(_)));
    }

    #[fuchsia::test]
    fn scan_result_sends_single_batch() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create and executor");
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a set of scan results that does not exceed the the max message
        // size, so it should be sent in a single batch.
        let fidl_scan_results =
            create_fidl_scan_results_from_size(vec![
                zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize / 4;
                3
            ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results, None);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results, fidl_scan_results);
        });
    }

    #[fuchsia::test]
    fn scan_result_sends_multiple_batches() {
        let mut exec = fasync::TestExecutor::new().expect("failed to create and executor");
        let (iter, iter_server) =
            fidl::endpoints::create_proxy().expect("failed to create iterator");

        // Create a set of scan results that exceed the max FIDL message size, so
        // they should be split into batches.
        let fidl_scan_results =
            create_fidl_scan_results_from_size(vec![
                zx::sys::ZX_CHANNEL_MAX_MSG_BYTES as usize / 8;
                8
            ]);

        let send_fut = send_scan_results_over_fidl(iter_server, &fidl_scan_results, None);
        pin_mut!(send_fut);

        let mut output_iter_fut = iter.get_next();

        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);

        let mut aggregate_results = vec![];
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), 7);
            aggregate_results.extend(results);
        });

        let mut output_iter_fut = iter.get_next();
        assert_variant!(exec.run_until_stalled(&mut send_fut), Poll::Pending);
        assert_variant!(exec.run_until_stalled(&mut output_iter_fut), Poll::Ready(result) => {
            let results = result.expect("Failed to get next scan results").unwrap();
            assert_eq!(results.len(), 1);
            aggregate_results.extend(results);
        });
        assert_eq!(aggregate_results, fidl_scan_results);
    }
}
