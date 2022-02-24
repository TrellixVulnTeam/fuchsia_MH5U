// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

mod api;
mod assisting_state;
mod driver_state;
mod error_adapter;
mod inbound;
mod init;
mod misc;
mod tasks;

#[cfg(test)]
mod tests;

use crate::spinel::*;
use assisting_state::*;
use driver_state::*;
use error_adapter::*;
use fidl_fuchsia_lowpan::{ConnectivityState, Role};
use fuchsia_syslog::macros::*;
use fuchsia_zircon::Duration;
use lowpan_driver_common::net::NetworkInterface;
pub use lowpan_driver_common::net::*;
use lowpan_driver_common::AsyncCondition;

const DEFAULT_SCAN_DWELL_TIME_MS: u16 = 100;

#[cfg(not(test))]
const DEFAULT_TIMEOUT: Duration = Duration::from_seconds(5);

#[cfg(test)]
const DEFAULT_TIMEOUT: Duration = Duration::from_seconds(90);

const MAX_NCP_DEBUG_LINE_LEN: usize = 240;

const STD_IPV6_NET_PREFIX_LEN: u8 = 64;

/// Convenience macro for handling timeouts.
#[macro_export]
macro_rules! ncp_cmd_timeout (
    ($self:ident) => {
        move || {
            fx_log_err!("Timeout");
            $self.ncp_is_misbehaving();
            Err(ZxStatus::TIMED_OUT)
        }
    };
);

pub use crate::ncp_cmd_timeout;

/// High-level LoWPAN driver implementation for Spinel-based devices.
/// It covers the basic high-level state machine as well as
/// the task definitions for all API commands.
#[derive(Debug)]
pub struct SpinelDriver<DS, NI> {
    /// Handles sending commands and routing responses.
    frame_handler: FrameHandler<DS>,

    /// Frame sink for sending raw Spinel commands, as well
    /// as managing `open`/`close`/`reset` for the Spinel device.
    device_sink: DS,

    /// The protected driver state.
    driver_state: parking_lot::Mutex<DriverState>,

    /// Condition that fires whenever the above `driver_state` changes.
    driver_state_change: AsyncCondition,

    /// Condition that fires whenever the device has been reset.
    ncp_did_reset: AsyncCondition,

    /// A task lock for ensuring that mutually-exclusive API operations
    /// don't step on each other.
    exclusive_task_lock: futures::lock::Mutex<()>,

    /// Debug Output Buffer
    ncp_debug_buffer: parking_lot::Mutex<Vec<u8>>,

    /// Network Interface
    net_if: NI,

    pending_outbound_frame_sender: futures::channel::mpsc::UnboundedSender<Vec<u8>>,

    /// This gets used by `take_main_task()`.
    pending_outbound_frame_receiver:
        parking_lot::Mutex<Option<futures::channel::mpsc::UnboundedReceiver<Vec<u8>>>>,
}

impl<DS: SpinelDeviceClient, NI> SpinelDriver<DS, NI> {
    pub fn new(device_sink: DS, net_if: NI) -> Self {
        let (sender, receiver) = futures::channel::mpsc::unbounded();
        SpinelDriver {
            frame_handler: FrameHandler::new(device_sink.clone()),
            device_sink,
            driver_state: parking_lot::Mutex::new(Default::default()),
            driver_state_change: AsyncCondition::new(),
            ncp_did_reset: AsyncCondition::new(),
            exclusive_task_lock: Default::default(),
            ncp_debug_buffer: Default::default(),
            net_if,
            pending_outbound_frame_sender: sender,
            pending_outbound_frame_receiver: parking_lot::Mutex::new(Some(receiver)),
        }
    }
}
