// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#![cfg(test)]

use {
    core::pin::Pin,
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_async::{self as fasync, DurationExt},
    fuchsia_zircon as zx,
    futures::{prelude::*, stream::StreamFuture, task::Poll},
    log::debug,
    wlan_common::assert_variant,
};

#[track_caller]
pub fn poll_sme_req(
    exec: &mut fasync::TestExecutor,
    next_sme_req: &mut StreamFuture<fidl_fuchsia_wlan_sme::ClientSmeRequestStream>,
) -> Poll<fidl_fuchsia_wlan_sme::ClientSmeRequest> {
    exec.run_until_stalled(next_sme_req).map(|(req, stream)| {
        *next_sme_req = stream.into_future();
        req.expect("did not expect the SME request stream to end")
            .expect("error polling SME request stream")
    })
}

#[track_caller]
pub fn validate_sme_scan_request_and_send_results(
    exec: &mut fasync::TestExecutor,
    sme_stream: &mut fidl_sme::ClientSmeRequestStream,
    expected_scan_request: &fidl_sme::ScanRequest,
    mut scan_results: Vec<fidl_sme::ScanResult>,
) {
    // Check that a scan request was sent to the sme and send back results
    assert_variant!(
        exec.run_until_stalled(&mut sme_stream.next()),
        Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
            txn, req, control_handle: _
        }))) => {
            // Validate the request
            assert_eq_sme_scan_requests(&req, expected_scan_request);
            // Send all the APs
            let (_stream, ctrl) = txn
                .into_stream_and_control_handle().expect("error accessing control handle");
            ctrl.send_on_result(&mut scan_results.iter_mut())
                .expect("failed to send scan data");

            // Send the end of data
            ctrl.send_on_finished()
                .expect("failed to send scan data");
        }
    );
}

// Assert equality between two SME scan requests. Active scan requests containing
// vectors of SSIDs and channels, but in a different order, should be considered
// equal.
fn assert_eq_sme_scan_requests(
    request: &fidl_sme::ScanRequest,
    expected_request: &fidl_sme::ScanRequest,
) {
    match (request, expected_request) {
        (fidl_sme::ScanRequest::Passive(_), fidl_sme::ScanRequest::Passive(_)) => {
            assert_eq!(request, expected_request);
        }
        (
            fidl_sme::ScanRequest::Active(scan_request),
            fidl_sme::ScanRequest::Active(expected_scan_request),
        ) => {
            assert_eq!(scan_request.ssids.len(), expected_scan_request.ssids.len());
            assert!(scan_request
                .ssids
                .iter()
                .all(|ssid| expected_scan_request.ssids.contains(ssid)));
            assert!(expected_scan_request
                .ssids
                .iter()
                .all(|ssid| scan_request.ssids.contains(ssid)));
            assert_eq!(scan_request.channels.len(), expected_scan_request.channels.len());
            assert!(scan_request
                .channels
                .iter()
                .all(|channel| expected_scan_request.channels.contains(channel)));
            assert!(expected_scan_request
                .channels
                .iter()
                .all(|channel| scan_request.channels.contains(channel)));
        }
        _ => panic!("mismatch sme scan_result types"),
    }
}

/// It takes an indeterminate amount of time for the scan module to either send the results
/// to the location sensor, or be notified by the component framework that the location
/// sensor's channel is closed / non-existent. This function continues trying to advance the
/// future until the next expected event happens (e.g. an event is present on the sme stream
/// for the expected active scan).
#[track_caller]
pub fn poll_for_and_validate_sme_scan_request_and_send_results(
    exec: &mut fasync::TestExecutor,
    network_selection_fut: &mut Pin<&mut impl futures::Future>,
    sme_stream: &mut fidl_sme::ClientSmeRequestStream,
    expected_scan_request: &fidl_sme::ScanRequest,
    mut scan_results: Vec<fidl_sme::ScanResult>,
) {
    let mut counter = 0;
    let sme_stream_result = loop {
        counter += 1;
        if counter > 1000 {
            panic!("Failed to progress network selection future until active scan");
        };
        let sleep_duration = zx::Duration::from_millis(2);
        exec.run_singlethreaded(fasync::Timer::new(sleep_duration.after_now()));
        assert_variant!(
            exec.run_until_stalled(network_selection_fut),
            Poll::Pending,
            "Did not get 'poll::Pending' on network_selection_fut"
        );
        match exec.run_until_stalled(&mut sme_stream.next()) {
            Poll::Pending => continue,
            other_result => {
                debug!("Required {} iterations to get an SME stream message", counter);
                break other_result;
            }
        }
    };

    assert_variant!(
        sme_stream_result,
        Poll::Ready(Some(Ok(fidl_sme::ClientSmeRequest::Scan {
            txn, req, control_handle: _
        }))) => {
            // Validate the request
            assert_eq!(req, *expected_scan_request);
            // Send all the APs
            let (_stream, ctrl) = txn
                .into_stream_and_control_handle().expect("error accessing control handle");
            ctrl.send_on_result(&mut scan_results.iter_mut())
                .expect("failed to send scan data");

            // Send the end of data
            ctrl.send_on_finished()
                .expect("failed to send scan data");
        }
    );
}
