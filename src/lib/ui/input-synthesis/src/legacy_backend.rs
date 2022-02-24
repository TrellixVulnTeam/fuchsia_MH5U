// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use {
    crate::{synthesizer, usages::Usages},
    anyhow::Error,
    async_trait::async_trait,
    fidl::endpoints,
    fidl_fuchsia_ui_input::{
        self, Axis, AxisScale, DeviceDescriptor, InputDeviceMarker, InputDeviceProxy,
        InputDeviceRegistryMarker, InputReport, KeyboardDescriptor, KeyboardReport,
        MediaButtonsDescriptor, MediaButtonsReport, Range, Touch, TouchscreenDescriptor,
        TouchscreenReport,
    },
    fuchsia_component as app,
};

// Provides a handle to an `impl synthesizer::InputDeviceRegistry`, which works with input
// pipelines that support the (legacy) `fuchsia.ui.input.InputDeviceRegistry` protocol.
pub struct InputDeviceRegistry {
    svc_dir_path: Option<String>,
}

// Wraps `DeviceDescriptor` FIDL table fields for descriptors into a single Rust type,
// allowing us to pass any of them to `self.register_device()`.
#[derive(Debug)]
enum UniformDeviceDescriptor {
    Keyboard(KeyboardDescriptor),
    MediaButtons(MediaButtonsDescriptor),
    Touchscreen(TouchscreenDescriptor),
}

impl synthesizer::InputDeviceRegistry for self::InputDeviceRegistry {
    fn add_touchscreen_device(
        &mut self,
        width: u32,
        height: u32,
    ) -> Result<Box<dyn synthesizer::InputDevice>, Error> {
        self.add_device(UniformDeviceDescriptor::Touchscreen(TouchscreenDescriptor {
            x: Axis {
                range: Range { min: 0, max: width as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            y: Axis {
                range: Range { min: 0, max: height as i32 },
                resolution: 1,
                scale: AxisScale::Linear,
            },
            max_finger_id: 255,
        }))
    }

    fn add_keyboard_device(&mut self) -> Result<Box<dyn synthesizer::InputDevice>, Error> {
        self.add_device(UniformDeviceDescriptor::Keyboard(KeyboardDescriptor {
            keys: (Usages::HidUsageKeyA as u32..Usages::HidUsageKeyRightGui as u32).collect(),
        }))
    }

    fn add_media_buttons_device(&mut self) -> Result<Box<dyn synthesizer::InputDevice>, Error> {
        self.add_device(UniformDeviceDescriptor::MediaButtons(MediaButtonsDescriptor {
            buttons: fidl_fuchsia_ui_input::MIC_MUTE
                | fidl_fuchsia_ui_input::VOLUME_DOWN
                | fidl_fuchsia_ui_input::VOLUME_UP,
        }))
    }

    fn add_mouse_device(
        &mut self,
        _width: u32,
        _height: u32,
    ) -> Result<Box<dyn synthesizer::InputDevice>, Error> {
        unimplemented!(
            "Mouse device is not supported for tests requiring input via Root Presenter."
        )
    }
}

impl InputDeviceRegistry {
    // Create a new injector. Use default path to the service directory
    // containing the InputDeviceRegistry protocol.
    pub fn new() -> Self {
        Self { svc_dir_path: None }
    }

    // Create a new injector. Use |svc_dir_path| as the custom path to
    // the service directory containing the InputDeviceRegistry protocol.
    pub fn new_with_path(svc_dir_path: String) -> Self {
        Self { svc_dir_path: Some(svc_dir_path) }
    }

    fn add_device(
        &self,
        descriptor: UniformDeviceDescriptor,
    ) -> Result<Box<dyn synthesizer::InputDevice>, Error> {
        let registry = if let Some(path) = &self.svc_dir_path {
            app::client::connect_to_protocol_at::<InputDeviceRegistryMarker>(path.as_str())?
        } else {
            app::client::connect_to_protocol::<InputDeviceRegistryMarker>()?
        };
        let mut device = DeviceDescriptor {
            device_info: None,
            keyboard: None,
            media_buttons: None,
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
        };

        match descriptor {
            UniformDeviceDescriptor::Keyboard(descriptor) => {
                device.keyboard = Some(Box::new(descriptor))
            }
            UniformDeviceDescriptor::Touchscreen(descriptor) => {
                device.touchscreen = Some(Box::new(descriptor))
            }
            UniformDeviceDescriptor::MediaButtons(descriptor) => {
                device.media_buttons = Some(Box::new(descriptor))
            }
        };

        let (input_device_client, input_device_server) =
            endpoints::create_endpoints::<InputDeviceMarker>()?;
        registry.register_device(&mut device, input_device_server)?;
        Ok(Box::new(InputDevice::new(input_device_client.into_proxy()?)))
    }
}

// Provides a handle to an `impl synthesizer::InputDevice`, which works with input
// pipelines that support the (legacy) `fuchsia.ui.input.InputDeviceRegistry` protocol.
struct InputDevice {
    fidl_proxy: InputDeviceProxy,
}

#[async_trait(?Send)]
impl synthesizer::InputDevice for self::InputDevice {
    fn media_buttons(
        &mut self,
        pressed_buttons: Vec<synthesizer::MediaButton>,
        time: u64,
    ) -> Result<(), Error> {
        self.fidl_proxy
            .dispatch_report(&mut self::media_buttons(pressed_buttons, time))
            .map_err(Into::into)
    }

    fn key_press(&mut self, keyboard: KeyboardReport, time: u64) -> Result<(), Error> {
        self.key_press_raw(keyboard, time)
    }

    /// See [crate::synthesizer::InputDevice::key_press_raw].
    fn key_press_raw(&mut self, keyboard: KeyboardReport, time: u64) -> Result<(), Error> {
        self.fidl_proxy.dispatch_report(&mut self::key_press(keyboard, time)).map_err(Into::into)
    }

    fn key_press_usage(&mut self, usage: Option<u32>, time: u64) -> Result<(), Error> {
        self.fidl_proxy.dispatch_report(&mut self::key_press_usage(usage, time)).map_err(Into::into)
    }

    fn tap(&mut self, pos: Option<(u32, u32)>, time: u64) -> Result<(), Error> {
        self.fidl_proxy.dispatch_report(&mut self::tap(pos, time)).map_err(Into::into)
    }

    fn multi_finger_tap(&mut self, fingers: Option<Vec<Touch>>, time: u64) -> Result<(), Error> {
        self.fidl_proxy
            .dispatch_report(&mut self::multi_finger_tap(fingers, time))
            .map_err(Into::into)
    }

    fn mouse(
        &mut self,
        _movement: Option<(u32, u32)>,
        _pressed_buttons: Vec<synthesizer::MouseButton>,
        _time: u64,
    ) -> Result<(), Error> {
        unimplemented!(
            "Mouse input injection is not supported for tests requiring input via Root Presenter."
        )
    }

    async fn serve_reports(self: Box<Self>) -> Result<(), Error> {
        Ok(())
    }
}

impl InputDevice {
    fn new(fidl_proxy: InputDeviceProxy) -> Self {
        Self { fidl_proxy }
    }
}

fn media_buttons(pressed_buttons: Vec<synthesizer::MediaButton>, time: u64) -> InputReport {
    let pressed_buttons: std::collections::BTreeSet<_> = pressed_buttons.into_iter().collect();
    InputReport {
        event_time: time,
        keyboard: None,
        media_buttons: Some(Box::new(MediaButtonsReport {
            volume_up: pressed_buttons.contains(&synthesizer::MediaButton::VolumeUp),
            volume_down: pressed_buttons.contains(&synthesizer::MediaButton::VolumeDown),
            mic_mute: pressed_buttons.contains(&synthesizer::MediaButton::MicMute),
            reset: pressed_buttons.contains(&synthesizer::MediaButton::FactoryReset),
            pause: pressed_buttons.contains(&synthesizer::MediaButton::Pause),
            camera_disable: pressed_buttons.contains(&synthesizer::MediaButton::CameraDisable),
        })),
        mouse: None,
        stylus: None,
        touchscreen: None,
        sensor: None,
        trace_id: 0,
    }
}

fn key_press(keyboard: KeyboardReport, time: u64) -> InputReport {
    InputReport {
        event_time: time,
        keyboard: Some(Box::new(keyboard)),
        media_buttons: None,
        mouse: None,
        stylus: None,
        touchscreen: None,
        sensor: None,
        trace_id: 0,
    }
}

fn key_press_usage(usage: Option<u32>, time: u64) -> InputReport {
    key_press(
        KeyboardReport {
            pressed_keys: match usage {
                Some(usage) => vec![usage],
                None => vec![],
            },
        },
        time,
    )
}

fn tap(pos: Option<(u32, u32)>, time: u64) -> InputReport {
    match pos {
        Some((x, y)) => multi_finger_tap(
            Some(vec![Touch { finger_id: 1, x: x as i32, y: y as i32, width: 0, height: 0 }]),
            time,
        ),
        None => multi_finger_tap(None, time),
    }
}

fn multi_finger_tap(fingers: Option<Vec<Touch>>, time: u64) -> InputReport {
    InputReport {
        event_time: time,
        keyboard: None,
        media_buttons: None,
        mouse: None,
        stylus: None,
        touchscreen: Some(Box::new(TouchscreenReport {
            touches: match fingers {
                Some(fingers) => fingers,
                None => vec![],
            },
        })),
        sensor: None,
        trace_id: 0,
    }
}

#[cfg(test)]
mod tests {
    use {
        super::*, assert_matches::assert_matches, fidl::endpoints,
        fidl_fuchsia_ui_input::InputDeviceRequest, fuchsia_async as fasync, futures::StreamExt,
        std::task::Poll, synthesizer::InputDevice as _,
    };

    #[fasync::run_until_stalled(test)]
    async fn media_buttons_populates_empty_report_correctly() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.media_buttons(vec![], 100)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        let expected_report = InputReport {
            event_time: 100,
            keyboard: None,
            media_buttons: Some(Box::new(MediaButtonsReport {
                volume_up: false,
                volume_down: false,
                mic_mute: false,
                reset: false,
                pause: false,
                camera_disable: false,
            })),
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
            trace_id: 0,
        };
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport { report, .. })] if *report == expected_report
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn media_buttons_populates_full_report_correctly() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.media_buttons(
            vec![
                synthesizer::MediaButton::VolumeUp,
                synthesizer::MediaButton::VolumeDown,
                synthesizer::MediaButton::MicMute,
                synthesizer::MediaButton::FactoryReset,
                synthesizer::MediaButton::Pause,
                synthesizer::MediaButton::CameraDisable,
            ],
            100,
        )?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        let expected_report = InputReport {
            event_time: 100,
            keyboard: None,
            media_buttons: Some(Box::new(MediaButtonsReport {
                volume_up: true,
                volume_down: true,
                mic_mute: true,
                reset: true,
                pause: true,
                camera_disable: true,
            })),
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
            trace_id: 0,
        };
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport { report, .. })] if *report == expected_report
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn media_buttons_populates_partial_report_correctly() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.media_buttons(
            vec![
                synthesizer::MediaButton::VolumeUp,
                synthesizer::MediaButton::MicMute,
                synthesizer::MediaButton::Pause,
            ],
            100,
        )?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        let expected_report = InputReport {
            event_time: 100,
            keyboard: None,
            media_buttons: Some(Box::new(MediaButtonsReport {
                volume_up: true,
                volume_down: false,
                mic_mute: true,
                reset: false,
                pause: true,
                camera_disable: false,
            })),
            mouse: None,
            stylus: None,
            touchscreen: None,
            sensor: None,
            trace_id: 0,
        };
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport { report, .. })] if *report == expected_report
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn key_press_populates_report_correctly() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.key_press(KeyboardReport { pressed_keys: vec![1, 2, 3] }, 200)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 200,
                        keyboard: Some(report),
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: None,
                        sensor: None,
                        trace_id: 0
                    },
                ..
            })] if **report == KeyboardReport { pressed_keys: vec![1, 2, 3] }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn key_press_usage_populates_report_correctly_when_a_key_is_pressed() -> Result<(), Error>
    {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.key_press_usage(Some(1), 300)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 300,
                        keyboard: Some(report),
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: None,
                        sensor: None,
                        trace_id: 0
                    },
                ..
            })] if **report == KeyboardReport { pressed_keys: vec![1] }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn key_press_usage_populates_report_correctly_when_no_key_is_pressed() -> Result<(), Error>
    {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.key_press_usage(None, 400)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 400,
                        keyboard: Some(report),
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: None,
                        sensor: None,
                        trace_id: 0
                    },
                ..
            })] if ** report == KeyboardReport { pressed_keys: vec![] }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn tap_populates_report_correctly_when_finger_is_present() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.tap(Some((10, 20)), 500)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 500,
                        keyboard: None,
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: Some(report),
                        sensor: None,
                        trace_id: 0
                    },
                ..
            })] if **report == TouchscreenReport {
                touches: vec![Touch { finger_id: 1, x: 10, y: 20, width: 0, height: 0 },]
            }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn tap_populates_report_correctly_when_finger_is_absent() -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.tap(None, 600)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 600,
                        keyboard: None,
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: Some(report),
                        sensor: None,
                        trace_id: 0
                    },
                    ..
            })] if **report == TouchscreenReport { touches: vec![] }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn multi_finger_tap_populates_report_correctly_when_fingers_are_present(
    ) -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.multi_finger_tap(
            Some(vec![
                Touch { finger_id: 1, x: 99, y: 100, width: 10, height: 20 },
                Touch { finger_id: 2, x: 199, y: 201, width: 30, height: 40 },
            ]),
            700,
        )?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 700,
                        keyboard: None,
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: Some(report),
                        sensor: None,
                        trace_id: 0
                    },
                    ..
            })] if **report == TouchscreenReport {
                touches: vec![
                    Touch { finger_id: 1, x: 99, y: 100, width: 10, height: 20 },
                    Touch { finger_id: 2, x: 199, y: 201, width: 30, height: 40 }
                ]
            }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    async fn multi_finger_tap_populates_report_correctly_when_no_fingers_are_present(
    ) -> Result<(), Error> {
        let (fidl_proxy, request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = InputDevice { fidl_proxy };
        input_device.multi_finger_tap(None, 800)?;
        std::mem::drop(input_device); // Close channel to terminate stream.

        let reports = request_stream.collect::<Vec<_>>().await;
        assert_matches!(
            reports.as_slice(),
            [Ok(InputDeviceRequest::DispatchReport {
                report:
                    InputReport {
                        event_time: 800,
                        keyboard: None,
                        media_buttons: None,
                        mouse: None,
                        stylus: None,
                        touchscreen: Some(report),
                        sensor: None,
                        trace_id: 0
                    },
                    ..
            })] if **report == TouchscreenReport { touches: vec![] }
        );
        Ok(())
    }

    #[fasync::run_until_stalled(test)]
    #[should_panic(
        expected = "not implemented: Mouse input injection is not supported for tests requiring input via Root Presenter."
    )]
    // TODO(https://fxbug.dev/88496): delete the below
    #[cfg_attr(feature = "variant_asan", ignore)]
    async fn mouse_is_not_implemented() {
        let (fidl_proxy, _request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()
                .expect("Failed to create InputDeviceProxy.");
        let mut input_device = InputDevice { fidl_proxy };
        let _ = input_device.mouse(Some((10, 15)), vec![1, 2, 3], 200);
    }

    #[test]
    fn serve_reports_resolves_immediately() -> Result<(), Error> {
        let mut executor =
            fasync::TestExecutor::new().expect("internal error: failed to create executor");
        let (fidl_proxy, _request_stream) =
            endpoints::create_proxy_and_stream::<InputDeviceMarker>()?;
        let mut input_device = Box::new(InputDevice { fidl_proxy });
        input_device.multi_finger_tap(None, 900)?; // Sends `InputReport`.
        assert_matches!(
            executor.run_until_stalled(&mut input_device.serve_reports()),
            Poll::Ready(Ok(()))
        );
        Ok(())
    }
}
