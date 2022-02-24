// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! An implementation that extends the generic control protocol to comply with IPCP.

use {
    crate::{
        link,
        ppp::{
            self, ControlProtocol as PppControlProtocol, FrameTransmitter, Opened, ProtocolError,
            ProtocolState, CODE_CODE_REJECT, CODE_CONFIGURE_ACK, CODE_CONFIGURE_NAK,
            CODE_CONFIGURE_REJECT, CODE_CONFIGURE_REQUEST, CODE_TERMINATE_ACK,
            CODE_TERMINATE_REQUEST, PROTOCOL_IPV4_CONTROL,
        },
    },
    packet::{GrowBuffer, InnerPacketBuilder, ParsablePacket, ParseBuffer, Serializer},
    ppp_packet::{
        ipv4,
        records::options::{Options, OptionsSerializer},
        CodeRejectPacket, ConfigurationPacket, ControlProtocolPacket, TerminationPacket,
    },
};

/// An implementation of the PPP IP Control Protocol.
#[derive(Debug, Copy, Clone)]
pub struct ControlProtocol;

impl ppp::ControlProtocol for ControlProtocol {
    type Option = ipv4::ControlOption;
    const PROTOCOL_IDENTIFIER: u16 = PROTOCOL_IPV4_CONTROL;

    fn unacceptable_options(received: &[ipv4::ControlOption]) -> Vec<ipv4::ControlOption> {
        received
            .iter()
            .filter(|option| match option {
                ipv4::ControlOption::IpAddress(_) => false,
                _ => true,
            })
            .cloned()
            .collect::<Vec<_>>()
    }

    fn parse_options(buf: &[u8]) -> Option<Vec<ipv4::ControlOption>> {
        Options::<_, ipv4::ControlOptionsImpl>::parse(buf)
            .ok()
            .map(|options| options.iter().collect())
    }

    fn serialize_options(options: &[ipv4::ControlOption]) -> ::packet::Buf<Vec<u8>> {
        crate::flatten_either(
            OptionsSerializer::<ipv4::ControlOptionsImpl, ipv4::ControlOption, _>::new(
                options.iter(),
            )
            .into_serializer()
            .serialize_vec_outer()
            .ok()
            .unwrap(),
        )
    }
}

/// Update the current IPCP state given the current time by performing restarts, or resetting if the
/// link state is no longer open.
pub async fn update<'a, T>(
    resumable_state: ProtocolState<ControlProtocol>,
    link_state: &'a ProtocolState<link::ControlProtocol>,
    transmitter: &'a T,
    time: std::time::Instant,
) -> Result<ProtocolState<ControlProtocol>, ProtocolError<ControlProtocol>>
where
    T: FrameTransmitter,
{
    if let ProtocolState::Opened(_) = link_state {
        resumable_state.restart(transmitter, time).await
    } else {
        Ok(resumable_state.reset())
    }
}

/// Process an incoming IPCP packet, driving the state machine forward, producing a new state or an
/// error. An opened link must be provided to ensure the protocols are not in an inconsistent state.
pub async fn receive<'a, T, B>(
    resumable_state: ProtocolState<ControlProtocol>,
    _link_opened: &'a Opened<link::ControlProtocol>,
    transmitter: &'a T,
    mut buf: ::packet::Buf<B>,
    time: std::time::Instant,
) -> Result<ProtocolState<ControlProtocol>, ProtocolError<ControlProtocol>>
where
    T: FrameTransmitter,
    B: AsRef<[u8]> + AsMut<[u8]>,
{
    let control_packet = if let Ok(control_packet) = buf.parse::<ControlProtocolPacket<_>>() {
        control_packet
    } else {
        return Ok(resumable_state);
    };
    let code = control_packet.code();
    let identifier = control_packet.identifier();

    match code {
        CODE_CONFIGURE_REQUEST
        | CODE_CONFIGURE_ACK
        | CODE_CONFIGURE_NAK
        | CODE_CONFIGURE_REJECT => {
            if buf.parse::<ConfigurationPacket<_>>().is_err() {
                return Ok(resumable_state);
            }
            let options = if let Some(options) = ControlProtocol::parse_options(buf.as_ref()) {
                options
            } else {
                return Ok(resumable_state);
            };
            match code {
                CODE_CONFIGURE_REQUEST => {
                    resumable_state.rx_configure_req(transmitter, &options, identifier, time).await
                }
                CODE_CONFIGURE_ACK => {
                    resumable_state.rx_configure_ack(transmitter, &options, identifier, time).await
                }
                CODE_CONFIGURE_NAK | CODE_CONFIGURE_REJECT => {
                    resumable_state.rx_configure_rej(transmitter, &options, identifier, time).await
                }
                _ => unreachable!(),
            }
        }
        CODE_TERMINATE_REQUEST | CODE_TERMINATE_ACK => {
            if buf.parse::<TerminationPacket<_>>().is_err() {
                return Ok(resumable_state);
            }
            resumable_state.rx_terminate_req(transmitter, identifier).await
        }
        CODE_CODE_REJECT => {
            if buf.parse::<CodeRejectPacket<_>>().is_err() {
                return Ok(resumable_state);
            }
            Err(ProtocolError::FatalCodeRej(buf.as_ref().to_vec()))
        }
        _ => {
            let metadata = control_packet.parse_metadata();
            buf.undo_parse(metadata);
            ppp::tx_code_rej::<ControlProtocol, _, _>(transmitter, buf, identifier).await?;
            Ok(resumable_state)
        }
    }
}
