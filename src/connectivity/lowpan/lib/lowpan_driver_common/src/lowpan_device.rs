// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use crate::prelude_internal::*;

use crate::ServeTo;
use anyhow::Error;
use core::future::ready;
use fidl_fuchsia_factory_lowpan::*;
use fidl_fuchsia_lowpan::*;
use fidl_fuchsia_lowpan_device::{
    AllCounters, CountersRequest, CountersRequestStream, DeviceExtraRequest,
    DeviceExtraRequestStream, DeviceRequest, DeviceRequestStream, DeviceRouteExtraRequest,
    DeviceRouteExtraRequestStream, DeviceRouteRequest, DeviceRouteRequestStream, DeviceState,
    EnergyScanParameters, EnergyScanResult, ExternalRoute, NetworkScanParameters, OnMeshPrefix,
    ProvisionError, ProvisioningProgress,
};
use fidl_fuchsia_lowpan_test::*;
use fidl_fuchsia_lowpan_thread::*;
use futures::stream::BoxStream;
use futures::{FutureExt, StreamExt, TryStreamExt};

/// Trait for implementing a LoWPAN Device Driver.
#[async_trait]
pub trait Driver: Send + Sync {
    /// Provisions this device with a network.
    ///
    /// See [`fidl_fuchsia_lowpan_device::Device::provision_network`] for more information.
    async fn provision_network(&self, params: ProvisioningParams) -> ZxResult<()>;

    /// See [`fidl_fuchsia_lowpan_device::Device::leave_network`] for more information.
    async fn leave_network(&self) -> ZxResult<()>;

    /// Change the 'active' state for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_device::Device::set_active`] for more information.
    async fn set_active(&self, active: bool) -> ZxResult<()>;

    /// Returns a list of the supported network types.
    ///
    /// See [`fidl_fuchsia_lowpan_device::Device::get_supported_network_types`] for more
    /// information.
    async fn get_supported_network_types(&self) -> ZxResult<Vec<String>>;

    /// Returns a list of supported channels.
    ///
    /// See [`fidl_fuchsia_lowpan_device::Device::get_supported_channels`] for more
    /// information.
    async fn get_supported_channels(&self) -> ZxResult<Vec<ChannelInfo>>;

    /// Returns a stream object which, after emitting the initial DeviceState, will
    /// procede to emit deltas for the DeviceState as it changes.
    ///
    /// This is used to implement [`fidl_fuchsia_lowpan_device::Device::watch_device_state`].
    /// Only one instance of this stream is created per channel.
    fn watch_device_state(&self) -> BoxStream<'_, ZxResult<DeviceState>>;

    /// Returns a stream object which, after emitting the initial identity, will
    /// procede to emit additional values as the network identity for the device changes.
    ///
    /// This is used to implement [`fidl_fuchsia_lowpan_device::DeviceExtra::watch_identity`].
    /// Only one instance of this stream is created per channel.
    fn watch_identity(&self) -> BoxStream<'_, ZxResult<Identity>>;

    /// Forms a new network.
    ///
    /// See [`fidl_fuchsia_lowpan_device::DeviceExtra::form_network`] for more information.
    ///
    /// Note that the future returned from this method is not
    /// for the return value, rather it is used for handling
    /// the `progress` ServerEnd and making progress on the
    /// `form` operation. From a FIDL point of view, this method
    /// returns immediately.
    fn form_network(
        &self,
        params: ProvisioningParams,
    ) -> BoxStream<'_, ZxResult<Result<ProvisioningProgress, ProvisionError>>>;

    /// Joins an existing network.
    ///
    /// See [`fidl_fuchsia_lowpan_device::DeviceExtra::join_network`] for more information.
    ///
    /// Note that the future returned from this method is not
    /// for the return value, rather it is used for handling
    /// the `progress` ServerEnd and making progress on the
    /// `join` operation. From a FIDL point of view, this method
    /// returns immediately.
    fn join_network(
        &self,
        params: JoinParams,
    ) -> BoxStream<'_, ZxResult<Result<ProvisioningProgress, ProvisionError>>>;

    /// Returns the value of the current credential, if one is available.
    ///
    /// See [`fidl_fuchsia_lowpan_device::DeviceExtra::get_credential`] for more information.
    async fn get_credential(&self) -> ZxResult<Option<fidl_fuchsia_lowpan::Credential>>;

    /// Starts a new energy scan operation and returns a stream for tracking the
    /// results.
    ///
    /// See [`fidl_fuchsia_lowpan_device::DeviceExtra::start_energy_scan`] for more information.
    fn start_energy_scan(
        &self,
        params: &EnergyScanParameters,
    ) -> BoxStream<'_, ZxResult<Vec<EnergyScanResult>>>;

    /// Starts a new network scan operation and returns a stream for tracking the
    /// results.
    ///
    /// See [`fidl_fuchsia_lowpan_device::DeviceExtra::start_network_scan`] for more information.
    fn start_network_scan(
        &self,
        params: &NetworkScanParameters,
    ) -> BoxStream<'_, ZxResult<Vec<BeaconInfo>>>;

    /// Performs a device reset.
    ///
    /// If the device is in [manufacturing mode](driver::send_mfg_command),
    /// calling this method will restore the device back into its normal
    /// operating state.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::reset`] for more information.
    async fn reset(&self) -> ZxResult<()>;

    /// Returns the factory-assigned static MAC address for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_factory_mac_address`] for more information.
    async fn get_factory_mac_address(&self) -> ZxResult<Vec<u8>>;

    /// Returns the currently assigned MAC address for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_current_mac_address`] for more information.
    async fn get_current_mac_address(&self) -> ZxResult<Vec<u8>>;

    /// Returns the NCP/Stack version string for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_ncp_version`] for more information.
    async fn get_ncp_version(&self) -> ZxResult<String>;

    /// Returns the current channel for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_current_channel`] for more information.
    async fn get_current_channel(&self) -> ZxResult<u16>;

    /// Returns the immediate RSSI value for the radio on this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_rssi`] for more information.
    async fn get_current_rssi(&self) -> ZxResult<i32>;

    /// Returns the partition id that this device is a part of.
    /// Returns 0xFFFFFFFF if this device is not a member of a partition.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_partition_id`] for more information.
    async fn get_partition_id(&self) -> ZxResult<u32>;

    /// Returns the current Thread RLOC16 for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_thread_rloc16`] for more information.
    async fn get_thread_rloc16(&self) -> ZxResult<u16>;

    /// Returns the current Thread router id for this device.
    ///
    /// See [`fidl_fuchsia_lowpan_test::DeviceTest::get_thread_router_id`] for more information.
    async fn get_thread_router_id(&self) -> ZxResult<u8>;

    /// Send a proprietary manufacturing command to the device and return the response.
    ///
    /// This method is intended to be used to facilitate device testing on the assembly line and is
    /// typically only used during device manufacturing.
    ///
    /// Commands are given as strings (command + arguments) and the response is also a string. The
    /// usage and format of the commands is dependent on the firmware on the LoWPAN device.
    ///
    /// See [`fidl_fuchsia_factory_lowpan::FactoryDevice::send_mfg_command`] for more information.
    async fn send_mfg_command(&self, command: &str) -> ZxResult<String>;

    async fn replace_mac_address_filter_settings(
        &self,
        settings: MacAddressFilterSettings,
    ) -> ZxResult<()>;
    async fn get_mac_address_filter_settings(&self) -> ZxResult<MacAddressFilterSettings>;

    async fn get_neighbor_table(&self) -> ZxResult<Vec<NeighborInfo>>;

    /// Returns a snapshot of the counters without resetting the counters.
    async fn get_counters(&self) -> ZxResult<AllCounters>;

    /// Resets all of the counters to zero returning the counter values
    /// immediately prior.
    async fn reset_counters(&self) -> ZxResult<AllCounters>;

    /// Registers an on-mesh prefix to be advertised on the
    /// current network.
    ///
    /// Subsequent calls with the same value for the `subnet` field will
    /// update the properties associated with that on-mesh prefix.
    ///
    /// These changes persist like adding an IP address would,
    /// and will stick around until explicitly removed or
    /// the interface component is reset/restarted.
    ///
    /// If the given `OnMeshPrefix` structure is invalid for some reason
    /// (missing required fields, invalid values, etc), the method
    /// shall return `Err(ZX_ERR_INVALID_ARGS)`.
    ///
    /// If registering a new on-mesh prefix and the maximum number of
    /// on-mesh prefixes has already been registered, the method
    /// shall return `Err(ZX_ERR_NO_RESOURCES)`.
    async fn register_on_mesh_prefix(&self, net: OnMeshPrefix) -> ZxResult<()>;

    /// Unregisters any on-mesh prefix that was previously registered with
    /// `RegisterOnMeshPrefix`.  It returns once the on-mesh prefix has
    /// been removed locally.
    ///
    /// If the given mesh prefix was not previously registered,
    /// no action is taken.
    async fn unregister_on_mesh_prefix(&self, net: Ipv6Subnet) -> ZxResult<()>;

    /// Registers an external route to be advertised on the
    /// current network.
    ///
    /// Subsequent calls with the same value for the `subnet` field will
    /// update the properties associated with that route.
    ///
    /// These changes persist like adding an IP address would,
    /// and will stick around until explicitly removed or
    /// the interface component is reset/restarted.
    ///
    /// If the given `ExternalRoute` structure is invalid for some reason
    /// (missing required fields, invalid values, etc), the method
    /// shall return `Err(ZX_ERR_INVALID_ARGS)`.
    ///
    /// If registering a new external route and the maximum number of
    /// external routes has already been registered, the method
    /// shall return `Err(ZX_ERR_NO_RESOURCES)`.
    async fn register_external_route(&self, net: ExternalRoute) -> ZxResult<()>;

    /// Unregisters any external route that was previously registered with
    /// `RegisterExternalRoute`. It returns once the external route has
    /// been removed locally.
    ///
    /// If the given external route was not previously registered,
    /// no action is taken.
    async fn unregister_external_route(&self, net: Ipv6Subnet) -> ZxResult<()>;

    /// Returns a vector containing all of the locally added on-mesh prefixes.
    async fn get_local_on_mesh_prefixes(&self) -> ZxResult<Vec<OnMeshPrefix>>;

    /// Returns a vector containing all of the locally added external routes.
    async fn get_local_external_routes(&self) -> ZxResult<Vec<ExternalRoute>>;

    /// Changes the joinability status of the interface
    async fn make_joinable(&self, duration: fuchsia_zircon::Duration, port: u16) -> ZxResult<()>;

    /// Fetches and returns the active Thread operational dataset in raw
    /// TLV form. Functionally equivalent to [`otDatasetGetActiveTlvs()`][2].
    ///
    /// This method returns the active dataset, or nothing in the case that
    /// there is no active operational dataset.
    ///
    /// [2]: https://openthread.io/reference/group/api-operational-dataset#otdatasetgetactivetlvs
    async fn get_active_dataset_tlvs(&self) -> ZxResult<Vec<u8>> {
        Err(ZxStatus::NOT_SUPPORTED)
    }

    /// Sets the active Thread Operational Dataset in raw TLV form.
    /// Functionally equivalent to [`otDatasetSetActiveTlvs()`][3].
    ///
    /// This method returns once the operation has completed successfully.
    ///
    /// [3]: https://openthread.io/reference/group/api-operational-dataset#otdatasetsetactivetlvs
    async fn set_active_dataset_tlvs(&self, _dataset: &[u8]) -> ZxResult {
        Err(ZxStatus::NOT_SUPPORTED)
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DeviceRequestStream> for T {
    async fn serve_to(&self, request_stream: DeviceRequestStream) -> anyhow::Result<()> {
        use futures::lock::Mutex;
        let watcher = Mutex::new(self.watch_device_state());
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                DeviceRequest::ProvisionNetwork { params, responder } => {
                    self.provision_network(params)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in provision_network request")?;
                }

                DeviceRequest::LeaveNetwork { responder, .. } => {
                    self.leave_network()
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in leave_network request")?;
                }

                DeviceRequest::SetActive { active, responder, .. } => {
                    self.set_active(active)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in set_active request")?;
                }

                DeviceRequest::GetSupportedNetworkTypes { responder } => {
                    self.get_supported_network_types()
                        .err_into::<Error>()
                        .and_then(|response| {
                            ready(
                                responder
                                    .send(&mut response.iter().map(String::as_str))
                                    .map_err(Error::from),
                            )
                        })
                        .await
                        .context("error in get_supported_network_types request")?;
                }

                DeviceRequest::GetSupportedChannels { responder } => {
                    self.get_supported_channels()
                        .err_into::<Error>()
                        .and_then(|response| {
                            ready(responder.send(&mut response.into_iter()).map_err(Error::from))
                        })
                        .await
                        .context("error in get_supported_channels request")?;
                }

                DeviceRequest::WatchDeviceState { responder } => {
                    watcher
                        .try_lock()
                        .ok_or(format_err!(
                            "No more than 1 outstanding call to watch_device_state is allowed"
                        ))?
                        .next()
                        .map(|x| match x {
                            Some(x) => x.map_err(Error::from),
                            None => Err(format_err!("watch_device_state stream ended early")),
                        })
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in watch_device_state request")?;
                }
            }
            Result::<(), anyhow::Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DeviceExtraRequestStream> for T {
    async fn serve_to(&self, request_stream: DeviceExtraRequestStream) -> anyhow::Result<()> {
        use futures::lock::Mutex;
        let watcher = Mutex::new(self.watch_identity());
        let request_control_handle = request_stream.control_handle();

        let closure = |command| {
            async {
                match command {
                    DeviceExtraRequest::JoinNetwork { params, progress, .. } => {
                        use fidl_fuchsia_lowpan_device::ProvisioningMonitorRequest;
                        let stream = progress.into_stream()?;
                        let control_handle = stream.control_handle();

                        // Convert the stream of requests (of which there is only one
                        // variant) into a stream of `ProvisioningMonitorNextResponder`
                        // instances, for clarity.
                        let responder_stream = stream.map_ok(|x| match x {
                            ProvisioningMonitorRequest::WatchProgress { responder } => responder,
                        });

                        let result_stream = self.join_network(params);

                        // We now have a stream of responder instances which need
                        // to be matched up to the results from the results stream.
                        // We zip these into a single stream and then send each result
                        // to the next responder.
                        let ret = responder_stream
                            .zip(result_stream)
                            .map(move |x| match x {
                                (Ok(responder), Ok(mut result)) => Ok(responder.send(&mut result)?),
                                (Err(err), _) => Err(Error::from(err)
                                    .context("DeviceExtraRequest::JoinNetwork:responder_stream")),
                                (_, Err(status)) => {
                                    control_handle.shutdown_with_epitaph(status);
                                    Err(Error::from(status)
                                        .context("DeviceExtraRequest::JoinNetwork:result_stream"))
                                }
                            })
                            .try_for_each(|_| ready(Ok(())))
                            .await;

                        if let Err(err) = ret {
                            // These errors only affect the provisioning session, so
                            // we only report them to the logs rather than passing
                            // them up.
                            fx_log_err!("Error during DeviceExtraRequest::JoinNetwork: {:?}", err);
                        }
                    }
                    DeviceExtraRequest::FormNetwork { params, progress, .. } => {
                        use fidl_fuchsia_lowpan_device::ProvisioningMonitorRequest;
                        let stream = progress.into_stream()?;
                        let control_handle = stream.control_handle();

                        // Convert the stream of requests (of which there is only one
                        // variant) into a stream of `ProvisioningMonitorNextResponder`
                        // instances, for clarity.
                        let responder_stream = stream.map_ok(|x| match x {
                            ProvisioningMonitorRequest::WatchProgress { responder } => responder,
                        });

                        let result_stream = self.form_network(params);

                        // We now have a stream of responder instances which need
                        // to be matched up to the results from the results stream.
                        // We zip these into a single stream and then send each result
                        // to the next responder.
                        let ret = responder_stream
                            .zip(result_stream)
                            .map(move |x| match x {
                                (Ok(responder), Ok(mut result)) => Ok(responder.send(&mut result)?),
                                (Err(err), _) => Err(Error::from(err)
                                    .context("DeviceExtraRequest::FormNetwork:responder_stream")),
                                (_, Err(status)) => {
                                    control_handle.shutdown_with_epitaph(status);
                                    Err(Error::from(status)
                                        .context("DeviceExtraRequest::FormNetwork:result_stream"))
                                }
                            })
                            .try_for_each(|_| ready(Ok(())))
                            .await;

                        if let Err(err) = ret {
                            // These errors only affect the provisioning session, so
                            // we only report them to the logs rather than passing
                            // them up.
                            fx_log_err!("Error during DeviceExtraRequest::FormNetwork: {:?}", err);
                        }
                    }
                    DeviceExtraRequest::GetCredential { responder, .. } => {
                        self.get_credential()
                            .err_into::<Error>()
                            .and_then(|mut response| {
                                ready(responder.send(response.as_mut()).map_err(Error::from))
                            })
                            .await
                            .context("error in get_credential request")?;
                    }
                    DeviceExtraRequest::StartEnergyScan { params, stream, .. } => {
                        use fidl_fuchsia_lowpan_device::EnergyScanResultStreamRequest;
                        let stream = stream.into_stream()?;
                        let control_handle = stream.control_handle();

                        // Convert the stream of requests (of which there is only one
                        // variant) into a stream of `EnergyScanResultStreamNextResponder`
                        // instances, for clarity.
                        let responder_stream = stream.map_ok(|x| match x {
                            EnergyScanResultStreamRequest::Next { responder } => responder,
                        });

                        // The reason for the `filter()` and `chain()` calls below is to
                        // avoid having two different ways to signal the end of the scan stream.
                        //
                        // The `EnergyScanResult::Next` FIDL method signals the end of the scan
                        // by returning an empty vector. However, the stream returned by
                        // `lowpan_driver_common::Driver::start_energy_scan()` denotes the end
                        // of the scan simply as the end of the stream (`next()` returning `None`
                        // instead of `Some(...)`).
                        //
                        // These two mechanisms are different. We don't want `start_energy_scan()`
                        // to be able to trigger the end of the scan when the stream hasn't
                        // finished yet. Likewise we want to make sure that we properly indicate
                        // via `EnergyScanResult::Next` that the scan has been terminated when the
                        // scan stream has ended, even when the last vector emitted was not empty.
                        //
                        // To do that we first strip all empty vectors from the scan stream, and
                        // then ensure that the last emitted vector is empty.
                        let result_stream = self
                            .start_energy_scan(&params)
                            .filter(|x| {
                                ready(match x {
                                    // Remove empty vectors that may
                                    // happen erroneously. We don't want them
                                    // confused with indicating the end
                                    // of the scan.
                                    Ok(v) if v.is_empty() => false,
                                    _ => true,
                                })
                            })
                            // Append an empty vector to signal the real end
                            // of the scan.
                            .chain(ready(Ok(vec![])).into_stream());

                        let ret = responder_stream
                            .zip(result_stream)
                            .map(move |x| match x {
                                (Ok(responder), Ok(result)) => {
                                    Ok(responder.send(&mut result.into_iter())?)
                                }
                                (Err(err), _) => {
                                    Err(Error::from(err).context("EnergyScanResultStreamRequest"))
                                }
                                (_, Err(status)) => {
                                    control_handle.shutdown_with_epitaph(status);
                                    Err(Error::from(status).context("energy_scan_result_stream"))
                                }
                            })
                            .try_for_each(|_| ready(Ok(())))
                            .await;

                        if let Err(err) = ret {
                            fx_log_err!("Error during energy scan: {:?}", err);
                        }
                    }
                    DeviceExtraRequest::StartNetworkScan { params, stream, .. } => {
                        use fidl_fuchsia_lowpan_device::BeaconInfoStreamRequest;
                        let stream = stream.into_stream()?;
                        let control_handle = stream.control_handle();

                        // Convert the stream of requests (of which there is only one
                        // variant) into a stream of `BeaconInfoStreamNextResponder`
                        // instances, for clarity.
                        let responder_stream = stream.map_ok(|x| match x {
                            BeaconInfoStreamRequest::Next { responder } => responder,
                        });

                        // The reason for the `filter()` and `chain()` calls below is to
                        // avoid having two different ways to signal the end of the scan stream.
                        //
                        // The `BeaconInfoStream::Next` FIDL method signals the end of the scan
                        // by returning an empty vector. However, the stream returned by
                        // `lowpan_driver_common::Driver::start_network_scan()` denotes the end
                        // of the scan simply as the end of the stream (`next()` returning `None`
                        // instead of `Some(...)`).
                        //
                        // These two mechanisms are different. We don't want `start_network_scan()`
                        // to be able to trigger the end of the scan when the stream hasn't
                        // finished yet. Likewise we want to make sure that we properly indicate
                        // via `BeaconInfoStream::Next` that the scan has been terminated when the
                        // scan stream has ended, even when the last vector emitted was not empty.
                        //
                        // To do that we first strip all empty vectors from the scan stream, and
                        // then ensure that the last emitted vector is empty.
                        let result_stream = self
                            .start_network_scan(&params)
                            .filter(|x| {
                                ready(match x {
                                    // Remove empty vectors that may
                                    // happen erroneously. We don't want them
                                    // confused with indicating the end
                                    // of the scan.
                                    Ok(v) if v.is_empty() => false,
                                    _ => true,
                                })
                            })
                            // Append an empty vector to signal the real end
                            // of the scan.
                            .chain(ready(Ok(vec![])).into_stream());

                        let ret = responder_stream
                            .zip(result_stream)
                            .map(move |x| match x {
                                (Ok(responder), Ok(mut result)) => {
                                    Ok(responder.send(&mut result.iter_mut())?)
                                }
                                (Err(err), _) => {
                                    Err(Error::from(err).context("BeaconInfoStreamRequestStream"))
                                }
                                (_, Err(status)) => {
                                    control_handle.shutdown_with_epitaph(status);
                                    Err(Error::from(status).context("network_scan_result_stream"))
                                }
                            })
                            .try_for_each(|_| ready(Ok(())))
                            .await;

                        if let Err(err) = ret {
                            // These errors only affect the scan channel, so
                            // we only report them to the logs rather than passing
                            // them up.
                            fx_log_err!("Error during network scan: {:?}", err);
                        }
                    }
                    DeviceExtraRequest::WatchIdentity { responder, .. } => {
                        watcher
                            .try_lock()
                            .ok_or(format_err!(
                                "No more than 1 outstanding call to watch_identity is allowed"
                            ))?
                            .next()
                            .map(|x| match x {
                                Some(x) => x.map_err(Error::from),
                                None => {
                                    Err(format_err!("Device combined state stream ended early"))
                                }
                            })
                            .and_then(|response| {
                                ready(responder.send(response).map_err(Error::from))
                            })
                            .await
                            .context("error in watch_identity request")?;
                    }
                }
                Result::<(), anyhow::Error>::Ok(())
            }
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceExtraRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DeviceTestRequestStream> for T {
    async fn serve_to(&self, request_stream: DeviceTestRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                DeviceTestRequest::Reset { responder, .. } => {
                    self.reset()
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in reset request")?;
                }

                DeviceTestRequest::GetNcpVersion { responder, .. } => {
                    self.get_ncp_version()
                        .err_into::<Error>()
                        .and_then(|response| {
                            ready(responder.send(response.as_str()).map_err(Error::from))
                        })
                        .await
                        .context("error in get_ncp_version request")?;
                }
                DeviceTestRequest::GetCurrentChannel { responder, .. } => {
                    self.get_current_channel()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in get_current_channel request")?;
                }
                DeviceTestRequest::GetCurrentRssi { responder, .. } => {
                    self.get_current_rssi()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in get_current_rssi request")?;
                }
                DeviceTestRequest::GetFactoryMacAddress { responder, .. } => {
                    self.get_factory_mac_address()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(&response).map_err(Error::from)))
                        .await
                        .context("error in get_factory_mac_address request")?;
                }
                DeviceTestRequest::GetCurrentMacAddress { responder, .. } => {
                    self.get_current_mac_address()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(&response).map_err(Error::from)))
                        .await
                        .context("error in get_current_mac_address request")?;
                }
                DeviceTestRequest::GetPartitionId { responder, .. } => {
                    self.get_partition_id()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in get_partition_id request")?;
                }
                DeviceTestRequest::GetThreadRloc16 { responder, .. } => {
                    self.get_thread_rloc16()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in get_thread_rloc16 request")?;
                }
                DeviceTestRequest::GetThreadRouterId { responder, .. } => {
                    self.get_thread_router_id()
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(response).map_err(Error::from)))
                        .await
                        .context("error in get_thread_router_id request")?;
                }
                DeviceTestRequest::ReplaceMacAddressFilterSettings {
                    settings, responder, ..
                } => {
                    self.replace_mac_address_filter_settings(settings)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in set_address_filter_settings request")?;
                }
                DeviceTestRequest::GetMacAddressFilterSettings { responder, .. } => {
                    self.get_mac_address_filter_settings()
                        .err_into::<Error>()
                        .and_then(|x| ready(responder.send(x).map_err(Error::from)))
                        .await
                        .context("error in get_address_filter_settings request")?;
                }
                DeviceTestRequest::GetNeighborTable { responder, .. } => {
                    self.get_neighbor_table()
                        .err_into::<Error>()
                        .and_then(|x| {
                            ready(responder.send(&mut x.into_iter()).map_err(Error::from))
                        })
                        .await
                        .context("error in get_neighbor_table_snapshot request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceTestRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DeviceRouteRequestStream> for T {
    async fn serve_to(&self, request_stream: DeviceRouteRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                DeviceRouteRequest::RegisterOnMeshPrefix { prefix, responder, .. } => {
                    self.register_on_mesh_prefix(prefix)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in register_on_mesh_prefix request")?;
                }
                DeviceRouteRequest::UnregisterOnMeshPrefix { subnet, responder, .. } => {
                    self.unregister_on_mesh_prefix(subnet)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in unregister_on_mesh_prefix request")?;
                }

                DeviceRouteRequest::RegisterExternalRoute { external_route, responder, .. } => {
                    self.register_external_route(external_route)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in register_external_route request")?;
                }
                DeviceRouteRequest::UnregisterExternalRoute { subnet, responder, .. } => {
                    self.unregister_external_route(subnet)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in unregister_external_route request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceRouteRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DeviceRouteExtraRequestStream> for T {
    async fn serve_to(&self, request_stream: DeviceRouteExtraRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                DeviceRouteExtraRequest::GetLocalOnMeshPrefixes { responder, .. } => {
                    self.get_local_on_mesh_prefixes()
                        .err_into::<Error>()
                        .and_then(|x| {
                            ready(responder.send(&mut x.into_iter()).map_err(Error::from))
                        })
                        .await
                        .context("error in get_local_on_mesh_prefixes request")?;
                }
                DeviceRouteExtraRequest::GetLocalExternalRoutes { responder, .. } => {
                    self.get_local_external_routes()
                        .err_into::<Error>()
                        .and_then(|x| {
                            ready(responder.send(&mut x.into_iter()).map_err(Error::from))
                        })
                        .await
                        .context("error in get_local_external_routes request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceRouteExtraRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<FactoryDeviceRequestStream> for T {
    async fn serve_to(&self, request_stream: FactoryDeviceRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                FactoryDeviceRequest::SendMfgCommand { responder, command, .. } => {
                    self.send_mfg_command(&command)
                        .err_into::<Error>()
                        .and_then(|response| ready(responder.send(&response).map_err(Error::from)))
                        .await
                        .context("error in send_mfg_command request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DeviceTestRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<CountersRequestStream> for T {
    async fn serve_to(&self, request_stream: CountersRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                CountersRequest::Get { responder, .. } => {
                    self.get_counters()
                        .err_into::<Error>()
                        .and_then(|x| ready(responder.send(x).map_err(Error::from)))
                        .await
                        .context("error in get_counters request")?;
                }
                CountersRequest::Reset { responder, .. } => {
                    self.reset_counters()
                        .err_into::<Error>()
                        .and_then(|x| ready(responder.send(x).map_err(Error::from)))
                        .await
                        .context("error in reset_counters request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving CountersRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<LegacyJoiningRequestStream> for T {
    async fn serve_to(&self, request_stream: LegacyJoiningRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                LegacyJoiningRequest::MakeJoinable { duration, port, responder, .. } => {
                    self.make_joinable(fuchsia_zircon::Duration::from_nanos(duration), port)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in make_joinable request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving LegacyJoiningRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[async_trait()]
impl<T: Driver> ServeTo<DatasetRequestStream> for T {
    async fn serve_to(&self, request_stream: DatasetRequestStream) -> anyhow::Result<()> {
        let request_control_handle = request_stream.control_handle();

        let closure = |command| async {
            match command {
                DatasetRequest::GetActiveTlvs { responder, .. } => {
                    self.get_active_dataset_tlvs()
                        .err_into::<Error>()
                        .and_then(|x| ready(responder.send(Some(x.as_ref())).map_err(Error::from)))
                        .await
                        .context("error in get_active_dataset_tlvs request")?;
                }
                DatasetRequest::SetActiveTlvs { dataset, responder, .. } => {
                    self.set_active_dataset_tlvs(&dataset)
                        .err_into::<Error>()
                        .and_then(|_| ready(responder.send().map_err(Error::from)))
                        .await
                        .context("error in set_active_dataset_tlvs request")?;
                }
            }
            Result::<(), Error>::Ok(())
        };

        if let Some(err) =
            request_stream.err_into::<Error>().try_for_each_concurrent(None, closure).await.err()
        {
            fx_log_err!("Error serving DatasetRequestStream: {:?}", err);

            if let Some(epitaph) = err.downcast_ref::<ZxStatus>() {
                request_control_handle.shutdown_with_epitaph(*epitaph);
            }

            Err(err)
        } else {
            Ok(())
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::DummyDevice;
    use assert_matches::assert_matches;
    use fidl::endpoints::create_endpoints;
    use fidl_fuchsia_lowpan_device::{BeaconInfoStreamMarker, DeviceExtraMarker};
    use fidl_fuchsia_lowpan_device::{EnergyScanParameters, EnergyScanResultStreamMarker};
    use fuchsia_async as fasync;

    #[fasync::run_until_stalled(test)]
    async fn test_send_mfg_command() {
        let device = DummyDevice::default();

        let (client_ep, server_ep) = create_endpoints::<FactoryDeviceMarker>().unwrap();

        let server_future = device.serve_to(server_ep.into_stream().unwrap());

        let proxy = client_ep.into_proxy().unwrap();

        let client_future = async move {
            let command = "help";

            match proxy.send_mfg_command(command).await {
                Ok(result) => println!("mfg_command({:?}) result: {:?}", command, result),
                Err(err) => panic!("mfg_command({:?}) error: {:?}", command, err),
            }
        };

        futures::select! {
            err = server_future.boxed_local().fuse() => panic!("Server task stopped: {:?}", err),
            _ = client_future.boxed().fuse() => (),
        }
    }

    #[fasync::run_until_stalled(test)]
    async fn test_energy_scan() {
        let device = DummyDevice::default();

        let (client_ep, server_ep) = create_endpoints::<DeviceExtraMarker>().unwrap();

        let server_future = device.serve_to(server_ep.into_stream().unwrap());

        let proxy = client_ep.into_proxy().unwrap();

        let client_future = async move {
            let (client_ep, server_ep) =
                create_endpoints::<EnergyScanResultStreamMarker>().unwrap();
            let params = EnergyScanParameters::EMPTY;

            assert_matches!(proxy.start_energy_scan(params, server_ep), Ok(()));

            let scanner = client_ep.into_proxy().unwrap();
            let mut results = vec![];

            loop {
                let mut next: Vec<EnergyScanResult> = scanner.next().await.unwrap();
                if next.is_empty() {
                    break;
                }
                results.append(&mut next);
            }

            assert_eq!(results.len(), 5, "Unexpected number of scan results");

            assert!(scanner.next().await.is_err(), "Calling next again should error");
        };

        futures::select! {
            err = server_future.boxed_local().fuse() => panic!("Server task stopped: {:?}", err),
            _ = client_future.boxed().fuse() => (),
        }
    }

    #[fasync::run_until_stalled(test)]
    async fn test_network_scan() {
        let device = DummyDevice::default();

        let (client_ep, server_ep) = create_endpoints::<DeviceExtraMarker>().unwrap();

        let server_future = device.serve_to(server_ep.into_stream().unwrap());

        let proxy = client_ep.into_proxy().unwrap();

        let client_future = async move {
            let (client_ep, server_ep) = create_endpoints::<BeaconInfoStreamMarker>().unwrap();
            let params = NetworkScanParameters::EMPTY;

            assert_matches!(proxy.start_network_scan(params, server_ep), Ok(()));

            let scanner = client_ep.into_proxy().unwrap();
            let mut results = vec![];

            loop {
                let mut next = scanner.next().await.unwrap();
                if next.is_empty() {
                    break;
                }
                results.append(&mut next);
            }

            assert_eq!(results.len(), 3, "Unexpected number of scan results");

            assert!(scanner.next().await.is_err(), "Calling next again should error");
        };

        futures::select! {
            err = server_future.boxed_local().fuse() => panic!("Server task stopped: {:?}", err),
            _ = client_future.boxed().fuse() => (),
        }
    }
}
