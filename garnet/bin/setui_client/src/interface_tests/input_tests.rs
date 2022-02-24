// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::input;
use crate::interface_tests::Services;
use crate::interface_tests::ENV_NAME;
use anyhow::{Context as _, Error};
use fidl_fuchsia_settings::{
    DeviceState, DeviceStateSource, DeviceType, InputDevice, InputMarker, InputRequest,
    InputSettings, SourceState, ToggleStateFlags,
};
use fuchsia_async as fasync;
use fuchsia_component::server::ServiceFs;
use futures::prelude::*;

/// Creates a one-item list of input devices with the given properties.
fn create_input_devices(
    device_type: DeviceType,
    device_name: &str,
    device_state: u64,
) -> Vec<InputDevice> {
    let mut devices = Vec::new();
    let mut source_states = Vec::new();
    source_states.push(SourceState {
        source: Some(DeviceStateSource::Hardware),
        state: Some(DeviceState {
            toggle_flags: ToggleStateFlags::from_bits(1),
            ..DeviceState::EMPTY
        }),
        ..SourceState::EMPTY
    });
    source_states.push(SourceState {
        source: Some(DeviceStateSource::Software),
        state: Some(u64_to_state(device_state)),
        ..SourceState::EMPTY
    });
    let device = InputDevice {
        device_name: Some(device_name.to_string()),
        device_type: Some(device_type),
        source_states: Some(source_states),
        mutable_toggle_state: ToggleStateFlags::from_bits(12),
        state: Some(u64_to_state(device_state)),
        ..InputDevice::EMPTY
    };
    devices.push(device);
    devices
}

/// Transforms an u64 into an fuchsia_fidl_settings::DeviceState.
fn u64_to_state(num: u64) -> DeviceState {
    DeviceState { toggle_flags: ToggleStateFlags::from_bits(num), ..DeviceState::EMPTY }
}

async fn validate_watch() -> Result<(), Error> {
    let env = create_service!(Services::Input,
        InputRequest::Watch { responder } => {
            responder.send(InputSettings {
                devices: Some(
                    create_input_devices(
                        DeviceType::Camera,
                        "camera",
                        1,
                    )
                ),
                ..InputSettings::EMPTY
            })?;
        }
    );

    let input_service =
        env.connect_to_protocol::<InputMarker>().context("Failed to connect to input service")?;

    let output = assert_watch!(input::command(input_service, None, None, None));
    // Just check that the output contains some key strings that confirms the watch returned.
    // The string representation may not necessarily be in the same order.
    assert!(output.contains("Software"));
    assert!(output.contains("source_states: Some"));
    assert!(output.contains("toggle_flags: Some"));
    assert!(output.contains("camera"));
    assert!(output.contains("Available"));
    Ok(())
}

// TODO(fxbug.dev/65686): Remove when clients are ported to new interface.
async fn validate_input2_watch() -> Result<(), Error> {
    let env = create_service!(Services::Input,
        InputRequest::Watch2 { responder } => {
            responder.send(InputSettings {
                devices: Some(
                    create_input_devices(
                        DeviceType::Camera,
                        "camera",
                        1,
                    )
                ),
                ..InputSettings::EMPTY
            })?;
        }
    );

    let input_service =
        env.connect_to_protocol::<InputMarker>().context("Failed to connect to input service")?;

    let output = assert_watch!(input::command2(input_service, None, None, None));
    // Just check that the output contains some key strings that confirms the watch returned.
    // The string representation may not necessarily be in the same order.
    assert!(output.contains("Software"));
    assert!(output.contains("source_states: Some"));
    assert!(output.contains("toggle_flags: Some"));
    assert!(output.contains("camera"));
    assert!(output.contains("Available"));
    Ok(())
}

async fn validate_set(
    device_type: DeviceType,
    device_name: &'static str,
    device_state: u64,
    expected_state_string: &str,
) -> Result<(), Error> {
    let env = create_service!(Services::Input,
        InputRequest::Set { input_states, responder } => {
            input_states.iter().for_each(move |state| {
                assert_eq!(Some(device_type), state.device_type);
                assert_eq!(Some(device_name.to_string()), state.name);
                assert_eq!(Some(u64_to_state(device_state)), state.state);
            });
            responder.send(&mut (Ok(())))?;
        }
    );

    let input_service =
        env.connect_to_protocol::<InputMarker>().context("Failed to connect to input service")?;

    let output = assert_set!(input::command(
        input_service,
        Some(device_type),
        Some(device_name.to_string()),
        Some(u64_to_state(device_state)),
    ));
    // Just check that the output contains some key strings that confirms the set returned.
    // The string representation may not necessarily be in the same order.
    assert!(output.contains(&format!("{:?}", device_type)));
    assert!(output.contains(&format!("{:?}", device_name)));
    assert!(output.contains(expected_state_string));
    Ok(())
}

// TODO(fxbug.dev/65686): Remove when clients are ported to new interface.
async fn validate_input2_set(
    device_type: DeviceType,
    device_name: &'static str,
    device_state: u64,
    expected_state_string: &str,
) -> Result<(), Error> {
    let env = create_service!(Services::Input,
        InputRequest::SetStates { input_states, responder } => {
            input_states.iter().for_each(move |state| {
                assert_eq!(Some(device_type), state.device_type);
                assert_eq!(Some(device_name.to_string()), state.name);
                assert_eq!(Some(u64_to_state(device_state)), state.state);
            });
            responder.send(&mut (Ok(())))?;
        }
    );

    let input_service =
        env.connect_to_protocol::<InputMarker>().context("Failed to connect to input service")?;

    let output = assert_set!(input::command2(
        input_service,
        Some(device_type),
        Some(device_name.to_string()),
        Some(u64_to_state(device_state)),
    ));
    // Just check that the output contains some key strings that confirms the set returned.
    // The string representation may not necessarily be in the same order.
    assert!(output.contains(&format!("{:?}", device_type)));
    assert!(output.contains(&format!("{:?}", device_name)));
    assert!(output.contains(expected_state_string));
    Ok(())
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_input() -> Result<(), Error> {
    println!("input service tests");
    println!("  client calls input watch");
    validate_watch().await?;

    println!("  client calls set input with microphone");
    validate_set(
        DeviceType::Microphone,
        "microphone",
        3,
        "AVAILABLE | Available | ACTIVE | Active",
    )
    .await?;
    println!("  client calls set input with camera");
    validate_set(DeviceType::Camera, "camera", 3, "AVAILABLE | Available | ACTIVE | Active")
        .await?;

    // TODO(fxbug.dev/65686): Remove when clients are ported to new interface.
    println!("input2 service tests");
    println!("  client calls input watch2");
    validate_input2_watch().await?;

    println!("  client calls set input with microphone");
    validate_input2_set(
        DeviceType::Microphone,
        "microphone",
        3,
        "AVAILABLE | Available | ACTIVE | Active",
    )
    .await?;
    println!("  client calls set input with camera");
    validate_input2_set(DeviceType::Camera, "camera", 3, "AVAILABLE | Available | ACTIVE | Active")
        .await?;

    Ok(())
}
