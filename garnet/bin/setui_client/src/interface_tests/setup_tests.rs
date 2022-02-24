// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::setup;
use anyhow::{Context as _, Error};
use fidl_fuchsia_settings::{ConfigurationInterfaces, SetupMarker, SetupRequest, SetupSettings};

use crate::interface_tests::Services;
use crate::interface_tests::ENV_NAME;
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use futures::prelude::*;

fn create_setup_setting(interfaces: ConfigurationInterfaces) -> SetupSettings {
    let mut settings = SetupSettings::EMPTY;
    settings.enabled_configuration_interfaces = Some(interfaces);

    settings
}

#[fuchsia_async::run_until_stalled(test)]
async fn validate_setup() -> Result<(), Error> {
    let expected_set_interfaces = ConfigurationInterfaces::ETHERNET;
    let expected_watch_interfaces =
        ConfigurationInterfaces::WIFI | ConfigurationInterfaces::ETHERNET;
    let env = create_service!(
        Services::Setup, SetupRequest::Set { settings, reboot_device: _, responder, } => {
            if let Some(interfaces) = settings.enabled_configuration_interfaces {
                assert_eq!(interfaces, expected_set_interfaces);
                responder.send(&mut Ok(()))?;
            } else {
                panic!("Unexpected call to set");
            }
        },
        SetupRequest::Watch { responder } => {
            responder.send(create_setup_setting(expected_watch_interfaces))?;
        }
    );

    let setup_service =
        env.connect_to_protocol::<SetupMarker>().context("Failed to connect to setup service")?;

    assert_set!(setup::command(setup_service.clone(), Some(expected_set_interfaces)));
    let output = assert_watch!(setup::command(setup_service.clone(), None));
    assert_eq!(
        output,
        setup::describe_setup_setting(&create_setup_setting(expected_watch_interfaces))
    );
    Ok(())
}
