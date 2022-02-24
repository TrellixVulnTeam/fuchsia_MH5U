// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::error::PowerManagerError;
use crate::log_if_err;
use crate::message::{Message, MessageReturn};
use crate::node::Node;
use crate::types::{Celsius, Nanoseconds, Seconds};
use crate::utils::connect_to_driver;
use anyhow::{format_err, Error};
use async_trait::async_trait;
use fidl_fuchsia_hardware_thermal as fthermal;
use fuchsia_async as fasync;
use fuchsia_inspect::{self as inspect, NumericProperty, Property};
use fuchsia_inspect_contrib::{inspect_log, nodes::BoundedListNode};
use fuchsia_zircon as zx;
use serde_derive::Deserialize;
use serde_json as json;
use std::cell::Cell;
use std::cell::RefCell;
use std::collections::HashMap;
use std::rc::Rc;

/// Node: TemperatureHandler
///
/// Summary: Responds to temperature requests from other nodes by polling the specified driver
///          using the thermal FIDL protocol. May be configured to cache the polled temperature
///          for a while to prevent excessive polling of the sensor. (Polling errors are not
///          cached.)
///
/// Handles Messages:
///     - ReadTemperature
///
/// Sends Messages: N/A
///
/// FIDL dependencies:
///     - fuchsia.hardware.thermal: the node uses this protocol to query the thermal driver
///       specified by `driver_path` in the TemperatureHandler constructor

/// A builder for constructing the TemperatureHandler node
pub struct TemperatureHandlerBuilder<'a> {
    driver_path: String,
    driver_proxy: Option<fthermal::DeviceProxy>,
    cache_duration: zx::Duration,
    inspect_root: Option<&'a inspect::Node>,
}

impl<'a> TemperatureHandlerBuilder<'a> {
    pub fn new(driver_path: String, cache_duration: zx::Duration) -> Self {
        Self { driver_path, driver_proxy: None, cache_duration, inspect_root: None }
    }

    #[cfg(test)]
    pub fn new_with_proxy(driver_path: String, proxy: fthermal::DeviceProxy) -> Self {
        Self {
            driver_path,
            driver_proxy: Some(proxy),
            cache_duration: zx::Duration::from_millis(0),
            inspect_root: None,
        }
    }

    pub fn new_from_json(json_data: json::Value, _nodes: &HashMap<String, Rc<dyn Node>>) -> Self {
        #[derive(Deserialize)]
        struct Config {
            driver_path: String,
            cache_duration_ms: u32,
        }

        #[derive(Deserialize)]
        struct JsonData {
            config: Config,
        }

        let data: JsonData = json::from_value(json_data).unwrap();
        Self::new(
            data.config.driver_path,
            zx::Duration::from_millis(data.config.cache_duration_ms as i64),
        )
    }

    #[cfg(test)]
    pub fn with_cache_duration(mut self, duration: zx::Duration) -> Self {
        self.cache_duration = duration;
        self
    }

    #[cfg(test)]
    pub fn with_inspect_root(mut self, root: &'a inspect::Node) -> Self {
        self.inspect_root = Some(root);
        self
    }

    pub async fn build(self) -> Result<Rc<TemperatureHandler>, Error> {
        // Optionally use the default proxy
        let proxy = if self.driver_proxy.is_none() {
            connect_to_driver::<fthermal::DeviceMarker>(&self.driver_path).await?
        } else {
            self.driver_proxy.unwrap()
        };

        // Optionally use the default inspect root node
        let inspect_root = self.inspect_root.unwrap_or(inspect::component::inspector().root());

        Ok(Rc::new(TemperatureHandler {
            driver_path: self.driver_path.clone(),
            driver_proxy: proxy,
            last_temperature: RefCell::new(Celsius(std::f64::NAN)),
            last_poll_time: RefCell::new(fasync::Time::INFINITE_PAST),
            cache_duration: self.cache_duration,
            inspect: InspectData::new(
                inspect_root,
                format!("TemperatureHandler ({})", self.driver_path),
            ),
        }))
    }
}

pub struct TemperatureHandler {
    driver_path: String,
    driver_proxy: fthermal::DeviceProxy,

    /// Last temperature returned by the handler.
    last_temperature: RefCell<Celsius>,

    /// Time of the last temperature poll, for determining cache freshness.
    last_poll_time: RefCell<fasync::Time>,

    /// Duration for which a polled temperature is cached. This prevents excessive polling of the
    /// sensor.
    cache_duration: zx::Duration,

    /// A struct for managing Component Inspection data
    inspect: InspectData,
}

impl TemperatureHandler {
    fn handle_get_driver_path(&self) -> Result<MessageReturn, PowerManagerError> {
        Ok(MessageReturn::GetDriverPath(self.driver_path.clone()))
    }

    async fn handle_read_temperature(&self) -> Result<MessageReturn, PowerManagerError> {
        fuchsia_trace::duration!(
            "power_manager",
            "TemperatureHandler::handle_read_temperature",
            "driver" => self.driver_path.as_str()
        );

        // If the last temperature value is sufficiently fresh, return it instead of polling.
        // Note that if the previous poll generated an error, `last_poll_time` was not updated,
        // and (barring clock glitches) a new poll will occur.
        if fasync::Time::now() <= *self.last_poll_time.borrow() + self.cache_duration {
            return Ok(MessageReturn::ReadTemperature(*self.last_temperature.borrow()));
        }

        let result = self.read_temperature().await;
        log_if_err!(
            result,
            format!("Failed to read temperature from {}", self.driver_path).as_str()
        );
        fuchsia_trace::instant!(
            "power_manager",
            "TemperatureHandler::read_temperature_result",
            fuchsia_trace::Scope::Thread,
            "driver" => self.driver_path.as_str(),
            "result" => format!("{:?}", result).as_str()
        );

        match result {
            Ok(temperature) => {
                *self.last_temperature.borrow_mut() = temperature;
                *self.last_poll_time.borrow_mut() = fasync::Time::now();
                self.inspect.log_temperature_reading(temperature);
                Ok(MessageReturn::ReadTemperature(temperature))
            }
            Err(e) => {
                self.inspect.read_errors.add(1);
                self.inspect.last_read_error.set(format!("{}", e).as_str());
                Err(e.into())
            }
        }
    }

    async fn read_temperature(&self) -> Result<Celsius, Error> {
        fuchsia_trace::duration!(
            "power_manager",
            "TemperatureHandler::read_temperature",
            "driver" => self.driver_path.as_str()
        );
        let (status, temperature) =
            self.driver_proxy.get_temperature_celsius().await.map_err(|e| {
                format_err!("{}: get_temperature_celsius IPC failed: {}", self.name(), e)
            })?;
        zx::Status::ok(status).map_err(|e| {
            format_err!("{}: get_temperature_celsius driver returned error: {}", self.name(), e)
        })?;
        Ok(Celsius(temperature.into()))
    }
}

#[async_trait(?Send)]
impl Node for TemperatureHandler {
    fn name(&self) -> String {
        format!("TemperatureHandler ({})", self.driver_path)
    }

    async fn handle_message(&self, msg: &Message) -> Result<MessageReturn, PowerManagerError> {
        match msg {
            Message::ReadTemperature => self.handle_read_temperature().await,
            Message::GetDriverPath => self.handle_get_driver_path(),
            _ => Err(PowerManagerError::Unsupported),
        }
    }
}

struct InspectData {
    temperature_readings: RefCell<BoundedListNode>,
    read_errors: inspect::UintProperty,
    last_read_error: inspect::StringProperty,
}

impl InspectData {
    /// Number of inspect samples to store in the `temperature_readings` BoundedListNode.
    // Store the last 60 seconds of temperature readings (fxbug.dev/59774)
    const NUM_INSPECT_TEMPERATURE_SAMPLES: usize = 60;

    fn new(parent: &inspect::Node, name: String) -> Self {
        // Create a local root node and properties
        let root = parent.create_child(name);
        let temperature_readings = RefCell::new(BoundedListNode::new(
            root.create_child("temperature_readings"),
            Self::NUM_INSPECT_TEMPERATURE_SAMPLES,
        ));
        let read_errors = root.create_uint("read_temperature_error_count", 0);
        let last_read_error = root.create_string("last_read_error", "");

        // Pass ownership of the new node to the parent node, otherwise it'll be dropped
        parent.record(root);

        InspectData { temperature_readings, read_errors, last_read_error }
    }

    fn log_temperature_reading(&self, temperature: Celsius) {
        inspect_log!(self.temperature_readings.borrow_mut(), temperature: temperature.0);
    }
}

#[cfg(test)]
pub mod tests {
    use super::*;
    use async_utils::PollExt as _;
    use fuchsia_inspect::testing::TreeAssertion;
    use futures::TryStreamExt;
    use inspect::assert_data_tree;
    use std::task::Poll;

    /// Spawns a new task that acts as a fake thermal driver for testing purposes. The driver only
    /// handles requests for GetTemperatureCelsius - trying to send any other requests to it is a
    /// bug. Each GetTemperatureCelsius responds with a value provided by the supplied
    /// `get_temperature` closure.
    fn setup_fake_driver(
        mut get_temperature: impl FnMut() -> Celsius + 'static,
    ) -> fthermal::DeviceProxy {
        let (proxy, mut stream) =
            fidl::endpoints::create_proxy_and_stream::<fthermal::DeviceMarker>().unwrap();
        fasync::Task::local(async move {
            while let Ok(req) = stream.try_next().await {
                match req {
                    Some(fthermal::DeviceRequest::GetTemperatureCelsius { responder }) => {
                        let _ =
                            responder.send(zx::Status::OK.into_raw(), get_temperature().0 as f32);
                    }
                    _ => assert!(false),
                }
            }
        })
        .detach();

        proxy
    }

    /// Sets up a test TemperatureHandler node that receives temperature readings from the
    /// provided closure.
    pub async fn setup_test_node(
        get_temperature: impl FnMut() -> Celsius + 'static,
        cache_duration: zx::Duration,
    ) -> Rc<TemperatureHandler> {
        TemperatureHandlerBuilder::new_with_proxy(
            "Fake".to_string(),
            setup_fake_driver(get_temperature),
        )
        .with_cache_duration(cache_duration)
        .build()
        .await
        .unwrap()
    }

    /// Tests that the node can handle the 'ReadTemperature' message as expected. The test
    /// checks for the expected temperature value which is returned by the fake thermal driver.
    #[fasync::run_singlethreaded(test)]
    async fn test_read_temperature() {
        // Readings for the fake temperature driver.
        let temperature_readings = vec![1.2, 3.4, 5.6, 7.8, 9.0];

        // Readings piped through the fake driver will be cast to f32 and back to f64.
        let expected_readings: Vec<f64> =
            temperature_readings.iter().map(|x| *x as f32 as f64).collect();

        // Each ReadTemperature request will respond with the next element from
        // `temperature_readings`, wrapping back around when the end of the vector is reached.
        let mut index = 0;
        let get_temperature = move || {
            let value = temperature_readings[index];
            index = (index + 1) % temperature_readings.len();
            Celsius(value)
        };
        let node = setup_test_node(get_temperature, zx::Duration::from_millis(0)).await;

        // Send ReadTemperature message and check for expected value.
        for expected_reading in expected_readings {
            let result = node.handle_message(&Message::ReadTemperature).await;
            let temperature = result.unwrap();
            if let MessageReturn::ReadTemperature(t) = temperature {
                assert_eq!(t.0, expected_reading);
            } else {
                assert!(false);
            }
        }
    }

    #[test]
    fn test_caching() -> Result<(), Error> {
        let mut executor = fasync::TestExecutor::new_with_fake_time().unwrap();
        executor.set_fake_time(Seconds(0.0).into());

        let sensor_temperature = Rc::new(Cell::new(Celsius(0.0)));

        let sensor_temperature_clone = sensor_temperature.clone();
        let get_temperature = move || sensor_temperature_clone.get();
        let node = executor
            .run_until_stalled(&mut Box::pin(setup_test_node(
                get_temperature,
                zx::Duration::from_millis(500),
            )))
            .unwrap();

        let run = move |executor: &mut fasync::TestExecutor, duration_ms: i64| {
            executor.set_fake_time(executor.now() + zx::Duration::from_millis(duration_ms));

            let poll =
                executor.run_until_stalled(&mut node.handle_message(&Message::ReadTemperature));
            if let Poll::Ready(Ok(MessageReturn::ReadTemperature(temperature))) = poll {
                temperature
            } else {
                panic!("Unexpected poll: {:?}", poll);
            }
        };

        // When advancing longer than the cache duration, we'll always poll the sensor.
        sensor_temperature.set(Celsius(20.0));
        assert_eq!(run(&mut executor, 1000), Celsius(20.0));
        sensor_temperature.set(Celsius(21.0));
        assert_eq!(run(&mut executor, 1000), Celsius(21.0));

        // If insufficient time has passed, we'll see the cached value.
        sensor_temperature.set(Celsius(22.0));
        assert_eq!(run(&mut executor, 200), Celsius(21.0));
        assert_eq!(run(&mut executor, 200), Celsius(21.0));
        assert_eq!(run(&mut executor, 200), Celsius(22.0));

        Ok(())
    }

    /// Tests that an unsupported message is handled gracefully and an error is returned.
    #[fasync::run_singlethreaded(test)]
    async fn test_unsupported_msg() {
        let node = setup_test_node(|| Celsius(0.0), zx::Duration::from_millis(0)).await;
        match node.handle_message(&Message::GetCpuLoads).await {
            Err(PowerManagerError::Unsupported) => {}
            e => panic!("Unexpected return value: {:?}", e),
        }
    }

    /// Tests for the presence and correctness of dynamically-added inspect data
    #[fasync::run_singlethreaded(test)]
    async fn test_inspect_data() {
        let temperature = Celsius(30.0);
        let inspector = inspect::Inspector::new();
        let node = TemperatureHandlerBuilder::new_with_proxy(
            "Fake".to_string(),
            setup_fake_driver(move || temperature),
        )
        .with_inspect_root(inspector.root())
        .build()
        .await
        .unwrap();

        // The node will read the current temperature and log the sample into Inspect. Read enough
        // samples to test that the correct number of samples are logged and older ones are dropped.
        for _ in 0..InspectData::NUM_INSPECT_TEMPERATURE_SAMPLES + 10 {
            node.handle_message(&Message::ReadTemperature).await.unwrap();
        }

        let mut root = TreeAssertion::new("TemperatureHandler (Fake)", false);
        let mut temperature_readings = TreeAssertion::new("temperature_readings", true);

        // Since we read 10 more samples than our limit allows, the first 10 should be dropped. So
        // test that the sample numbering starts at 10 and continues for the expected number of
        // samples.
        for i in 10..InspectData::NUM_INSPECT_TEMPERATURE_SAMPLES + 10 {
            let mut sample_child = TreeAssertion::new(&i.to_string(), true);
            sample_child.add_property_assertion("temperature", Box::new(temperature.0));
            sample_child.add_property_assertion("@time", Box::new(inspect::testing::AnyProperty));
            temperature_readings.add_child_assertion(sample_child);
        }

        root.add_child_assertion(temperature_readings);
        assert_data_tree!(inspector, root: { root, });
    }

    /// Tests that well-formed configuration JSON does not panic the `new_from_json` function.
    #[fasync::run_singlethreaded(test)]
    async fn test_new_from_json() {
        let json_data = json::json!({
            "type": "TemperatureHandler",
            "name": "temperature",
            "config": {
                "driver_path": "/dev/class/thermal/000",
                "cache_duration_ms": 1000
            }
        });
        let _ = TemperatureHandlerBuilder::new_from_json(json_data, &HashMap::new());
    }

    /// Tests that the node correctly reports its driver path.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_driver_path() {
        let node = setup_test_node(|| Celsius(0.0), zx::Duration::from_millis(0)).await;
        let driver_path = match node.handle_message(&Message::GetDriverPath).await.unwrap() {
            MessageReturn::GetDriverPath(driver_path) => driver_path,
            e => panic!("Unexpected message response: {:?}", e),
        };

        // "Fake" is the driver path assigned in `setup_test_node`
        assert_eq!(driver_path, "Fake".to_string());
    }
}

/// Contains both the raw and filtered temperature values returned from the TemperatureFilter
/// `get_temperature` function.
#[derive(PartialEq, Debug)]
pub struct TemperatureReadings {
    pub raw: Celsius,
    pub filtered: Celsius,
}

/// Wrapper for reading and filtering temperature samples from a TemperatureHandler node.
pub struct TemperatureFilter {
    /// Filter time constant
    time_constant: Seconds,

    /// Previous sample temperature
    prev_temperature: Cell<Option<Celsius>>,

    /// Previous sample timestamp
    prev_timestamp: Cell<Nanoseconds>,

    /// TemperatureHandler node that is used to read temperature
    temperature_handler: Rc<dyn Node>,
}

impl TemperatureFilter {
    /// Constucts a new TemperatureFilter with the specified TemperatureHandler node and filter time
    /// constant.
    pub fn new(temperature_handler: Rc<dyn Node>, time_constant: Seconds) -> Self {
        assert!(time_constant > Seconds(0.0));
        Self {
            time_constant,
            prev_temperature: Cell::new(None),
            prev_timestamp: Cell::new(Nanoseconds(0)),
            temperature_handler,
        }
    }

    /// Reads a new temperature sample and returns a Temperature instance containing both the raw
    /// and filtered temperature values.
    pub async fn get_temperature(
        &self,
        timestamp: Nanoseconds,
    ) -> Result<TemperatureReadings, Error> {
        fuchsia_trace::duration!("power_manager", "TemperatureFilter::get_temperature");

        let raw_temperature = self.read_temperature().await?;
        let filtered_temperature = match self.prev_temperature.get() {
            Some(prev_temperature) => Self::low_pass_filter(
                raw_temperature,
                prev_temperature,
                (timestamp - self.prev_timestamp.get()).into(),
                self.time_constant,
            ),
            None => raw_temperature,
        };

        self.prev_temperature.set(Some(filtered_temperature));
        self.prev_timestamp.set(timestamp);

        Ok(TemperatureReadings { raw: raw_temperature, filtered: filtered_temperature })
    }

    /// Reset the internal state of the temperature filter. This has the effect of causing the
    /// filter to return the same value for both the `raw` and `filtered` temperature fields on the
    /// next call to `get_temperature`.
    #[cfg(test)]
    pub fn reset(&self) {
        self.prev_temperature.set(None);
        self.prev_timestamp.set(Nanoseconds(0));
    }

    /// Queries the current temperature from the temperature handler node
    async fn read_temperature(&self) -> Result<Celsius, Error> {
        fuchsia_trace::duration!("power_manager", "TemperatureFilter::read_temperature");
        match self.temperature_handler.handle_message(&Message::ReadTemperature).await {
            Ok(MessageReturn::ReadTemperature(t)) => Ok(t),
            Ok(r) => Err(format_err!("ReadTemperature had unexpected return value: {:?}", r)),
            Err(e) => Err(format_err!("ReadTemperature failed: {:?}", e)),
        }
    }

    /// Filters the input temperature value using the specified previous temperature value `y_prev`,
    /// `time_delta`, and `time_constant`.
    fn low_pass_filter(
        y: Celsius,
        y_prev: Celsius,
        time_delta: Seconds,
        time_constant: Seconds,
    ) -> Celsius {
        Celsius(y_prev.0 + (time_delta.0 / time_constant.0) * (y.0 - y_prev.0))
    }
}

#[cfg(test)]
mod temperature_filter_tests {
    use super::*;
    use crate::test::mock_node::{MessageMatcher, MockNodeMaker};
    use crate::{msg_eq, msg_ok_return};
    use fuchsia_async as fasync;

    /// Tests the low_pass_filter function for correctness.
    #[test]
    fn test_low_pass_filter() {
        let y_0 = Celsius(0.0);
        let y_1 = Celsius(10.0);
        let time_delta = Seconds(1.0);
        let time_constant = Seconds(10.0);
        assert_eq!(
            TemperatureFilter::low_pass_filter(y_1, y_0, time_delta, time_constant),
            Celsius(1.0)
        );
    }

    /// Tests that the TemperatureFilter `get_temperature` function queries the TemperatureHandler
    /// node and returns the expected raw and filtered temperature values.
    #[fasync::run_singlethreaded(test)]
    async fn test_get_temperature() {
        let mut mock_maker = MockNodeMaker::new();
        let temperature_node = mock_maker.make(
            "Temperature",
            vec![
                (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(50.0)))),
                (msg_eq!(ReadTemperature), msg_ok_return!(ReadTemperature(Celsius(80.0)))),
            ],
        );

        // Create a TemperatureFilter instance using 5 seconds as the filter constant
        let filter = TemperatureFilter::new(temperature_node, Seconds(5.0));

        // The first reading should return identical raw/filtered values
        assert_eq!(
            filter.get_temperature(Nanoseconds(0)).await.unwrap(),
            TemperatureReadings { raw: Celsius(50.0), filtered: Celsius(50.0) }
        );

        // The next reading should return the raw value (80C) along with the calculated filtered
        // value for the given elapsed time (1 second)
        assert_eq!(
            filter.get_temperature(Seconds(1.0).into()).await.unwrap(),
            TemperatureReadings { raw: Celsius(80.0), filtered: Celsius(56.0) }
        );
    }
}
