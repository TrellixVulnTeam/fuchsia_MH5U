// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::agent::storage::device_storage::testing::InMemoryStorageFactory;
use crate::agent::storage::device_storage::DeviceStorage;
use crate::agent::BlueprintHandle;
use crate::base::SettingType;
use crate::config::base::AgentType;
use crate::handler::base::{Context, GenerateHandler};
use crate::handler::setting_handler::persist::ClientProxy;
use crate::handler::setting_handler::{BoxedController, ClientImpl};
use crate::ingress::fidl::Interface;
use crate::input::common::MediaButtonsEventBuilder;
use crate::light::light_controller::LightController;
use crate::light::light_hardware_configuration::{DisableConditions, LightGroupConfiguration};
use crate::light::types::{ColorRgb, LightGroup, LightInfo, LightState, LightType, LightValue};
use crate::tests::fakes::hardware_light_service::HardwareLightService;
use crate::tests::fakes::input_device_registry_service::InputDeviceRegistryService;
use crate::tests::fakes::service_registry::ServiceRegistry;
use crate::{EnvironmentBuilder, LightHardwareConfiguration};
use fidl_fuchsia_settings::{LightError, LightMarker, LightProxy};
use futures::lock::Mutex;
use std::option::Option::Some;
use std::sync::Arc;

type HardwareLightServiceHandle = Arc<Mutex<HardwareLightService>>;

const ENV_NAME: &str = "settings_service_light_test_environment";

const LIGHT_NAME_1: &str = "light_name_1";
const LIGHT_NAME_2: &str = "light_name_2";
const LIGHT_NAME_3: &str = "light_name_3";

fn get_test_light_info() -> LightInfo {
    LightInfo {
        light_groups: [
            (
                LIGHT_NAME_1.to_string(),
                LightGroup {
                    name: LIGHT_NAME_1.to_string(),
                    enabled: true,
                    light_type: LightType::Brightness,
                    lights: vec![LightState { value: Some(LightValue::Brightness(0.42)) }],
                    hardware_index: vec![0],
                    disable_conditions: vec![],
                },
            ),
            (
                LIGHT_NAME_2.to_string(),
                LightGroup {
                    name: LIGHT_NAME_2.to_string(),
                    enabled: true,
                    light_type: LightType::Simple,
                    lights: vec![LightState { value: Some(LightValue::Simple(true)) }],
                    hardware_index: vec![1],
                    disable_conditions: vec![],
                },
            ),
        ]
        .into(),
    }
}

/// Populates the given HardwareLightService fake with the lights from a LightInfo.
///
/// Assumes that each light group has one light, as if the light groups were read from the
/// underlying fuchsia.hardware.light API.
async fn populate_single_test_lights(
    hardware_light_service_handle: HardwareLightServiceHandle,
    light_info: LightInfo,
) {
    for (name, group) in light_info.light_groups.into_iter() {
        hardware_light_service_handle
            .lock()
            .await
            .insert_light(
                group.hardware_index[0],
                name,
                group.light_type,
                group.lights[0].value.clone().unwrap(),
            )
            .await;
    }
}

/// Populates the given HardwareLightService fake with the lights from a LightInfo.
///
/// For each light group, adds a new light with the light's index in the LightGroup appended to it.
/// For example a light group with name RED and 3 lights would result in RED_1, RED_2, RED_3 being
/// inserted into the fake service.
async fn populate_multiple_test_lights(
    hardware_light_service_handle: HardwareLightServiceHandle,
    light_info: LightInfo,
) {
    for (name, group) in light_info.light_groups.into_iter() {
        for i in 0..group.hardware_index.len() {
            hardware_light_service_handle
                .lock()
                .await
                .insert_light(
                    group.hardware_index[i],
                    format!("{}_{}", name, i),
                    group.light_type,
                    group.lights[i].value.clone().unwrap(),
                )
                .await;
        }
    }
}

struct TestLightEnvironment {
    light_service: LightProxy,
    input_service: Arc<Mutex<InputDeviceRegistryService>>,
    store: Arc<DeviceStorage>,
}

struct TestLightEnvironmentBuilder {
    starting_light_info: Option<LightInfo>,
    hardware_light_service_handle: Option<HardwareLightServiceHandle>,
    light_hardware_config: Option<LightHardwareConfiguration>,
    agents: Vec<AgentType>,
}

impl TestLightEnvironmentBuilder {
    fn new() -> Self {
        Self {
            starting_light_info: None,
            hardware_light_service_handle: None,
            light_hardware_config: None,
            agents: vec![AgentType::Restore],
        }
    }

    /// Sets the initial light info that the fake `HardwareLightService` and device storage will
    /// use.
    fn set_starting_light_info(mut self, starting_light_info: LightInfo) -> Self {
        self.starting_light_info = Some(starting_light_info);
        self
    }

    /// Provides a handle to a custom fake `HardwareLightService`. This only works in conjunction
    /// with specifying `starting_light_info`.
    fn set_hardware_light_service_handle(
        mut self,
        hardware_light_service_handle: HardwareLightServiceHandle,
    ) -> Self {
        self.hardware_light_service_handle = Some(hardware_light_service_handle);
        self
    }

    fn set_light_hardware_config(
        mut self,
        light_hardware_config: LightHardwareConfiguration,
    ) -> Self {
        self.light_hardware_config = Some(light_hardware_config);
        self
    }

    fn add_agent(mut self, agent: AgentType) -> Self {
        self.agents.push(agent);
        self
    }

    async fn build(self) -> TestLightEnvironment {
        let service_registry = ServiceRegistry::create();
        let service_handle = match self.hardware_light_service_handle.clone() {
            Some(service) => service,
            None => Arc::new(Mutex::new(HardwareLightService::new())),
        };
        service_registry.lock().await.register_service(service_handle.clone());

        // Register fake input device registry service.
        let input_service_handle = Arc::new(Mutex::new(InputDeviceRegistryService::new()));
        service_registry.lock().await.register_service(input_service_handle.clone());
        let storage_factory = Arc::new(if let Some(info) = self.starting_light_info {
            let storage_factory = InMemoryStorageFactory::with_initial_data(&info);
            if self.hardware_light_service_handle.is_none() {
                // If a fake hardware light service wasn't provided for us, populate the initial lights.
                populate_single_test_lights(service_handle.clone(), info).await;
            }
            storage_factory
        } else {
            InMemoryStorageFactory::new()
        });

        let mut environment_builder = EnvironmentBuilder::new(Arc::clone(&storage_factory))
            .service(Box::new(ServiceRegistry::serve(service_registry)))
            .agents(&self.agents.into_iter().map(BlueprintHandle::from).collect::<Vec<_>>())
            .fidl_interfaces(&[Interface::Light]);

        if let Some(config) = self.light_hardware_config {
            // If hardware configuration was specified, we need a generate_handler to include the
            // specified configuration. This generate_handler method is a copy-paste of
            // persist::controller::spawn from setting_handler.rs, with the innermost controller
            // create method replaced with our custome constructor from light controller.
            // TODO(fxbug.dev/63832): See if we can reduce this rightward drift.
            let generate_handler: GenerateHandler = Box::new(move |context: Context| {
                let config = config.clone();
                Box::pin(async move {
                    let setting_type = context.setting_type;
                    ClientImpl::create(
                        context,
                        Box::new(move |proxy| {
                            let config = config.clone();
                            Box::pin(async move {
                                let proxy = ClientProxy::new(proxy, setting_type).await;
                                let controller_result = LightController::create_with_config(
                                    proxy,
                                    Some(config.clone()),
                                )
                                .await;

                                controller_result
                                    .map(|controller| Box::new(controller) as BoxedController)
                            })
                        }),
                    )
                    .await
                })
            });

            environment_builder = environment_builder.handler(SettingType::Light, generate_handler);
        }

        let env = environment_builder.spawn_and_get_nested_environment(ENV_NAME).await.unwrap();

        let light_service = env.connect_to_protocol::<LightMarker>().unwrap();
        let store = storage_factory.get_device_storage().await;

        TestLightEnvironment { light_service, store, input_service: input_service_handle }
    }
}

async fn set_light_value(service: &LightProxy, light_group: LightGroup) {
    service
        .set_light_group_values(
            light_group.name.as_str(),
            &mut light_group.lights.into_iter().map(LightState::into),
        )
        .await
        .expect("set completed")
        .expect("set successful");
}

/// Compares a vector of light group from the settings FIDL API and the light groups from a
/// service-internal LightInfo object for equality.
fn assert_lights_eq(mut groups: Vec<fidl_fuchsia_settings::LightGroup>, info: LightInfo) {
    // Watch returns vector, internally we use a HashMap, so convert into a vector for comparison.
    let mut expected_value = info
        .light_groups
        .into_iter()
        .map(|(_, value)| fidl_fuchsia_settings::LightGroup::from(value))
        .collect::<Vec<_>>();

    assert_eq!(groups.len(), expected_value.len());
    // Sort by names for stability
    groups.sort_by_key(|group: &fidl_fuchsia_settings::LightGroup| group.name.clone());
    expected_value.sort_by_key(|group| group.name.clone());
    for i in 0..groups.len() {
        assert_fidl_light_group_eq(&groups[i], &expected_value[i]);
    }
}

fn assert_fidl_light_group_eq(
    left: &fidl_fuchsia_settings::LightGroup,
    right: &fidl_fuchsia_settings::LightGroup,
) {
    assert_eq!(left.name, right.name);
    assert_eq!(left.enabled, right.enabled);
    assert_eq!(left.type_, right.type_);
    assert_eq!(left.lights.as_ref().unwrap().len(), right.lights.as_ref().unwrap().len());
    if left.type_ == Some(fidl_fuchsia_settings::LightType::Simple) {
        assert_eq!(left.lights, right.lights);
    }
}

fn assert_light_group_eq(left: &LightGroup, right: &LightGroup) {
    assert_eq!(left.name, right.name);
    assert_eq!(left.enabled, right.enabled);
    assert_eq!(left.light_type, right.light_type);
    assert_eq!(left.lights.len(), right.lights.len());
    assert_eq!(left.hardware_index, right.hardware_index);
    if left.light_type == LightType::Simple {
        assert_eq!(left.lights, right.lights);
    }
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_light_restore() {
    // Populate the fake service with the initial lights that will be restored.
    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    populate_single_test_lights(hardware_light_service_handle.clone(), get_test_light_info()).await;

    let env = TestLightEnvironmentBuilder::new()
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .build()
        .await;

    let expected_light_group = get_test_light_info().light_groups.remove(LIGHT_NAME_1).unwrap();

    // Verify that the restored value is persisted.
    let retrieved_struct = env.store.get::<LightInfo>().await;
    assert_light_group_eq(
        &expected_light_group.clone(),
        retrieved_struct.light_groups.get(LIGHT_NAME_1).unwrap(),
    );

    // Verify that the restored value is returned on a watch call.
    let settings: fidl_fuchsia_settings::LightGroup =
        env.light_service.watch_light_group(LIGHT_NAME_1).await.expect("watch completed");
    assert_fidl_light_group_eq(
        &fidl_fuchsia_settings::LightGroup::from(expected_light_group),
        &settings,
    );
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_light_restores_on_watch() {
    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));

    let env = TestLightEnvironmentBuilder::new()
        .set_hardware_light_service_handle(hardware_light_service_handle.clone())
        .build()
        .await;

    // Don't populate the fake service with lights until after the service starts.
    let expected_light_info = get_test_light_info();
    populate_single_test_lights(hardware_light_service_handle, expected_light_info.clone()).await;

    // Upon a watch call, light controller will read the underlying value from the fake service.
    let settings = env.light_service.watch_light_groups().await.expect("watch completed");
    assert_lights_eq(settings, expected_light_info);
}

// Tests that when a `LightHardwareConfiguration` is specified, that the service will restore
// lights based on the configuration, while still reading light values from the underlying hardware.
#[fuchsia_async::run_until_stalled(test)]
async fn test_light_restore_from_configuration() {
    // Populate the fake service with an initial set of lights.
    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    populate_single_test_lights(hardware_light_service_handle.clone(), get_test_light_info()).await;

    // Hardware config only includes on light group, basically a renamed version of LIGHT_NAME_1.
    let light_hardware_config = LightHardwareConfiguration {
        light_groups: vec![LightGroupConfiguration {
            name: LIGHT_NAME_3.to_string(),
            hardware_index: vec![0],
            light_type: LightType::Brightness,
            persist: false,
            disable_conditions: vec![],
        }],
    };

    let env = TestLightEnvironmentBuilder::new()
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .set_light_hardware_config(light_hardware_config)
        .build()
        .await;

    // We're expecting LIGHT_NAME_1 except it's been renamed to LIGHT_NAME_3 by the hardware config.
    let mut expected_light_group = get_test_light_info().light_groups.remove(LIGHT_NAME_1).unwrap();
    expected_light_group.name = LIGHT_NAME_3.to_string();

    // Verify that the restored value is persisted.
    let retrieved_struct = env.store.get::<LightInfo>().await;
    assert_light_group_eq(
        &expected_light_group.clone(),
        retrieved_struct.light_groups.get(LIGHT_NAME_3).unwrap(),
    );

    // Verify that the restored value is returned on a watch call.
    let settings: fidl_fuchsia_settings::LightGroup =
        env.light_service.watch_light_group(LIGHT_NAME_3).await.expect("watch completed");
    assert_fidl_light_group_eq(
        &fidl_fuchsia_settings::LightGroup::from(expected_light_group),
        &settings,
    );

    // None of the light names provided by in the hardware itself are valid as they're not in the
    // configuration.
    let _ = env.light_service.watch_light_group(LIGHT_NAME_1).await.expect_err("watch should fail");
    let _ = env.light_service.watch_light_group(LIGHT_NAME_2).await.expect_err("watch should fail");
}

// Tests that setting a light that's specified in a `LightHardwareConfiguration` also sets the value
// in the underlying hardware.
#[fuchsia_async::run_until_stalled(test)]
async fn test_light_set_from_configuration() {
    // Populate the fake service with an initial set of lights.
    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    populate_single_test_lights(hardware_light_service_handle.clone(), get_test_light_info()).await;

    // Hardware config only includes on light group, basically a renamed version of LIGHT_NAME_1.
    let light_hardware_config = LightHardwareConfiguration {
        light_groups: vec![LightGroupConfiguration {
            name: LIGHT_NAME_3.to_string(),
            hardware_index: vec![0],
            light_type: LightType::Brightness,
            persist: false,
            disable_conditions: vec![],
        }],
    };

    let mut changed_light_group =
        get_test_light_info().light_groups.get(LIGHT_NAME_1).unwrap().clone();
    // The light hardware configuration basically renames LIGHT_NAME_1 to LIGHT_NAME_3.
    changed_light_group.name = LIGHT_NAME_3.to_string();
    changed_light_group.lights = vec![LightState { value: Some(LightValue::Brightness(0.99)) }];
    let expected_light_info = LightInfo {
        light_groups: [(LIGHT_NAME_3.to_string(), changed_light_group.clone())].into(),
    };

    let env = TestLightEnvironmentBuilder::new()
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .set_light_hardware_config(light_hardware_config)
        .build()
        .await;

    // Set a light value.
    set_light_value(&env.light_service, changed_light_group).await;

    // Verify the value we set is persisted in DeviceStorage.
    assert_eq!(expected_light_info, env.store.get().await);

    // Ensure value from Watch matches set value.
    let settings: Vec<fidl_fuchsia_settings::LightGroup> =
        env.light_service.watch_light_groups().await.expect("watch completed");
    assert_lights_eq(settings, expected_light_info);
}

// Tests that when a `LightHardwareConfiguration` is specified, that light groups configured with
// `DisableConditions::MicSwitch` have their enabled bits set to off when the mic is unmuted.
#[fuchsia_async::run_until_stalled(test)]
async fn test_light_disabled_by_mic_mute_off() {
    // Populate the fake service with an initial set of lights.
    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    populate_single_test_lights(hardware_light_service_handle.clone(), get_test_light_info()).await;

    // Hardware config only includes on light group, basically a renamed version of LIGHT_NAME_1.
    let light_hardware_config = LightHardwareConfiguration {
        light_groups: vec![LightGroupConfiguration {
            name: LIGHT_NAME_3.to_string(),
            hardware_index: vec![0],
            light_type: LightType::Brightness,
            persist: false,
            disable_conditions: vec![DisableConditions::MicSwitch],
        }],
    };

    // We're expecting LIGHT_NAME_1 except it's been renamed to LIGHT_NAME_3 by the hardware config,
    // and the enabled bit will be set to off.
    let mut expected_light_group = get_test_light_info().light_groups.remove(LIGHT_NAME_1).unwrap();
    expected_light_group.name = LIGHT_NAME_3.to_string();
    expected_light_group.enabled = false;

    let env = TestLightEnvironmentBuilder::new()
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .add_agent(AgentType::MediaButtons)
        .set_light_hardware_config(light_hardware_config)
        .build()
        .await;

    // Send mic unmuted, which should disable the light.
    let buttons_event = MediaButtonsEventBuilder::new().set_mic_mute(false).build();
    env.input_service.lock().await.send_media_button_event(buttons_event).await;

    // Verify that the expected value is returned on a watch call.
    let settings: fidl_fuchsia_settings::LightGroup =
        env.light_service.watch_light_group(LIGHT_NAME_3).await.expect("watch completed");
    assert_fidl_light_group_eq(
        &fidl_fuchsia_settings::LightGroup::from(expected_light_group),
        &settings,
    );
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_light_store_and_watch() {
    let mut expected_light_info = get_test_light_info();
    let mut changed_light_group =
        expected_light_info.light_groups.get(LIGHT_NAME_1).unwrap().clone();
    changed_light_group.lights = vec![LightState { value: Some(LightValue::Brightness(0.128)) }];
    let _ = expected_light_info
        .light_groups
        .insert(LIGHT_NAME_1.to_string(), changed_light_group.clone());

    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;

    // Set a light value.
    set_light_value(&env.light_service, changed_light_group).await;

    // Verify the value we set is persisted in DeviceStorage.
    assert_eq!(expected_light_info, env.store.get().await);

    // Ensure value from Watch matches set value.
    let settings: Vec<fidl_fuchsia_settings::LightGroup> =
        env.light_service.watch_light_groups().await.expect("watch completed");
    assert_lights_eq(settings, expected_light_info);
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_light_set_wrong_size() {
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;

    // Light group only has one light, attempt to set two lights.
    let _ = env
        .light_service
        .set_light_group_values(
            LIGHT_NAME_1,
            &mut vec![
                LightState { value: Some(LightValue::Brightness(0.128)) },
                LightState { value: Some(LightValue::Brightness(0.11)) },
            ]
            .into_iter()
            .map(LightState::into),
        )
        .await
        .expect("set completed")
        .expect_err("set failed");
}

// Tests that when there are multiple lights, one light can be set at a time.
#[fuchsia_async::run_until_stalled(test)]
async fn test_light_set_single_light() {
    const TEST_LIGHT_NAME: &str = "multiple_lights";
    const LIGHT_1_VAL: f64 = 0.42;
    const LIGHT_2_START_VAL: f64 = 0.24;
    const LIGHT_2_CHANGED_VAL: f64 = 0.11;
    // For this test, create a light group with two lights to test setting one light at a time.
    let original_light_group = LightGroup {
        name: TEST_LIGHT_NAME.to_string(),
        enabled: true,
        light_type: LightType::Brightness,
        lights: vec![
            LightState { value: Some(LightValue::Brightness(LIGHT_1_VAL)) },
            LightState { value: Some(LightValue::Brightness(LIGHT_2_START_VAL)) },
        ],
        hardware_index: vec![0, 1],
        disable_conditions: vec![],
    };

    // When changing the light group, specify None for the first light.
    let mut changed_light_group = original_light_group.clone();
    changed_light_group.lights = vec![
        LightState { value: None },
        LightState { value: Some(LightValue::Brightness(LIGHT_2_CHANGED_VAL)) },
    ];

    // Only the second light should change.
    let mut expected_light_group = original_light_group.clone();
    expected_light_group.lights = vec![
        LightState { value: Some(LightValue::Brightness(LIGHT_1_VAL)) },
        LightState { value: Some(LightValue::Brightness(LIGHT_2_CHANGED_VAL)) },
    ];

    let starting_light_info =
        LightInfo { light_groups: [(TEST_LIGHT_NAME.to_string(), original_light_group)].into() };

    let mut expected_light_info = starting_light_info.clone();
    let _ = expected_light_info
        .light_groups
        .insert(TEST_LIGHT_NAME.to_string(), expected_light_group.clone());

    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));

    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(starting_light_info.clone())
        .set_hardware_light_service_handle(hardware_light_service_handle.clone())
        .build()
        .await;

    // Populate the fake service with the initial lights so the set calls aren't rejected.
    populate_multiple_test_lights(hardware_light_service_handle, starting_light_info).await;

    set_light_value(&env.light_service, changed_light_group).await;

    // Verify the value we set is persisted in DeviceStorage.
    assert_eq!(expected_light_info, env.store.get().await);

    // Ensure value from Watch matches set value.
    let mut settings: Vec<fidl_fuchsia_settings::LightGroup> =
        env.light_service.watch_light_groups().await.expect("watch completed");
    settings.sort_by_key(|group: &fidl_fuchsia_settings::LightGroup| group.name.clone());

    let settings =
        env.light_service.watch_light_group(TEST_LIGHT_NAME).await.expect("watch completed");
    assert_eq!(settings, expected_light_group.into());
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_individual_light_group() {
    let test_light_info = get_test_light_info();
    let light_group_1 = test_light_info.light_groups.get(LIGHT_NAME_1).unwrap().clone();
    let mut light_group_1_updated = light_group_1.clone();
    light_group_1_updated.lights = vec![LightState { value: Some(LightValue::Brightness(0.128)) }];

    let light_group_2 = test_light_info.light_groups.get(LIGHT_NAME_2).unwrap().clone();
    let mut light_group_2_updated = light_group_2.clone();
    light_group_2_updated.lights = vec![LightState { value: Some(LightValue::Simple(false)) }];

    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;
    let light_service = env.light_service;

    // Ensure values from Watch matches set values.
    let settings = light_service.watch_light_group(LIGHT_NAME_1).await.expect("watch completed");
    assert_fidl_light_group_eq(&settings, &light_group_1.into());
    let settings = light_service.watch_light_group(LIGHT_NAME_2).await.expect("watch completed");
    assert_fidl_light_group_eq(&settings, &light_group_2.into());

    // Set updated values for the two lights.
    set_light_value(&light_service, light_group_1_updated.clone()).await;

    let settings = light_service.watch_light_group(LIGHT_NAME_1).await.expect("watch completed");
    assert_fidl_light_group_eq(&settings, &light_group_1_updated.into());

    set_light_value(&light_service, light_group_2_updated.clone()).await;

    let settings = light_service.watch_light_group(LIGHT_NAME_2).await.expect("watch completed");
    assert_fidl_light_group_eq(&settings, &light_group_2_updated.into());
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_watch_unknown_light_group_name() {
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;

    // Unknown name should be rejected.
    let _ =
        env.light_service.watch_light_group("unknown_name").await.expect_err("watch should fail");
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_set_unknown_light_group_name() {
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;

    // Unknown name should be rejected.
    let result = env
        .light_service
        .set_light_group_values("unknown_name", &mut vec![].into_iter())
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidName));
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_set_wrong_state_length() {
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(get_test_light_info())
        .build()
        .await;

    // Set with no light state should fail.
    let result = env
        .light_service
        .set_light_group_values(LIGHT_NAME_1, &mut vec![].into_iter())
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));

    // Set with an extra light state should fail.
    let extra_state = vec![
        fidl_fuchsia_settings::LightState {
            value: None,
            ..fidl_fuchsia_settings::LightState::EMPTY
        },
        fidl_fuchsia_settings::LightState {
            value: None,
            ..fidl_fuchsia_settings::LightState::EMPTY
        },
    ];
    let result = env
        .light_service
        .set_light_group_values(LIGHT_NAME_1, &mut extra_state.into_iter())
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_set_wrong_value_type() {
    const TEST_LIGHT_NAME: &str = "multiple_lights";
    const LIGHT_START_VAL: f64 = 0.24;
    const LIGHT_CHANGED_VAL: f64 = 0.11;
    let original_light_group = LightGroup {
        name: TEST_LIGHT_NAME.to_string(),
        enabled: true,
        light_type: LightType::Brightness,
        lights: vec![
            LightState { value: Some(LightValue::Brightness(LIGHT_START_VAL)) },
            LightState { value: Some(LightValue::Brightness(LIGHT_START_VAL)) },
        ],
        hardware_index: vec![0, 1],
        disable_conditions: vec![],
    };
    let starting_light_info =
        LightInfo { light_groups: [(TEST_LIGHT_NAME.to_string(), original_light_group)].into() };

    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(starting_light_info.clone())
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .build()
        .await;

    // One of the light values is On instead of brightness, the set should fail.
    let result = env
        .light_service
        .set_light_group_values(
            TEST_LIGHT_NAME,
            &mut vec![
                fidl_fuchsia_settings::LightState {
                    value: None,
                    ..fidl_fuchsia_settings::LightState::EMPTY
                },
                fidl_fuchsia_settings::LightState {
                    value: Some(fidl_fuchsia_settings::LightValue::On(true)),
                    ..fidl_fuchsia_settings::LightState::EMPTY
                },
            ]
            .into_iter(),
        )
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));

    // One of the values is the right type, but the other is still wrong, the set should fail.
    let result = env
        .light_service
        .set_light_group_values(
            TEST_LIGHT_NAME,
            &mut vec![
                fidl_fuchsia_settings::LightState {
                    value: Some(fidl_fuchsia_settings::LightValue::Brightness(LIGHT_CHANGED_VAL)),
                    ..fidl_fuchsia_settings::LightState::EMPTY
                },
                fidl_fuchsia_settings::LightState {
                    value: Some(fidl_fuchsia_settings::LightValue::On(true)),
                    ..fidl_fuchsia_settings::LightState::EMPTY
                },
            ]
            .into_iter(),
        )
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));
}

#[fuchsia_async::run_until_stalled(test)]
async fn test_set_invalid_rgb_values() {
    const TEST_LIGHT_NAME: &str = "light";
    const LIGHT_START_VAL: f32 = 0.25;
    const INVALID_VAL_1: f32 = 1.1;
    const INVALID_VAL_2: f32 = -0.1;
    let original_light_group = LightGroup {
        name: TEST_LIGHT_NAME.to_string(),
        enabled: true,
        light_type: LightType::Rgb,
        lights: vec![LightState {
            value: Some(LightValue::Rgb(ColorRgb {
                red: LIGHT_START_VAL,
                green: LIGHT_START_VAL,
                blue: LIGHT_START_VAL,
            })),
        }],
        hardware_index: vec![0],
        disable_conditions: vec![],
    };
    let starting_light_info =
        LightInfo { light_groups: [(TEST_LIGHT_NAME.to_string(), original_light_group)].into() };

    let hardware_light_service_handle = Arc::new(Mutex::new(HardwareLightService::new()));
    let env = TestLightEnvironmentBuilder::new()
        .set_starting_light_info(starting_light_info.clone())
        .set_hardware_light_service_handle(hardware_light_service_handle)
        .build()
        .await;

    // One of the RGB components is too big, the set should fail.
    let result = env
        .light_service
        .set_light_group_values(
            TEST_LIGHT_NAME,
            &mut vec![fidl_fuchsia_settings::LightState {
                value: Some(fidl_fuchsia_settings::LightValue::Color(
                    fidl_fuchsia_ui_types::ColorRgb {
                        red: LIGHT_START_VAL,
                        green: LIGHT_START_VAL,
                        blue: INVALID_VAL_1,
                    },
                )),
                ..fidl_fuchsia_settings::LightState::EMPTY
            }]
            .into_iter(),
        )
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));

    // One of the RGB components is negative, the set should fail.
    let result = env
        .light_service
        .set_light_group_values(
            TEST_LIGHT_NAME,
            &mut vec![fidl_fuchsia_settings::LightState {
                value: Some(fidl_fuchsia_settings::LightValue::Color(
                    fidl_fuchsia_ui_types::ColorRgb {
                        red: LIGHT_START_VAL,
                        green: INVALID_VAL_2,
                        blue: LIGHT_START_VAL,
                    },
                )),
                ..fidl_fuchsia_settings::LightState::EMPTY
            }]
            .into_iter(),
        )
        .await
        .expect("set returns");
    assert_eq!(result, Err(LightError::InvalidValue));
}
