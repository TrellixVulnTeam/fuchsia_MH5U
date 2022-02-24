// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::agent::storage::device_storage::testing::InMemoryStorageFactory;
use crate::ingress::fidl::Interface;
use crate::setup::types::{ConfigurationInterfaceFlags, SetupInfo};
use crate::tests::fakes::hardware_power_statecontrol_service::{
    Action, HardwarePowerStatecontrolService,
};
use crate::tests::fakes::service_registry::ServiceRegistry;
use crate::EnvironmentBuilder;
use fidl_fuchsia_settings::*;
use futures::lock::Mutex;
use std::sync::Arc;

const ENV_NAME: &str = "settings_service_setup_test_environment";

// Ensures the default value returned is WiFi.
#[fuchsia_async::run_until_stalled(test)]
async fn test_setup_default() {
    let env = EnvironmentBuilder::new(Arc::new(InMemoryStorageFactory::new()))
        .fidl_interfaces(&[Interface::Setup])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let setup_service = env.connect_to_protocol::<SetupMarker>().unwrap();

    // Ensure retrieved value matches default value
    let settings = setup_service.watch().await.expect("watch completed");
    assert_eq!(
        settings.enabled_configuration_interfaces,
        Some(fidl_fuchsia_settings::ConfigurationInterfaces::WIFI)
    );
}

// Setup doesn't rely on any service yet. In the future this test will be
// updated to verify restart request is made on interface change.
#[fuchsia_async::run_until_stalled(test)]
async fn test_setup_with_reboot() {
    // Prepopulate initial value
    let initial_data = SetupInfo {
        configuration_interfaces: ConfigurationInterfaceFlags::WIFI
            | ConfigurationInterfaceFlags::ETHERNET,
    };
    // Ethernet and WiFi is written out as initial value since the default
    // is currently WiFi only.
    let storage_factory = Arc::new(InMemoryStorageFactory::with_initial_data(&initial_data));

    let service_registry = ServiceRegistry::create();
    let hardware_power_statecontrol_service_handle =
        Arc::new(Mutex::new(HardwarePowerStatecontrolService::new()));
    service_registry
        .lock()
        .await
        .register_service(hardware_power_statecontrol_service_handle.clone());

    // Handle reboot
    let env = EnvironmentBuilder::new(Arc::clone(&storage_factory))
        .service(ServiceRegistry::serve(service_registry.clone()))
        .fidl_interfaces(&[Interface::Setup])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let setup_service = env.connect_to_protocol::<SetupMarker>().unwrap();

    // Ensure retrieved value matches set value
    let settings = setup_service.watch().await.expect("watch completed");
    assert_eq!(
        settings.enabled_configuration_interfaces,
        Some(
            fidl_fuchsia_settings::ConfigurationInterfaces::WIFI
                | fidl_fuchsia_settings::ConfigurationInterfaces::ETHERNET
        )
    );

    let expected_interfaces = fidl_fuchsia_settings::ConfigurationInterfaces::ETHERNET;

    // Ensure setting interface propagates  change correctly
    let mut setup_settings = fidl_fuchsia_settings::SetupSettings::EMPTY;
    setup_settings.enabled_configuration_interfaces = Some(expected_interfaces);
    setup_service.set(setup_settings, true).await.expect("set completed").expect("set successful");

    // Ensure retrieved value matches set value
    let settings = setup_service.watch().await.expect("watch completed");
    assert_eq!(settings.enabled_configuration_interfaces, Some(expected_interfaces));

    let store = storage_factory.get_device_storage().await;
    // Check to make sure value wrote out to store correctly
    assert_eq!(
        store.get::<SetupInfo>().await.configuration_interfaces,
        ConfigurationInterfaceFlags::ETHERNET
    );

    // Ensure reboot was requested by the controller
    assert!(hardware_power_statecontrol_service_handle
        .lock()
        .await
        .verify_action_sequence([Action::Reboot].to_vec()));
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_setup_no_reboot_with_set() {
    let service_registry = ServiceRegistry::create();
    let hardware_power_statecontrol_service_handle =
        Arc::new(Mutex::new(HardwarePowerStatecontrolService::new()));
    service_registry
        .lock()
        .await
        .register_service(hardware_power_statecontrol_service_handle.clone());

    let env = EnvironmentBuilder::new(Arc::new(InMemoryStorageFactory::new()))
        .fidl_interfaces(&[Interface::Setup])
        .spawn_and_get_nested_environment(ENV_NAME)
        .await
        .unwrap();

    let setup_service = env.connect_to_protocol::<SetupMarker>().unwrap();

    // Ensure retrieved value matches set value
    let settings = setup_service.watch().await.expect("watch completed");
    assert_eq!(
        settings.enabled_configuration_interfaces,
        Some(fidl_fuchsia_settings::ConfigurationInterfaces::WIFI)
    );

    let expected_interfaces = fidl_fuchsia_settings::ConfigurationInterfaces::ETHERNET;

    // Ensure setting interface propagates  change correctly
    let mut setup_settings = fidl_fuchsia_settings::SetupSettings::EMPTY;
    setup_settings.enabled_configuration_interfaces = Some(expected_interfaces);
    setup_service.set(setup_settings, false).await.expect("set completed").expect("set successful");

    // Ensure retrieved value matches set value
    let settings = setup_service.watch().await.expect("watch completed");
    assert_eq!(settings.enabled_configuration_interfaces, Some(expected_interfaces));

    // No reboot is called.
    assert!(hardware_power_statecontrol_service_handle
        .lock()
        .await
        .verify_action_sequence([].to_vec()));
}
