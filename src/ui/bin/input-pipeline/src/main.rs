// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    anyhow::Error,
    fidl_fuchsia_input_injection::InputDeviceRegistryRequestStream,
    fidl_fuchsia_recovery_policy::DeviceRequestStream,
    fidl_fuchsia_recovery_ui::FactoryResetCountdownRequestStream,
    fidl_fuchsia_ui_policy::DeviceListenerRegistryRequestStream as MediaButtonsListenerRegistryRequestStream,
    fuchsia_async as fasync,
    fuchsia_component::server::ServiceFs,
    fuchsia_syslog::fx_log_warn,
    futures::StreamExt,
    input_pipeline as input_pipeline_lib,
    input_pipeline::input_device,
    input_pipeline::{
        factory_reset_handler::FactoryResetHandler, media_buttons_handler::MediaButtonsHandler,
    },
    std::rc::Rc,
};

mod input_handlers;

const IGNORE_REAL_DEVICES_CONFIG_PATH: &'static str = "/config/data/ignore_real_devices";

enum ExposedServices {
    FactoryResetCountdown(FactoryResetCountdownRequestStream),
    InputDeviceRegistry(InputDeviceRegistryRequestStream),
    MediaButtonsListenerRegistry(MediaButtonsListenerRegistryRequestStream),
    RecoveryPolicy(DeviceRequestStream),
}

#[fasync::run_singlethreaded]
async fn main() -> Result<(), Error> {
    fuchsia_syslog::init_with_tags(&["input-pipeline"]).expect("Failed to init syslog");

    // Expose InputDeviceRegistry to allow input injection for testing.
    let mut fs = ServiceFs::new_local();
    fs.dir("svc").add_fidl_service(ExposedServices::FactoryResetCountdown);
    fs.dir("svc").add_fidl_service(ExposedServices::InputDeviceRegistry);
    fs.dir("svc").add_fidl_service(ExposedServices::MediaButtonsListenerRegistry);
    fs.dir("svc").add_fidl_service(ExposedServices::RecoveryPolicy);
    fs.take_and_serve_directory_handle()?;

    // Declare the types of input to support.
    let device_types =
        vec![input_device::InputDeviceType::Touch, input_device::InputDeviceType::ConsumerControls];

    // Create a new input pipeline.
    let factory_reset_handler = FactoryResetHandler::new();
    let media_buttons_handler = MediaButtonsHandler::new();
    let input_handlers =
        input_handlers::create(factory_reset_handler.clone(), media_buttons_handler.clone()).await;

    let input_pipeline = if std::path::Path::new(IGNORE_REAL_DEVICES_CONFIG_PATH).exists() {
        input_pipeline_lib::input_pipeline::InputPipeline::new_for_test(
            device_types.clone(),
            input_pipeline_lib::input_pipeline::InputPipelineAssembly::new()
                .add_all_handlers(input_handlers),
        )
    } else {
        input_pipeline_lib::input_pipeline::InputPipeline::new(
            device_types.clone(),
            input_pipeline_lib::input_pipeline::InputPipelineAssembly::new()
                .add_all_handlers(input_handlers)
                .add_autorepeater(),
        )
        .expect("Failed to create input pipeline")
    };

    let input_event_sender = input_pipeline.input_event_sender().clone();
    let bindings = input_pipeline.input_device_bindings().clone();

    // Handle input events.
    fasync::Task::local(input_pipeline.handle_input_events()).detach();

    // Handle incoming injection input devices. Start with u32::Max to avoid conflicting device ids.
    let mut injected_device_id = u32::MAX;
    while let Some(service_request) = fs.next().await {
        match service_request {
            ExposedServices::FactoryResetCountdown(stream) => {
                let factory_reset_countdown_fut = handle_factory_reset_countdown_request_stream(
                    factory_reset_handler.clone(),
                    stream,
                );

                fasync::Task::local(factory_reset_countdown_fut).detach();
            }
            ExposedServices::InputDeviceRegistry(stream) => {
                let input_device_registry_fut = handle_input_device_registry_request_stream(
                    stream,
                    device_types.clone(),
                    input_event_sender.clone(),
                    bindings.clone(),
                    injected_device_id,
                );

                fasync::Task::local(input_device_registry_fut).detach();
                injected_device_id -= 1;
            }
            ExposedServices::MediaButtonsListenerRegistry(stream) => {
                let device_listener_registry_fut =
                    handle_media_buttons_listener_registry(media_buttons_handler.clone(), stream);

                fasync::Task::local(device_listener_registry_fut).detach();
            }
            ExposedServices::RecoveryPolicy(stream) => {
                let recovery_policy_fut = handle_recovery_policy_device_request_stream(
                    factory_reset_handler.clone(),
                    stream,
                );

                fasync::Task::local(recovery_policy_fut).detach();
            }
        }
    }

    Ok(())
}

async fn handle_factory_reset_countdown_request_stream(
    factory_reset_handler: Rc<FactoryResetHandler>,
    stream: FactoryResetCountdownRequestStream,
) {
    if let Err(error) =
        factory_reset_handler.handle_factory_reset_countdown_request_stream(stream).await
    {
        fx_log_warn!("failure while serving FactoryResetCountdown: {}", error);
    }
}

async fn handle_input_device_registry_request_stream(
    stream: InputDeviceRegistryRequestStream,
    device_types: Vec<input_pipeline_lib::input_device::InputDeviceType>,
    input_event_sender: futures::channel::mpsc::Sender<
        input_pipeline_lib::input_device::InputEvent,
    >,
    bindings: input_pipeline_lib::input_pipeline::InputDeviceBindingHashMap,
    device_id: u32,
) {
    match input_pipeline_lib::input_pipeline::InputPipeline::handle_input_device_registry_request_stream(
        stream,
        &device_types,
        &input_event_sender,
        &bindings,
        device_id,
    )
    .await
    {
        Ok(()) => (),
        Err(e) => {
            fx_log_warn!(
                "failure while serving InputDeviceRegistry: {}; \
                     will continue serving other clients",
                e
            );
        }
    }
}

async fn handle_media_buttons_listener_registry(
    media_buttons_handler: Rc<MediaButtonsHandler>,
    stream: MediaButtonsListenerRegistryRequestStream,
) {
    match media_buttons_handler.handle_device_listener_registry_request_stream(stream).await {
        Ok(()) => (),
        Err(e) => {
            fx_log_warn!("failure while serving DeviceListenerRegistry: {}", e);
        }
    }
}

async fn handle_recovery_policy_device_request_stream(
    factory_reset_handler: Rc<FactoryResetHandler>,
    stream: DeviceRequestStream,
) {
    match factory_reset_handler.handle_recovery_policy_device_request_stream(stream).await {
        Ok(()) => (),
        Err(e) => {
            fx_log_warn!("failure while serving fuchsia.recovery.policy.Device: {}", e);
        }
    }
}
