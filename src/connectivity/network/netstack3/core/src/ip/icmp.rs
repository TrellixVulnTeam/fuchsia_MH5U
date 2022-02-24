// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The Internet Control Message Protocol (ICMP).

use alloc::vec::Vec;
use core::{convert::TryInto as _, fmt::Debug};

use log::{debug, error, trace};
use net_types::{
    ip::{Ip, IpAddress, IpVersionMarker, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr, Ipv6SourceAddr},
    MulticastAddress, SpecifiedAddr, UnicastAddr, Witness,
};
use packet::{BufferMut, ParseBuffer, Serializer, TruncateDirection, TruncatingSerializer};
use packet_formats::{
    icmp::{
        peek_message_type, IcmpDestUnreachable, IcmpEchoRequest, IcmpMessage, IcmpMessageType,
        IcmpPacket, IcmpPacketBuilder, IcmpPacketRaw, IcmpParseArgs, IcmpTimeExceeded,
        IcmpUnusedCode, Icmpv4DestUnreachableCode, Icmpv4Packet, Icmpv4ParameterProblem,
        Icmpv4ParameterProblemCode, Icmpv4RedirectCode, Icmpv4TimeExceededCode,
        Icmpv6DestUnreachableCode, Icmpv6Packet, Icmpv6PacketTooBig, Icmpv6ParameterProblem,
        Icmpv6ParameterProblemCode, Icmpv6TimeExceededCode, MessageBody, OriginalPacket,
    },
    ip::{Ipv4Proto, Ipv6Proto},
    ipv4::{Ipv4FragmentType, Ipv4Header},
    ipv6::{ExtHdrParseError, Ipv6Header},
};
use thiserror::Error;
use zerocopy::ByteSlice;

use crate::{
    context::{CounterContext, InstantContext, StateContext},
    data_structures::{token_bucket::TokenBucket, IdMapCollectionKey},
    device::ndp::NdpPacketHandler,
    device::FrameDestination,
    ip::{
        forwarding::ForwardingTable,
        gmp::mld::MldPacketHandler,
        path_mtu::PmtuHandler,
        socket::{
            BufferIpSocketHandler, IpSock, IpSockCreationError, IpSockSendError, IpSockUpdate,
            IpSocket, IpSocketHandler, UnroutableBehavior,
        },
        BufferIpTransportContext, IpDeviceIdContext, IpExt, IpTransportContext,
        TransportReceiveError,
    },
    socket::{ConnSocketEntry, ConnSocketMap, Socket},
    BufferDispatcher, Ctx, EventDispatcher,
};

/// The default number of ICMP error messages to send per second.
///
/// Beyond this rate, error messages will be silently dropped.
pub const DEFAULT_ERRORS_PER_SECOND: u64 = 2 << 16;

/// An ICMPv4 error type and code.
///
/// Each enum variant corresponds to a particular error type, and contains the
/// possible codes for that error type.
#[derive(Copy, Clone, Debug, PartialEq)]
#[allow(missing_docs)]
pub enum Icmpv4ErrorCode {
    DestUnreachable(Icmpv4DestUnreachableCode),
    Redirect(Icmpv4RedirectCode),
    TimeExceeded(Icmpv4TimeExceededCode),
    ParameterProblem(Icmpv4ParameterProblemCode),
}

/// An ICMPv6 error type and code.
///
/// Each enum variant corresponds to a particular error type, and contains the
/// possible codes for that error type.
#[derive(Copy, Clone, Debug, PartialEq)]
#[allow(missing_docs)]
pub enum Icmpv6ErrorCode {
    DestUnreachable(Icmpv6DestUnreachableCode),
    PacketTooBig,
    TimeExceeded(Icmpv6TimeExceededCode),
    ParameterProblem(Icmpv6ParameterProblemCode),
}

/// Information used by ICMPv4 to decide whether an ICMP error message should
/// be sent.
#[derive(Copy, Clone)]
pub struct ShouldSendIcmpv4ErrorInfo {
    /// Whether the IPv4 packet received was the initial fragment or not.
    pub fragment_type: Ipv4FragmentType,
}

/// Information used by ICMPv6 to decide whether an ICMP error message should
/// be sent.
#[derive(Copy, Clone)]
pub struct ShouldSendIcmpv6ErrorInfo {
    /// Whether ICMPv6 should send an error message even if the destination
    /// address is a multicast address.
    pub allow_dst_multicast: bool,
}

pub(crate) struct IcmpState<A: IpAddress, Instant, S> {
    conns: ConnSocketMap<IcmpAddr<A>, IcmpConn<S>>,
    error_send_bucket: TokenBucket<Instant>,
}

/// A builder for ICMPv4 state.
#[derive(Copy, Clone)]
pub struct Icmpv4StateBuilder {
    send_timestamp_reply: bool,
    errors_per_second: u64,
}

impl Default for Icmpv4StateBuilder {
    fn default() -> Icmpv4StateBuilder {
        Icmpv4StateBuilder {
            send_timestamp_reply: false,
            errors_per_second: DEFAULT_ERRORS_PER_SECOND,
        }
    }
}

impl Icmpv4StateBuilder {
    /// Enable or disable replying to ICMPv4 Timestamp Request messages with
    /// Timestamp Reply messages (default: disabled).
    ///
    /// Enabling this can introduce a very minor vulnerability in which an
    /// attacker can learn the system clock's time, which in turn can aid in
    /// attacks against time-based authentication systems.
    pub fn send_timestamp_reply(&mut self, send_timestamp_reply: bool) -> &mut Self {
        self.send_timestamp_reply = send_timestamp_reply;
        self
    }

    /// Configure the number of ICMPv4 error messages to send per second
    /// (default: [`DEFAULT_ERRORS_PER_SECOND`]).
    ///
    /// The number of ICMPv4 error messages sent by the stack will be rate
    /// limited to the given number of errors per second. Any messages that
    /// exceed this rate will be silently dropped.
    pub fn errors_per_second(&mut self, errors_per_second: u64) -> &mut Self {
        self.errors_per_second = errors_per_second;
        self
    }

    pub(crate) fn build<Instant, S>(self) -> Icmpv4State<Instant, S> {
        Icmpv4State {
            inner: IcmpState {
                conns: ConnSocketMap::default(),
                error_send_bucket: TokenBucket::new(self.errors_per_second),
            },
            send_timestamp_reply: self.send_timestamp_reply,
        }
    }
}

/// The state associated with the ICMPv4 protocol.
pub(crate) struct Icmpv4State<Instant, S> {
    inner: IcmpState<Ipv4Addr, Instant, S>,
    send_timestamp_reply: bool,
}

// Used by `receive_icmp_echo_reply`.
impl<Instant, S> AsRef<IcmpState<Ipv4Addr, Instant, S>> for Icmpv4State<Instant, S> {
    fn as_ref(&self) -> &IcmpState<Ipv4Addr, Instant, S> {
        &self.inner
    }
}

// Used by `send_icmpv4_echo_request_inner`.
impl<Instant, S> AsMut<IcmpState<Ipv4Addr, Instant, S>> for Icmpv4State<Instant, S> {
    fn as_mut(&mut self) -> &mut IcmpState<Ipv4Addr, Instant, S> {
        &mut self.inner
    }
}

/// A builder for ICMPv6 state.
#[derive(Copy, Clone)]
pub struct Icmpv6StateBuilder {
    errors_per_second: u64,
}

impl Default for Icmpv6StateBuilder {
    fn default() -> Icmpv6StateBuilder {
        Icmpv6StateBuilder { errors_per_second: DEFAULT_ERRORS_PER_SECOND }
    }
}

impl Icmpv6StateBuilder {
    /// Configure the number of ICMPv6 error messages to send per second
    /// (default: [`DEFAULT_ERRORS_PER_SECOND`]).
    ///
    /// The number of ICMPv6 error messages sent by the stack will be rate
    /// limited to the given number of errors per second. Any messages that
    /// exceed this rate will be silently dropped.
    ///
    /// This rate limit is required by [RFC 4443 Section 2.4] (f).
    ///
    /// [RFC 4443 Section 2.4]: https://tools.ietf.org/html/rfc4443#section-2.4
    pub fn errors_per_second(&mut self, errors_per_second: u64) -> &mut Self {
        self.errors_per_second = errors_per_second;
        self
    }

    pub(crate) fn build<Instant, S>(self) -> Icmpv6State<Instant, S> {
        Icmpv6State {
            inner: IcmpState {
                conns: ConnSocketMap::default(),
                error_send_bucket: TokenBucket::new(self.errors_per_second),
            },
        }
    }
}

/// The state associated with the ICMPv6 protocol.
pub(crate) struct Icmpv6State<Instant, S> {
    inner: IcmpState<Ipv6Addr, Instant, S>,
}

// Used by `receive_icmp_echo_reply`.
impl<Instant, S> AsRef<IcmpState<Ipv6Addr, Instant, S>> for Icmpv6State<Instant, S> {
    fn as_ref(&self) -> &IcmpState<Ipv6Addr, Instant, S> {
        &self.inner
    }
}

// Used by `send_icmpv6_echo_request_inner`.
impl<Instant, S> AsMut<IcmpState<Ipv6Addr, Instant, S>> for Icmpv6State<Instant, S> {
    fn as_mut(&mut self) -> &mut IcmpState<Ipv6Addr, Instant, S> {
        &mut self.inner
    }
}

#[derive(Copy, Clone, Eq, PartialEq, Hash)]
pub(crate) struct IcmpAddr<A: IpAddress> {
    local_addr: SpecifiedAddr<A>,
    remote_addr: SpecifiedAddr<A>,
    icmp_id: u16,
}

#[derive(Clone)]
pub(crate) struct IcmpConn<S> {
    icmp_id: u16,
    ip: S,
}

impl<'a, A: IpAddress, S: IpSocket<A::Version>> From<&'a IcmpConn<S>> for IcmpAddr<A> {
    fn from(conn: &'a IcmpConn<S>) -> IcmpAddr<A> {
        IcmpAddr {
            local_addr: *conn.ip.local_ip(),
            remote_addr: *conn.ip.remote_ip(),
            icmp_id: conn.icmp_id,
        }
    }
}

/// The ID identifying an ICMP connection.
///
/// When a new ICMP connection is added, it is given a unique `IcmpConnId`.
/// These are opaque `usize`s which are intentionally allocated as densely as
/// possible around 0, making it possible to store any associated data in a
/// `Vec` indexed by the ID. `IcmpConnId` implements `Into<usize>`.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct IcmpConnId<I: Ip>(usize, IpVersionMarker<I>);

impl<I: Ip> IcmpConnId<I> {
    fn new(id: usize) -> IcmpConnId<I> {
        IcmpConnId(id, IpVersionMarker::default())
    }
}

impl<I: Ip> Into<usize> for IcmpConnId<I> {
    fn into(self) -> usize {
        let Self(id, _marker) = self;
        id
    }
}

impl<I: Ip> IdMapCollectionKey for IcmpConnId<I> {
    const VARIANT_COUNT: usize = 1;

    fn get_variant(&self) -> usize {
        0
    }

    fn get_id(&self) -> usize {
        let Self(id, _marker) = *self;
        id
    }
}

/// Apply an update to all IPv4 sockets that this module is responsible for
/// - namely, those contained in ICMPv4 sockets.
pub(super) fn update_all_ipv4_sockets<C: InnerIcmpv4Context>(
    ctx: &mut C,
    update: IpSockUpdate<Ipv4>,
) {
    let (state, meta) = ctx.get_state_and_update_meta();
    // We have to collect into a `Vec` here because the iterator borrows `ctx`,
    // which we need mutable access to in order to report the closures.
    let closed_conns = state
        .conns
        .update_retain(|conn, _addr| conn.ip.apply_update(&update, meta))
        .collect::<Vec<_>>();
    for (id, _entry, err) in closed_conns {
        ctx.close_icmp_connection(IcmpConnId::new(id), err);
    }
}

/// Apply an update to all IPv6 sockets that this module is responsible for
/// - namely, those contained in ICMPv6 sockets.
pub(super) fn update_all_ipv6_sockets<C: InnerIcmpv6Context>(
    ctx: &mut C,
    update: IpSockUpdate<Ipv6>,
) {
    let (state, meta) = ctx.get_state_and_update_meta();
    // We have to collect into a `Vec` here because the iterator borrows `ctx`,
    // which we need mutable access to in order to report the closures.
    let closed_conns = state
        .conns
        .update_retain(|conn, _addr| conn.ip.apply_update(&update, meta))
        .collect::<Vec<_>>();
    for (id, _entry, err) in closed_conns {
        ctx.close_icmp_connection(IcmpConnId::new(id), err);
    }
}

/// An extension trait adding extra ICMP-related functionality to IP versions.
pub trait IcmpIpExt: packet_formats::ip::IpExt + packet_formats::icmp::IcmpIpExt {
    /// The type of error code for this version of ICMP - [`Icmpv4ErrorCode`] or
    /// [`Icmpv6ErrorCode`].
    type ErrorCode: Debug;

    /// Information used to decide whether ICMP should reply with an error
    /// message.
    type ShouldSendIcmpErrorInfo;
}

impl IcmpIpExt for Ipv4 {
    type ErrorCode = Icmpv4ErrorCode;
    type ShouldSendIcmpErrorInfo = ShouldSendIcmpv4ErrorInfo;
}

impl IcmpIpExt for Ipv6 {
    type ErrorCode = Icmpv6ErrorCode;
    type ShouldSendIcmpErrorInfo = ShouldSendIcmpv6ErrorInfo;
}

/// An extension trait providing ICMP handler properties.
pub(super) trait IcmpHandlerIpExt: Ip {
    type SourceAddress: Witness<Self::Addr>;
    type IcmpError;
}

impl IcmpHandlerIpExt for Ipv4 {
    type SourceAddress = SpecifiedAddr<Ipv4Addr>;
    type IcmpError = Icmpv4Error;
}

impl IcmpHandlerIpExt for Ipv6 {
    type SourceAddress = UnicastAddr<Ipv6Addr>;
    type IcmpError = Icmpv6ErrorKind;
}

/// A kind of ICMPv4 error.
pub(super) enum Icmpv4ErrorKind {
    ParameterProblem {
        code: Icmpv4ParameterProblemCode,
        pointer: u8,
        fragment_type: Ipv4FragmentType,
    },
    TtlExpired {
        proto: Ipv4Proto,
        fragment_type: Ipv4FragmentType,
    },
    NetUnreachable {
        proto: Ipv4Proto,
        fragment_type: Ipv4FragmentType,
    },
    ProtocolUnreachable,
    PortUnreachable,
}

/// An ICMPv4 error.
pub(super) struct Icmpv4Error {
    pub(super) kind: Icmpv4ErrorKind,
    pub(super) header_len: usize,
}

/// A kind of ICMPv6 error.
pub(super) enum Icmpv6ErrorKind {
    ParameterProblem { code: Icmpv6ParameterProblemCode, pointer: u32, allow_dst_multicast: bool },
    TtlExpired { proto: Ipv6Proto, header_len: usize },
    NetUnreachable { proto: Ipv6Proto, header_len: usize },
    PacketTooBig { proto: Ipv6Proto, header_len: usize, mtu: u32 },
    ProtocolUnreachable { header_len: usize },
    PortUnreachable,
}

/// The handler exposed by ICMP.
pub(super) trait BufferIcmpHandler<I: IcmpHandlerIpExt, B: BufferMut>:
    IpDeviceIdContext<I>
{
    /// Sends an error message in response to an incoming packet.
    ///
    /// `src_ip` and `dst_ip` are the source and destination addresses of the
    /// incoming packet.
    fn send_icmp_error_message(
        &mut self,
        device: Self::DeviceId,
        frame_dst: FrameDestination,
        src_ip: I::SourceAddress,
        dst_ip: SpecifiedAddr<I::Addr>,
        original_packet: B,
        error: I::IcmpError,
    );
}

impl<B: BufferMut, C: InnerBufferIcmpv4Context<B>> BufferIcmpHandler<Ipv4, B> for C {
    fn send_icmp_error_message(
        &mut self,
        device: C::DeviceId,
        frame_dst: FrameDestination,
        src_ip: SpecifiedAddr<Ipv4Addr>,
        dst_ip: SpecifiedAddr<Ipv4Addr>,
        original_packet: B,
        Icmpv4Error { kind, header_len }: Icmpv4Error,
    ) {
        match kind {
            Icmpv4ErrorKind::ParameterProblem { code, pointer, fragment_type } => {
                send_icmpv4_parameter_problem(
                    self,
                    device,
                    frame_dst,
                    src_ip,
                    dst_ip,
                    code,
                    Icmpv4ParameterProblem::new(pointer),
                    original_packet,
                    header_len,
                    fragment_type,
                )
            }
            Icmpv4ErrorKind::TtlExpired { proto, fragment_type } => send_icmpv4_ttl_expired(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                proto,
                original_packet,
                header_len,
                fragment_type,
            ),
            Icmpv4ErrorKind::NetUnreachable { proto, fragment_type } => {
                send_icmpv4_net_unreachable(
                    self,
                    device,
                    frame_dst,
                    src_ip,
                    dst_ip,
                    proto,
                    original_packet,
                    header_len,
                    fragment_type,
                )
            }
            Icmpv4ErrorKind::ProtocolUnreachable => send_icmpv4_protocol_unreachable(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                original_packet,
                header_len,
            ),
            Icmpv4ErrorKind::PortUnreachable => send_icmpv4_port_unreachable(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                original_packet,
                header_len,
            ),
        }
    }
}

impl<B: BufferMut, C: InnerBufferIcmpv6Context<B>> BufferIcmpHandler<Ipv6, B> for C {
    fn send_icmp_error_message(
        &mut self,
        device: C::DeviceId,
        frame_dst: FrameDestination,
        src_ip: UnicastAddr<Ipv6Addr>,
        dst_ip: SpecifiedAddr<Ipv6Addr>,
        original_packet: B,
        error: Icmpv6ErrorKind,
    ) {
        match error {
            Icmpv6ErrorKind::ParameterProblem { code, pointer, allow_dst_multicast } => {
                send_icmpv6_parameter_problem(
                    self,
                    device,
                    frame_dst,
                    src_ip,
                    dst_ip,
                    code,
                    Icmpv6ParameterProblem::new(pointer),
                    original_packet,
                    allow_dst_multicast,
                )
            }
            Icmpv6ErrorKind::TtlExpired { proto, header_len } => send_icmpv6_ttl_expired(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                proto,
                original_packet,
                header_len,
            ),
            Icmpv6ErrorKind::NetUnreachable { proto, header_len } => send_icmpv6_net_unreachable(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                proto,
                original_packet,
                header_len,
            ),
            Icmpv6ErrorKind::PacketTooBig { proto, header_len, mtu } => send_icmpv6_packet_too_big(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                proto,
                mtu,
                original_packet,
                header_len,
            ),
            Icmpv6ErrorKind::ProtocolUnreachable { header_len } => {
                send_icmpv6_protocol_unreachable(
                    self,
                    device,
                    frame_dst,
                    src_ip,
                    dst_ip,
                    original_packet,
                    header_len,
                )
            }
            Icmpv6ErrorKind::PortUnreachable => send_icmpv6_port_unreachable(
                self,
                device,
                frame_dst,
                src_ip,
                dst_ip,
                original_packet,
            ),
        }
    }
}

/// The context required by the ICMP layer in order to deliver events related to
/// ICMP sockets.
pub trait IcmpContext<I: IcmpIpExt> {
    /// Receives an ICMP error message related to a previously-sent ICMP echo
    /// request.
    ///
    /// `seq_num` is the sequence number of the original echo request that
    /// triggered the error, and `err` is the specific error identified by the
    /// incoming ICMP error message.
    fn receive_icmp_error(&mut self, conn: IcmpConnId<I>, seq_num: u16, err: I::ErrorCode);

    /// Closes an ICMP connection because it is no longer routable.
    ///
    /// `close_icmp_connection` is called when a change to routing state has
    /// made an ICMP socket no longer routable. After the call has returned, the
    /// core will completely remove the socket, and any future calls referencing
    /// it will either panic (because of an unrecognized [`IcmpConnId`]) or
    /// incorrectly refer to a different, newly-opened ICMP connection.
    fn close_icmp_connection(&mut self, conn: IcmpConnId<I>, err: IpSockCreationError);
}

/// The context required by the ICMP layer in order to deliver packets on ICMP
/// sockets.
pub trait BufferIcmpContext<I: IcmpIpExt, B: BufferMut>: IcmpContext<I> {
    /// Receives an ICMP echo reply.
    fn receive_icmp_echo_reply(
        &mut self,
        conn: IcmpConnId<I>,
        src_ip: I::Addr,
        dst_ip: I::Addr,
        id: u16,
        seq_num: u16,
        data: B,
    );
}

impl<I: IcmpIpExt, D: EventDispatcher + IcmpContext<I>> IcmpContext<I> for Ctx<D> {
    fn receive_icmp_error(&mut self, conn: IcmpConnId<I>, seq_num: u16, err: I::ErrorCode) {
        IcmpContext::receive_icmp_error(&mut self.dispatcher, conn, seq_num, err);
    }

    fn close_icmp_connection(&mut self, conn: IcmpConnId<I>, err: IpSockCreationError) {
        self.dispatcher.close_icmp_connection(conn, err);
    }
}

impl<I: IcmpIpExt, B: BufferMut, D: EventDispatcher + BufferIcmpContext<I, B>>
    BufferIcmpContext<I, B> for Ctx<D>
{
    fn receive_icmp_echo_reply(
        &mut self,
        conn: IcmpConnId<I>,
        src_ip: I::Addr,
        dst_ip: I::Addr,
        id: u16,
        seq_num: u16,
        data: B,
    ) {
        self.dispatcher.receive_icmp_echo_reply(conn, src_ip, dst_ip, id, seq_num, data);
    }
}

/// The execution context shared by ICMP(v4) and ICMPv6 for the internal
/// operations of the IP stack.
///
/// Unlike [`IcmpContext`], `InnerIcmpContext` is not exposed outside of this
/// crate.
pub(crate) trait InnerIcmpContext<I: IcmpIpExt + IpExt>:
    IcmpContext<I>
    + IpSocketHandler<I>
    + IpDeviceIdContext<I>
    + CounterContext
    + InstantContext
    + StateContext<
        IcmpState<
            I::Addr,
            <Self as InstantContext>::Instant,
            IpSock<I, <Self as IpDeviceIdContext<I>>::DeviceId>,
        >,
    >
{
    // TODO(joshlf): If we end up needing to respond to these messages with new
    // outbound packets, then perhaps it'd be worth passing the original buffer
    // so that it can be reused?
    //
    // NOTE(joshlf): We don't guarantee the packet body length here for two
    // reasons:
    // - It's possible that some IPv4 protocol does or will exist for which
    //   valid packets are less than 8 bytes in length. If we were to reject all
    //   packets with bodies of less than 8 bytes, we might silently discard
    //   legitimate error messages for such protocols.
    // - Even if we were to guarantee this, there's no good way to encode such a
    //   guarantee in the type system, and so the caller would have no recourse
    //   but to panic, and panics have a habit of becoming bugs or DoS
    //   vulnerabilities when invariants change.

    /// Receives an ICMP error message and demultiplexes it to a transport layer
    /// protocol.
    ///
    /// All arguments beginning with `original_` are fields from the IP packet
    /// that triggered the error. The `original_body` is provided here so that
    /// the error can be associated with a transport-layer socket.
    ///
    /// While ICMPv4 error messages are supposed to contain the first 8 bytes of
    /// the body of the offending packet, and ICMPv6 error messages are supposed
    /// to contain as much of the offending packet as possible without violating
    /// the IPv6 minimum MTU, the caller does NOT guarantee that either of these
    /// hold. It is `receive_icmp_error`'s responsibility to handle any length
    /// of `original_body`, and to perform any necessary validation.
    fn receive_icmp_error(
        &mut self,
        original_src_ip: Option<SpecifiedAddr<I::Addr>>,
        original_dst_ip: SpecifiedAddr<I::Addr>,
        original_proto: I::Proto,
        original_body: &[u8],
        err: I::ErrorCode,
    );

    /// Gets the [`IcmpState`] and the metadata needed to apply an IP socket
    /// update at the same time.
    fn get_state_and_update_meta(
        &mut self,
    ) -> (
        &mut IcmpState<I::Addr, Self::Instant, IpSock<I, Self::DeviceId>>,
        &ForwardingTable<I, Self::DeviceId>,
    );
}

/// The execution context shared by ICMP(v4) and ICMPv6 for the internal
/// operations of the IP stack when a buffer is required.
pub(crate) trait InnerBufferIcmpContext<I: IcmpIpExt + IpExt, B: BufferMut>:
    InnerIcmpContext<I> + BufferIcmpContext<I, B> + BufferIpSocketHandler<I, B>
{
    /// Sends an ICMP error message to a remote host.
    ///
    /// `send_icmp_error_message` sends an ICMP error message. It takes the
    /// ingress device, source IP, and destination IP of the packet that
    /// generated the error. It uses ICMP-specific logic to figure out whether
    /// and how to send an ICMP error message. `ip_mtu` is an optional MTU size
    /// for the final IP packet generated by this ICMP response. `src_ip` must
    /// not be the unspecified address, as we are replying to this packet, and
    /// the unspecified address is not routable (RFCs 792 and 4443 also disallow
    /// sending error messages in response to packets with an unspecified source
    /// address, probably for exactly this reason).
    ///
    /// `send_icmp_error_message` is responsible for calling
    /// [`should_send_icmpv4_error`] or [`should_send_icmpv6_error`], and `info`
    /// contains information that is used by these functions. If they return
    /// `false`, then it must not send the message regardless of whatever other
    /// logic is used.
    ///
    /// `get_body_from_src_ip` returns a `Serializer` with the bytes of the ICMP
    /// packet, and, when called, is given the source IP address chosen for the
    /// outbound packet. This allows `get_body_from_src_ip` to properly compute
    /// the ICMP checksum, which relies on both the source and destination IP
    /// addresses of the IP packet it's encapsulated in.
    fn send_icmp_error_message<S: Serializer<Buffer = B>, F: FnOnce(SpecifiedAddr<I::Addr>) -> S>(
        &mut self,
        device: Self::DeviceId,
        frame_dst: FrameDestination,
        original_src_ip: SpecifiedAddr<I::Addr>,
        original_dst_ip: SpecifiedAddr<I::Addr>,
        get_body_from_src_ip: F,
        ip_mtu: Option<u32>,
        info: I::ShouldSendIcmpErrorInfo,
    ) -> Result<(), S>;
}

/// The execution context for ICMPv4.
///
/// `InnerIcmpv4Context` is a shorthand for a larger collection of traits.
pub(crate) trait InnerIcmpv4Context:
    InnerIcmpContext<Ipv4>
    + StateContext<
        Icmpv4State<
            <Self as InstantContext>::Instant,
            IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
        >,
    >
{
}

impl<
        C: InnerIcmpContext<Ipv4>
            + StateContext<
                Icmpv4State<
                    <Self as InstantContext>::Instant,
                    IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
                >,
            >,
    > InnerIcmpv4Context for C
{
}

/// The execution context for ICMPv4 where a buffer is required.
///
/// `InnerBufferIcmpv4Context` is a shorthand for a larger collection of traits.
pub(crate) trait InnerBufferIcmpv4Context<B: BufferMut>:
    InnerIcmpv4Context + InnerBufferIcmpContext<Ipv4, B>
{
}

impl<B: BufferMut, C: InnerIcmpv4Context + InnerBufferIcmpContext<Ipv4, B>>
    InnerBufferIcmpv4Context<B> for C
{
}

impl<C>
    StateContext<
        IcmpState<
            Ipv4Addr,
            <Self as InstantContext>::Instant,
            IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
        >,
    > for C
where
    C: InstantContext
        + IpSocketHandler<Ipv4>
        + StateContext<
            Icmpv4State<
                <Self as InstantContext>::Instant,
                IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
            >,
        >,
{
    fn get_state_with(
        &self,
        _id: (),
    ) -> &IcmpState<
        Ipv4Addr,
        <Self as InstantContext>::Instant,
        IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
    > {
        &self.get_state().inner
    }

    fn get_state_mut_with(
        &mut self,
        _id: (),
    ) -> &mut IcmpState<
        Ipv4Addr,
        <Self as InstantContext>::Instant,
        IpSock<Ipv4, <Self as IpDeviceIdContext<Ipv4>>::DeviceId>,
    > {
        &mut self.get_state_mut().inner
    }
}

/// The execution context for ICMPv6.
///
/// `InnerIcmpv6Context` is a shorthand for a larger collection of traits.
pub(crate) trait InnerIcmpv6Context:
    InnerIcmpContext<Ipv6>
    + StateContext<
        Icmpv6State<
            <Self as InstantContext>::Instant,
            IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
        >,
    >
{
}

impl<C> InnerIcmpv6Context for C where
    C: InnerIcmpContext<Ipv6>
        + StateContext<
            Icmpv6State<
                <Self as InstantContext>::Instant,
                IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
            >,
        >
{
}

/// The execution context for ICMPv6 where a buffer is required.
///
/// `InnerBufferIcmpv6Context` is a shorthand for a larger collection of traits.
pub(crate) trait InnerBufferIcmpv6Context<B: BufferMut>:
    InnerIcmpv6Context + InnerBufferIcmpContext<Ipv6, B>
{
}

impl<B: BufferMut, C: InnerIcmpv6Context + InnerBufferIcmpContext<Ipv6, B>>
    InnerBufferIcmpv6Context<B> for C
{
}

impl<C>
    StateContext<
        IcmpState<
            Ipv6Addr,
            <Self as InstantContext>::Instant,
            IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
        >,
    > for C
where
    C: InstantContext
        + IpSocketHandler<Ipv6>
        + StateContext<
            Icmpv6State<
                <Self as InstantContext>::Instant,
                IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
            >,
        >,
{
    fn get_state_with(
        &self,
        _id: (),
    ) -> &IcmpState<
        Ipv6Addr,
        <Self as InstantContext>::Instant,
        IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
    > {
        &self.get_state().inner
    }

    fn get_state_mut_with(
        &mut self,
        _id: (),
    ) -> &mut IcmpState<
        Ipv6Addr,
        <Self as InstantContext>::Instant,
        IpSock<Ipv6, <Self as IpDeviceIdContext<Ipv6>>::DeviceId>,
    > {
        &mut self.get_state_mut().inner
    }
}

/// Attempt to send an ICMP or ICMPv6 error message, applying a rate limit.
///
/// `try_send_error!($ctx, $e)` attempts to consume a token from the token
/// bucket at `$ctx.get_state_mut().error_send_bucket`. If it succeeds, it
/// invokes the expression `$e`, and otherwise does nothing. It assumes that the
/// type of `$e` is `Result<(), _>` and, in the case that the rate limit is
/// exceeded and it does not invoke `$e`, returns `Ok(())`.
///
/// [RFC 4443 Section 2.4] (f) requires that we MUST limit the rate of outbound
/// ICMPv6 error messages. To our knowledge, there is no similar requirement for
/// ICMPv4, but the same rationale applies, so we do it for ICMPv4 as well.
///
/// [RFC 4443 Section 2.4]: https://tools.ietf.org/html/rfc4443#section-2.4
macro_rules! try_send_error {
    ($ctx:expr, $e:expr) => {{
        // TODO(joshlf): Figure out a way to avoid querying for the current time
        // unconditionally. See the documentation on the `CachedInstantCtx` type
        // for more information.
        let instant_ctx = crate::context::new_cached_instant_context($ctx);
        let state: &mut IcmpState<_, _, _> = $ctx.get_state_mut();
        if state.error_send_bucket.try_take(&instant_ctx) {
            $e
        } else {
            trace!("ip::icmp::try_send_error!: dropping rate-limited ICMP error message");
            Ok(())
        }
    }};
}

/// An implementation of [`IpTransportContext`] for ICMP.
pub(crate) enum IcmpIpTransportContext {}

impl<I: IcmpIpExt + IpExt, C: InnerIcmpContext<I>> IpTransportContext<I, C>
    for IcmpIpTransportContext
where
    IcmpEchoRequest: for<'a> IcmpMessage<I, &'a [u8]>,
{
    fn receive_icmp_error(
        ctx: &mut C,
        original_src_ip: Option<SpecifiedAddr<I::Addr>>,
        original_dst_ip: SpecifiedAddr<I::Addr>,
        mut original_body: &[u8],
        err: I::ErrorCode,
    ) {
        ctx.increment_counter("IcmpIpTransportContext::receive_icmp_error");
        trace!("IcmpIpTransportContext::receive_icmp_error({:?})", err);

        let echo_request = if let Ok(echo_request) =
            original_body.parse::<IcmpPacketRaw<I, _, IcmpEchoRequest>>()
        {
            echo_request
        } else {
            // NOTE: This might just mean that the error message was in response
            // to a packet that we sent that wasn't an echo request, so we just
            // silently ignore it.
            return;
        };

        let original_src_ip = match original_src_ip {
            Some(ip) => ip,
            None => {
                trace!("IcmpIpTransportContext::receive_icmp_error: Got ICMP error message for IP packet with an unspecified destination IP address");
                return;
            }
        };
        let id = echo_request.message().id();
        if let Some(conn) = ctx.get_state().conns.get_id_by_addr(&IcmpAddr {
            local_addr: original_src_ip,
            remote_addr: original_dst_ip,
            icmp_id: id,
        }) {
            let seq = echo_request.message().seq();
            IcmpContext::receive_icmp_error(ctx, IcmpConnId::new(conn), seq, err);
        } else {
            trace!("IcmpIpTransportContext::receive_icmp_error: Got ICMP error message for nonexistent ICMP echo socket; either the socket responsible has since been removed, or the error message was sent in error or corrupted");
        }
    }
}

impl<B: BufferMut, C: InnerBufferIcmpv4Context<B> + PmtuHandler<Ipv4>>
    BufferIpTransportContext<Ipv4, B, C> for IcmpIpTransportContext
{
    fn receive_ip_packet(
        ctx: &mut C,
        device: Option<C::DeviceId>,
        src_ip: Ipv4Addr,
        dst_ip: SpecifiedAddr<Ipv4Addr>,
        mut buffer: B,
    ) -> Result<(), (B, TransportReceiveError)> {
        trace!(
            "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet({}, {})",
            src_ip,
            dst_ip
        );
        let packet =
            match buffer.parse_with::<_, Icmpv4Packet<_>>(IcmpParseArgs::new(src_ip, dst_ip)) {
                Ok(packet) => packet,
                Err(_) => return Ok(()), // TODO(joshlf): Do something else here?
            };

        match packet {
            Icmpv4Packet::EchoRequest(echo_request) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::echo_request");

                if let Some(src_ip) = SpecifiedAddr::new(src_ip) {
                    let req = *echo_request.message();
                    let code = echo_request.code();
                    let (local_ip, remote_ip) = (dst_ip, src_ip);
                    // TODO(joshlf): Do something if send_icmp_reply returns an
                    // error?
                    let _ = send_icmp_reply(ctx, device, remote_ip, local_ip, |src_ip| {
                        buffer.encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                            src_ip,
                            remote_ip,
                            code,
                            req.reply(),
                        ))
                    });
                } else {
                    trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received echo request with an unspecified source address");
                }
            }
            Icmpv4Packet::EchoReply(echo_reply) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::echo_reply");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received an EchoReply message");
                let id = echo_reply.message().id();
                let seq = echo_reply.message().seq();
                receive_icmp_echo_reply(ctx, src_ip, dst_ip, id, seq, buffer);
            }
            Icmpv4Packet::TimestampRequest(timestamp_request) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::timestamp_request");
                if let Some(src_ip) = SpecifiedAddr::new(src_ip) {
                    if StateContext::<Icmpv4State<_, _>>::get_state(ctx).send_timestamp_reply {
                        trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Responding to Timestamp Request message");
                        // We're supposed to respond with the time that we
                        // processed this message as measured in milliseconds
                        // since midnight UT. However, that would require that
                        // we knew the local time zone and had a way to convert
                        // `InstantContext::Instant` to a `u32` value. We can't
                        // do that, and probably don't want to introduce all of
                        // the machinery necessary just to support this one use
                        // case. Luckily, RFC 792 page 17 provides us with an
                        // out:
                        //
                        //   If the time is not available in miliseconds [sic]
                        //   or cannot be provided with respect to midnight UT
                        //   then any time can be inserted in a timestamp
                        //   provided the high order bit of the timestamp is
                        //   also set to indicate this non-standard value.
                        //
                        // Thus, we provide a zero timestamp with the high order
                        // bit set.
                        const NOW: u32 = 0x80000000;
                        let reply = timestamp_request.message().reply(NOW, NOW);
                        let (local_ip, remote_ip) = (dst_ip, src_ip);
                        // We don't actually want to use any of the _contents_
                        // of the buffer, but we would like to reuse it as
                        // scratch space. Eventually, `IcmpPacketBuilder` will
                        // implement `InnerPacketBuilder` for messages without
                        // bodies, but until that happens, we need to give it an
                        // empty buffer.
                        buffer.shrink_front_to(0);
                        // TODO(joshlf): Do something if send_icmp_reply returns
                        // an error?
                        let _ = send_icmp_reply(ctx, device, remote_ip, local_ip, |src_ip| {
                            buffer.encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                                src_ip,
                                remote_ip,
                                IcmpUnusedCode,
                                reply,
                            ))
                        });
                    } else {
                        trace!(
                            "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Silently ignoring Timestamp Request message"
                        );
                    }
                } else {
                    trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received timestamp request with an unspecified source address");
                }
            }
            Icmpv4Packet::TimestampReply(_) => {
                // TODO(joshlf): Support sending Timestamp Requests and
                // receiving Timestamp Replies?
                debug!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received unsolicited Timestamp Reply message");
            }
            Icmpv4Packet::DestUnreachable(dest_unreachable) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::dest_unreachable");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received a Destination Unreachable message");

                if dest_unreachable.code() == Icmpv4DestUnreachableCode::FragmentationRequired {
                    if let Some(next_hop_mtu) = dest_unreachable.message().next_hop_mtu() {
                        // We are updating the path MTU from the destination
                        // address of this `packet` (which is an IP address on
                        // this node) to some remote (identified by the source
                        // address of this `packet`).
                        //
                        // `update_pmtu_if_less` may return an error, but it
                        // will only happen if the Dest Unreachable message's
                        // MTU field had a value that was less than the IPv4
                        // minimum MTU (which as per IPv4 RFC 791, must not
                        // happen).
                        ctx.update_pmtu_if_less(
                            dst_ip.get(),
                            src_ip,
                            u32::from(next_hop_mtu.get()),
                        );
                    } else {
                        // If the Next-Hop MTU from an incoming ICMP message is
                        // `0`, then we assume the source node of the ICMP
                        // message does not implement RFC 1191 and therefore
                        // does not actually use the Next-Hop MTU field and
                        // still considers it as an unused field.
                        //
                        // In this case, the only information we have is the
                        // size of the original IP packet that was too big (the
                        // original packet header should be included in the ICMP
                        // response). Here we will simply reduce our PMTU
                        // estimate to a value less than the total length of the
                        // original packet. See RFC 1191 Section 5.
                        //
                        // `update_pmtu_next_lower` may return an error, but it
                        // will only happen if no valid lower value exists from
                        // the original packet's length. It is safe to silently
                        // ignore the error when we have no valid lower PMTU
                        // value as the node from `src_ip` would not be IP RFC
                        // compliant and we expect this to be very rare (for
                        // IPv4, the lowest MTU value for a link can be 68
                        // bytes).
                        let original_packet_buf = dest_unreachable.body().bytes();
                        if original_packet_buf.len() >= 4 {
                            // We need the first 4 bytes as the total length
                            // field is at bytes 2/3 of the original packet
                            // buffer.
                            let total_len = u16::from_be_bytes(original_packet_buf[2..4].try_into().unwrap());

                            trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Next-Hop MTU is 0 so using the next best PMTU value from {}", total_len);

                            ctx.update_pmtu_next_lower(dst_ip.get(), src_ip, u32::from(total_len));
                        } else {
                            // Ok to silently ignore as RFC 792 requires nodes
                            // to send the original IP packet header + 64 bytes
                            // of the original IP packet's body so the node
                            // itself is already violating the RFC.
                            trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Original packet buf is too small to get original packet len so ignoring");
                        }
                    }
                }

                receive_icmpv4_error(
                    ctx,
                    &dest_unreachable,
                    Icmpv4ErrorCode::DestUnreachable(dest_unreachable.code()),
                );
            }
            Icmpv4Packet::TimeExceeded(time_exceeded) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::time_exceeded");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received a Time Exceeded message");

                receive_icmpv4_error(
                    ctx,
                    &time_exceeded,
                    Icmpv4ErrorCode::TimeExceeded(time_exceeded.code()),
                );
            }
            Icmpv4Packet::Redirect(_) => log_unimplemented!((), "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::redirect"),
            Icmpv4Packet::ParameterProblem(parameter_problem) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::parameter_problem");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet: Received a Parameter Problem message");

                receive_icmpv4_error(
                    ctx,
                    &parameter_problem,
                    Icmpv4ErrorCode::ParameterProblem(parameter_problem.code()),
                );
            }
        }

        Ok(())
    }
}

impl<
        B: BufferMut,
        C: InnerIcmpv6Context
            + InnerBufferIcmpContext<Ipv6, B>
            + PmtuHandler<Ipv6>
            + MldPacketHandler<<C as IpDeviceIdContext<Ipv6>>::DeviceId>
            + NdpPacketHandler<<C as IpDeviceIdContext<Ipv6>>::DeviceId>,
    > BufferIpTransportContext<Ipv6, B, C> for IcmpIpTransportContext
{
    fn receive_ip_packet(
        ctx: &mut C,
        device: Option<C::DeviceId>,
        src_ip: Ipv6SourceAddr,
        dst_ip: SpecifiedAddr<Ipv6Addr>,
        mut buffer: B,
    ) -> Result<(), (B, TransportReceiveError)> {
        trace!(
            "<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet({:?}, {})",
            src_ip,
            dst_ip
        );

        let packet = match buffer
            .parse_with::<_, Icmpv6Packet<_>>(IcmpParseArgs::new(src_ip.get(), dst_ip))
        {
            Ok(packet) => packet,
            Err(_) => return Ok(()), // TODO(joshlf): Do something else here?
        };

        match packet {
            Icmpv6Packet::EchoRequest(echo_request) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet::echo_request");

                if let Ipv6SourceAddr::Unicast(src_ip) = src_ip {
                    let req = *echo_request.message();
                    let code = echo_request.code();
                    let (local_ip, remote_ip) = (dst_ip, src_ip);
                    // TODO(joshlf): Do something if send_icmp_reply returns an
                    // error?
                    let _ = send_icmp_reply(
                        ctx,
                        device,
                        remote_ip.into_specified(),
                        local_ip,
                        |src_ip| {
                            buffer.encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                                src_ip,
                                remote_ip,
                                code,
                                req.reply(),
                            ))
                        },
                    );
                } else {
                    trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet: Received echo request with an unspecified source address");
                }
            }
            Icmpv6Packet::EchoReply(echo_reply) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet::echo_reply");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet: Received an EchoReply message");
                // We don't allow creating echo sockets connected to the
                // unspecified address, so it's OK to bail early here if the
                // source address is unspecified.
                if let Ipv6SourceAddr::Unicast(src_ip) = src_ip {
                    let id = echo_reply.message().id();
                    let seq = echo_reply.message().seq();
                    receive_icmp_echo_reply(ctx, src_ip.get(), dst_ip, id, seq, buffer);
                }
            }
            Icmpv6Packet::Ndp(packet) => ctx.receive_ndp_packet(
                device.expect("received NDP packet from localhost"),
                src_ip,
                dst_ip,
                packet,
            ),
            Icmpv6Packet::PacketTooBig(packet_too_big) => {
                ctx.increment_counter("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet::packet_too_big");
                trace!("<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet: Received a Packet Too Big message");
                if let Ipv6SourceAddr::Unicast(src_ip) = src_ip {
                    // We are updating the path MTU from the destination address
                    // of this `packet` (which is an IP address on this node) to
                    // some remote (identified by the source address of this
                    // `packet`).
                    //
                    // `update_pmtu_if_less` may return an error, but it will
                    // only happen if the Packet Too Big message's MTU field had
                    // a value that was less than the IPv6 minimum MTU (which as
                    // per IPv6 RFC 8200, must not happen).
                    ctx.update_pmtu_if_less(
                        dst_ip.get(),
                        src_ip.get(),
                        packet_too_big.message().mtu(),
                    );
                }
                receive_icmpv6_error(ctx, &packet_too_big, Icmpv6ErrorCode::PacketTooBig);
            }
            Icmpv6Packet::Mld(packet) => {
                ctx.receive_mld_packet(
                    device.expect("MLD messages must come from a device"),
                    src_ip,
                    dst_ip,
                    packet,
                );
            }
            Icmpv6Packet::DestUnreachable(dest_unreachable) => receive_icmpv6_error(
                ctx,
                &dest_unreachable,
                Icmpv6ErrorCode::DestUnreachable(dest_unreachable.code()),
            ),
            Icmpv6Packet::TimeExceeded(time_exceeded) => receive_icmpv6_error(
                ctx,
                &time_exceeded,
                Icmpv6ErrorCode::TimeExceeded(time_exceeded.code()),
            ),
            Icmpv6Packet::ParameterProblem(parameter_problem) => receive_icmpv6_error(
                ctx,
                &parameter_problem,
                Icmpv6ErrorCode::ParameterProblem(parameter_problem.code()),
            ),
        }

        Ok(())
    }
}

/// Sends an ICMP reply to a remote host.
///
/// `send_icmp_reply` sends a reply to a non-error message (e.g., "echo request"
/// or "timestamp request" messages). It takes the ingress device, source IP,
/// and destination IP of the packet *being responded to*. It uses ICMP-specific
/// logic to figure out whether and how to send an ICMP reply.
///
/// `get_body_from_src_ip` returns a `Serializer` with the bytes of the ICMP
/// packet, and, when called, is given the source IP address chosen for the
/// outbound packet. This allows `get_body_from_src_ip` to properly compute the
/// ICMP checksum, which relies on both the source and destination IP addresses
/// of the IP packet it's encapsulated in.
fn send_icmp_reply<
    I: crate::ip::IpExt,
    B: BufferMut,
    C: BufferIpSocketHandler<I, B> + IpDeviceIdContext<I> + CounterContext,
    S: Serializer<Buffer = B>,
    F: FnOnce(SpecifiedAddr<I::Addr>) -> S,
>(
    ctx: &mut C,
    device: Option<C::DeviceId>,
    original_src_ip: SpecifiedAddr<I::Addr>,
    original_dst_ip: SpecifiedAddr<I::Addr>,
    get_body_from_src_ip: F,
) -> Result<(), S> {
    trace!("send_icmp_reply({:?}, {}, {})", device, original_src_ip, original_dst_ip);
    ctx.increment_counter("send_icmp_reply");
    ctx.send_oneshot_ip_packet(
        Some(original_dst_ip),
        original_src_ip,
        I::ICMP_IP_PROTO,
        None,
        get_body_from_src_ip,
    )
    .map_err(|(body, err)| {
        error!("failed to send ICMP reply: {}", err);
        body
    })
}

/// Receive an ICMP(v4) error message.
///
/// `receive_icmpv4_error` handles an incoming ICMP error message by parsing the
/// original IPv4 packet and then delegating to the context.
fn receive_icmpv4_error<
    B: BufferMut,
    C: InnerBufferIcmpv4Context<B>,
    BB: ByteSlice,
    M: IcmpMessage<Ipv4, BB, Body = OriginalPacket<BB>>,
>(
    ctx: &mut C,
    packet: &IcmpPacket<Ipv4, BB, M>,
    err: Icmpv4ErrorCode,
) {
    packet.with_original_packet(|res| match res {
        Ok(original_packet) => {
            let dst_ip = match SpecifiedAddr::new(original_packet.dst_ip()) {
                Some(ip) => ip,
                None => {
                    trace!("receive_icmpv4_error: Got ICMP error message whose original IPv4 packet contains an unspecified destination address; discarding");
                    return;
                },
            };
            InnerIcmpContext::receive_icmp_error(
                ctx,
                SpecifiedAddr::new(original_packet.src_ip()),
                dst_ip,
                original_packet.proto(),
                original_packet.body().into_inner(),
                err,
            );
        }
        Err(_) => debug!(
            "receive_icmpv4_error: Got ICMP error message with unparsable original IPv4 packet"
        ),
    })
}

/// Receive an ICMPv6 error message.
///
/// `receive_icmpv6_error` handles an incoming ICMPv6 error message by parsing
/// the original IPv6 packet and then delegating to the context.
fn receive_icmpv6_error<
    B: BufferMut,
    C: InnerBufferIcmpv6Context<B>,
    BB: ByteSlice,
    M: IcmpMessage<Ipv6, BB, Body = OriginalPacket<BB>>,
>(
    ctx: &mut C,
    packet: &IcmpPacket<Ipv6, BB, M>,
    err: Icmpv6ErrorCode,
) {
    packet.with_original_packet(|res| match res {
        Ok(original_packet) => {
            let dst_ip = match SpecifiedAddr::new(original_packet.dst_ip()) {
                Some(ip)=>ip,
                None => {
                    trace!("receive_icmpv6_error: Got ICMP error message whose original IPv6 packet contains an unspecified destination address; discarding");
                    return;
                },
            };
            match original_packet.body_proto() {
                Ok((body, proto)) => {
                    InnerIcmpContext::receive_icmp_error(
                        ctx,
                        SpecifiedAddr::new(original_packet.src_ip()),
                        dst_ip,
                        proto,
                        body.into_inner(),
                        err,
                    );
                }
                Err(ExtHdrParseError) => {
                    trace!("receive_icmpv6_error: We could not parse the original packet's extension headers, and so we don't know where the original packet's body begins; discarding");
                    // There's nothing we can do in this case, so we just
                    // return.
                    return;
                }
            }
        }
        Err(_body) => debug!(
            "receive_icmpv6_error: Got ICMPv6 error message with unparsable original IPv6 packet"
        ),
    })
}

/// Send an ICMP(v4) message in response to receiving a packet destined for an
/// unsupported IPv4 protocol.
///
/// `send_icmpv4_protocol_unreachable` sends the appropriate ICMP message in
/// response to receiving an IP packet from `src_ip` to `dst_ip` identifying an
/// unsupported protocol - in particular, a "destination unreachable" message
/// with a "protocol unreachable" code.
///
/// `original_packet` contains the contents of the entire original packet,
/// including the IP header. This must be a whole packet, not a packet fragment.
/// `header_len` is the length of the header including all options.
pub(crate) fn send_icmpv4_protocol_unreachable<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv4_protocol_unreachable");

    send_icmpv4_dest_unreachable(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv4DestUnreachableCode::DestProtocolUnreachable,
        original_packet,
        header_len,
        // If we are sending a protocol unreachable error it is correct to assume that, if the
        // packet was initially fragmented, it has been successfully reassembled by now. It
        // guarantees that we won't send more than one ICMP Destination Unreachable message for
        // different fragments of the same original packet, so we should behave as if we are
        // handling an initial fragment.
        Ipv4FragmentType::InitialFragment,
    );
}

/// Send an ICMPv6 message in response to receiving a packet destined for an
/// unsupported Next Header.
///
/// `send_icmpv6_protocol_unreachable` is like
/// [`send_icmpv4_protocol_unreachable`], but for ICMPv6. It sends an ICMPv6
/// "parameter problem" message with an "unrecognized next header type" code.
///
/// `header_len` is the length of all IPv6 headers (including extension headers)
/// *before* the payload with the problematic Next Header type.
pub(crate) fn send_icmpv6_protocol_unreachable<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv6_protocol_unreachable");

    send_icmpv6_parameter_problem(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
        // Per RFC 4443, the pointer refers to the first byte of the packet
        // whose Next Header field was unrecognized. It is measured as an offset
        // from the beginning of the first IPv6 header. E.g., a pointer of 40
        // (the length of a single IPv6 header) would indicate that the Next
        // Header field from that header - and hence of the first encapsulated
        // packet - was unrecognized.
        //
        // NOTE: Since header_len is a usize, this could theoretically be a
        // lossy conversion. However, all that means in practice is that, if a
        // remote host somehow managed to get us to process a frame with a 4GB
        // IP header and send an ICMP response, the pointer value would be
        // wrong. It's not worth wasting special logic to avoid generating a
        // malformed packet in a case that will almost certainly never happen.
        Icmpv6ParameterProblem::new(header_len as u32),
        original_packet,
        false,
    );
}

/// Send an ICMP(v4) message in response to receiving a packet destined for an
/// unreachable local transport-layer port.
///
/// `send_icmpv4_port_unreachable` sends the appropriate ICMP message in
/// response to receiving an IP packet from `src_ip` to `dst_ip` with an
/// unreachable local transport-layer port. In particular, this is an ICMP
/// "destination unreachable" message with a "port unreachable" code.
///
/// `original_packet` contains the contents of the entire original packet,
/// including the IP header. This must be a whole packet, not a packet fragment.
/// `header_len` is the length of the header including all options.
pub(crate) fn send_icmpv4_port_unreachable<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv4_port_unreachable");

    send_icmpv4_dest_unreachable(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv4DestUnreachableCode::DestPortUnreachable,
        original_packet,
        header_len,
        // If we are sending a port unreachable error it is correct to assume that, if the packet
        // was initially fragmented, it has been successfully reassembled by now. It guarantees that
        // we won't send more than one ICMP Destination Unreachable message for different fragments
        // of the same original packet, so we should behave as if we are handling an initial
        // fragment.
        Ipv4FragmentType::InitialFragment,
    );
}

/// Send an ICMPv6 message in response to receiving a packet destined for an
/// unreachable local transport-layer port.
///
/// `send_icmpv6_port_unreachable` is like [`send_icmpv4_port_unreachable`], but
/// for ICMPv6.
///
/// `original_packet` contains the contents of the entire original packet,
/// including extension headers.
pub(crate) fn send_icmpv6_port_unreachable<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    original_packet: B,
) {
    ctx.increment_counter("send_icmpv6_port_unreachable");

    send_icmpv6_dest_unreachable(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv6DestUnreachableCode::PortUnreachable,
        original_packet,
    );
}

/// Send an ICMP(v4) message in response to receiving a packet destined for an
/// unreachable network.
///
/// `send_icmpv4_net_unreachable` sends the appropriate ICMP message in response
/// to receiving an IP packet from `src_ip` to an unreachable `dst_ip`. In
/// particular, this is an ICMP "destination unreachable" message with a "net
/// unreachable" code.
///
/// `original_packet` contains the contents of the entire original packet -
/// including all IP headers. `header_len` is the length of the IPv4 header. It
/// is ignored for IPv6.
pub(crate) fn send_icmpv4_net_unreachable<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    proto: Ipv4Proto,
    original_packet: B,
    header_len: usize,
    fragment_type: Ipv4FragmentType,
) {
    ctx.increment_counter("send_icmpv4_net_unreachable");

    // Check whether we MUST NOT send an ICMP error message
    // because the original packet was itself an ICMP error message.
    if is_icmp_error_message::<Ipv4>(proto, &original_packet.as_ref()[header_len..]) {
        return;
    }

    send_icmpv4_dest_unreachable(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv4DestUnreachableCode::DestNetworkUnreachable,
        original_packet,
        header_len,
        fragment_type,
    );
}

/// Send an ICMPv6 message in response to receiving a packet destined for an
/// unreachable network.
///
/// `send_icmpv6_net_unreachable` is like [`send_icmpv4_net_unreachable`], but
/// for ICMPv6. It sends an ICMPv6 "destination unreachable" message with a "no
/// route to destination" code.
///
/// `original_packet` contains the contents of the entire original packet
/// including extension headers. `header_len` is the length of the IP header and
/// all extension headers.
pub(crate) fn send_icmpv6_net_unreachable<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    proto: Ipv6Proto,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv6_net_unreachable");

    // Check whether we MUST NOT send an ICMP error message
    // because the original packet was itself an ICMP error message.
    if is_icmp_error_message::<Ipv6>(proto, &original_packet.as_ref()[header_len..]) {
        return;
    }

    send_icmpv6_dest_unreachable(
        ctx,
        device,
        frame_dst,
        src_ip,
        dst_ip,
        Icmpv6DestUnreachableCode::NoRoute,
        original_packet,
    );
}

/// Send an ICMP(v4) message in response to receiving a packet whose TTL has
/// expired.
///
/// `send_icmpv4_ttl_expired` sends the appropriate ICMP in response to
/// receiving an IP packet from `src_ip` to `dst_ip` whose TTL has expired. In
/// particular, this is an ICMP "time exceeded" message with a "time to live
/// exceeded in transit" code.
///
/// `original_packet` contains the contents of the entire original packet,
/// including the header. `header_len` is the length of the IP header including
/// options.
pub(crate) fn send_icmpv4_ttl_expired<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    proto: Ipv4Proto,
    mut original_packet: B,
    header_len: usize,
    fragment_type: Ipv4FragmentType,
) {
    ctx.increment_counter("send_icmpv4_ttl_expired");

    // Check whether we MUST NOT send an ICMP error message because the original
    // packet was itself an ICMP error message.
    if is_icmp_error_message::<Ipv4>(proto, &original_packet.as_ref()[header_len..]) {
        return;
    }

    // Per RFC 792, body contains entire IPv4 header + 64 bytes of original
    // body.
    original_packet.shrink_back_to(header_len + 64);
    // TODO(joshlf): Do something if send_icmp_error_message returns an error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip,
            dst_ip,
            |local_ip| {
                original_packet.encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                    local_ip,
                    src_ip,
                    Icmpv4TimeExceededCode::TtlExpired,
                    IcmpTimeExceeded::default(),
                ))
            },
            None,
            ShouldSendIcmpv4ErrorInfo { fragment_type },
        )
    );
}

/// Send an ICMPv6 message in response to receiving a packet whose hop limit has
/// expired.
///
/// `send_icmpv6_ttl_expired` is like [`send_icmpv4_ttl_expired`], but for
/// ICMPv6. It sends an ICMPv6 "time exceeded" message with a "hop limit
/// exceeded in transit" code.
///
/// `original_packet` contains the contents of the entire original packet
/// including extension headers. `header_len` is the length of the IP header and
/// all extension headers.
pub(crate) fn send_icmpv6_ttl_expired<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    proto: Ipv6Proto,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv6_ttl_expired");

    // Check whether we MUST NOT send an ICMP error message because the
    // original packet was itself an ICMP error message.
    if is_icmp_error_message::<Ipv6>(proto, &original_packet.as_ref()[header_len..]) {
        return;
    }

    // TODO(joshlf): Do something if send_icmp_error_message returns an
    // error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip.into_specified(),
            dst_ip,
            |local_ip| {
                let icmp_builder = IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    local_ip,
                    src_ip.into_specified(),
                    Icmpv6TimeExceededCode::HopLimitExceeded,
                    IcmpTimeExceeded::default(),
                );

                // Per RFC 4443, body contains as much of the original body
                // as possible without exceeding IPv6 minimum MTU.
                TruncatingSerializer::new(original_packet, TruncateDirection::DiscardBack)
                    .encapsulate(icmp_builder)
            },
            Some(Ipv6::MINIMUM_LINK_MTU.into()),
            ShouldSendIcmpv6ErrorInfo { allow_dst_multicast: false },
        )
    );
}

// TODO(joshlf): Test send_icmpv6_packet_too_big once we support dummy IPv6 test
// setups.

/// Send an ICMPv6 message in response to receiving a packet whose size exceeds
/// the MTU of the next hop interface.
///
/// `send_icmpv6_packet_too_big` sends an ICMPv6 "packet too big" message in
/// response to receiving an IP packet from `src_ip` to `dst_ip` whose size
/// exceeds the `mtu` of the next hop interface.
pub(crate) fn send_icmpv6_packet_too_big<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    proto: Ipv6Proto,
    mtu: u32,
    original_packet: B,
    header_len: usize,
) {
    ctx.increment_counter("send_icmpv6_packet_too_big");
    // Check whether we MUST NOT send an ICMP error message because the
    // original packet was itself an ICMP error message.
    if is_icmp_error_message::<Ipv6>(proto, &original_packet.as_ref()[header_len..]) {
        return;
    }

    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip.into_specified(),
            dst_ip,
            |local_ip| {
                let icmp_builder = IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    local_ip,
                    src_ip.into_specified(),
                    IcmpUnusedCode,
                    Icmpv6PacketTooBig::new(mtu),
                );

                // Per RFC 4443, body contains as much of the original body as
                // possible without exceeding IPv6 minimum MTU.
                //
                // The final IP packet must fit within the MTU, so we shrink the
                // original packet to the MTU minus the IPv6 and ICMP header
                // sizes.
                TruncatingSerializer::new(original_packet, TruncateDirection::DiscardBack)
                    .encapsulate(icmp_builder)
            },
            Some(Ipv6::MINIMUM_LINK_MTU.into()),
            // Note, here we explicitly let `should_send_icmpv6_error` allow a
            // multicast destination (link-layer or destination IP) as RFC 4443
            // Section 2.4.e explicitly allows sending an ICMP response if the
            // original packet was sent to a multicast IP or link layer if the
            // ICMP response message will be a Packet Too Big Message.
            ShouldSendIcmpv6ErrorInfo { allow_dst_multicast: true },
        )
    );
}

pub(crate) fn send_icmpv4_parameter_problem<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    code: Icmpv4ParameterProblemCode,
    parameter_problem: Icmpv4ParameterProblem,
    mut original_packet: B,
    header_len: usize,
    fragment_type: Ipv4FragmentType,
) {
    ctx.increment_counter("send_icmpv4_parameter_problem");

    // Per RFC 792, body contains entire IPv4 header + 64 bytes of original
    // body.
    original_packet.shrink_back_to(header_len + 64);
    // TODO(joshlf): Do something if send_icmp_error_message returns an error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip,
            dst_ip,
            |local_ip| {
                original_packet.encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                    local_ip,
                    src_ip,
                    code,
                    parameter_problem,
                ))
            },
            None,
            ShouldSendIcmpv4ErrorInfo { fragment_type },
        )
    );
}

/// Send an ICMPv6 Parameter Problem error message.
///
/// If the error message is Code 2 reporting an unrecognized IPv6 option that
/// has the Option Type highest-order two bits set to 10, `allow_dst_multicast`
/// must be set to `true`. See [`should_send_icmpv6_error`] for more details.
///
/// # Panics
///
/// Panics if `allow_multicast_addr` is set to `true`, but this Parameter
/// Problem's code is not 2 (Unrecognized IPv6 Option).
pub(crate) fn send_icmpv6_parameter_problem<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    code: Icmpv6ParameterProblemCode,
    parameter_problem: Icmpv6ParameterProblem,
    original_packet: B,
    allow_dst_multicast: bool,
) {
    // Only allow the `allow_dst_multicast` parameter to be set if the code is
    // the unrecognized IPv6 option as that is one of the few exceptions where
    // we can send an ICMP packet in response to a packet that was destined for
    // a multicast address.
    assert!(!allow_dst_multicast || code == Icmpv6ParameterProblemCode::UnrecognizedIpv6Option);

    ctx.increment_counter("send_icmpv6_parameter_problem");

    // TODO(joshlf): Do something if send_icmp_error_message returns an error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip.into_specified(),
            dst_ip,
            |local_ip| {
                let icmp_builder = IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    local_ip,
                    src_ip,
                    code,
                    parameter_problem,
                );

                // Per RFC 4443, body contains as much of the original body as
                // possible without exceeding IPv6 minimum MTU.
                TruncatingSerializer::new(original_packet, TruncateDirection::DiscardBack)
                    .encapsulate(icmp_builder)
            },
            Some(Ipv6::MINIMUM_LINK_MTU.into()),
            ShouldSendIcmpv6ErrorInfo { allow_dst_multicast },
        )
    );
}

fn send_icmpv4_dest_unreachable<B: BufferMut, C: InnerBufferIcmpv4Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    code: Icmpv4DestUnreachableCode,
    mut original_packet: B,
    header_len: usize,
    fragment_type: Ipv4FragmentType,
) {
    ctx.increment_counter("send_icmpv4_dest_unreachable");

    // Per RFC 792, body contains entire IPv4 header + 64 bytes of original
    // body.
    original_packet.shrink_back_to(header_len + 64);
    // TODO(joshlf): Do something if send_icmp_error_message returns an error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip,
            dst_ip,
            |local_ip| {
                original_packet.encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                    local_ip,
                    src_ip,
                    code,
                    IcmpDestUnreachable::default(),
                ))
            },
            None,
            ShouldSendIcmpv4ErrorInfo { fragment_type },
        )
    );
}

fn send_icmpv6_dest_unreachable<B: BufferMut, C: InnerBufferIcmpv6Context<B>>(
    ctx: &mut C,
    device: C::DeviceId,
    frame_dst: FrameDestination,
    src_ip: UnicastAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    code: Icmpv6DestUnreachableCode,
    original_packet: B,
) {
    // TODO(joshlf): Do something if send_icmp_error_message returns an error?
    let _ = try_send_error!(
        ctx,
        ctx.send_icmp_error_message(
            device,
            frame_dst,
            src_ip.into_specified(),
            dst_ip,
            |local_ip| {
                let icmp_builder = IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                    local_ip,
                    src_ip,
                    code,
                    IcmpDestUnreachable::default(),
                );

                // Per RFC 4443, body contains as much of the original body as
                // possible without exceeding IPv6 minimum MTU.
                TruncatingSerializer::new(original_packet, TruncateDirection::DiscardBack)
                    .encapsulate(icmp_builder)
            },
            Some(Ipv6::MINIMUM_LINK_MTU.into()),
            ShouldSendIcmpv6ErrorInfo { allow_dst_multicast: false },
        )
    );
}

/// Should we send an ICMP(v4) response?
///
/// `should_send_icmpv4_error` implements the logic described in RFC 1122
/// Section 3.2.2. It decides whether, upon receiving an incoming packet with
/// the given parameters, we should send an ICMP response or not. In particular,
/// we do not send an ICMP response if we've received:
/// - a packet destined to a broadcast or multicast address
/// - a packet sent in a link-layer broadcast
/// - a non-initial fragment
/// - a packet whose source address does not define a single host (a
///   zero/unspecified address, a loopback address, a broadcast address, a
///   multicast address, or a Class E address)
///
/// Note that `should_send_icmpv4_error` does NOT check whether the incoming
/// packet contained an ICMP error message. This is because that check is
/// unnecessary for some ICMP error conditions. The ICMP error message check can
/// be performed separately with `is_icmp_error_message`.
pub(crate) fn should_send_icmpv4_error(
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv4Addr>,
    dst_ip: SpecifiedAddr<Ipv4Addr>,
    info: ShouldSendIcmpv4ErrorInfo,
) -> bool {
    // NOTE: We do not explicitly implement the "unspecified address" check, as
    // it is enforced by the types of the arguments.

    // TODO(joshlf): Implement the rest of the rules:
    // - a packet destined to a subnet broadcast address
    // - a packet whose source address is a subnet broadcast address

    // NOTE: The FrameDestination type has variants for unicast, multicast, and
    // broadcast. One implication of the fact that we only check for broadcast
    // here (in compliance with the RFC) is that we could, in one very unlikely
    // edge case, respond with an ICMP error message to an IP packet which was
    // sent in a link-layer multicast frame. In particular, that can happen if
    // we subscribe to a multicast IP group and, as a result, subscribe to the
    // corresponding multicast MAC address, and we receive a unicast IP packet
    // in a multicast link-layer frame destined to that MAC address.
    //
    // TODO(joshlf): Should we filter incoming multicast IP traffic to make sure
    // that it matches the multicast MAC address of the frame it was
    // encapsulated in?
    info.fragment_type == Ipv4FragmentType::InitialFragment
        && !(dst_ip.is_multicast()
            || dst_ip.is_limited_broadcast()
            || frame_dst.is_broadcast()
            || src_ip.is_loopback()
            || src_ip.is_limited_broadcast()
            || src_ip.is_multicast()
            || src_ip.is_class_e())
}

/// Should we send an ICMPv6 response?
///
/// `should_send_icmpv6_error` implements the logic described in RFC 4443
/// Section 2.4.e. It decides whether, upon receiving an incoming packet with
/// the given parameters, we should send an ICMP response or not. In particular,
/// we do not send an ICMP response if we've received:
/// - a packet destined to a multicast address
///   - Two exceptions to this rules:
///     1) the Packet Too Big Message to allow Path MTU discovery to work for
///        IPv6 multicast
///     2) the Parameter Problem Message, Code 2 reporting an unrecognized IPv6
///        option that has the Option Type highest-order two bits set to 10
/// - a packet sent as a link-layer multicast or broadcast
///   - same exceptions apply here as well.
/// - a packet whose source address does not define a single host (a
///   zero/unspecified address, a loopback address, or a multicast address)
///
/// If an ICMP response will be a Packet Too Big Message or a Parameter Problem
/// Message, Code 2 reporting an unrecognized IPv6 option that has the Option
/// Type highest-order two bits set to 10, `info.allow_dst_multicast` must be
/// set to `true` so this function will allow the exception mentioned above.
///
/// Note that `should_send_icmpv6_error` does NOT check whether the incoming
/// packet contained an ICMP error message. This is because that check is
/// unnecessary for some ICMP error conditions. The ICMP error message check can
/// be performed separately with `is_icmp_error_message`.
pub(crate) fn should_send_icmpv6_error(
    frame_dst: FrameDestination,
    src_ip: SpecifiedAddr<Ipv6Addr>,
    dst_ip: SpecifiedAddr<Ipv6Addr>,
    info: ShouldSendIcmpv6ErrorInfo,
) -> bool {
    // NOTE: We do not explicitly implement the "unspecified address" check, as
    // it is enforced by the types of the arguments.
    !((!info.allow_dst_multicast
        && (dst_ip.is_multicast() || frame_dst.is_multicast() || frame_dst.is_broadcast()))
        || src_ip.is_loopback()
        || src_ip.is_multicast())
}

/// Determine whether or not an IP packet body contains an ICMP error message
/// for the purposes of determining whether or not to send an ICMP response.
///
/// `is_icmp_error_message` checks whether `proto` is ICMP(v4) for IPv4 or
/// ICMPv6 for IPv6 and, if so, attempts to parse `buf` as an ICMP packet in
/// order to determine whether it is an error message or not. If parsing fails,
/// it conservatively assumes that it is an error packet in order to avoid
/// violating the MUST NOT directives of RFC 1122 Section 3.2.2 and [RFC 4443
/// Section 2.4.e].
///
/// [RFC 4443 Section 2.4.e]: https://tools.ietf.org/html/rfc4443#section-2.4
fn is_icmp_error_message<I: IcmpIpExt>(proto: I::Proto, buf: &[u8]) -> bool {
    proto == I::ICMP_IP_PROTO
        && peek_message_type::<I::IcmpMessageType>(buf).map(IcmpMessageType::is_err).unwrap_or(true)
}

/// Common logic for receiving an ICMP echo reply.
fn receive_icmp_echo_reply<I: IcmpIpExt + IpExt, B: BufferMut, C: InnerBufferIcmpContext<I, B>>(
    ctx: &mut C,
    src_ip: I::Addr,
    dst_ip: SpecifiedAddr<I::Addr>,
    id: u16,
    seq: u16,
    body: B,
) {
    if let Some(src_ip) = SpecifiedAddr::new(src_ip) {
        if let Some(conn) = ctx.get_state().conns.get_id_by_addr(&IcmpAddr {
            local_addr: dst_ip,
            remote_addr: src_ip,
            icmp_id: id,
        }) {
            trace!("receive_icmp_echo_reply: Received echo reply for local socket");
            ctx.receive_icmp_echo_reply(
                IcmpConnId::new(conn),
                src_ip.get(),
                dst_ip.get(),
                id,
                seq,
                body,
            );
        } else {
            // TODO(fxbug.dev/47952): Neither the ICMPv4 or ICMPv6 RFCs
            // explicitly state what to do in case we receive an "unsolicited"
            // echo reply. We only expose the replies if we have a registered
            // connection for the IcmpAddr of the incoming reply for now. Given
            // that a reply should only be sent in response to a request, an
            // ICMP unreachable-type message is probably not appropriate for
            // unsolicited replies. However, it's also possible that we sent a
            // request and then closed the socket before receiving the reply, so
            // this doesn't necessarily indicate a buggy or malicious remote
            // host. We should figure this out definitively.
            //
            // If we do decide to send an ICMP error message, the appropriate
            // thing to do is probably to have this function return a `Result`,
            // and then have the top-level implementation of
            // `BufferIpTransportContext::receive_ip_packet` return the
            // appropriate error.
            trace!("receive_icmp_echo_reply: Received echo reply with no local socket");
        }
    } else {
        trace!("receive_icmp_echo_reply: Received echo reply with an unspecified source address");
    }
}

/// Send an ICMPv4 echo request on an existing connection.
///
/// # Panics
///
/// `send_icmpv4_echo_request` panics if `conn` is not associated with an ICMPv4
/// connection.
pub fn send_icmpv4_echo_request<B: BufferMut, D: BufferDispatcher<B>>(
    ctx: &mut Ctx<D>,
    conn: IcmpConnId<Ipv4>,
    seq_num: u16,
    body: B,
) -> Result<(), (B, IpSockSendError)> {
    send_icmp_echo_request_inner(ctx, conn, seq_num, body)
}

/// Send an ICMPv6 echo request on an existing connection.
///
/// # Panics
///
/// `send_icmpv6_echo_request` panics if `conn` is not associated with an ICMPv6
/// connection.
pub fn send_icmpv6_echo_request<B: BufferMut, D: BufferDispatcher<B>>(
    ctx: &mut Ctx<D>,
    conn: IcmpConnId<Ipv6>,
    seq_num: u16,
    body: B,
) -> Result<(), (B, IpSockSendError)> {
    send_icmp_echo_request_inner(ctx, conn, seq_num, body)
}

fn send_icmp_echo_request_inner<
    I: IcmpIpExt + IpExt,
    B: BufferMut,
    C: InnerBufferIcmpContext<I, B>,
>(
    ctx: &mut C,
    conn: IcmpConnId<I>,
    seq_num: u16,
    body: B,
) -> Result<(), (B, IpSockSendError)>
where
    IcmpEchoRequest: for<'a> IcmpMessage<I, &'a [u8], Code = IcmpUnusedCode>,
{
    // TODO(joshlf): Come up with a better approach to the lifetimes issues than
    // cloning the entire socket.
    let ConnSocketEntry { sock, addr: _ } = ctx
        .get_state_mut()
        .conns
        .get_sock_by_id(conn.0)
        .expect("icmp::send_icmp_echo_request_inner: no such socket");
    let conn = sock.clone();
    ctx.send_ip_packet(
        &conn.ip,
        body.encapsulate(IcmpPacketBuilder::<I, &[u8], _>::new(
            conn.ip.local_ip().get(),
            conn.ip.remote_ip().get(),
            IcmpUnusedCode,
            IcmpEchoRequest::new(conn.icmp_id, seq_num),
        )),
    )
    .map_err(|(encapsulated, err)| (encapsulated.into_inner(), err))
}

/// An error when attempting to create ang ICMP socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum IcmpSockCreationError {
    /// An error was encountered when attempting to create the underlying IP
    /// socket.
    #[error("{}", _0)]
    Ip(#[from] IpSockCreationError),
    /// The specified socket addresses (IP addresses and ICMP ID) conflict with
    /// an existing ICMP socket.
    #[error("addresses conflict with an existing ICMP socket")]
    SockAddrConflict,
}

/// Creates a new ICMPv4 connection.
///
/// Creates a new ICMPv4 connection with the provided parameters `local_addr`,
/// `remote_addr` and `icmp_id`, and returns its newly-allocated ID. If
/// `local_addr` is `None`, one will be chosen automatically.
///
/// If a connection with the conflicting parameters already exists, the call
/// fails and returns [`IcmpSockCreationError::SockAddrConflict`].
pub fn new_icmpv4_connection<D: EventDispatcher>(
    ctx: &mut Ctx<D>,
    local_addr: Option<SpecifiedAddr<Ipv4Addr>>,
    remote_addr: SpecifiedAddr<Ipv4Addr>,
    icmp_id: u16,
) -> Result<IcmpConnId<Ipv4>, IcmpSockCreationError> {
    new_icmpv4_connection_inner(ctx, local_addr, remote_addr, icmp_id)
}

// TODO(joshlf): Make this the external function (replacing the existing
// `new_icmpv4_connection`) once the ICMP context traits are part of the public
// API.
fn new_icmpv4_connection_inner<C: InnerIcmpv4Context>(
    ctx: &mut C,
    local_addr: Option<SpecifiedAddr<Ipv4Addr>>,
    remote_addr: SpecifiedAddr<Ipv4Addr>,
    icmp_id: u16,
) -> Result<IcmpConnId<Ipv4>, IcmpSockCreationError> {
    let ip = ctx.new_ip_socket(
        local_addr,
        remote_addr,
        Ipv4Proto::Icmp,
        UnroutableBehavior::Close,
        None,
    )?;
    let state: &mut IcmpState<_, _, _> = ctx.get_state_mut();
    Ok(new_icmp_connection_inner(&mut state.conns, remote_addr, icmp_id, ip)?)
}

/// Creates a new ICMPv4 connection.
///
/// Creates a new ICMPv4 connection with the provided parameters `local_addr`,
/// `remote_addr` and `icmp_id`, and returns its newly-allocated ID. If
/// `local_addr` is `None`, one will be chosen automatically.
///
/// If a connection with the conflicting parameters already exists, the call
/// fails and returns [`IcmpSockCreationError::SockAddrConflict`].
pub fn new_icmpv6_connection<D: EventDispatcher>(
    ctx: &mut Ctx<D>,
    local_addr: Option<SpecifiedAddr<Ipv6Addr>>,
    remote_addr: SpecifiedAddr<Ipv6Addr>,
    icmp_id: u16,
) -> Result<IcmpConnId<Ipv6>, IcmpSockCreationError> {
    new_icmpv6_connection_inner(ctx, local_addr, remote_addr, icmp_id)
}

// TODO(joshlf): Make this the external function (replacing the existing
// `new_icmpv6_connection`) once the ICMP context traits are part of the public
// API.
fn new_icmpv6_connection_inner<C: InnerIcmpv6Context>(
    ctx: &mut C,
    local_addr: Option<SpecifiedAddr<Ipv6Addr>>,
    remote_addr: SpecifiedAddr<Ipv6Addr>,
    icmp_id: u16,
) -> Result<IcmpConnId<Ipv6>, IcmpSockCreationError> {
    let ip = ctx.new_ip_socket(
        local_addr,
        remote_addr,
        Ipv6Proto::Icmpv6,
        UnroutableBehavior::Close,
        None,
    )?;
    let state: &mut IcmpState<_, _, _> = ctx.get_state_mut();
    Ok(new_icmp_connection_inner(&mut state.conns, remote_addr, icmp_id, ip)?)
}

fn new_icmp_connection_inner<I: IcmpIpExt + IpExt, S: IpSocket<I>>(
    conns: &mut ConnSocketMap<IcmpAddr<I::Addr>, IcmpConn<S>>,
    remote_addr: SpecifiedAddr<I::Addr>,
    icmp_id: u16,
    ip: S,
) -> Result<IcmpConnId<I>, IcmpSockCreationError> {
    let addr = IcmpAddr { local_addr: *ip.local_ip(), remote_addr, icmp_id };
    if conns.get_id_by_addr(&addr).is_some() {
        return Err(IcmpSockCreationError::SockAddrConflict);
    }
    Ok(IcmpConnId::new(conns.insert(addr, IcmpConn { icmp_id, ip })))
}

#[cfg(test)]
mod tests {
    use alloc::{format, vec};
    use core::{convert::TryInto, fmt::Debug, num::NonZeroU16, time::Duration};

    use net_types::ip::{Ip, IpVersion, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr};
    use packet::{Buf, Serializer};
    use packet_formats::{
        icmp::{
            mld::MldPacket, ndp::NdpPacket, IcmpEchoReply, IcmpEchoRequest, IcmpMessage,
            IcmpPacket, IcmpUnusedCode, Icmpv4TimestampRequest, MessageBody,
        },
        ip::{IpPacketBuilder, IpProto},
        testutil::parse_icmp_packet_in_ip_packet_in_ethernet_frame,
        udp::UdpPacketBuilder,
    };
    use specialize_ip_macro::ip_test;

    use super::*;
    use crate::{
        assert_empty,
        context::testutil::{DummyCtx, DummyInstant},
        device::{DeviceId, FrameDestination, IpFrameMeta},
        ip::{
            device::{set_routing_enabled, state::IpDeviceStateIpExt},
            gmp::mld::MldPacketHandler,
            path_mtu::testutil::DummyPmtuState,
            receive_ipv4_packet, receive_ipv6_packet,
            socket::testutil::DummyIpSocketCtx,
            DummyDeviceId,
        },
        testutil::{
            get_counter_val, DummyEventDispatcherBuilder, DUMMY_CONFIG_V4, DUMMY_CONFIG_V6,
        },
        transport::udp::UdpStateBuilder,
        StackStateBuilder,
    };

    trait TestIpExt: crate::testutil::TestIpExt + crate::testutil::TestutilIpExt {
        fn new_icmp_connection<D: EventDispatcher>(
            ctx: &mut Ctx<D>,
            local_addr: Option<SpecifiedAddr<Self::Addr>>,
            remote_addr: SpecifiedAddr<Self::Addr>,
            icmp_id: u16,
        ) -> Result<IcmpConnId<Self>, IcmpSockCreationError>;

        fn send_icmp_echo_request<B: BufferMut, D: BufferDispatcher<B>>(
            ctx: &mut Ctx<D>,
            conn: IcmpConnId<Self>,
            seq_num: u16,
            body: B,
        ) -> Result<(), IpSockSendError>;
    }

    impl TestIpExt for Ipv4 {
        fn new_icmp_connection<D: EventDispatcher>(
            ctx: &mut Ctx<D>,
            local_addr: Option<SpecifiedAddr<Ipv4Addr>>,
            remote_addr: SpecifiedAddr<Ipv4Addr>,
            icmp_id: u16,
        ) -> Result<IcmpConnId<Ipv4>, IcmpSockCreationError> {
            new_icmpv4_connection(ctx, local_addr, remote_addr, icmp_id)
        }

        fn send_icmp_echo_request<B: BufferMut, D: BufferDispatcher<B>>(
            ctx: &mut Ctx<D>,
            conn: IcmpConnId<Ipv4>,
            seq_num: u16,
            body: B,
        ) -> Result<(), IpSockSendError> {
            send_icmpv4_echo_request(ctx, conn, seq_num, body).map_err(|(_body, err)| err)
        }
    }

    impl TestIpExt for Ipv6 {
        fn new_icmp_connection<D: EventDispatcher>(
            ctx: &mut Ctx<D>,
            local_addr: Option<SpecifiedAddr<Ipv6Addr>>,
            remote_addr: SpecifiedAddr<Ipv6Addr>,
            icmp_id: u16,
        ) -> Result<IcmpConnId<Ipv6>, IcmpSockCreationError> {
            new_icmpv6_connection(ctx, local_addr, remote_addr, icmp_id)
        }

        fn send_icmp_echo_request<B: BufferMut, D: BufferDispatcher<B>>(
            ctx: &mut Ctx<D>,
            conn: IcmpConnId<Ipv6>,
            seq_num: u16,
            body: B,
        ) -> Result<(), IpSockSendError> {
            send_icmpv6_echo_request(ctx, conn, seq_num, body).map_err(|(_body, err)| err)
        }
    }

    // Tests that require an entire IP stack.

    /// Test that receiving a particular IP packet results in a particular ICMP
    /// response.
    ///
    /// Test that receiving an IP packet from remote host
    /// `I::DUMMY_CONFIG.remote_ip` to host `dst_ip` with `ttl` and `proto`
    /// results in all of the counters in `assert_counters` being triggered at
    /// least once.
    ///
    /// If `expect_message_code` is `Some`, expect that exactly one ICMP packet
    /// was sent in response with the given message and code, and invoke the
    /// given function `f` on the packet. Otherwise, if it is `None`, expect
    /// that no response was sent.
    ///
    /// `modify_packet_builder` is invoked on the `PacketBuilder` before the
    /// packet is serialized.
    ///
    /// `modify_stack_state_builder` is invoked on the `StackStateBuilder`
    /// before it is used to build the context.
    ///
    /// The state is initialized to `I::DUMMY_CONFIG` when testing.
    #[allow(clippy::too_many_arguments)]
    fn test_receive_ip_packet<
        I: TestIpExt + IcmpIpExt,
        C: PartialEq + Debug,
        M: for<'a> IcmpMessage<I, &'a [u8], Code = C> + PartialEq + Debug,
        PBF: FnOnce(&mut <I as packet_formats::ip::IpExt>::PacketBuilder),
        SSBF: FnOnce(&mut StackStateBuilder),
        F: for<'a> FnOnce(&IcmpPacket<I, &'a [u8], M>),
    >(
        modify_packet_builder: PBF,
        modify_stack_state_builder: SSBF,
        body: &mut [u8],
        dst_ip: SpecifiedAddr<I::Addr>,
        ttl: u8,
        proto: I::Proto,
        assert_counters: &[&str],
        expect_message_code: Option<(M, C)>,
        f: F,
    ) {
        crate::testutil::set_logger_for_test();
        let mut pb = <I as packet_formats::ip::IpExt>::PacketBuilder::new(
            *I::DUMMY_CONFIG.remote_ip,
            dst_ip.get(),
            ttl,
            proto,
        );
        modify_packet_builder(&mut pb);
        let buffer = Buf::new(body, ..).encapsulate(pb).serialize_vec_outer().unwrap();

        let mut ctx = DummyEventDispatcherBuilder::from_config(I::DUMMY_CONFIG)
            .build_with_modifications(modify_stack_state_builder);

        let device = DeviceId::new_ethernet(0);
        set_routing_enabled::<_, I>(&mut ctx, device, true).expect("error setting routing enabled");
        match I::VERSION {
            IpVersion::V4 => {
                receive_ipv4_packet(&mut ctx, device, FrameDestination::Unicast, buffer)
            }
            IpVersion::V6 => {
                receive_ipv6_packet(&mut ctx, device, FrameDestination::Unicast, buffer)
            }
        }

        for counter in assert_counters {
            assert!(get_counter_val(&ctx, counter) > 0, "counter at zero: {}", counter);
        }

        if let Some((expect_message, expect_code)) = expect_message_code {
            assert_eq!(ctx.dispatcher.frames_sent().len(), 1);
            let (src_mac, dst_mac, src_ip, dst_ip, _, message, code) =
                parse_icmp_packet_in_ip_packet_in_ethernet_frame::<I, _, M, _>(
                    &ctx.dispatcher.frames_sent()[0].1,
                    f,
                )
                .unwrap();

            assert_eq!(src_mac, I::DUMMY_CONFIG.local_mac.get());
            assert_eq!(dst_mac, I::DUMMY_CONFIG.remote_mac.get());
            assert_eq!(src_ip, I::DUMMY_CONFIG.local_ip.get());
            assert_eq!(dst_ip, I::DUMMY_CONFIG.remote_ip.get());
            assert_eq!(message, expect_message);
            assert_eq!(code, expect_code);
        } else {
            assert_empty(ctx.dispatcher.frames_sent().iter());
        }
    }

    #[test]
    fn test_receive_echo() {
        crate::testutil::set_logger_for_test();

        // Test that, when receiving an echo request, we respond with an echo
        // reply with the appropriate parameters.

        fn test<I: TestIpExt + IcmpIpExt>(assert_counters: &[&str])
        where
            IcmpEchoRequest: for<'a> IcmpMessage<I, &'a [u8], Code = IcmpUnusedCode>,
            IcmpEchoReply: for<'a> IcmpMessage<
                I,
                &'a [u8],
                Code = IcmpUnusedCode,
                Body = OriginalPacket<&'a [u8]>,
            >,
        {
            let req = IcmpEchoRequest::new(0, 0);
            let req_body = &[1, 2, 3, 4];
            let mut buffer = Buf::new(req_body.to_vec(), ..)
                .encapsulate(IcmpPacketBuilder::<I, &[u8], _>::new(
                    I::DUMMY_CONFIG.remote_ip.get(),
                    I::DUMMY_CONFIG.local_ip.get(),
                    IcmpUnusedCode,
                    req,
                ))
                .serialize_vec_outer()
                .unwrap();
            test_receive_ip_packet::<I, _, _, _, _, _>(
                |_| {},
                |_| {},
                buffer.as_mut(),
                I::DUMMY_CONFIG.local_ip,
                64,
                I::ICMP_IP_PROTO,
                assert_counters,
                Some((req.reply(), IcmpUnusedCode)),
                |packet| assert_eq!(packet.original_packet().bytes(), req_body),
            );
        }

        test::<Ipv4>(&["<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::echo_request", "send_ipv4_packet"]);
        test::<Ipv6>(&["<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet::echo_request", "send_ipv6_packet"]);
    }

    #[test]
    fn test_receive_timestamp() {
        crate::testutil::set_logger_for_test();

        let req = Icmpv4TimestampRequest::new(1, 2, 3);
        let mut buffer = Buf::new(Vec::new(), ..)
            .encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                DUMMY_CONFIG_V4.remote_ip,
                DUMMY_CONFIG_V4.local_ip,
                IcmpUnusedCode,
                req,
            ))
            .serialize_vec_outer()
            .unwrap();
        test_receive_ip_packet::<Ipv4, _, _, _, _, _>(
            |_| {},
            |builder| {
                let _: &mut Icmpv4StateBuilder = builder.ipv4_builder().icmpv4_builder().send_timestamp_reply(true);
            },
            buffer.as_mut(),
            DUMMY_CONFIG_V4.local_ip,
            64,
            Ipv4Proto::Icmp,
            &["<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::timestamp_request", "send_ipv4_packet"],
            Some((req.reply(0x80000000, 0x80000000), IcmpUnusedCode)),
            |_| {},
        );
    }

    #[test]
    fn test_protocol_unreachable() {
        // Test receiving an IP packet for an unreachable protocol. Check to
        // make sure that we respond with the appropriate ICMP message.
        //
        // Currently, for IPv4, we test for all unreachable protocols, while for
        // IPv6, we only test for IGMP and TCP. See the comment below for why
        // that limitation exists. Once the limitation is fixed, we should test
        // with all unreachable protocols for both versions.

        for proto in 0u8..=255 {
            let v4proto = Ipv4Proto::from(proto);
            match v4proto {
                Ipv4Proto::Proto(IpProto::Tcp) | Ipv4Proto::Other(_) => {
                    test_receive_ip_packet::<Ipv4, _, _, _, _, _>(
                        |_| {},
                        |_| {},
                        &mut [0u8; 128],
                        DUMMY_CONFIG_V4.local_ip,
                        64,
                        v4proto,
                        &["send_icmpv4_protocol_unreachable", "send_icmp_error_message"],
                        Some((
                            IcmpDestUnreachable::default(),
                            Icmpv4DestUnreachableCode::DestProtocolUnreachable,
                        )),
                        // Ensure packet is truncated to the right length.
                        |packet| assert_eq!(packet.original_packet().bytes().len(), 84),
                    );
                }
                Ipv4Proto::Icmp | Ipv4Proto::Igmp | Ipv4Proto::Proto(IpProto::Udp) => {}
            }

            // TODO(fxbug.dev/47953): We seem to fail to parse an IPv6 packet if
            // its Next Header value is unrecognized (rather than treating this
            // as a valid parsing but then replying with a parameter problem
            // error message). We should a) fix this and, b) expand this test to
            // ensure we don't regress.
            let v6proto = Ipv6Proto::from(proto);
            match v6proto {
                Ipv6Proto::Proto(IpProto::Tcp) => {
                    test_receive_ip_packet::<Ipv6, _, _, _, _, _>(
                        |_| {},
                        |_| {},
                        &mut [0u8; 128],
                        DUMMY_CONFIG_V6.local_ip,
                        64,
                        v6proto,
                        &["send_icmpv6_protocol_unreachable", "send_icmp_error_message"],
                        Some((
                            Icmpv6ParameterProblem::new(40),
                            Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
                        )),
                        // Ensure packet is truncated to the right length.
                        |packet| assert_eq!(packet.original_packet().bytes().len(), 168),
                    );
                }
                Ipv6Proto::Icmpv6
                | Ipv6Proto::NoNextHeader
                | Ipv6Proto::Proto(IpProto::Udp)
                | Ipv6Proto::Other(_) => {}
            }
        }
    }

    #[test]
    fn test_port_unreachable() {
        // TODO(joshlf): Test TCP as well.

        // Receive an IP packet for an unreachable UDP port (1234). Check to
        // make sure that we respond with the appropriate ICMP message. Then, do
        // the same for a stack which has the UDP `send_port_unreachable` option
        // disable, and make sure that we DON'T respond with an ICMP message.

        fn test<I: TestIpExt + IcmpIpExt, C: PartialEq + Debug>(
            code: C,
            assert_counters: &[&str],
            original_packet_len: usize,
        ) where
            IcmpDestUnreachable:
                for<'a> IcmpMessage<I, &'a [u8], Code = C, Body = OriginalPacket<&'a [u8]>>,
        {
            let mut buffer = Buf::new(vec![0; 128], ..)
                .encapsulate(UdpPacketBuilder::new(
                    I::DUMMY_CONFIG.remote_ip.get(),
                    I::DUMMY_CONFIG.local_ip.get(),
                    None,
                    NonZeroU16::new(1234).unwrap(),
                ))
                .serialize_vec_outer()
                .unwrap();
            test_receive_ip_packet::<I, _, _, _, _, _>(
                |_| {},
                // Enable the `send_port_unreachable` feature.
                |builder| {
                    let _: &mut UdpStateBuilder =
                        builder.transport_builder().udp_builder().send_port_unreachable(true);
                },
                buffer.as_mut(),
                I::DUMMY_CONFIG.local_ip,
                64,
                IpProto::Udp.into(),
                assert_counters,
                Some((IcmpDestUnreachable::default(), code)),
                // Ensure packet is truncated to the right length.
                |packet| assert_eq!(packet.original_packet().bytes().len(), original_packet_len),
            );
            test_receive_ip_packet::<I, C, IcmpDestUnreachable, _, _, _>(
                |_| {},
                // Leave the `send_port_unreachable` feature disabled.
                |_: &mut StackStateBuilder| {},
                buffer.as_mut(),
                I::DUMMY_CONFIG.local_ip,
                64,
                IpProto::Udp.into(),
                &[],
                None,
                |_| {},
            );
        }

        test::<Ipv4, _>(
            Icmpv4DestUnreachableCode::DestPortUnreachable,
            &["send_icmpv4_port_unreachable", "send_icmp_error_message"],
            84,
        );
        test::<Ipv6, _>(
            Icmpv6DestUnreachableCode::PortUnreachable,
            &["send_icmpv6_port_unreachable", "send_icmp_error_message"],
            176,
        );
    }

    #[test]
    fn test_net_unreachable() {
        // Receive an IP packet for an unreachable destination address. Check to
        // make sure that we respond with the appropriate ICMP message.
        test_receive_ip_packet::<Ipv4, _, _, _, _, _>(
            |_| {},
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            SpecifiedAddr::new(Ipv4Addr::new([1, 2, 3, 4])).unwrap(),
            64,
            IpProto::Udp.into(),
            &["send_icmpv4_net_unreachable", "send_icmp_error_message"],
            Some((
                IcmpDestUnreachable::default(),
                Icmpv4DestUnreachableCode::DestNetworkUnreachable,
            )),
            // Ensure packet is truncated to the right length.
            |packet| assert_eq!(packet.original_packet().bytes().len(), 84),
        );
        test_receive_ip_packet::<Ipv6, _, _, _, _, _>(
            |_| {},
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            SpecifiedAddr::new(Ipv6Addr::from_bytes([
                1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8,
            ]))
            .unwrap(),
            64,
            IpProto::Udp.into(),
            &["send_icmpv6_net_unreachable", "send_icmp_error_message"],
            Some((IcmpDestUnreachable::default(), Icmpv6DestUnreachableCode::NoRoute)),
            // Ensure packet is truncated to the right length.
            |packet| assert_eq!(packet.original_packet().bytes().len(), 168),
        );
        // Same test for IPv4 but with a non-initial fragment. No ICMP error
        // should be sent.
        test_receive_ip_packet::<Ipv4, _, IcmpDestUnreachable, _, _, _>(
            |pb| pb.fragment_offset(64),
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            SpecifiedAddr::new(Ipv4Addr::new([1, 2, 3, 4])).unwrap(),
            64,
            IpProto::Udp.into(),
            &[],
            None,
            |_| {},
        );
    }

    #[test]
    fn test_ttl_expired() {
        // Receive an IP packet with an expired TTL. Check to make sure that we
        // respond with the appropriate ICMP message.
        test_receive_ip_packet::<Ipv4, _, _, _, _, _>(
            |_| {},
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            DUMMY_CONFIG_V4.remote_ip,
            1,
            IpProto::Udp.into(),
            &["send_icmpv4_ttl_expired", "send_icmp_error_message"],
            Some((IcmpTimeExceeded::default(), Icmpv4TimeExceededCode::TtlExpired)),
            // Ensure packet is truncated to the right length.
            |packet| assert_eq!(packet.original_packet().bytes().len(), 84),
        );
        test_receive_ip_packet::<Ipv6, _, _, _, _, _>(
            |_| {},
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            DUMMY_CONFIG_V6.remote_ip,
            1,
            IpProto::Udp.into(),
            &["send_icmpv6_ttl_expired", "send_icmp_error_message"],
            Some((IcmpTimeExceeded::default(), Icmpv6TimeExceededCode::HopLimitExceeded)),
            // Ensure packet is truncated to the right length.
            |packet| assert_eq!(packet.original_packet().bytes().len(), 168),
        );
        // Same test for IPv4 but with a non-initial fragment. No ICMP error
        // should be sent.
        test_receive_ip_packet::<Ipv4, _, IcmpTimeExceeded, _, _, _>(
            |pb| pb.fragment_offset(64),
            |_: &mut StackStateBuilder| {},
            &mut [0u8; 128],
            SpecifiedAddr::new(Ipv4Addr::new([1, 2, 3, 4])).unwrap(),
            64,
            IpProto::Udp.into(),
            &[],
            None,
            |_| {},
        );
    }

    #[test]
    fn test_should_send_icmpv4_error() {
        let src_ip = DUMMY_CONFIG_V4.local_ip;
        let dst_ip = DUMMY_CONFIG_V4.remote_ip;
        let frame_dst = FrameDestination::Unicast;
        let multicast_ip_1 = SpecifiedAddr::new(Ipv4Addr::new([224, 0, 0, 1])).unwrap();
        let multicast_ip_2 = SpecifiedAddr::new(Ipv4Addr::new([224, 0, 0, 2])).unwrap();
        let is_initial_fragment =
            ShouldSendIcmpv4ErrorInfo { fragment_type: Ipv4FragmentType::InitialFragment };
        let is_not_initial_fragment =
            ShouldSendIcmpv4ErrorInfo { fragment_type: Ipv4FragmentType::NonInitialFragment };

        // Should Send, unless non initial fragment.
        assert!(should_send_icmpv4_error(frame_dst, src_ip, dst_ip, is_initial_fragment));
        assert!(!should_send_icmpv4_error(frame_dst, src_ip, dst_ip, is_not_initial_fragment));

        // Should not send because destined for IP broadcast addr
        assert!(!should_send_icmpv4_error(
            frame_dst,
            src_ip,
            Ipv4::LIMITED_BROADCAST_ADDRESS,
            is_initial_fragment
        ));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            src_ip,
            Ipv4::LIMITED_BROADCAST_ADDRESS,
            is_not_initial_fragment
        ));

        // Should not send because destined for multicast addr
        assert!(!should_send_icmpv4_error(frame_dst, src_ip, multicast_ip_1, is_initial_fragment));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            src_ip,
            multicast_ip_1,
            is_not_initial_fragment
        ));

        // Should not send because Link Layer Broadcast.
        assert!(!should_send_icmpv4_error(
            FrameDestination::Broadcast,
            src_ip,
            dst_ip,
            is_initial_fragment
        ));
        assert!(!should_send_icmpv4_error(
            FrameDestination::Broadcast,
            src_ip,
            dst_ip,
            is_not_initial_fragment
        ));

        // Should not send because from loopback addr
        assert!(!should_send_icmpv4_error(
            frame_dst,
            Ipv4::LOOPBACK_ADDRESS,
            dst_ip,
            is_initial_fragment
        ));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            Ipv4::LOOPBACK_ADDRESS,
            dst_ip,
            is_not_initial_fragment
        ));

        // Should not send because from limited broadcast addr
        assert!(!should_send_icmpv4_error(
            frame_dst,
            Ipv4::LIMITED_BROADCAST_ADDRESS,
            dst_ip,
            is_initial_fragment
        ));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            Ipv4::LIMITED_BROADCAST_ADDRESS,
            dst_ip,
            is_not_initial_fragment
        ));

        // Should not send because from multicast addr
        assert!(!should_send_icmpv4_error(frame_dst, multicast_ip_2, dst_ip, is_initial_fragment));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            multicast_ip_2,
            dst_ip,
            is_not_initial_fragment
        ));

        // Should not send because from class E addr
        assert!(!should_send_icmpv4_error(
            frame_dst,
            SpecifiedAddr::new(Ipv4Addr::new([240, 0, 0, 1])).unwrap(),
            dst_ip,
            is_initial_fragment
        ));
        assert!(!should_send_icmpv4_error(
            frame_dst,
            SpecifiedAddr::new(Ipv4Addr::new([240, 0, 0, 1])).unwrap(),
            dst_ip,
            is_not_initial_fragment
        ));
    }

    #[test]
    fn test_should_send_icmpv6_error() {
        let src_ip = DUMMY_CONFIG_V6.local_ip;
        let dst_ip = DUMMY_CONFIG_V6.remote_ip;
        let frame_dst = FrameDestination::Unicast;
        let multicast_ip_1 =
            SpecifiedAddr::new(Ipv6Addr::new([0xff00, 0, 0, 0, 0, 0, 0, 1])).unwrap();
        let multicast_ip_2 =
            SpecifiedAddr::new(Ipv6Addr::new([0xff00, 0, 0, 0, 0, 0, 0, 2])).unwrap();
        let allow_dst_multicast = ShouldSendIcmpv6ErrorInfo { allow_dst_multicast: true };
        let disallow_dst_multicast = ShouldSendIcmpv6ErrorInfo { allow_dst_multicast: false };

        // Should Send.
        assert!(should_send_icmpv6_error(frame_dst, src_ip, dst_ip, disallow_dst_multicast));
        assert!(should_send_icmpv6_error(frame_dst, src_ip, dst_ip, allow_dst_multicast));

        // Should not send because destined for multicast addr, unless exception
        // applies.
        assert!(!should_send_icmpv6_error(
            frame_dst,
            src_ip,
            multicast_ip_1,
            disallow_dst_multicast
        ));
        assert!(should_send_icmpv6_error(frame_dst, src_ip, multicast_ip_1, allow_dst_multicast));

        // Should not send because Link Layer Broadcast, unless exception
        // applies.
        assert!(!should_send_icmpv6_error(
            FrameDestination::Broadcast,
            src_ip,
            dst_ip,
            disallow_dst_multicast
        ));
        assert!(should_send_icmpv6_error(
            FrameDestination::Broadcast,
            src_ip,
            dst_ip,
            allow_dst_multicast
        ));

        // Should not send because from loopback addr.
        assert!(!should_send_icmpv6_error(
            frame_dst,
            Ipv6::LOOPBACK_ADDRESS,
            dst_ip,
            disallow_dst_multicast
        ));
        assert!(!should_send_icmpv6_error(
            frame_dst,
            Ipv6::LOOPBACK_ADDRESS,
            dst_ip,
            allow_dst_multicast
        ));

        // Should not send because from multicast addr.
        assert!(!should_send_icmpv6_error(
            frame_dst,
            multicast_ip_2,
            dst_ip,
            disallow_dst_multicast
        ));
        assert!(!should_send_icmpv6_error(frame_dst, multicast_ip_2, dst_ip, allow_dst_multicast));

        // Should not send because from multicast addr, even though dest
        // multicast exception applies.
        assert!(!should_send_icmpv6_error(
            FrameDestination::Broadcast,
            multicast_ip_2,
            dst_ip,
            disallow_dst_multicast
        ));
        assert!(!should_send_icmpv6_error(
            FrameDestination::Broadcast,
            multicast_ip_2,
            dst_ip,
            allow_dst_multicast
        ));
        assert!(!should_send_icmpv6_error(
            frame_dst,
            multicast_ip_2,
            multicast_ip_1,
            disallow_dst_multicast
        ));
        assert!(!should_send_icmpv6_error(
            frame_dst,
            multicast_ip_2,
            multicast_ip_1,
            allow_dst_multicast
        ));
    }

    enum IcmpConnectionType {
        Local,
        Remote,
    }

    fn test_icmp_connection<I: Ip + TestIpExt>(conn_type: IcmpConnectionType) {
        crate::testutil::set_logger_for_test();

        let recv_icmp_packet_name = match I::VERSION {
            IpVersion::V4 => {
                "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet"
            }
            IpVersion::V6 => {
                "<IcmpIpTransportContext as BufferIpTransportContext<Ipv6>>::receive_ip_packet"
            }
        };

        let config = I::DUMMY_CONFIG;

        const LOCAL_CTX_NAME: &str = "alice";
        const REMOTE_CTX_NAME: &str = "bob";
        let mut net = crate::testutil::new_dummy_network_from_config(
            LOCAL_CTX_NAME,
            REMOTE_CTX_NAME,
            config.clone(),
        );

        let icmp_id = 13;

        let (remote_addr, ctx_name_receiving_req) = match conn_type {
            IcmpConnectionType::Local => (config.local_ip, LOCAL_CTX_NAME),
            IcmpConnectionType::Remote => (config.remote_ip, REMOTE_CTX_NAME),
        };

        let conn = I::new_icmp_connection(
            net.context(LOCAL_CTX_NAME),
            Some(config.local_ip),
            remote_addr,
            icmp_id,
        )
        .unwrap();

        let echo_body = vec![1, 2, 3, 4];

        I::send_icmp_echo_request(
            net.context(LOCAL_CTX_NAME),
            conn,
            7,
            Buf::new(echo_body.clone(), ..),
        )
        .unwrap();

        net.run_until_idle().unwrap();

        assert_eq!(
            get_counter_val(
                net.context(ctx_name_receiving_req),
                &format!("{}::echo_request", recv_icmp_packet_name)
            ),
            1
        );
        assert_eq!(
            get_counter_val(
                net.context(LOCAL_CTX_NAME),
                &format!("{}::echo_reply", recv_icmp_packet_name)
            ),
            1
        );
        let replies = net.context(LOCAL_CTX_NAME).dispatcher.take_icmp_replies(conn);
        assert_matches::assert_matches!(&replies[..], [(7, body)] if *body == echo_body);
    }

    #[ip_test]
    fn test_local_icmp_connection<I: Ip + TestIpExt>() {
        test_icmp_connection::<I>(IcmpConnectionType::Local);
    }

    #[ip_test]
    fn test_remote_icmp_connection<I: Ip + TestIpExt>() {
        test_icmp_connection::<I>(IcmpConnectionType::Remote);
    }

    // Tests that only require an ICMP stack. Unlike the preceding tests, these
    // only test the ICMP stack and state, and mock everything else. We define
    // the `DummyIcmpv4Ctx` and `DummyIcmpv6Ctx` types, which we wrap in a
    // `DummyCtx` to provide automatic implementations of a number of required
    // traits. The rest we implement manually.

    // The arguments to `InnerIcmpContext::send_icmp_reply`.
    #[derive(Debug, PartialEq)]
    struct SendIcmpReplyArgs<A: IpAddress> {
        device: Option<DummyDeviceId>,
        src_ip: SpecifiedAddr<A>,
        dst_ip: SpecifiedAddr<A>,
        body: Vec<u8>,
    }

    // The arguments to `InnerIcmpContext::send_icmp_error_message`.
    #[derive(Debug, PartialEq)]
    struct SendIcmpErrorMessageArgs<I: IcmpIpExt> {
        frame_dst: FrameDestination,
        src_ip: SpecifiedAddr<I::Addr>,
        dst_ip: SpecifiedAddr<I::Addr>,
        body: Vec<u8>,
        ip_mtu: Option<u32>,
        info: I::ShouldSendIcmpErrorInfo,
        // Whether `should_send_icmpv{4,6}_error` returned true.
        sent: bool,
    }

    // The arguments to `BufferIcmpContext::receive_icmp_echo_reply`.
    #[allow(unused)] // TODO(joshlf): Remove once we access these fields.
    struct ReceiveIcmpEchoReply<I: Ip> {
        conn: IcmpConnId<I>,
        seq_num: u16,
        src_ip: I::Addr,
        dst_ip: I::Addr,
        id: u16,
        data: Vec<u8>,
    }

    // The arguments to `IcmpContext::close_icmp_connection`.
    #[allow(unused)] // TODO(joshlf): Remove once we access these fields.
    struct CloseIcmpConnectionArgs<I: Ip> {
        conn: IcmpConnId<I>,
        err: IpSockCreationError,
    }

    // The arguments to `IcmpContext::receive_icmp_error`.
    #[derive(Debug, PartialEq)]
    struct ReceiveIcmpSocketErrorArgs<I: IcmpIpExt> {
        conn: IcmpConnId<I>,
        seq_num: u16,
        err: I::ErrorCode,
    }

    struct DummyIcmpCtx<I: IcmpIpExt + IpDeviceStateIpExt<DummyInstant>> {
        // All calls to `InnerIcmpContext::send_icmp_error_message`.
        send_icmp_error_message: Vec<SendIcmpErrorMessageArgs<I>>,
        // We store calls to `InnerIcmpContext::receive_icmp_error` AND calls to
        // `IcmpContext::receive_icmp_error`. Any call to
        // `InnerIcmpContext::receive_icmp_error` with an IP proto of ICMP(v4|v6)
        // will be stored here and also passed to `receive_icmp_socket_error`,
        // which will in turn call `IcmpContext::receive_icmp_error`.
        receive_icmp_echo_reply: Vec<ReceiveIcmpEchoReply<I>>,
        receive_icmp_error: Vec<I::ErrorCode>,
        receive_icmp_socket_error: Vec<ReceiveIcmpSocketErrorArgs<I>>,
        close_icmp_connection: Vec<CloseIcmpConnectionArgs<I>>,
        pmtu_state: DummyPmtuState<I::Addr>,
        socket_ctx: DummyIpSocketCtx<I>,
    }

    impl Default for DummyIcmpCtx<Ipv4> {
        fn default() -> DummyIcmpCtx<Ipv4> {
            DummyIcmpCtx::new(DummyIpSocketCtx::new_ipv4(
                vec![DUMMY_CONFIG_V4.local_ip],
                vec![DUMMY_CONFIG_V4.remote_ip],
            ))
        }
    }

    impl Default for DummyIcmpCtx<Ipv6> {
        fn default() -> DummyIcmpCtx<Ipv6> {
            DummyIcmpCtx::new(DummyIpSocketCtx::new_ipv6(
                vec![DUMMY_CONFIG_V6.local_ip],
                vec![DUMMY_CONFIG_V6.remote_ip],
            ))
        }
    }

    impl<I: IcmpIpExt + IpDeviceStateIpExt<DummyInstant>> DummyIcmpCtx<I> {
        fn new(socket_ctx: DummyIpSocketCtx<I>) -> DummyIcmpCtx<I> {
            DummyIcmpCtx {
                send_icmp_error_message: Vec::new(),
                receive_icmp_echo_reply: Vec::new(),
                receive_icmp_error: Vec::new(),
                receive_icmp_socket_error: Vec::new(),
                close_icmp_connection: Vec::new(),
                pmtu_state: DummyPmtuState::default(),
                socket_ctx,
            }
        }
    }

    struct DummyIcmpv4Ctx {
        inner: DummyIcmpCtx<Ipv4>,
        icmp_state: Icmpv4State<DummyInstant, IpSock<Ipv4, DummyDeviceId>>,
    }

    struct DummyIcmpv6Ctx {
        inner: DummyIcmpCtx<Ipv6>,
        icmp_state: Icmpv6State<DummyInstant, IpSock<Ipv6, DummyDeviceId>>,
    }

    impl Default for DummyIcmpv4Ctx {
        fn default() -> DummyIcmpv4Ctx {
            DummyIcmpv4Ctx {
                inner: DummyIcmpCtx::default(),
                icmp_state: Icmpv4StateBuilder::default().build(),
            }
        }
    }

    impl Default for DummyIcmpv6Ctx {
        fn default() -> DummyIcmpv6Ctx {
            DummyIcmpv6Ctx {
                inner: DummyIcmpCtx::default(),
                icmp_state: Icmpv6StateBuilder::default().build(),
            }
        }
    }

    /// Implement a number of traits and methods for the `$inner` and `$outer`
    /// context types.
    macro_rules! impl_context_traits {
        ($ip:ident, $inner:ident, $outer:ident, $state:ident, $info_type:ident, $should_send:expr) => {
            type $outer = DummyCtx<$inner, (), IpFrameMeta<<$ip as Ip>::Addr, DummyDeviceId>>;

            impl $inner {
                fn with_errors_per_second(errors_per_second: u64) -> $inner {
                    let mut ctx = $inner::default();
                    ctx.icmp_state.inner.error_send_bucket = TokenBucket::new(errors_per_second);
                    ctx
                }
            }

            impl_pmtu_handler!($outer, $ip);

            impl AsMut<DummyPmtuState<<$ip as Ip>::Addr>> for $outer {
                fn as_mut(&mut self) -> &mut DummyPmtuState<<$ip as Ip>::Addr> {
                    &mut self.get_mut().inner.pmtu_state
                }
            }

            impl AsRef<DummyIpSocketCtx<$ip>> for $inner {
                fn as_ref(&self) -> &DummyIpSocketCtx<$ip> {
                    &self.inner.socket_ctx
                }
            }

            impl AsMut<DummyIpSocketCtx<$ip>> for $inner {
                fn as_mut(&mut self) -> &mut DummyIpSocketCtx<$ip> {
                    &mut self.inner.socket_ctx
                }
            }

            impl StateContext<$state<DummyInstant, IpSock<$ip, DummyDeviceId>>> for $outer {
                fn get_state_with(
                    &self,
                    _id: (),
                ) -> &$state<DummyInstant, IpSock<$ip, DummyDeviceId>> {
                    &self.get_ref().icmp_state
                }

                fn get_state_mut_with(
                    &mut self,
                    _id: (),
                ) -> &mut $state<DummyInstant, IpSock<$ip, DummyDeviceId>> {
                    &mut self.get_mut().icmp_state
                }
            }

            impl IcmpContext<$ip> for $outer {
                fn receive_icmp_error(
                    &mut self,
                    conn: IcmpConnId<$ip>,
                    seq_num: u16,
                    err: <$ip as IcmpIpExt>::ErrorCode,
                ) {
                    self.increment_counter("IcmpContext::receive_icmp_error");
                    self.get_mut()
                        .inner
                        .receive_icmp_socket_error
                        .push(ReceiveIcmpSocketErrorArgs { conn, seq_num, err });
                }

                fn close_icmp_connection(
                    &mut self,
                    conn: IcmpConnId<$ip>,
                    err: IpSockCreationError,
                ) {
                    self.get_mut()
                        .inner
                        .close_icmp_connection
                        .push(CloseIcmpConnectionArgs { conn, err });
                }
            }

            impl<B: BufferMut> BufferIcmpContext<$ip, B> for $outer {
                fn receive_icmp_echo_reply(
                    &mut self,
                    conn: IcmpConnId<$ip>,
                    src_ip: <$ip as Ip>::Addr,
                    dst_ip: <$ip as Ip>::Addr,
                    id: u16,
                    seq_num: u16,
                    data: B,
                ) {
                    self.get_mut().inner.receive_icmp_echo_reply.push(ReceiveIcmpEchoReply {
                        conn,
                        src_ip,
                        dst_ip,
                        id,
                        seq_num,
                        data: data.as_ref().to_vec(),
                    });
                }
            }

            impl InnerIcmpContext<$ip> for $outer {
                fn receive_icmp_error(
                    &mut self,
                    original_src_ip: Option<SpecifiedAddr<<$ip as Ip>::Addr>>,
                    original_dst_ip: SpecifiedAddr<<$ip as Ip>::Addr>,
                    original_proto: <$ip as packet_formats::ip::IpExt>::Proto,
                    original_body: &[u8],
                    err: <$ip as IcmpIpExt>::ErrorCode,
                ) {
                    self.increment_counter("InnerIcmpContext::receive_icmp_error");
                    self.get_mut().inner.receive_icmp_error.push(err);
                    if original_proto == <$ip as packet_formats::icmp::IcmpIpExt>::ICMP_IP_PROTO {
                        <IcmpIpTransportContext as IpTransportContext<$ip, _>>::receive_icmp_error(
                            self,
                            original_src_ip,
                            original_dst_ip,
                            original_body,
                            err,
                        );
                    }
                }

                fn get_state_and_update_meta(
                    &mut self,
                ) -> (
                    &mut IcmpState<<$ip as Ip>::Addr, DummyInstant, IpSock<$ip, DummyDeviceId>>,
                    &ForwardingTable<$ip, DummyDeviceId>,
                ) {
                    let state = self.get_mut();
                    (&mut state.icmp_state.inner, &state.inner.socket_ctx.table)
                }
            }

            impl<B: BufferMut> InnerBufferIcmpContext<$ip, B> for $outer {
                fn send_icmp_error_message<
                    S: Serializer<Buffer = B>,
                    F: FnOnce(SpecifiedAddr<<$ip as Ip>::Addr>) -> S,
                >(
                    &mut self,
                    _device: DummyDeviceId,
                    frame_dst: FrameDestination,
                    src_ip: SpecifiedAddr<<$ip as Ip>::Addr>,
                    dst_ip: SpecifiedAddr<<$ip as Ip>::Addr>,
                    get_body: F,
                    ip_mtu: Option<u32>,
                    info: $info_type,
                ) -> Result<(), S> {
                    self.increment_counter("InnerIcmpContext::send_icmp_error_message");
                    let sent = $should_send(frame_dst, src_ip, dst_ip, info);
                    self.get_mut().inner.send_icmp_error_message.push(SendIcmpErrorMessageArgs {
                        frame_dst,
                        src_ip,
                        dst_ip,
                        // Use `unwrap_or_else` like this rather than `unwrap`
                        // because the error variant of the return value of
                        // `serialize_vec_outer` doesn't implement `Debug`,
                        // which is required for `unwrap`.
                        body: get_body(dst_ip)
                            .serialize_vec_outer()
                            .unwrap_or_else(|_| panic!("failed to serialize body"))
                            .as_ref()
                            .to_vec(),
                        ip_mtu,
                        info,
                        sent,
                    });
                    Ok(())
                }
            }
        };
    }

    impl_context_traits!(
        Ipv4,
        DummyIcmpv4Ctx,
        Dummyv4Ctx,
        Icmpv4State,
        ShouldSendIcmpv4ErrorInfo,
        |f, s, d, i| { should_send_icmpv4_error(f, s, d, i) }
    );
    impl_context_traits!(
        Ipv6,
        DummyIcmpv6Ctx,
        Dummyv6Ctx,
        Icmpv6State,
        ShouldSendIcmpv6ErrorInfo,
        |f, s, d, i| { should_send_icmpv6_error(f, s, d, i) }
    );

    impl NdpPacketHandler<DummyDeviceId> for Dummyv6Ctx {
        fn receive_ndp_packet<B: ByteSlice>(
            &mut self,
            _device: DummyDeviceId,
            _src_ip: Ipv6SourceAddr,
            _dst_ip: SpecifiedAddr<Ipv6Addr>,
            _packet: NdpPacket<B>,
        ) {
            unimplemented!()
        }
    }

    impl MldPacketHandler<DummyDeviceId> for Dummyv6Ctx {
        fn receive_mld_packet<B: ByteSlice>(
            &mut self,
            _device: DummyDeviceId,
            _src_ip: Ipv6SourceAddr,
            _dst_ip: SpecifiedAddr<Ipv6Addr>,
            _packet: MldPacket<B>,
        ) {
            unimplemented!()
        }
    }

    #[test]
    fn test_receive_icmpv4_error() {
        // Chosen arbitrarily to be a) non-zero (it's easy to accidentally get
        // the value 0) and, b) different from each other.
        const ICMP_ID: u16 = 0x0F;
        const SEQ_NUM: u16 = 0xF0;

        /// Test receiving an ICMP error message.
        ///
        /// Test that receiving an ICMP error message with the given code and
        /// message contents, and containing the given original IPv4 packet,
        /// results in the counter values in `assert_counters`. After that
        /// assertion passes, `f` is called on the context so that the caller
        /// can perform whatever extra validation they want.
        ///
        /// The error message will be sent from `DUMMY_CONFIG_V4.remote_ip` to
        /// `DUMMY_CONFIG_V4.local_ip`. Before the message is sent, an ICMP
        /// socket will be established with the ID `ICMP_ID`, and
        /// `test_receive_icmpv4_error_helper` will assert that its `IcmpConnId`
        /// is 0. This allows the caller to craft the `original_packet` so that
        /// it should be delivered to this socket.
        fn test_receive_icmpv4_error_helper<
            C: Debug,
            M: for<'a> IcmpMessage<Ipv4, &'a [u8], Code = C> + Debug,
            F: Fn(&Dummyv4Ctx),
        >(
            original_packet: &mut [u8],
            code: C,
            msg: M,
            assert_counters: &[(&str, usize)],
            f: F,
        ) {
            crate::testutil::set_logger_for_test();

            let mut ctx = Dummyv4Ctx::default();
            // NOTE: This assertion is not a correctness requirement. It's just
            // that the rest of this test assumes that the new connection has ID
            // 0. If this assertion fails in the future, that isn't necessarily
            // evidence of a bug; we may just have to update this test to
            // accommodate whatever new ID allocation scheme is being used.
            assert_eq!(
                new_icmpv4_connection_inner(
                    &mut ctx,
                    Some(DUMMY_CONFIG_V4.local_ip),
                    DUMMY_CONFIG_V4.remote_ip,
                    ICMP_ID
                )
                .unwrap(),
                IcmpConnId::new(0)
            );

            <IcmpIpTransportContext as BufferIpTransportContext<Ipv4, _, _>>::receive_ip_packet(
                &mut ctx,
                Some(DummyDeviceId),
                DUMMY_CONFIG_V4.remote_ip.get(),
                DUMMY_CONFIG_V4.local_ip,
                Buf::new(original_packet, ..)
                    .encapsulate(IcmpPacketBuilder::new(
                        DUMMY_CONFIG_V4.remote_ip,
                        DUMMY_CONFIG_V4.local_ip,
                        code,
                        msg,
                    ))
                    .serialize_vec_outer()
                    .unwrap(),
            )
            .unwrap();

            for (ctr, count) in assert_counters {
                assert_eq!(ctx.get_counter(ctr), *count, "wrong count for counter {}", ctr);
            }
            f(&ctx);
        }
        // Test that, when we receive various ICMPv4 error messages, we properly
        // pass them up to the IP layer and, sometimes, to the transport layer.

        // First, test with an original packet containing an ICMP message. Since
        // this test mock supports ICMP sockets, this error can be delivered all
        // the way up the stack.

        // A buffer containing an ICMP echo request with ID `ICMP_ID` and
        // sequence number `SEQ_NUM` from the local IP to the remote IP. Any
        // ICMP error message which contains this as its original packet should
        // be delivered to the socket created in
        // `test_receive_icmpv4_error_helper`.
        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                DUMMY_CONFIG_V4.local_ip,
                DUMMY_CONFIG_V4.remote_ip,
                IcmpUnusedCode,
                IcmpEchoRequest::new(ICMP_ID, SEQ_NUM),
            ))
            .encapsulate(<Ipv4 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V4.local_ip,
                DUMMY_CONFIG_V4.remote_ip,
                64,
                Ipv4Proto::Icmp,
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4DestUnreachableCode::DestNetworkUnreachable,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::DestUnreachable(
                    Icmpv4DestUnreachableCode::DestNetworkUnreachable,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4TimeExceededCode::TtlExpired,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::TimeExceeded(Icmpv4TimeExceededCode::TtlExpired);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4ParameterProblemCode::PointerIndicatesError,
            Icmpv4ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::ParameterProblem(
                    Icmpv4ParameterProblemCode::PointerIndicatesError,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        // Second, test with an original packet containing a malformed ICMP
        // packet (we accomplish this by leaving the IP packet's body empty). We
        // should process this packet in
        // `IcmpIpTransportContext::receive_icmp_error`, but we should go no
        // further - in particular, we should not call
        // `IcmpContext::receive_icmp_error`.

        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(<Ipv4 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V4.local_ip,
                DUMMY_CONFIG_V4.remote_ip,
                64,
                Ipv4Proto::Icmp,
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4DestUnreachableCode::DestNetworkUnreachable,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::DestUnreachable(
                    Icmpv4DestUnreachableCode::DestNetworkUnreachable,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4TimeExceededCode::TtlExpired,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::TimeExceeded(Icmpv4TimeExceededCode::TtlExpired);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4ParameterProblemCode::PointerIndicatesError,
            Icmpv4ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::ParameterProblem(
                    Icmpv4ParameterProblemCode::PointerIndicatesError,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        // Third, test with an original packet containing a UDP packet. This
        // allows us to verify that protocol numbers are handled properly by
        // checking that `IcmpIpTransportContext::receive_icmp_error` was NOT
        // called.

        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(<Ipv4 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V4.local_ip,
                DUMMY_CONFIG_V4.remote_ip,
                64,
                IpProto::Udp.into(),
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4DestUnreachableCode::DestNetworkUnreachable,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::DestUnreachable(
                    Icmpv4DestUnreachableCode::DestNetworkUnreachable,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4TimeExceededCode::TtlExpired,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::TimeExceeded(Icmpv4TimeExceededCode::TtlExpired);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv4_error_helper(
            buffer.as_mut(),
            Icmpv4ParameterProblemCode::PointerIndicatesError,
            Icmpv4ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv4ErrorCode::ParameterProblem(
                    Icmpv4ParameterProblemCode::PointerIndicatesError,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );
    }

    #[test]
    fn test_receive_icmpv6_error() {
        // Chosen arbitrarily to be a) non-zero (it's easy to accidentally get
        // the value 0) and, b) different from each other.
        const ICMP_ID: u16 = 0x0F;
        const SEQ_NUM: u16 = 0xF0;

        /// Test receiving an ICMPv6 error message.
        ///
        /// Test that receiving an ICMP error message with the given code and
        /// message contents, and containing the given original IPv4 packet,
        /// results in the counter values in `assert_counters`. After that
        /// assertion passes, `f` is called on the context so that the caller
        /// can perform whatever extra validation they want.
        ///
        /// The error message will be sent from `DUMMY_CONFIG_V6.remote_ip` to
        /// `DUMMY_CONFIG_V6.local_ip`. Before the message is sent, an ICMP
        /// socket will be established with the ID `ICMP_ID`, and
        /// `test_receive_icmpv6_error_helper` will assert that its `IcmpConnId`
        /// is 0. This allows the caller to craft the `original_packet` so that
        /// it should be delivered to this socket.
        fn test_receive_icmpv6_error_helper<
            C: Debug,
            M: for<'a> IcmpMessage<Ipv6, &'a [u8], Code = C> + Debug,
            F: Fn(&Dummyv6Ctx),
        >(
            original_packet: &mut [u8],
            code: C,
            msg: M,
            assert_counters: &[(&str, usize)],
            f: F,
        ) {
            crate::testutil::set_logger_for_test();

            let mut ctx = Dummyv6Ctx::default();
            // NOTE: This assertion is not a correctness requirement. It's just
            // that the rest of this test assumes that the new connection has ID
            // 0. If this assertion fails in the future, that isn't necessarily
            // evidence of a bug; we may just have to update this test to
            // accommodate whatever new ID allocation scheme is being used.
            assert_eq!(
                new_icmpv6_connection_inner(
                    &mut ctx,
                    Some(DUMMY_CONFIG_V6.local_ip),
                    DUMMY_CONFIG_V6.remote_ip,
                    ICMP_ID
                )
                .unwrap(),
                IcmpConnId::new(0)
            );

            <IcmpIpTransportContext as BufferIpTransportContext<Ipv6, _, _>>::receive_ip_packet(
                &mut ctx,
                Some(DummyDeviceId),
                DUMMY_CONFIG_V6.remote_ip.get().try_into().unwrap(),
                DUMMY_CONFIG_V6.local_ip,
                Buf::new(original_packet, ..)
                    .encapsulate(IcmpPacketBuilder::new(
                        DUMMY_CONFIG_V6.remote_ip,
                        DUMMY_CONFIG_V6.local_ip,
                        code,
                        msg,
                    ))
                    .serialize_vec_outer()
                    .unwrap(),
            )
            .unwrap();

            for (ctr, count) in assert_counters {
                assert_eq!(ctx.get_counter(ctr), *count, "wrong count for counter {}", ctr);
            }
            f(&ctx);
        }
        // Test that, when we receive various ICMPv6 error messages, we properly
        // pass them up to the IP layer and, sometimes, to the transport layer.

        // First, test with an original packet containing an ICMPv6 message.
        // Since this test mock supports ICMPv6 sockets, this error can be
        // delivered all the way up the stack.

        // A buffer containing an ICMPv6 echo request with ID `ICMP_ID` and
        // sequence number `SEQ_NUM` from the local IP to the remote IP. Any
        // ICMPv6 error message which contains this as its original packet
        // should be delivered to the socket created in
        // `test_receive_icmpv6_error_helper`.
        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(IcmpPacketBuilder::<Ipv6, &[u8], _>::new(
                DUMMY_CONFIG_V6.local_ip,
                DUMMY_CONFIG_V6.remote_ip,
                IcmpUnusedCode,
                IcmpEchoRequest::new(ICMP_ID, SEQ_NUM),
            ))
            .encapsulate(<Ipv6 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V6.local_ip,
                DUMMY_CONFIG_V6.remote_ip,
                64,
                Ipv6Proto::Icmpv6,
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6DestUnreachableCode::NoRoute,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::DestUnreachable(Icmpv6DestUnreachableCode::NoRoute);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6TimeExceededCode::HopLimitExceeded,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::TimeExceeded(Icmpv6TimeExceededCode::HopLimitExceeded);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
            Icmpv6ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 1),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::ParameterProblem(
                    Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(
                    ctx.get_ref().inner.receive_icmp_socket_error,
                    [ReceiveIcmpSocketErrorArgs {
                        conn: IcmpConnId::new(0),
                        seq_num: SEQ_NUM,
                        err
                    }]
                );
            },
        );

        // Second, test with an original packet containing a malformed ICMPv6
        // packet (we accomplish this by leaving the IP packet's body empty). We
        // should process this packet in
        // `IcmpIpTransportContext::receive_icmp_error`, but we should go no
        // further - in particular, we should not call
        // `IcmpContext::receive_icmp_error`.

        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(<Ipv6 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V6.local_ip,
                DUMMY_CONFIG_V6.remote_ip,
                64,
                Ipv6Proto::Icmpv6,
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6DestUnreachableCode::NoRoute,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::DestUnreachable(Icmpv6DestUnreachableCode::NoRoute);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6TimeExceededCode::HopLimitExceeded,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::TimeExceeded(Icmpv6TimeExceededCode::HopLimitExceeded);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
            Icmpv6ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 1),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::ParameterProblem(
                    Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        // Third, test with an original packet containing a UDP packet. This
        // allows us to verify that protocol numbers are handled properly by
        // checking that `IcmpIpTransportContext::receive_icmp_error` was NOT
        // called.

        let mut buffer = Buf::new(&mut [], ..)
            .encapsulate(<Ipv6 as packet_formats::ip::IpExt>::PacketBuilder::new(
                DUMMY_CONFIG_V6.local_ip,
                DUMMY_CONFIG_V6.remote_ip,
                64,
                IpProto::Udp.into(),
            ))
            .serialize_vec_outer()
            .unwrap();

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6DestUnreachableCode::NoRoute,
            IcmpDestUnreachable::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::DestUnreachable(Icmpv6DestUnreachableCode::NoRoute);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6TimeExceededCode::HopLimitExceeded,
            IcmpTimeExceeded::default(),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::TimeExceeded(Icmpv6TimeExceededCode::HopLimitExceeded);
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );

        test_receive_icmpv6_error_helper(
            buffer.as_mut(),
            Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
            Icmpv6ParameterProblem::new(0),
            &[
                ("InnerIcmpContext::receive_icmp_error", 1),
                ("IcmpIpTransportContext::receive_icmp_error", 0),
                ("IcmpContext::receive_icmp_error", 0),
            ],
            |ctx| {
                let err = Icmpv6ErrorCode::ParameterProblem(
                    Icmpv6ParameterProblemCode::UnrecognizedNextHeaderType,
                );
                assert_eq!(ctx.get_ref().inner.receive_icmp_error, [err]);
                assert_eq!(ctx.get_ref().inner.receive_icmp_socket_error, []);
            },
        );
    }

    #[test]
    fn test_error_rate_limit() {
        crate::testutil::set_logger_for_test();

        /// Call `send_icmpv4_ttl_expired` with dummy values.
        fn send_icmpv4_ttl_expired_helper(ctx: &mut Dummyv4Ctx) {
            send_icmpv4_ttl_expired(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                DUMMY_CONFIG_V4.remote_ip,
                DUMMY_CONFIG_V4.local_ip,
                IpProto::Udp.into(),
                Buf::new(&mut [], ..),
                0,
                Ipv4FragmentType::InitialFragment,
            );
        }

        /// Call `send_icmpv4_parameter_problem` with dummy values.
        fn send_icmpv4_parameter_problem_helper(ctx: &mut Dummyv4Ctx) {
            send_icmpv4_parameter_problem(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                DUMMY_CONFIG_V4.remote_ip,
                DUMMY_CONFIG_V4.local_ip,
                Icmpv4ParameterProblemCode::PointerIndicatesError,
                Icmpv4ParameterProblem::new(0),
                Buf::new(&mut [], ..),
                0,
                Ipv4FragmentType::InitialFragment,
            );
        }

        /// Call `send_icmpv4_dest_unreachable` with dummy values.
        fn send_icmpv4_dest_unreachable_helper(ctx: &mut Dummyv4Ctx) {
            send_icmpv4_dest_unreachable(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                DUMMY_CONFIG_V4.remote_ip,
                DUMMY_CONFIG_V4.local_ip,
                Icmpv4DestUnreachableCode::DestNetworkUnreachable,
                Buf::new(&mut [], ..),
                0,
                Ipv4FragmentType::InitialFragment,
            );
        }

        /// Call `send_icmpv6_ttl_expired` with dummy values.
        fn send_icmpv6_ttl_expired_helper(ctx: &mut Dummyv6Ctx) {
            send_icmpv6_ttl_expired(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                UnicastAddr::from_witness(DUMMY_CONFIG_V6.remote_ip).unwrap(),
                DUMMY_CONFIG_V6.local_ip,
                IpProto::Udp.into(),
                Buf::new(&mut [], ..),
                0,
            );
        }

        /// Call `send_icmpv6_packet_too_big` with dummy values.
        fn send_icmpv6_packet_too_big_helper(ctx: &mut Dummyv6Ctx) {
            send_icmpv6_packet_too_big(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                UnicastAddr::from_witness(DUMMY_CONFIG_V6.remote_ip).unwrap(),
                DUMMY_CONFIG_V6.local_ip,
                IpProto::Udp.into(),
                0,
                Buf::new(&mut [], ..),
                0,
            );
        }

        /// Call `send_icmpv6_parameter_problem` with dummy values.
        fn send_icmpv6_parameter_problem_helper(ctx: &mut Dummyv6Ctx) {
            send_icmpv6_parameter_problem(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                UnicastAddr::new(*DUMMY_CONFIG_V6.remote_ip).expect("unicast source address"),
                DUMMY_CONFIG_V6.local_ip,
                Icmpv6ParameterProblemCode::ErroneousHeaderField,
                Icmpv6ParameterProblem::new(0),
                Buf::new(&mut [], ..),
                false,
            );
        }

        /// Call `send_icmpv6_dest_unreachable` with dummy values.
        fn send_icmpv6_dest_unreachable_helper(ctx: &mut Dummyv6Ctx) {
            send_icmpv6_dest_unreachable(
                ctx,
                DummyDeviceId,
                FrameDestination::Unicast,
                UnicastAddr::from_witness(DUMMY_CONFIG_V6.remote_ip).unwrap(),
                DUMMY_CONFIG_V6.local_ip,
                Icmpv6DestUnreachableCode::NoRoute,
                Buf::new(&mut [], ..),
            );
        }

        // Run tests for each function that sends error messages to make sure
        // they're all properly rate limited.

        fn run_test<
            I: IpExt,
            C,
            W: Fn(u64) -> DummyCtx<C, (), IpFrameMeta<I::Addr, DummyDeviceId>>,
            S: Fn(&mut DummyCtx<C, (), IpFrameMeta<I::Addr, DummyDeviceId>>),
        >(
            with_errors_per_second: W,
            send: S,
        ) {
            // Note that we could theoretically have more precise tests here
            // (e.g., a test that we send at the correct rate over the long
            // term), but those would amount to testing the `TokenBucket`
            // implementation, which has its own exhaustive tests. Instead, we
            // just have a few sanity checks to make sure that we're actually
            // invoking it when we expect to (as opposed to bypassing it
            // entirely or something).

            // Test that, if no time has elapsed, we can successfully send up to
            // `ERRORS_PER_SECOND` error messages, but no more.

            // Don't use `DEFAULT_ERRORS_PER_SECOND` because it's 2^16 and it
            // makes this test take a long time.
            const ERRORS_PER_SECOND: u64 = 64;

            let mut ctx = with_errors_per_second(ERRORS_PER_SECOND);

            for i in 0..ERRORS_PER_SECOND {
                send(&mut ctx);
                assert_eq!(
                    ctx.get_counter("InnerIcmpContext::send_icmp_error_message"),
                    i as usize + 1
                );
            }

            assert_eq!(
                ctx.get_counter("InnerIcmpContext::send_icmp_error_message"),
                ERRORS_PER_SECOND as usize
            );
            send(&mut ctx);
            assert_eq!(
                ctx.get_counter("InnerIcmpContext::send_icmp_error_message"),
                ERRORS_PER_SECOND as usize
            );

            // Test that, if we set a rate of 0, we are not able to send any
            // error messages regardless of how much time has elapsed.

            let mut ctx = with_errors_per_second(0);
            send(&mut ctx);
            assert_eq!(ctx.get_counter("InnerIcmpContext::send_icmp_error_message"), 0);
            ctx.sleep_skip_timers(Duration::from_secs(1));
            send(&mut ctx);
            assert_eq!(ctx.get_counter("InnerIcmpContext::send_icmp_error_message"), 0);
            ctx.sleep_skip_timers(Duration::from_secs(1));
            send(&mut ctx);
            assert_eq!(ctx.get_counter("InnerIcmpContext::send_icmp_error_message"), 0);
        }

        fn with_errors_per_second_v4(errors_per_second: u64) -> Dummyv4Ctx {
            Dummyv4Ctx::with_state(DummyIcmpv4Ctx::with_errors_per_second(errors_per_second))
        }
        run_test::<Ipv4, _, _, _>(with_errors_per_second_v4, send_icmpv4_ttl_expired_helper);
        run_test::<Ipv4, _, _, _>(with_errors_per_second_v4, send_icmpv4_parameter_problem_helper);
        run_test::<Ipv4, _, _, _>(with_errors_per_second_v4, send_icmpv4_dest_unreachable_helper);

        fn with_errors_per_second_v6(errors_per_second: u64) -> Dummyv6Ctx {
            Dummyv6Ctx::with_state(DummyIcmpv6Ctx::with_errors_per_second(errors_per_second))
        }

        run_test::<Ipv6, _, _, _>(with_errors_per_second_v6, send_icmpv6_ttl_expired_helper);
        run_test::<Ipv6, _, _, _>(with_errors_per_second_v6, send_icmpv6_packet_too_big_helper);
        run_test::<Ipv6, _, _, _>(with_errors_per_second_v6, send_icmpv6_parameter_problem_helper);
        run_test::<Ipv6, _, _, _>(with_errors_per_second_v6, send_icmpv6_dest_unreachable_helper);
    }
}
