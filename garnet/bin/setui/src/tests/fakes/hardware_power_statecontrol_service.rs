// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
use crate::tests::fakes::base::Service;
use anyhow::{format_err, Error};
use fidl::endpoints::{ProtocolMarker, ServerEnd};
use fidl_fuchsia_hardware_power_statecontrol::{AdminRequest, RebootReason};
use fuchsia_async as fasync;
use fuchsia_zircon as zx;
use futures::TryStreamExt;
use parking_lot::RwLock;
use std::sync::Arc;

#[derive(PartialEq, Debug, Eq, Hash, Clone, Copy)]
pub(crate) enum Action {
    Reboot,
}

/// An implementation of hardware power statecontrol services that records the
/// actions invoked on it.
pub(crate) struct HardwarePowerStatecontrolService {
    recorded_actions: Arc<RwLock<Vec<Action>>>,
}

impl HardwarePowerStatecontrolService {
    pub(crate) fn new() -> Self {
        Self { recorded_actions: Arc::new(RwLock::new(Vec::new())) }
    }

    pub(crate) fn verify_action_sequence(&self, actions: Vec<Action>) -> bool {
        let recorded_actions = self.recorded_actions.read();
        actions.len() == recorded_actions.len()
            && recorded_actions
                .iter()
                .zip(actions.iter())
                .all(|(action1, action2)| action1 == action2)
    }
}

impl Service for HardwarePowerStatecontrolService {
    fn can_handle_service(&self, service_name: &str) -> bool {
        service_name == fidl_fuchsia_hardware_power_statecontrol::AdminMarker::NAME
    }

    fn process_stream(&mut self, service_name: &str, channel: zx::Channel) -> Result<(), Error> {
        if !self.can_handle_service(service_name) {
            return Err(format_err!("unsupported"));
        }

        let mut manager_stream =
            ServerEnd::<fidl_fuchsia_hardware_power_statecontrol::AdminMarker>::new(channel)
                .into_stream()?;

        let recorded_actions_clone = self.recorded_actions.clone();
        fasync::Task::spawn(async move {
            while let Some(req) = manager_stream.try_next().await.unwrap() {
                if let AdminRequest::Reboot { reason: RebootReason::UserRequest, responder } = req {
                    recorded_actions_clone.write().push(Action::Reboot);
                    responder.send(&mut Ok(())).unwrap();
                }
            }
        })
        .detach();

        Ok(())
    }
}
