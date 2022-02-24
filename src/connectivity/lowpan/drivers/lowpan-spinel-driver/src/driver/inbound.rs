// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

use super::*;
use crate::prelude::*;
use crate::spinel::*;
use futures::prelude::*;

use anyhow::Error;

impl<DS: SpinelDeviceClient, NI: NetworkInterface> SpinelDriver<DS, NI> {
    /// Method for handling inbound frames and pushing them
    /// to where they need to go.
    ///
    /// Specifically, all inbound frames that are property change notifications,
    /// (PROP_IS, PROP_INSERTED, and PROP_REMOVED) are dispatched
    /// to the appropriate `on_prop_value_*()` handler. Also, if the TID is
    /// non-zero, the frame also is also sent to the frame handler for
    /// response dispatch.
    ///
    /// Resets are a special case and are handled directly.
    /// Everything else is delegated as described above.
    async fn on_inbound_frame(&self, raw_frame: &[u8]) -> Result<(), Error> {
        const RESET_RESPONSE_PREFIX: &[u8] = &[0x80, 0x06, 0x00];

        // Parse the header.
        let frame = SpinelFrameRef::try_unpack_from_slice(raw_frame).context("on_inbound_frame")?;

        fx_log_debug!("[NCP->] {:?}", frame);

        // If we are waiting for a reset and this frame doesn't
        // look like it might be a reset indication then we drop the frame.
        if self.driver_state.lock().init_state == InitState::WaitingForReset
            && !raw_frame.starts_with(RESET_RESPONSE_PREFIX)
        {
            fx_log_info!("on_inbound_frame: Waiting for reset, dropping {:?}", frame);
            return Ok(());
        }

        // We only support NLI zero for the foreseeable future.
        if frame.header.nli() != 0 {
            fx_log_info!("on_inbound_frame: Skipping unexpected NLI for frame {:?}", frame);
            return Ok(());
        }

        // Is the command `PropValueIs`, `PropValueInserted`, or `PropValueRemoved`?
        // If so, dispatch to state update.
        match frame.cmd {
            Cmd::PropValueIs => {
                let prop_value = SpinelPropValueRef::try_unpack_from_slice(frame.payload)
                    .context("on_inbound_frame:PropValueIs")?;

                if prop_value.prop == Prop::Stream(PropStream::Net) {
                    let network_packet = NetworkPacket::try_unpack_from_slice(prop_value.value)
                        .context("on_inbound_frame:parsing_net_packet")?;

                    self.net_if.inbound_packet_to_stack(network_packet.packet).await?;

                    let mut driver_state = self.driver_state.lock();
                    if driver_state.assisting_state.update_from_secure(network_packet.packet) {
                        fx_log_info!("on_inbound_frame: Commissioning session successfully migrated to mesh network: {:?}", Ipv6PacketDebug(network_packet.packet));
                    }
                } else if prop_value.prop == Prop::Stream(PropStream::NetInsecure) {
                    let network_packet = NetworkPacket::try_unpack_from_slice(prop_value.value)
                        .context("on_inbound_frame:parsing_insecure_net_packet")?;

                    fx_log_info!(
                        "on_inbound_frame: Commissioning packet: {:?}",
                        Ipv6PacketDebug(network_packet.packet)
                    );

                    let should_allow_from_insecure = {
                        let mut driver_state = self.driver_state.lock();
                        driver_state
                            .assisting_state
                            .should_allow_from_insecure(network_packet.packet)
                    };

                    if should_allow_from_insecure {
                        fx_log_info!("on_inbound_frame: Commissioning packet routed to stack");
                        self.net_if.inbound_packet_to_stack(network_packet.packet).await?;
                    } else {
                        fx_log_info!("on_inbound_frame: Dropping commissioning packet packet");
                    }
                } else {
                    self.on_prop_value_is(prop_value.prop, prop_value.value)?;
                }

                // We handle resets explicitly here.
                // An actionable reset is defined as an unsolicited (TID Zero/None)
                // `Prop::LastStatus` update with a status code in the reset range.
                // Solicited responses (TID 1-15) are not considered resets.
                if prop_value.prop == Prop::LastStatus && frame.header.tid().is_none() {
                    if let Status::Reset(x) = Status::try_unpack_from_slice(prop_value.value)? {
                        fx_log_info!("on_inbound_frame: Reset: {:?}", x);
                        self.frame_handler.clear();
                        {
                            let mut driver_state = self.driver_state.lock();
                            if driver_state.init_state != InitState::WaitingForReset {
                                let maybe_old_state = driver_state.prepare_for_init();

                                // Avoid holding the mutex for longer than we need to.
                                std::mem::drop(driver_state);

                                self.driver_state_change.trigger();

                                if let Some(old_state) = maybe_old_state {
                                    // Make sure we signal a connectivity state change.
                                    self.on_connectivity_state_change(
                                        ConnectivityState::Attaching,
                                        old_state,
                                    );
                                }
                            }
                        }
                        self.ncp_did_reset.trigger();
                    }
                }
            }

            Cmd::PropValueInserted => {
                let prop_value = SpinelPropValueRef::try_unpack_from_slice(frame.payload)
                    .context("on_inbound_frame:PropValueInserted")?;
                self.on_prop_value_inserted(prop_value.prop, prop_value.value)?;
            }

            Cmd::PropValueRemoved => {
                let prop_value = SpinelPropValueRef::try_unpack_from_slice(frame.payload)
                    .context("on_inbound_frame:PropValueRemoved")?;
                self.on_prop_value_removed(prop_value.prop, prop_value.value)?;
            }

            // Ignore all other commands for now.
            _ => {}
        }

        // Finally pass the frame to the response frame handler.
        self.frame_handler.handle_inbound_frame(frame)
    }

    /// Wraps the given inbound frame stream with the
    /// logic to parse and handle the inbound frames.
    /// The resulting stream should be asynchronously
    /// drained to process inbound frames.
    ///
    /// In most cases you will want to call `take_inbound_stream` instead,
    /// but if it is unavailable (or you want to add filters or whatnot)
    /// then this is what you will use.
    ///
    /// You may only wrap an inbound stream once. Calling this
    /// method twice will cause a panic.
    pub fn wrap_inbound_stream<'a, T>(
        &'a self,
        device_stream: T,
    ) -> impl Stream<Item = Result<(), Error>> + Send + 'a
    where
        T: futures::stream::Stream<Item = Result<Vec<u8>, Error>> + Send + 'a,
    {
        let wrapped_future = device_stream
            .and_then(move |f| {
                async move {
                    // Note that this really does need to be an async block
                    // in order to keep the lifetime of `f` in scope for the
                    // duration of the async call.
                    self.on_inbound_frame(&f).await
                }
            })
            .or_else(move |err| {
                futures::future::ready(match err {
                    // I/O errors are fatal.
                    err if err.is::<std::io::Error>() => Err(err),

                    // Cancelled errors are ignored at this level.
                    err if err.is::<Canceled>() => Ok(()),

                    // Other error cases may be added here in the future.
                    err => {
                        if self.driver_state.lock().is_waiting_for_reset() {
                            // Non-I/O errors while waiting for a reset are just a warning.
                            fx_log_warn!("inbound_frame_stream: Warning: {:?}", err);
                        } else {
                            // Non-I/O errors when not waiting for a reset will cause us to re-init.
                            fx_log_err!("inbound_frame_stream: Error: {:?}", err);
                            self.ncp_is_misbehaving();
                        }
                        Ok(())
                    }
                })
            });
        futures::stream::select(
            wrapped_future,
            self.take_main_task().boxed().map_err(|x| x.context("main_task")).into_stream(),
        )
    }
}

impl<NI: NetworkInterface>
    SpinelDriver<SpinelDeviceSink<fidl_fuchsia_lowpan_spinel::DeviceProxy>, NI>
{
    /// Takes the inbound frame stream from `SpinelDeviceSink::take_stream()`,
    /// adds the frame-handling logic, and returns the resulting stream.
    ///
    /// Since `SpinelDeviceSink::take_stream()` can only be called once, this
    /// method likewise can only be called once. Subsequent calls will panic.
    ///
    /// This method is only available if the type of `DS` is
    /// `SpinelDeviceSink<fidl_fuchsia_lowpan_spinel::DeviceProxy>`.
    /// In all other cases you must use `wrap_inbound_stream()` instead.
    pub fn take_inbound_stream(&self) -> impl Stream<Item = Result<(), Error>> + Send + '_ {
        self.wrap_inbound_stream(self.device_sink.take_stream())
    }
}
