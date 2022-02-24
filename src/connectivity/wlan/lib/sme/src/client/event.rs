// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use fuchsia_zircon::{self as zx, prelude::DurationNum};

use ieee80211::{Bssid, MacAddr};
use wlan_common::timer::TimeoutDuration;

pub const ESTABLISHING_RSNA_TIMEOUT_SECONDS: i64 = 3;
pub const KEY_FRAME_EXCHANGE_TIMEOUT_MILLIS: i64 = 200;
pub const INSPECT_PULSE_CHECK_MINUTES: i64 = 1;
pub const INSPECT_PULSE_PERSIST_MINUTES: i64 = 5;
pub const SAE_RETRANSMISSION_TIMEOUT_MILLIS: i64 = 200;

#[derive(Debug, Clone)]
pub enum Event {
    EstablishingRsnaTimeout(EstablishingRsnaTimeout),
    KeyFrameExchangeTimeout(KeyFrameExchangeTimeout),
    InspectPulseCheck(InspectPulseCheck),
    /// From startup, periodically schedule an event to persist the Inspect pulse data
    InspectPulsePersist(InspectPulsePersist),
    SaeTimeout(SaeTimeout),
}
impl From<EstablishingRsnaTimeout> for Event {
    fn from(timeout: EstablishingRsnaTimeout) -> Self {
        Event::EstablishingRsnaTimeout(timeout)
    }
}
impl From<KeyFrameExchangeTimeout> for Event {
    fn from(timeout: KeyFrameExchangeTimeout) -> Self {
        Event::KeyFrameExchangeTimeout(timeout)
    }
}
impl From<InspectPulseCheck> for Event {
    fn from(this: InspectPulseCheck) -> Self {
        Event::InspectPulseCheck(this)
    }
}
impl From<InspectPulsePersist> for Event {
    fn from(this: InspectPulsePersist) -> Self {
        Event::InspectPulsePersist(this)
    }
}
impl From<SaeTimeout> for Event {
    fn from(this: SaeTimeout) -> Self {
        Event::SaeTimeout(this)
    }
}

#[derive(Debug, Clone)]
pub struct EstablishingRsnaTimeout;
impl TimeoutDuration for EstablishingRsnaTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        ESTABLISHING_RSNA_TIMEOUT_SECONDS.seconds()
    }
}

#[derive(Debug, Clone)]
pub struct KeyFrameExchangeTimeout {
    pub bssid: Bssid,
    pub sta_addr: MacAddr,
}
impl TimeoutDuration for KeyFrameExchangeTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        KEY_FRAME_EXCHANGE_TIMEOUT_MILLIS.millis()
    }
}

#[derive(Debug, Clone)]
pub struct InspectPulseCheck;
impl TimeoutDuration for InspectPulseCheck {
    fn timeout_duration(&self) -> zx::Duration {
        INSPECT_PULSE_CHECK_MINUTES.minutes()
    }
}

#[derive(Debug, Clone)]
pub struct InspectPulsePersist;
impl TimeoutDuration for InspectPulsePersist {
    fn timeout_duration(&self) -> zx::Duration {
        INSPECT_PULSE_PERSIST_MINUTES.minutes()
    }
}

#[derive(Debug, Clone)]
pub struct SaeTimeout(pub u64);
impl TimeoutDuration for SaeTimeout {
    fn timeout_duration(&self) -> zx::Duration {
        SAE_RETRANSMISSION_TIMEOUT_MILLIS.millis()
    }
}
