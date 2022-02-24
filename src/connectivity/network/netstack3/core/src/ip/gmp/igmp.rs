// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Internet Group Management Protocol, Version 2 (IGMPv2).
//!
//! IGMPv2 is a communications protocol used by hosts and adjacent routers on
//! IPv4 networks to establish multicast group memberships.

use core::{
    fmt::{Debug, Display},
    time::Duration,
};

use log::{debug, error, trace};
use net_types::{
    ip::{AddrSubnet, Ipv4, Ipv4Addr},
    MulticastAddr, SpecifiedAddr, Witness,
};
use packet::{BufferMut, EmptyBuf, InnerPacketBuilder, Serializer};
use packet_formats::{
    igmp::{
        messages::{IgmpLeaveGroup, IgmpMembershipReportV1, IgmpMembershipReportV2, IgmpPacket},
        IgmpMessage, IgmpPacketBuilder, MessageType,
    },
    ip::Ipv4Proto,
    ipv4::{
        options::{Ipv4Option, Ipv4OptionData},
        Ipv4OptionsTooLongError, Ipv4PacketBuilder, Ipv4PacketBuilderWithOptions,
    },
};
use thiserror::Error;
use zerocopy::ByteSlice;

use crate::{
    context::{FrameContext, InstantContext, RngContext, TimerContext, TimerHandler},
    ip::{
        gmp::{
            gmp_join_group, gmp_leave_group, handle_gmp_message, Action, Actions, GmpAction,
            GmpContext, GmpHandler, GmpMessage, GmpStateMachine, GroupJoinResult, GroupLeaveResult,
            MulticastGroupSet, ProtocolSpecific,
        },
        IpDeviceIdContext,
    },
    Instant,
};

/// Metadata for sending an IGMP packet.
///
/// `IgmpPacketMetadata` is used by [`IgmpContext`]'s [`FrameContext`] bound.
/// When [`FrameContext::send_frame`] is called with an `IgmpPacketMetadata`,
/// the body will be encapsulated in an IP packet with a TTL of 1 and with the
/// "Router Alert" option set.
#[cfg_attr(test, derive(Debug, PartialEq))]
pub(crate) struct IgmpPacketMetadata<D> {
    pub(crate) device: D,
    pub(crate) dst_ip: MulticastAddr<Ipv4Addr>,
}

impl<D> IgmpPacketMetadata<D> {
    fn new(device: D, dst_ip: MulticastAddr<Ipv4Addr>) -> IgmpPacketMetadata<D> {
        IgmpPacketMetadata { device, dst_ip }
    }
}

/// The execution context for the Internet Group Management Protocol (IGMP).
pub(crate) trait IgmpContext:
    IpDeviceIdContext<Ipv4>
    + TimerContext<IgmpTimerId<Self::DeviceId>>
    + FrameContext<EmptyBuf, IgmpPacketMetadata<Self::DeviceId>>
    + RngContext
{
    /// Gets an IP address and subnet associated with this device.
    fn get_ip_addr_subnet(&self, device: Self::DeviceId) -> Option<AddrSubnet<Ipv4Addr>>;

    /// Is IGMP enabled for `device`?
    ///
    /// If `igmp_enabled` returns false, then [`GmpHandler::gmp_join_group`] and
    /// [`GmpHandler::gmp_leave_group`] will still join/leave multicast groups
    /// locally, and inbound IGMP packets will still be processed, but no timers
    /// will be installed, and no outbound IGMP traffic will be generated.
    fn igmp_enabled(&self, device: Self::DeviceId) -> bool;

    /// Gets mutable access to the device's IGMP state and RNG.
    fn get_state_mut_and_rng(
        &mut self,
        device: Self::DeviceId,
    ) -> (
        &mut MulticastGroupSet<Ipv4Addr, IgmpGroupState<<Self as InstantContext>::Instant>>,
        &mut Self::Rng,
    );

    /// Gets mutable access to the device's IGMP state.
    fn get_state_mut(
        &mut self,
        device: Self::DeviceId,
    ) -> &mut MulticastGroupSet<Ipv4Addr, IgmpGroupState<<Self as InstantContext>::Instant>> {
        let (state, _rng): (_, &mut Self::Rng) = self.get_state_mut_and_rng(device);
        state
    }
}

impl<C: IgmpContext> GmpHandler<Ipv4> for C {
    fn gmp_join_group(
        &mut self,
        device: C::DeviceId,
        group_addr: MulticastAddr<Ipv4Addr>,
    ) -> GroupJoinResult {
        gmp_join_group(self, device, group_addr)
    }

    fn gmp_leave_group(
        &mut self,
        device: C::DeviceId,
        group_addr: MulticastAddr<Ipv4Addr>,
    ) -> GroupLeaveResult {
        gmp_leave_group(self, device, group_addr)
    }
}

/// A handler for incoming IGMP packets.
///
/// A blanket implementation is provided for all `C: IgmpContext`.
pub(crate) trait IgmpPacketHandler<DeviceId, B: BufferMut> {
    /// Receive an IGMP message in an IP packet.
    fn receive_igmp_packet(
        &mut self,
        device: DeviceId,
        src_ip: Ipv4Addr,
        dst_ip: SpecifiedAddr<Ipv4Addr>,
        buffer: B,
    );
}

impl<C: IgmpContext, B: BufferMut> IgmpPacketHandler<C::DeviceId, B> for C {
    fn receive_igmp_packet(
        &mut self,
        device: C::DeviceId,
        _src_ip: Ipv4Addr,
        _dst_ip: SpecifiedAddr<Ipv4Addr>,
        mut buffer: B,
    ) {
        let packet = match buffer.parse_with::<_, IgmpPacket<&[u8]>>(()) {
            Ok(packet) => packet,
            Err(_) => {
                debug!("Cannot parse the incoming IGMP packet, dropping.");
                return;
            } // TODO: Do something else here?
        };

        if let Err(e) = match packet {
            IgmpPacket::MembershipQueryV2(msg) => {
                let now = self.now();
                handle_gmp_message(self, device, &msg, |rng, IgmpGroupState(state)| {
                    state.query_received(rng, msg.max_response_time().into(), now)
                })
            }
            IgmpPacket::MembershipReportV1(msg) => {
                handle_gmp_message(self, device, &msg, |_, IgmpGroupState(state)| {
                    state.report_received()
                })
            }
            IgmpPacket::MembershipReportV2(msg) => {
                handle_gmp_message(self, device, &msg, |_, IgmpGroupState(state)| {
                    state.report_received()
                })
            }
            IgmpPacket::LeaveGroup(_) => {
                debug!("Hosts are not interested in Leave Group messages");
                return;
            }
            _ => {
                debug!("We do not support IGMPv3 yet");
                return;
            }
        } {
            error!("Error occurred when handling IGMPv2 message: {}", e);
        }
    }
}

impl<B: ByteSlice, M: MessageType<B, FixedHeader = Ipv4Addr>> GmpMessage<Ipv4>
    for IgmpMessage<B, M>
{
    fn group_addr(&self) -> Ipv4Addr {
        self.group_addr()
    }
}

impl<C: IgmpContext> GmpContext<Ipv4, Igmpv2ProtocolSpecific> for C {
    type Err = IgmpError<C::DeviceId>;
    type GroupState = IgmpGroupState<Self::Instant>;
    fn run_actions(
        &mut self,
        device: Self::DeviceId,
        actions: Actions<Igmpv2ProtocolSpecific>,
        group_addr: MulticastAddr<Ipv4Addr>,
    ) {
        if self.igmp_enabled(device) {
            for action in actions {
                if let Err(err) = run_action(self, device, action, group_addr) {
                    error!(
                        "Error performing action on {} on device {}: {}",
                        group_addr, device, err
                    );
                }
            }
        } else {
            trace!("skipping executing IGMP actions on device {}: IGMP disabled on device", device);
        }
    }

    fn not_a_member_err(addr: Ipv4Addr) -> Self::Err {
        Self::Err::NotAMember { addr }
    }

    fn get_state_mut_and_rng(
        &mut self,
        device: C::DeviceId,
    ) -> (&mut MulticastGroupSet<Ipv4Addr, Self::GroupState>, &mut Self::Rng) {
        self.get_state_mut_and_rng(device)
    }
}

#[derive(Debug, Error)]
pub(crate) enum IgmpError<D: Display + Debug + Send + Sync + 'static> {
    /// The host is trying to operate on an group address of which the host is
    /// not a member.
    #[error("the host has not already been a member of the address: {}", addr)]
    NotAMember { addr: Ipv4Addr },
    /// Failed to send an IGMP packet.
    #[error("failed to send out an IGMP packet to address: {}", addr)]
    SendFailure { addr: Ipv4Addr },
    /// The given device does not have an assigned IP address.
    #[error("no ip address is associated with the device: {}", device)]
    NoIpAddress { device: D },
}

pub(crate) type IgmpResult<T, D> = Result<T, IgmpError<D>>;

#[derive(Debug, PartialEq, Eq, Clone, Copy, Hash)]
pub(crate) enum IgmpTimerId<DeviceId> {
    /// The timer used to switch a host from Delay Member state to Idle Member
    /// state.
    ReportDelay { device: DeviceId, group_addr: MulticastAddr<Ipv4Addr> },
    /// The timer used to determine whether there is a router speaking IGMPv1.
    V1RouterPresent { device: DeviceId },
}

impl<DeviceId> IgmpTimerId<DeviceId> {
    fn new_report_delay(
        device: DeviceId,
        group_addr: MulticastAddr<Ipv4Addr>,
    ) -> IgmpTimerId<DeviceId> {
        IgmpTimerId::ReportDelay { device, group_addr }
    }

    fn new_v1_router_present(device: DeviceId) -> IgmpTimerId<DeviceId> {
        IgmpTimerId::V1RouterPresent { device }
    }
}

impl<C: IgmpContext> TimerHandler<IgmpTimerId<C::DeviceId>> for C {
    fn handle_timer(&mut self, timer: IgmpTimerId<C::DeviceId>) {
        match timer {
            IgmpTimerId::ReportDelay { device, group_addr } => {
                let actions = match self.get_state_mut(device).get_mut(&group_addr) {
                    Some(IgmpGroupState(state)) => state.report_timer_expired(),
                    None => {
                        error!("Not already a member");
                        return;
                    }
                };
                self.run_actions(device, actions, group_addr);
            }
            IgmpTimerId::V1RouterPresent { device } => {
                for (_, IgmpGroupState(state)) in self.get_state_mut(device).iter_mut() {
                    state.v1_router_present_timer_expired();
                }
            }
        }
    }
}

fn send_igmp_message<C: IgmpContext, M>(
    ctx: &mut C,
    device: C::DeviceId,
    group_addr: MulticastAddr<Ipv4Addr>,
    dst_ip: MulticastAddr<Ipv4Addr>,
    max_resp_time: M::MaxRespTime,
) -> IgmpResult<(), C::DeviceId>
where
    M: MessageType<EmptyBuf, FixedHeader = Ipv4Addr, VariableBody = ()>,
{
    let src_ip = match ctx.get_ip_addr_subnet(device) {
        Some(addr_subnet) => addr_subnet.addr(),
        None => return Err(IgmpError::NoIpAddress { device }),
    };
    let body =
        IgmpPacketBuilder::<EmptyBuf, M>::new_with_resp_time(group_addr.get(), max_resp_time);
    let builder = match Ipv4PacketBuilderWithOptions::new(
        Ipv4PacketBuilder::new(src_ip, dst_ip, 1, Ipv4Proto::Igmp),
        &[Ipv4Option { copied: true, data: Ipv4OptionData::RouterAlert { data: 0 } }],
    ) {
        Err(Ipv4OptionsTooLongError) => return Err(IgmpError::SendFailure { addr: *group_addr }),
        Ok(builder) => builder,
    };
    let body = body.into_serializer().encapsulate(builder);

    ctx.send_frame(IgmpPacketMetadata::new(device, dst_ip), body)
        .map_err(|_| IgmpError::SendFailure { addr: *group_addr })
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
struct Igmpv2ProtocolSpecific {
    v1_router_present: bool,
}

impl Default for Igmpv2ProtocolSpecific {
    fn default() -> Self {
        Igmpv2ProtocolSpecific { v1_router_present: false }
    }
}

#[derive(PartialEq, Eq, Debug)]
enum Igmpv2Actions {
    ScheduleV1RouterPresentTimer(Duration),
}

#[derive(Debug)]
struct Igmpv2HostConfig {
    // When a host wants to send a report not because of a query, this value is
    // used as the delay timer.
    unsolicited_report_interval: Duration,
    // When this option is true, the host can send a leave message even when it
    // is not the last one in the multicast group.
    send_leave_anyway: bool,
    // Default timer value for Version 1 Router Present Timeout.
    v1_router_present_timeout: Duration,
}

/// The default value for `unsolicited_report_interval` as per [RFC 2236 Section
/// 8.10].
///
/// [RFC 2236 Section 8.10]: https://tools.ietf.org/html/rfc2236#section-8.10
const DEFAULT_UNSOLICITED_REPORT_INTERVAL: Duration = Duration::from_secs(10);
/// The default value for `v1_router_present_timeout` as per [RFC 2236 Section
/// 8.11].
///
/// [RFC 2236 Section 8.11]: https://tools.ietf.org/html/rfc2236#section-8.11
const DEFAULT_V1_ROUTER_PRESENT_TIMEOUT: Duration = Duration::from_secs(400);
/// The default value for the `MaxRespTime` if the query is a V1 query, whose
/// `MaxRespTime` field is 0 in the packet. Please refer to [RFC 2236 Section
/// 4].
///
/// [RFC 2236 Section 4]: https://tools.ietf.org/html/rfc2236#section-4
const DEFAULT_V1_QUERY_MAX_RESP_TIME: Duration = Duration::from_secs(10);

impl Default for Igmpv2HostConfig {
    fn default() -> Self {
        Igmpv2HostConfig {
            unsolicited_report_interval: DEFAULT_UNSOLICITED_REPORT_INTERVAL,
            send_leave_anyway: false,
            v1_router_present_timeout: DEFAULT_V1_ROUTER_PRESENT_TIMEOUT,
        }
    }
}

impl ProtocolSpecific for Igmpv2ProtocolSpecific {
    type Action = Igmpv2Actions;
    type Config = Igmpv2HostConfig;

    fn cfg_unsolicited_report_interval(cfg: &Self::Config) -> Duration {
        cfg.unsolicited_report_interval
    }

    fn cfg_send_leave_anyway(cfg: &Self::Config) -> bool {
        cfg.send_leave_anyway
    }

    fn get_max_resp_time(resp_time: Duration) -> Duration {
        if resp_time.as_micros() == 0 {
            DEFAULT_V1_QUERY_MAX_RESP_TIME
        } else {
            resp_time
        }
    }

    fn do_query_received_specific(
        cfg: &Self::Config,
        actions: &mut Actions<Self>,
        max_resp_time: Duration,
        _old: Igmpv2ProtocolSpecific,
    ) -> Igmpv2ProtocolSpecific {
        // IGMPv2 hosts should be compatible with routers that only speak
        // IGMPv1. When an IGMPv2 host receives an IGMPv1 query (whose
        // `MaxRespCode` is 0), it should set up a timer and only respond with
        // IGMPv1 responses before the timer expires. Please refer to
        // https://tools.ietf.org/html/rfc2236#section-4 for details.
        let new_ps = Igmpv2ProtocolSpecific { v1_router_present: max_resp_time.as_micros() == 0 };
        if new_ps.v1_router_present {
            actions.push_specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(
                cfg.v1_router_present_timeout,
            ));
        }
        new_ps
    }
}

#[cfg_attr(test, derive(Debug))]
pub(crate) struct IgmpGroupState<I: Instant>(GmpStateMachine<I, Igmpv2ProtocolSpecific>);

impl<I: Instant> From<GmpStateMachine<I, Igmpv2ProtocolSpecific>> for IgmpGroupState<I> {
    fn from(state: GmpStateMachine<I, Igmpv2ProtocolSpecific>) -> IgmpGroupState<I> {
        IgmpGroupState(state)
    }
}

impl<I: Instant> From<IgmpGroupState<I>> for GmpStateMachine<I, Igmpv2ProtocolSpecific> {
    fn from(
        IgmpGroupState(state): IgmpGroupState<I>,
    ) -> GmpStateMachine<I, Igmpv2ProtocolSpecific> {
        state
    }
}

impl<I: Instant> GmpStateMachine<I, Igmpv2ProtocolSpecific> {
    fn v1_router_present_timer_expired(&mut self) {
        let Actions(actions) =
            self.update_with_protocol_specific(Igmpv2ProtocolSpecific { v1_router_present: false });
        assert_eq!(actions, []);
    }
}

/// Interprets the `action`.
fn run_action<C: IgmpContext>(
    ctx: &mut C,
    device: C::DeviceId,
    action: Action<Igmpv2ProtocolSpecific>,
    group_addr: MulticastAddr<Ipv4Addr>,
) -> IgmpResult<(), C::DeviceId> {
    match action {
        Action::Generic(GmpAction::ScheduleReportTimer(duration)) => {
            let _: Option<C::Instant> =
                ctx.schedule_timer(duration, IgmpTimerId::new_report_delay(device, group_addr));
        }
        Action::Generic(GmpAction::StopReportTimer) => {
            let _: Option<C::Instant> =
                ctx.cancel_timer(IgmpTimerId::new_report_delay(device, group_addr));
        }
        Action::Generic(GmpAction::SendLeave) => {
            send_igmp_message::<_, IgmpLeaveGroup>(
                ctx,
                device,
                group_addr,
                Ipv4::ALL_ROUTERS_MULTICAST_ADDRESS,
                (),
            )?;
        }
        Action::Generic(GmpAction::SendReport(Igmpv2ProtocolSpecific { v1_router_present })) => {
            if v1_router_present {
                send_igmp_message::<_, IgmpMembershipReportV1>(
                    ctx,
                    device,
                    group_addr,
                    group_addr,
                    (),
                )?;
            } else {
                send_igmp_message::<_, IgmpMembershipReportV2>(
                    ctx,
                    device,
                    group_addr,
                    group_addr,
                    (),
                )?;
            }
        }
        Action::Specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(duration)) => {
            let _: Option<C::Instant> =
                ctx.schedule_timer(duration, IgmpTimerId::new_v1_router_present(device));
        }
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use alloc::vec::Vec;
    use core::convert::TryInto;

    use net_types::ip::AddrSubnet;
    use packet::serialize::{Buf, InnerPacketBuilder, Serializer};
    use packet_formats::igmp::messages::IgmpMembershipQueryV2;
    use rand_xorshift::XorShiftRng;

    use super::*;
    use crate::{
        assert_empty,
        context::{
            testutil::{DummyInstant, DummyTimerCtxExt},
            DualStateContext,
        },
        ip::{
            gmp::{Action, GmpAction, MemberState},
            DummyDeviceId,
        },
        testutil::{new_rng, run_with_many_seeds, FakeCryptoRng},
    };

    /// A dummy [`IgmpContext`] that stores the [`MulticastGroupSet`] and an
    /// optional IPv4 address and subnet that may be returned in calls to
    /// [`IgmpContext::get_ip_addr_subnet`].
    struct DummyIgmpCtx {
        groups: MulticastGroupSet<Ipv4Addr, IgmpGroupState<DummyInstant>>,
        igmp_enabled: bool,
        addr_subnet: Option<AddrSubnet<Ipv4Addr>>,
    }

    impl Default for DummyIgmpCtx {
        fn default() -> DummyIgmpCtx {
            DummyIgmpCtx {
                groups: MulticastGroupSet::default(),
                igmp_enabled: true,
                addr_subnet: None,
            }
        }
    }

    type DummyCtx = crate::context::testutil::DummyCtx<
        DummyIgmpCtx,
        IgmpTimerId<DummyDeviceId>,
        IgmpPacketMetadata<DummyDeviceId>,
    >;

    impl IgmpContext for DummyCtx {
        fn get_ip_addr_subnet(&self, _device: DummyDeviceId) -> Option<AddrSubnet<Ipv4Addr>> {
            self.get_ref().addr_subnet
        }

        fn igmp_enabled(&self, _device: DummyDeviceId) -> bool {
            self.get_ref().igmp_enabled
        }

        fn get_state_mut_and_rng(
            &mut self,
            _id0: DummyDeviceId,
        ) -> (
            &mut MulticastGroupSet<Ipv4Addr, IgmpGroupState<DummyInstant>>,
            &mut FakeCryptoRng<XorShiftRng>,
        ) {
            let (state, rng) = self.get_states_mut();
            (&mut state.groups, rng)
        }
    }

    fn at_least_one_action(
        actions: Actions<Igmpv2ProtocolSpecific>,
        action: Action<Igmpv2ProtocolSpecific>,
    ) -> bool {
        actions.into_iter().any(|a| a == action)
    }

    #[test]
    fn test_igmp_state_with_igmpv1_router() {
        run_with_many_seeds(|seed| {
            let mut rng = new_rng(seed);
            let (mut s, _actions) = GmpStateMachine::join_group(&mut rng, DummyInstant::default());
            let Actions(actions) =
                s.query_received(&mut rng, Duration::from_secs(0), DummyInstant::default());
            assert_eq!(
                actions,
                [Action::Specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(
                    DEFAULT_V1_ROUTER_PRESENT_TIMEOUT
                ))]
            );
            let actions = s.report_timer_expired();
            assert!(at_least_one_action(
                actions,
                Action::<Igmpv2ProtocolSpecific>::Generic(GmpAction::SendReport(
                    Igmpv2ProtocolSpecific { v1_router_present: true },
                )),
            ));
        });
    }

    #[test]
    fn test_igmp_state_igmpv1_router_present_timer_expires() {
        run_with_many_seeds(|seed| {
            let mut rng = new_rng(seed);
            let (mut s, _actions) = GmpStateMachine::<_, Igmpv2ProtocolSpecific>::join_group(
                &mut rng,
                DummyInstant::default(),
            );
            let Actions(actions) =
                s.query_received(&mut rng, Duration::from_secs(0), DummyInstant::default());
            assert_eq!(
                actions,
                [Action::Specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(
                    DEFAULT_V1_ROUTER_PRESENT_TIMEOUT
                ))]
            );
            match s.get_inner() {
                MemberState::Delaying(state) => {
                    assert!(state.get_protocol_specific().v1_router_present);
                }
                _ => panic!("Wrong State!"),
            }
            s.v1_router_present_timer_expired();
            match s.get_inner() {
                MemberState::Delaying(state) => {
                    assert!(!state.get_protocol_specific().v1_router_present);
                }
                _ => panic!("Wrong State!"),
            }
            let Actions(actions) =
                s.query_received(&mut rng, Duration::from_secs(0), DummyInstant::default());
            assert_eq!(
                actions,
                [Action::Specific(Igmpv2Actions::ScheduleV1RouterPresentTimer(
                    DEFAULT_V1_ROUTER_PRESENT_TIMEOUT
                ))]
            );
            let Actions(actions) = s.report_received();
            assert_eq!(actions, [Action::Generic(GmpAction::StopReportTimer)]);
            s.v1_router_present_timer_expired();
            match s.get_inner() {
                MemberState::Idle(state) => {
                    assert!(!state.get_protocol_specific().v1_router_present);
                }
                _ => panic!("Wrong State!"),
            }
        });
    }

    const MY_ADDR: SpecifiedAddr<Ipv4Addr> =
        unsafe { SpecifiedAddr::new_unchecked(Ipv4Addr::new([192, 168, 0, 2])) };
    const ROUTER_ADDR: Ipv4Addr = Ipv4Addr::new([192, 168, 0, 1]);
    const OTHER_HOST_ADDR: Ipv4Addr = Ipv4Addr::new([192, 168, 0, 3]);
    const GROUP_ADDR: MulticastAddr<Ipv4Addr> = Ipv4::ALL_ROUTERS_MULTICAST_ADDRESS;
    const GROUP_ADDR_2: MulticastAddr<Ipv4Addr> =
        unsafe { MulticastAddr::new_unchecked(Ipv4Addr::new([224, 0, 0, 4])) };
    const REPORT_DELAY_TIMER_ID: IgmpTimerId<DummyDeviceId> =
        IgmpTimerId::ReportDelay { device: DummyDeviceId, group_addr: GROUP_ADDR };
    const REPORT_DELAY_TIMER_ID_2: IgmpTimerId<DummyDeviceId> =
        IgmpTimerId::ReportDelay { device: DummyDeviceId, group_addr: GROUP_ADDR_2 };
    const V1_ROUTER_PRESENT_TIMER_ID: IgmpTimerId<DummyDeviceId> =
        IgmpTimerId::V1RouterPresent { device: DummyDeviceId };

    fn receive_igmp_query(ctx: &mut DummyCtx, resp_time: Duration) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipQueryV2>::new_with_resp_time(
            GROUP_ADDR.get(),
            resp_time.try_into().unwrap(),
        );
        let buff = ser.into_serializer().serialize_vec_outer().unwrap();
        ctx.receive_igmp_packet(DummyDeviceId, ROUTER_ADDR, MY_ADDR, buff);
    }

    fn receive_igmp_general_query(ctx: &mut DummyCtx, resp_time: Duration) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipQueryV2>::new_with_resp_time(
            Ipv4Addr::new([0, 0, 0, 0]),
            resp_time.try_into().unwrap(),
        );
        let buff = ser.into_serializer().serialize_vec_outer().unwrap();
        ctx.receive_igmp_packet(DummyDeviceId, ROUTER_ADDR, MY_ADDR, buff);
    }

    fn receive_igmp_report(ctx: &mut DummyCtx) {
        let ser = IgmpPacketBuilder::<Buf<Vec<u8>>, IgmpMembershipReportV2>::new(GROUP_ADDR.get());
        let buff = ser.into_serializer().serialize_vec_outer().unwrap();
        ctx.receive_igmp_packet(DummyDeviceId, OTHER_HOST_ADDR, MY_ADDR, buff);
    }

    fn setup_simple_test_environment(seed: u128) -> DummyCtx {
        let mut ctx = DummyCtx::default();
        ctx.seed_rng(seed);
        ctx.get_mut().addr_subnet = Some(AddrSubnet::new(MY_ADDR.get(), 24).unwrap());
        ctx
    }

    fn ensure_ttl_ihl_rtr(ctx: &DummyCtx) {
        for (_, frame) in ctx.frames() {
            assert_eq!(frame[8], 1); // TTL,
            assert_eq!(&frame[20..24], &[148, 4, 0, 0]); // RTR
            assert_eq!(frame[0], 0x46); // IHL
        }
    }

    #[test]
    fn test_igmp_simple_integration() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);
            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));

            receive_igmp_query(&mut ctx, Duration::from_secs(10));
            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );

            // We should get two Igmpv2 reports - one for the unsolicited one
            // for the host to turn into Delay Member state and the other one
            // for the timer being fired.
            assert_eq!(ctx.frames().len(), 2);
        });
    }

    #[test]
    fn test_igmp_integration_fallback_from_idle() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);
            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            assert_eq!(ctx.frames().len(), 1);

            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );
            assert_eq!(ctx.frames().len(), 2);

            receive_igmp_query(&mut ctx, Duration::from_secs(10));

            // We have received a query, hence we are falling back to Delay
            // Member state.
            let IgmpGroupState(group_state) =
                IgmpContext::get_state_mut(&mut ctx, DummyDeviceId).get(&GROUP_ADDR).unwrap();
            match group_state.get_inner() {
                MemberState::Delaying(_) => {}
                _ => panic!("Wrong State!"),
            }

            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );
            assert_eq!(ctx.frames().len(), 3);
            ensure_ttl_ihl_rtr(&ctx);
        });
    }

    #[test]
    fn test_igmp_integration_igmpv1_router_present() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);

            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            let now = ctx.now();
            ctx.timer_ctx().assert_timers_installed([(
                REPORT_DELAY_TIMER_ID,
                now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL),
            )]);
            let instant1 = ctx.timer_ctx().timers()[0].0.clone();

            receive_igmp_query(&mut ctx, Duration::from_secs(0));
            assert_eq!(ctx.frames().len(), 1);

            // Since we have heard from the v1 router, we should have set our
            // flag.
            let IgmpGroupState(group_state) =
                IgmpContext::get_state_mut(&mut ctx, DummyDeviceId).get(&GROUP_ADDR).unwrap();
            match group_state.get_inner() {
                MemberState::Delaying(state) => {
                    assert!(state.get_protocol_specific().v1_router_present)
                }
                _ => panic!("Wrong State!"),
            }

            assert_eq!(ctx.frames().len(), 1);
            // Two timers: one for the delayed report, one for the v1 router
            // timer.
            let now = ctx.now();
            ctx.timer_ctx().assert_timers_installed([
                (REPORT_DELAY_TIMER_ID, now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL)),
                (V1_ROUTER_PRESENT_TIMER_ID, now..=(now + DEFAULT_V1_ROUTER_PRESENT_TIMEOUT)),
            ]);
            let instant2 = ctx.timer_ctx().timers()[1].0.clone();
            assert_eq!(instant1, instant2);

            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );
            // After the first timer, we send out our V1 report.
            assert_eq!(ctx.frames().len(), 2);
            // The last frame being sent should be a V1 report.
            let (_, frame) = ctx.frames().last().unwrap();
            // 34 and 0x12 are hacky but they can quickly tell it is a V1
            // report.
            assert_eq!(frame[24], 0x12);

            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(V1_ROUTER_PRESENT_TIMER_ID)
            );
            // After the second timer, we should reset our flag for v1 routers.
            let IgmpGroupState(group_state) =
                IgmpContext::get_state_mut(&mut ctx, DummyDeviceId).get(&GROUP_ADDR).unwrap();
            match group_state.get_inner() {
                MemberState::Idle(state) => {
                    assert!(!state.get_protocol_specific().v1_router_present)
                }
                _ => panic!("Wrong State!"),
            }

            receive_igmp_query(&mut ctx, Duration::from_secs(10));
            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );
            assert_eq!(ctx.frames().len(), 3);
            // Now we should get V2 report
            assert_eq!(ctx.frames().last().unwrap().1[24], 0x16);
            ensure_ttl_ihl_rtr(&ctx);
        });
    }

    #[test]
    fn test_igmp_integration_delay_reset_timer() {
        // This seed value was chosen to later produce a timer duration > 100ms.
        let mut ctx = setup_simple_test_environment(123456);
        assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
        let now = ctx.now();
        ctx.timer_ctx().assert_timers_installed([(
            REPORT_DELAY_TIMER_ID,
            now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL),
        )]);
        let instant1 = ctx.timer_ctx().timers()[0].0.clone();
        let start = ctx.now();
        let duration = Duration::from_micros(((instant1 - start).as_micros() / 2) as u64);
        assert!(duration.as_millis() > 100);
        receive_igmp_query(&mut ctx, duration);
        assert_eq!(ctx.frames().len(), 1);
        let now = ctx.now();
        ctx.timer_ctx().assert_timers_installed([(REPORT_DELAY_TIMER_ID, now..=(now + duration))]);
        let instant2 = ctx.timer_ctx().timers()[0].0.clone();
        // Because of the message, our timer should be reset to a nearer future.
        assert!(instant2 <= instant1);
        assert_eq!(ctx.trigger_next_timer(TimerHandler::handle_timer), Some(REPORT_DELAY_TIMER_ID));
        assert!(ctx.now() - start <= duration);
        assert_eq!(ctx.frames().len(), 2);
        // Make sure it is a V2 report.
        assert_eq!(ctx.frames().last().unwrap().1[24], 0x16);
        ensure_ttl_ihl_rtr(&ctx);
    }

    #[test]
    fn test_igmp_integration_last_send_leave() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);
            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            let now = ctx.now();
            ctx.timer_ctx().assert_timers_installed([(
                REPORT_DELAY_TIMER_ID,
                now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL),
            )]);
            // The initial unsolicited report.
            assert_eq!(ctx.frames().len(), 1);
            assert_eq!(
                ctx.trigger_next_timer(TimerHandler::handle_timer),
                Some(REPORT_DELAY_TIMER_ID)
            );
            // The report after the delay.
            assert_eq!(ctx.frames().len(), 2);
            assert_eq!(ctx.gmp_leave_group(DummyDeviceId, GROUP_ADDR), GroupLeaveResult::Left(()));
            // Our leave message.
            assert_eq!(ctx.frames().len(), 3);

            let leave_frame = &ctx.frames().last().unwrap().1;

            // Make sure it is a leave message.
            assert_eq!(leave_frame[24], 0x17);
            // Make sure the destination is ALL-ROUTERS (224.0.0.2).
            assert_eq!(leave_frame[16], 224);
            assert_eq!(leave_frame[17], 0);
            assert_eq!(leave_frame[18], 0);
            assert_eq!(leave_frame[19], 2);
            ensure_ttl_ihl_rtr(&ctx);
        });
    }

    #[test]
    fn test_igmp_integration_not_last_does_not_send_leave() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);
            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            let now = ctx.now();
            ctx.timer_ctx().assert_timers_installed([(
                REPORT_DELAY_TIMER_ID,
                now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL),
            )]);
            assert_eq!(ctx.frames().len(), 1);
            receive_igmp_report(&mut ctx);
            ctx.timer_ctx().assert_no_timers_installed();
            // The report should be discarded because we have received from
            // someone else.
            assert_eq!(ctx.frames().len(), 1);
            assert_eq!(ctx.gmp_leave_group(DummyDeviceId, GROUP_ADDR), GroupLeaveResult::Left(()));
            // A leave message is not sent.
            assert_eq!(ctx.frames().len(), 1);
            ensure_ttl_ihl_rtr(&ctx);
        });
    }

    #[test]
    fn test_receive_general_query() {
        run_with_many_seeds(|seed| {
            let mut ctx = setup_simple_test_environment(seed);
            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            assert_eq!(
                ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR_2),
                GroupJoinResult::Joined(())
            );
            let now = ctx.now();
            let range = now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL);
            ctx.timer_ctx().assert_timers_installed([
                (REPORT_DELAY_TIMER_ID, range.clone()),
                (REPORT_DELAY_TIMER_ID_2, range),
            ]);
            // The initial unsolicited report.
            assert_eq!(ctx.frames().len(), 2);
            ctx.trigger_timers_and_expect_unordered(
                [REPORT_DELAY_TIMER_ID, REPORT_DELAY_TIMER_ID_2],
                TimerHandler::handle_timer,
            );
            assert_eq!(ctx.frames().len(), 4);
            const RESP_TIME: Duration = Duration::from_secs(10);
            receive_igmp_general_query(&mut ctx, RESP_TIME);
            // Two new timers should be there.
            let now = ctx.now();
            let range = now..=(now + RESP_TIME);
            ctx.timer_ctx().assert_timers_installed([
                (REPORT_DELAY_TIMER_ID, range.clone()),
                (REPORT_DELAY_TIMER_ID_2, range),
            ]);
            ctx.trigger_timers_and_expect_unordered(
                [REPORT_DELAY_TIMER_ID, REPORT_DELAY_TIMER_ID_2],
                TimerHandler::handle_timer,
            );
            // Two new reports should be sent.
            assert_eq!(ctx.frames().len(), 6);
            ensure_ttl_ihl_rtr(&ctx);
        });
    }

    #[test]
    fn test_skip_igmp() {
        run_with_many_seeds(|seed| {
            // Test that we properly skip executing any `Actions` when IGMP is
            // disabled for the device.

            let mut ctx = DummyCtx::default();
            ctx.seed_rng(seed);
            ctx.get_mut().igmp_enabled = false;

            // Assert that no observable effects have taken place.
            let assert_no_effect = |ctx: &DummyCtx| {
                ctx.timer_ctx().assert_no_timers_installed();
                assert_empty(ctx.frames());
            };

            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            // We should have joined the group but not executed any `Actions`.
            assert_gmp_state!(ctx, &GROUP_ADDR, Delaying);
            assert_no_effect(&ctx);

            receive_igmp_report(&mut ctx);
            // We should have executed the transition but not executed any
            // `Actions`.
            assert_gmp_state!(ctx, &GROUP_ADDR, Idle);
            assert_no_effect(&ctx);

            receive_igmp_query(&mut ctx, Duration::from_secs(10));
            // We should have executed the transition but not executed any
            // `Actions`.
            assert_gmp_state!(ctx, &GROUP_ADDR, Delaying);
            assert_no_effect(&ctx);

            assert_eq!(ctx.gmp_leave_group(DummyDeviceId, GROUP_ADDR), GroupLeaveResult::Left(()));
            // We should have left the group but not executed any `Actions`.
            assert!(ctx.get_ref().groups.get(&GROUP_ADDR).is_none());
            assert_no_effect(&ctx);
        });
    }

    #[test]
    fn test_igmp_integration_with_local_join_leave() {
        run_with_many_seeds(|seed| {
            // Simple IGMP integration test to check that when we call top-level
            // multicast join and leave functions, IGMP is performed.

            let mut ctx = setup_simple_test_environment(seed);

            assert_eq!(ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR), GroupJoinResult::Joined(()));
            assert_gmp_state!(ctx, &GROUP_ADDR, Delaying);
            assert_eq!(ctx.frames().len(), 1);
            let now = ctx.now();
            let range = now..=(now + DEFAULT_UNSOLICITED_REPORT_INTERVAL);
            ctx.timer_ctx().assert_timers_installed([(REPORT_DELAY_TIMER_ID, range.clone())]);
            ensure_ttl_ihl_rtr(&ctx);

            assert_eq!(
                ctx.gmp_join_group(DummyDeviceId, GROUP_ADDR),
                GroupJoinResult::AlreadyMember
            );
            assert_gmp_state!(ctx, &GROUP_ADDR, Delaying);
            assert_eq!(ctx.frames().len(), 1);
            ctx.timer_ctx().assert_timers_installed([(REPORT_DELAY_TIMER_ID, range.clone())]);

            assert_eq!(
                ctx.gmp_leave_group(DummyDeviceId, GROUP_ADDR),
                GroupLeaveResult::StillMember
            );
            assert_gmp_state!(ctx, &GROUP_ADDR, Delaying);
            assert_eq!(ctx.frames().len(), 1);
            ctx.timer_ctx().assert_timers_installed([(REPORT_DELAY_TIMER_ID, range)]);

            assert_eq!(ctx.gmp_leave_group(DummyDeviceId, GROUP_ADDR), GroupLeaveResult::Left(()));
            assert_eq!(ctx.frames().len(), 2);
            ctx.timer_ctx().assert_no_timers_installed();
            ensure_ttl_ihl_rtr(&ctx);
        });
    }
}
