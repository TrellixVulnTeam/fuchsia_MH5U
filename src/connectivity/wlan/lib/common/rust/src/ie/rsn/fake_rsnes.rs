// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::ie::rsn::{
        akm::AKM_PSK,
        cipher::{CIPHER_CCMP_128, CIPHER_TKIP},
        rsne::Rsne,
    },
    fidl_fuchsia_wlan_common::DriverFeature,
};

pub fn fake_wpa2_a_rsne() -> Rsne {
    Rsne {
        group_data_cipher_suite: Some(CIPHER_CCMP_128),
        pairwise_cipher_suites: vec![CIPHER_CCMP_128, CIPHER_TKIP],
        akm_suites: vec![AKM_PSK],
        ..Default::default()
    }
}

pub fn fake_wpa2_s_rsne() -> Rsne {
    fake_wpa2_a_rsne().derive_wpa2_s_rsne(&vec![]).expect("Unable to derive supplicant RSNE")
}

pub fn fake_wpa3_a_rsne() -> Rsne {
    Rsne::wpa3_rsne()
}

pub fn fake_wpa3_s_rsne() -> Rsne {
    fake_wpa3_a_rsne()
        .derive_wpa3_s_rsne(&vec![DriverFeature::Mfp])
        .expect("Unable to derive supplicant RSNE")
}
