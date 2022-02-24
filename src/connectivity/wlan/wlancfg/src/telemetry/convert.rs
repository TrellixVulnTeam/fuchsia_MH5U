// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    fidl_fuchsia_wlan_sme as fidl_sme,
    fuchsia_zircon::{self as zx, DurationNum},
    wlan_common::bss::Protection as BssProtection,
    wlan_metrics_registry as metrics,
};

pub fn convert_disconnect_source(
    source: &fidl_sme::DisconnectSource,
) -> metrics::ConnectivityWlanMetricDimensionDisconnectSource {
    use metrics::ConnectivityWlanMetricDimensionDisconnectSource::*;
    match source {
        fidl_sme::DisconnectSource::Ap(..) => Ap,
        fidl_sme::DisconnectSource::User(..) => User,
        fidl_sme::DisconnectSource::Mlme(..) => Mlme,
    }
}

pub fn convert_user_wait_time(
    duration: zx::Duration,
) -> metrics::ConnectivityWlanMetricDimensionWaitTime {
    use metrics::ConnectivityWlanMetricDimensionWaitTime::*;
    match duration {
        x if x < 1.second() => LessThan1Second,
        x if x < 3.seconds() => LessThan3Seconds,
        x if x < 5.seconds() => LessThan5Seconds,
        x if x < 8.seconds() => LessThan8Seconds,
        x if x < 15.seconds() => LessThan15Seconds,
        _ => AtLeast15Seconds,
    }
}

pub fn convert_is_multi_bss(
    multiple_bss_candidates: bool,
) -> metrics::SuccessfulConnectBreakdownByIsMultiBssMetricDimensionIsMultiBss {
    use metrics::SuccessfulConnectBreakdownByIsMultiBssMetricDimensionIsMultiBss::*;
    match multiple_bss_candidates {
        true => Yes,
        false => No,
    }
}

pub fn convert_security_type(
    protection: &BssProtection,
) -> metrics::SuccessfulConnectBreakdownBySecurityTypeMetricDimensionSecurityType {
    use metrics::SuccessfulConnectBreakdownBySecurityTypeMetricDimensionSecurityType::*;
    match protection {
        BssProtection::Unknown => Unknown,
        BssProtection::Open => Open,
        BssProtection::Wep => Wep,
        BssProtection::Wpa1 => Wpa1,
        BssProtection::Wpa1Wpa2PersonalTkipOnly => Wpa1Wpa2PersonalTkipOnly,
        BssProtection::Wpa2PersonalTkipOnly => Wpa2PersonalTkipOnly,
        BssProtection::Wpa1Wpa2Personal => Wpa1Wpa2Personal,
        BssProtection::Wpa2Personal => Wpa2Personal,
        BssProtection::Wpa2Wpa3Personal => Wpa2Wpa3Personal,
        BssProtection::Wpa3Personal => Wpa3Personal,
        BssProtection::Wpa2Enterprise => Wpa2Enterprise,
        BssProtection::Wpa3Enterprise => Wpa3Enterprise,
    }
}

pub fn convert_channel_band(
    primary_channel: u8,
) -> metrics::SuccessfulConnectBreakdownByChannelBandMetricDimensionChannelBand {
    use metrics::SuccessfulConnectBreakdownByChannelBandMetricDimensionChannelBand::*;
    if primary_channel > 14 {
        Band5Ghz
    } else {
        Band2Dot4Ghz
    }
}

pub fn convert_rssi_bucket(
    rssi: i8,
) -> metrics::SuccessfulConnectBreakdownByRssiBucketMetricDimensionRssiBucket {
    use metrics::SuccessfulConnectBreakdownByRssiBucketMetricDimensionRssiBucket::*;
    match rssi {
        -128..=-90 => From128To90,
        -89..=-86 => From89To86,
        -85..=-83 => From85To83,
        -82..=-80 => From82To80,
        -79..=-77 => From79To77,
        -76..=-74 => From76To74,
        -73..=-71 => From73To71,
        -70..=-66 => From70To66,
        -65..=-61 => From65To61,
        -60..=-51 => From60To51,
        -50..=-35 => From50To35,
        -34..=-28 => From34To28,
        -27..=-1 => From27To1,
        _ => _0,
    }
}

pub fn convert_snr_bucket(
    snr: i8,
) -> metrics::SuccessfulConnectBreakdownBySnrBucketMetricDimensionSnrBucket {
    use metrics::SuccessfulConnectBreakdownBySnrBucketMetricDimensionSnrBucket::*;
    match snr {
        1..=10 => From1To10,
        11..=15 => From11To15,
        16..=25 => From16To25,
        26..=40 => From26To40,
        41..=127 => MoreThan40,
        _ => _0,
    }
}
