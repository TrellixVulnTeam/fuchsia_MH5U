// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! The User Datagram Protocol (UDP).

use alloc::collections::HashSet;
use core::{
    mem,
    num::{NonZeroU16, NonZeroUsize},
    ops::RangeInclusive,
};

use log::trace;
use net_types::{
    ip::{Ip, IpAddress, IpVersionMarker, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr},
    SpecifiedAddr, Witness,
};
use nonzero_ext::nonzero;
use packet::{BufferMut, ParsablePacket, ParseBuffer, Serializer};
use packet_formats::{
    error::ParseError,
    ip::IpProto,
    udp::{UdpPacket, UdpPacketBuilder, UdpPacketRaw, UdpParseArgs},
};
use specialize_ip_macro::specialize_ip;
use thiserror::Error;

use crate::{
    algorithm::{PortAlloc, PortAllocImpl, ProtocolFlowId},
    context::{CounterContext, DualStateContext, RngStateContext, RngStateContextExt},
    data_structures::IdMapCollectionKey,
    device::DeviceId,
    error::LocalAddressError,
    ip::{
        icmp::{IcmpIpExt, Icmpv4ErrorCode, Icmpv6ErrorCode},
        socket::{IpSock, IpSockCreationError, IpSockSendError, IpSocket, UnroutableBehavior},
        BufferIpTransportContext, BufferTransportIpContext, IpDeviceIdContext, IpExt,
        IpTransportContext, TransportIpContext, TransportReceiveError,
    },
    socket::{ConnSocketEntry, ConnSocketMap, ListenerSocketMap},
    BufferDispatcher, Ctx, EventDispatcher,
};

/// A builder for UDP layer state.
#[derive(Clone)]
pub struct UdpStateBuilder {
    send_port_unreachable: bool,
}

impl Default for UdpStateBuilder {
    fn default() -> UdpStateBuilder {
        UdpStateBuilder { send_port_unreachable: false }
    }
}

impl UdpStateBuilder {
    /// Enable or disable sending ICMP Port Unreachable messages in response to
    /// inbound UDP packets for which a corresponding local socket does not
    /// exist (default: disabled).
    ///
    /// Responding with an ICMP Port Unreachable error is a vector for reflected
    /// Denial-of-Service (DoS) attacks. The attacker can send a UDP packet to a
    /// closed port with the source address set to the address of the victim,
    /// and ICMP response will be sent there.
    ///
    /// According to [RFC 1122 Section 4.1.3.1], "\[i\]f a datagram arrives
    /// addressed to a UDP port for which there is no pending LISTEN call, UDP
    /// SHOULD send an ICMP Port Unreachable message." Since an ICMP response is
    /// not mandatory, and due to the security risks described, responses are
    /// disabled by default.
    ///
    /// [RFC 1122 Section 4.1.3.1]: https://tools.ietf.org/html/rfc1122#section-4.1.3.1
    pub fn send_port_unreachable(&mut self, send_port_unreachable: bool) -> &mut Self {
        self.send_port_unreachable = send_port_unreachable;
        self
    }

    pub(crate) fn build<I: IpExt, D>(self) -> UdpState<I, D> {
        UdpState {
            conn_state: UdpConnectionState::default(),
            lazy_port_alloc: None,
            send_port_unreachable: self.send_port_unreachable,
        }
    }
}

/// The state associated with the UDP protocol.
///
/// `D` is the device ID type.
pub struct UdpState<I: IpExt, D> {
    conn_state: UdpConnectionState<I, IpSock<I, D>>,
    /// lazy_port_alloc is lazy-initialized when it's used.
    lazy_port_alloc: Option<PortAlloc<UdpConnectionState<I, IpSock<I, D>>>>,
    send_port_unreachable: bool,
}

impl<I: IpExt, D> Default for UdpState<I, D> {
    fn default() -> UdpState<I, D> {
        UdpStateBuilder::default().build()
    }
}

/// Holder structure that keeps all the connection maps for UDP connections.
///
/// `UdpConnectionState` provides a [`PortAllocImpl`] implementation to
/// allocate unused local ports.
struct UdpConnectionState<I: Ip, S> {
    conns: ConnSocketMap<ConnAddr<I::Addr>, S>,
    listeners: ListenerSocketMap<ListenerAddr<I::Addr>>,
    wildcard_listeners: ListenerSocketMap<NonZeroU16>,
}

impl<I: Ip, S> Default for UdpConnectionState<I, S> {
    fn default() -> UdpConnectionState<I, S> {
        UdpConnectionState {
            conns: ConnSocketMap::default(),
            listeners: ListenerSocketMap::default(),
            wildcard_listeners: ListenerSocketMap::default(),
        }
    }
}

enum LookupResult<I: Ip> {
    Conn(UdpConnId<I>, ConnAddr<I::Addr>),
    Listener(UdpListenerId<I>, ListenerAddr<I::Addr>),
    WildcardListener(UdpListenerId<I>, NonZeroU16),
}

impl<I: Ip, S> UdpConnectionState<I, S> {
    fn lookup(
        &self,
        local_ip: SpecifiedAddr<I::Addr>,
        remote_ip: SpecifiedAddr<I::Addr>,
        local_port: NonZeroU16,
        remote_port: NonZeroU16,
    ) -> Option<LookupResult<I>> {
        let addr = ConnAddr { local_ip, remote_ip, local_port, remote_port };
        self.conns
            .get_id_by_addr(&addr)
            .map(move |id| LookupResult::Conn(UdpConnId::new(id), addr))
            .or_else(|| {
                let listener = ListenerAddr { addr: local_ip, port: local_port };
                self.listeners.get_by_addr(&listener).map(move |id| {
                    LookupResult::Listener(UdpListenerId::new_specified(id), listener)
                })
            })
            .or_else(|| {
                self.wildcard_listeners.get_by_addr(&local_port).map(move |id| {
                    LookupResult::WildcardListener(UdpListenerId::new_wildcard(id), local_port)
                })
            })
    }

    /// Collects the currently used local ports into a [`HashSet`].
    ///
    /// If `addrs` is empty, `collect_used_local_ports` returns all the local
    /// ports currently in use, otherwise it returns all the local ports in use
    /// for the addresses in `addrs`.
    fn collect_used_local_ports<'a>(
        &self,
        addrs: impl ExactSizeIterator<Item = &'a SpecifiedAddr<I::Addr>> + Clone,
    ) -> HashSet<NonZeroU16> {
        let mut ports = HashSet::new();
        ports.extend(self.wildcard_listeners.iter_addrs());
        if addrs.len() == 0 {
            // For wildcard addresses, collect ALL local ports.
            ports.extend(self.listeners.iter_addrs().map(|l| l.port));
            ports.extend(self.conns.iter_addrs().map(|c| c.local_port));
        } else {
            // If `addrs` is not empty, just collect the ones that use the same
            // local addresses.
            ports.extend(self.listeners.iter_addrs().filter_map(|l| {
                if addrs.clone().any(|a| a == &l.addr) {
                    Some(l.port)
                } else {
                    None
                }
            }));
            ports.extend(self.conns.iter_addrs().filter_map(|c| {
                if addrs.clone().any(|a| a == &c.local_ip) {
                    Some(c.local_port)
                } else {
                    None
                }
            }));
        }
        ports
    }

    /// Checks whether the provided port is available to be used for a listener.
    ///
    /// If `addr` is `None`, `is_listen_port_available` will only return `true`
    /// if *no* connections or listeners bound to any addresses are using the
    /// provided `port`.
    fn is_listen_port_available(
        &self,
        addr: Option<SpecifiedAddr<I::Addr>>,
        port: NonZeroU16,
    ) -> bool {
        self.wildcard_listeners.get_by_addr(&port).is_none()
            && addr
                .map(|addr| {
                    self.listeners.get_by_addr(&ListenerAddr { addr, port }).is_none()
                        && !self
                            .conns
                            .iter_addrs()
                            .any(|c| c.local_ip == addr && c.local_port == port)
                })
                .unwrap_or_else(|| {
                    !(self.listeners.iter_addrs().any(|l| l.port == port)
                        || self.conns.iter_addrs().any(|c| c.local_port == port))
                })
    }
}

/// Helper function to allocate a local port.
///
/// Attempts to allocate a new unused local port with the given flow identifier
/// `id`.
fn try_alloc_local_port<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    id: &ProtocolFlowId<I::Addr>,
) -> Option<NonZeroU16> {
    let (state, rng) = ctx.get_state_rng();
    // Lazily init port_alloc if it hasn't been inited yet.
    let port_alloc = state.lazy_port_alloc.get_or_insert_with(|| PortAlloc::new(rng));
    port_alloc.try_alloc(&id, &state.conn_state).and_then(NonZeroU16::new)
}

/// Helper function to allocate a listen port.
///
/// Finds a random ephemeral port that is not in the provided `used_ports` set.
fn try_alloc_listen_port<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    used_ports: &HashSet<NonZeroU16>,
) -> Option<NonZeroU16> {
    let mut port = UdpConnectionState::<I, IpSock<I, C::DeviceId>>::rand_ephemeral(ctx.rng_mut());
    for _ in UdpConnectionState::<I, IpSock<I, C::DeviceId>>::EPHEMERAL_RANGE {
        // We can unwrap here because we know that the EPHEMERAL_RANGE doesn't
        // include 0.
        let tryport = NonZeroU16::new(port.get()).unwrap();
        if !used_ports.contains(&tryport) {
            return Some(tryport);
        }
        port.next();
    }
    None
}

impl<I: Ip, S> PortAllocImpl for UdpConnectionState<I, S> {
    const TABLE_SIZE: NonZeroUsize = nonzero!(20usize);
    const EPHEMERAL_RANGE: RangeInclusive<u16> = 49152..=65535;
    type Id = ProtocolFlowId<I::Addr>;

    fn is_port_available(&self, id: &Self::Id, port: u16) -> bool {
        // We can safely unwrap here, because the ports received in
        // `is_port_available` are guaranteed to be in `EPHEMERAL_RANGE`.
        let port = NonZeroU16::new(port).unwrap();
        // Check if we have any listeners. Return true if we have no listeners
        // or active connections using the selected local port.
        self.listeners.get_by_addr(&ListenerAddr { addr: *id.local_addr(), port }).is_none()
            && self.wildcard_listeners.get_by_addr(&port).is_none()
            && self
                .conns
                .get_id_by_addr(&ConnAddr::from_protocol_flow_and_local_port(id, port))
                .is_none()
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
struct ConnAddr<A: IpAddress> {
    local_ip: SpecifiedAddr<A>,
    local_port: NonZeroU16,
    remote_ip: SpecifiedAddr<A>,
    remote_port: NonZeroU16,
}

impl<'a, A: IpAddress> From<&'a ConnAddr<A>> for ConnAddr<A> {
    fn from(c: &'a ConnAddr<A>) -> Self {
        c.clone()
    }
}

impl<A: IpAddress> ConnAddr<A> {
    fn from_protocol_flow_and_local_port(id: &ProtocolFlowId<A>, local_port: NonZeroU16) -> Self {
        Self {
            local_ip: *id.local_addr(),
            local_port,
            remote_ip: *id.remote_addr(),
            remote_port: id.remote_port(),
        }
    }
}

/// Information associated with a UDP connection.
#[derive(Debug)]
pub struct UdpConnInfo<A: IpAddress> {
    /// The local address associated with a UDP connection.
    pub local_ip: SpecifiedAddr<A>,
    /// The local port associated with a UDP connection.
    pub local_port: NonZeroU16,
    /// The remote address associated with a UDP connection.
    pub remote_ip: SpecifiedAddr<A>,
    /// The remote port associated with a UDP connection.
    pub remote_port: NonZeroU16,
}

impl<A: IpAddress> From<ConnAddr<A>> for UdpConnInfo<A> {
    fn from(c: ConnAddr<A>) -> Self {
        let ConnAddr { local_ip, local_port, remote_ip, remote_port } = c;
        Self { local_ip, local_port, remote_ip, remote_port }
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
struct ListenerAddr<A: IpAddress> {
    addr: SpecifiedAddr<A>,
    port: NonZeroU16,
}

/// Information associated with a UDP listener
pub struct UdpListenerInfo<A: IpAddress> {
    /// The local address associated with a UDP listener, or `None` for any
    /// address.
    pub local_ip: Option<SpecifiedAddr<A>>,
    /// The local port associated with a UDP listener.
    pub local_port: NonZeroU16,
}

impl<A: IpAddress> From<ListenerAddr<A>> for UdpListenerInfo<A> {
    fn from(l: ListenerAddr<A>) -> Self {
        Self { local_ip: Some(l.addr), local_port: l.port }
    }
}

impl<A: IpAddress> From<NonZeroU16> for UdpListenerInfo<A> {
    fn from(local_port: NonZeroU16) -> Self {
        Self { local_ip: None, local_port }
    }
}

/// The ID identifying a UDP connection.
///
/// When a new UDP connection is added, it is given a unique `UdpConnId`. These
/// are opaque `usize`s which are intentionally allocated as densely as possible
/// around 0, making it possible to store any associated data in a `Vec` indexed
/// by the ID. `UdpConnId` implements `Into<usize>`.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct UdpConnId<I: Ip>(usize, IpVersionMarker<I>);

impl<I: Ip> UdpConnId<I> {
    fn new(id: usize) -> UdpConnId<I> {
        UdpConnId(id, IpVersionMarker::default())
    }
}

impl<I: Ip> From<UdpConnId<I>> for usize {
    fn from(id: UdpConnId<I>) -> usize {
        id.0
    }
}

impl<I: Ip> IdMapCollectionKey for UdpConnId<I> {
    const VARIANT_COUNT: usize = 1;

    fn get_variant(&self) -> usize {
        0
    }

    fn get_id(&self) -> usize {
        self.0
    }
}

#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
enum ListenerType {
    Specified,
    Wildcard,
}

/// The ID identifying a UDP listener.
///
/// When a new UDP listener is added, it is given a unique `UdpListenerId`.
/// These are opaque `usize`s which are intentionally allocated as densely as
/// possible around 0, making it possible to store any associated data in a
/// `Vec` indexed by the ID. The `listener_type` field is used to look at the
/// correct backing `Vec`: `listeners` or `wildcard_listeners`.
#[derive(Copy, Clone, Debug, Eq, PartialEq, Hash)]
pub struct UdpListenerId<I: Ip> {
    id: usize,
    listener_type: ListenerType,
    _marker: IpVersionMarker<I>,
}

impl<I: Ip> UdpListenerId<I> {
    fn new_specified(id: usize) -> Self {
        UdpListenerId {
            id,
            listener_type: ListenerType::Specified,
            _marker: IpVersionMarker::default(),
        }
    }

    fn new_wildcard(id: usize) -> Self {
        UdpListenerId {
            id,
            listener_type: ListenerType::Wildcard,
            _marker: IpVersionMarker::default(),
        }
    }
}

impl<I: Ip> IdMapCollectionKey for UdpListenerId<I> {
    const VARIANT_COUNT: usize = 2;
    fn get_variant(&self) -> usize {
        match self.listener_type {
            ListenerType::Specified => 0,
            ListenerType::Wildcard => 1,
        }
    }
    fn get_id(&self) -> usize {
        self.id
    }
}

/// An execution context for the UDP protocol.
pub trait UdpContext<I: IcmpIpExt> {
    /// Receives an ICMP error message related to a previously-sent UDP packet.
    ///
    /// `err` is the specific error identified by the incoming ICMP error
    /// message.
    ///
    /// Concretely, this method is called when an ICMP error message is received
    /// which contains an original packet which - based on its source and
    /// destination IPs and ports - most likely originated from the given
    /// socket. Note that the offending packet is not guaranteed to have
    /// originated from the given socket. For example, it may have originated
    /// from a previous socket with the same addresses, it may be the result of
    /// packet corruption on the network, it may have been injected by a
    /// malicious party, etc.
    fn receive_icmp_error(
        &mut self,
        _id: Result<UdpConnId<I>, UdpListenerId<I>>,
        _err: I::ErrorCode,
    ) {
        log_unimplemented!((), "UdpContext::receive_icmp_error: not implemented");
    }
}

impl<D: EventDispatcher> UdpContext<Ipv4> for Ctx<D> {
    fn receive_icmp_error(
        &mut self,
        id: Result<UdpConnId<Ipv4>, UdpListenerId<Ipv4>>,
        err: Icmpv4ErrorCode,
    ) {
        UdpContext::receive_icmp_error(&mut self.dispatcher, id, err);
    }
}

impl<D: EventDispatcher> UdpContext<Ipv6> for Ctx<D> {
    fn receive_icmp_error(
        &mut self,
        id: Result<UdpConnId<Ipv6>, UdpListenerId<Ipv6>>,
        err: Icmpv6ErrorCode,
    ) {
        UdpContext::receive_icmp_error(&mut self.dispatcher, id, err);
    }
}

/// An execution context for the UDP protocol which also provides access to state.
pub trait UdpStateContext<I: IpExt>:
    UdpContext<I>
    + CounterContext
    + TransportIpContext<I>
    + RngStateContext<UdpState<I, <Self as IpDeviceIdContext<I>>::DeviceId>>
{
}

impl<
        I: IpExt,
        C: UdpContext<I>
            + CounterContext
            + TransportIpContext<I>
            + RngStateContext<UdpState<I, C::DeviceId>>,
    > UdpStateContext<I> for C
{
}

/// An execution context for the UDP protocol when a buffer is provided.
///
/// `BufferUdpContext` is like [`UdpContext`], except that it also requires that
/// the context be capable of receiving frames in buffers of type `B`. This is
/// used when a buffer of type `B` is provided to UDP (in particular, in
/// [`send_udp_conn`] and [`send_udp_listener`]), and allows any generated
/// link-layer frames to reuse that buffer rather than needing to always
/// allocate a new one.
pub trait BufferUdpContext<I: IpExt, B: BufferMut>: UdpContext<I> {
    /// Receives a UDP packet from a connection socket.
    fn receive_udp_from_conn(
        &mut self,
        _conn: UdpConnId<I>,
        _src_ip: I::Addr,
        _src_port: NonZeroU16,
        _body: B,
    ) {
        log_unimplemented!((), "BufferUdpContext::receive_udp_from_conn: not implemented");
    }

    /// Receives a UDP packet from a listener socket.
    fn receive_udp_from_listen(
        &mut self,
        _listener: UdpListenerId<I>,
        _src_ip: I::Addr,
        _dst_ip: I::Addr,
        _src_port: Option<NonZeroU16>,
        _body: B,
    ) {
        log_unimplemented!((), "BufferUdpContext::receive_udp_from_listen: not implemented");
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>> BufferUdpContext<Ipv4, B> for Ctx<D> {
    fn receive_udp_from_conn(
        &mut self,
        conn: UdpConnId<Ipv4>,
        src_ip: Ipv4Addr,
        src_port: NonZeroU16,
        body: B,
    ) {
        BufferUdpContext::receive_udp_from_conn(&mut self.dispatcher, conn, src_ip, src_port, body)
    }

    fn receive_udp_from_listen(
        &mut self,
        listener: UdpListenerId<Ipv4>,
        src_ip: Ipv4Addr,
        dst_ip: Ipv4Addr,
        src_port: Option<NonZeroU16>,
        body: B,
    ) {
        BufferUdpContext::receive_udp_from_listen(
            &mut self.dispatcher,
            listener,
            src_ip,
            dst_ip,
            src_port,
            body,
        )
    }
}

impl<B: BufferMut, D: BufferDispatcher<B>> BufferUdpContext<Ipv6, B> for Ctx<D> {
    fn receive_udp_from_conn(
        &mut self,
        conn: UdpConnId<Ipv6>,
        src_ip: Ipv6Addr,
        src_port: NonZeroU16,
        body: B,
    ) {
        BufferUdpContext::receive_udp_from_conn(&mut self.dispatcher, conn, src_ip, src_port, body)
    }

    fn receive_udp_from_listen(
        &mut self,
        listener: UdpListenerId<Ipv6>,
        src_ip: Ipv6Addr,
        dst_ip: Ipv6Addr,
        src_port: Option<NonZeroU16>,
        body: B,
    ) {
        BufferUdpContext::receive_udp_from_listen(
            &mut self.dispatcher,
            listener,
            src_ip,
            dst_ip,
            src_port,
            body,
        )
    }
}

/// An execution context for the UDP protocol when a buffer is provided which
/// also provides access to state.
pub trait BufferUdpStateContext<I: IpExt, B: BufferMut>:
    BufferUdpContext<I, B> + BufferTransportIpContext<I, B> + UdpStateContext<I>
{
}

impl<
        I: IpExt,
        B: BufferMut,
        C: BufferUdpContext<I, B> + BufferTransportIpContext<I, B> + UdpStateContext<I>,
    > BufferUdpStateContext<I, B> for C
{
}

impl<I: IpExt, D: EventDispatcher> DualStateContext<UdpState<I, DeviceId>, D::Rng> for Ctx<D> {
    fn get_states_with(&self, _id0: (), _id1: ()) -> (&UdpState<I, DeviceId>, &D::Rng) {
        // Since `specialize_ip` doesn't support multiple trait bounds (ie, `I:
        // Ip + IpExt`) and requires that the single trait bound is named `Ip`,
        // introduce this trait - effectively an alias for `IpExt`.
        trait Ip: IpExt {}
        impl<I: IpExt> Ip for I {}
        #[specialize_ip]
        fn get<I: Ip, D: EventDispatcher>(ctx: &Ctx<D>) -> (&UdpState<I, DeviceId>, &D::Rng) {
            #[ipv4]
            return (&ctx.state.transport.udpv4, ctx.dispatcher.rng());
            #[ipv6]
            return (&ctx.state.transport.udpv6, ctx.dispatcher.rng());
        }

        get(self)
    }

    fn get_states_mut_with(
        &mut self,
        _id0: (),
        _id1: (),
    ) -> (&mut UdpState<I, DeviceId>, &mut D::Rng) {
        // Since `specialize_ip` doesn't support multiple trait bounds (ie, `I:
        // Ip + IpExt`) and requires that the single trait bound is named `Ip`,
        // introduce this trait - effectively an alias for `IpExt`.
        trait Ip: IpExt {}
        impl<I: IpExt> Ip for I {}
        #[specialize_ip]
        fn get<I: Ip, D: EventDispatcher>(
            ctx: &mut Ctx<D>,
        ) -> (&mut UdpState<I, DeviceId>, &mut D::Rng) {
            let Ctx { state, dispatcher } = ctx;
            #[ipv4]
            return (&mut state.transport.udpv4, dispatcher.rng_mut());
            #[ipv6]
            return (&mut state.transport.udpv6, dispatcher.rng_mut());
        }

        get(self)
    }
}

/// An implementation of [`IpTransportContext`] for UDP.
pub(crate) enum UdpIpTransportContext {}

impl<I: IpExt, C: UdpStateContext<I>> IpTransportContext<I, C> for UdpIpTransportContext {
    fn receive_icmp_error(
        ctx: &mut C,
        src_ip: Option<SpecifiedAddr<I::Addr>>,
        dst_ip: SpecifiedAddr<I::Addr>,
        mut udp_packet: &[u8],
        err: I::ErrorCode,
    ) {
        ctx.increment_counter("UdpIpTransportContext::receive_icmp_error");
        trace!("UdpIpTransportContext::receive_icmp_error({:?})", err);

        let udp_packet =
            match udp_packet.parse_with::<_, UdpPacketRaw<_>>(IpVersionMarker::<I>::default()) {
                Ok(packet) => packet,
                Err(e) => {
                    let _: ParseError = e;
                    // TODO(joshlf): Do something with this error.
                    return;
                }
            };
        if let (Some(src_ip), Some(src_port), Some(dst_port)) =
            (src_ip, udp_packet.src_port(), udp_packet.dst_port())
        {
            if let Some(socket) =
                ctx.get_first_state().conn_state.lookup(src_ip, dst_ip, src_port, dst_port)
            {
                let id = match socket {
                    LookupResult::Conn(id, _) => Ok(id),
                    LookupResult::Listener(id, _) | LookupResult::WildcardListener(id, _) => {
                        Err(id)
                    }
                };
                ctx.receive_icmp_error(id, err);
            } else {
                trace!("UdpIpTransportContext::receive_icmp_error: Got ICMP error message for nonexistent UDP socket; either the socket responsible has since been removed, or the error message was sent in error or corrupted");
            }
        } else {
            trace!("UdpIpTransportContext::receive_icmp_error: Got ICMP error message for IP packet with an invalid source or destination IP or port");
        }
    }
}

impl<I: IpExt, B: BufferMut, C: BufferUdpStateContext<I, B>> BufferIpTransportContext<I, B, C>
    for UdpIpTransportContext
{
    fn receive_ip_packet(
        ctx: &mut C,
        _device: Option<C::DeviceId>,
        src_ip: I::RecvSrcAddr,
        dst_ip: SpecifiedAddr<I::Addr>,
        mut buffer: B,
    ) -> Result<(), (B, TransportReceiveError)> {
        trace!("received UDP packet: {:x?}", buffer.as_mut());
        let src_ip = src_ip.into();
        let packet = if let Ok(packet) =
            buffer.parse_with::<_, UdpPacket<_>>(UdpParseArgs::new(src_ip, dst_ip.get()))
        {
            packet
        } else {
            // TODO(joshlf): Do something with ICMP here?
            return Ok(());
        };

        let state = ctx.get_first_state();

        if let Some(socket) = SpecifiedAddr::new(src_ip)
            .and_then(|src_ip| packet.src_port().map(|src_port| (src_ip, src_port)))
            .and_then(|(src_ip, src_port)| {
                state.conn_state.lookup(dst_ip, src_ip, packet.dst_port(), src_port)
            })
        {
            match socket {
                LookupResult::Conn(id, conn) => {
                    mem::drop(packet);
                    ctx.receive_udp_from_conn(id, conn.remote_ip.get(), conn.remote_port, buffer)
                }
                LookupResult::Listener(id, _) | LookupResult::WildcardListener(id, _) => {
                    let src_port = packet.src_port();
                    mem::drop(packet);
                    ctx.receive_udp_from_listen(id, src_ip, dst_ip.get(), src_port, buffer)
                }
            }
            Ok(())
        } else if state.send_port_unreachable {
            // Unfortunately, type inference isn't smart enough for us to just
            // do packet.parse_metadata().
            let meta =
                ParsablePacket::<_, packet_formats::udp::UdpParseArgs<I::Addr>>::parse_metadata(
                    &packet,
                );
            core::mem::drop(packet);
            buffer.undo_parse(meta);
            Err((buffer, TransportReceiveError::new_port_unreachable()))
        } else {
            Ok(())
        }
    }
}

/// An error when sending using [`send_udp`].
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum UdpSendError {
    /// An error was encountered while trying to create a temporary UDP
    /// connection socket.
    #[error("could not create a temporary connection socket: {}", _0)]
    CreateSock(UdpSockCreationError),
    /// An error was encountered while sending.
    #[error("{}", _0)]
    Send(IpSockSendError),
}

/// Sends a single UDP frame without creating a connection or listener.
///
/// `send_udp` is equivalent to creating a UDP connection with [`connect_udp`]
/// with the same arguments provided to `send_udp`, sending `body` over the
/// created connection and, finally, destroying the connection.
///
/// # Errors
///
/// `send_udp` fails if the selected 4-tuple conflicts with any existing socket.
///
/// On error, the original `body` is returned unmodified so that it can be
/// reused by the caller.
///
// TODO(brunodalbo): We may need more arguments here to express REUSEADDR and
// BIND_TO_DEVICE options.
pub fn send_udp<I: IpExt, B: BufferMut, C: BufferUdpStateContext<I, B>>(
    ctx: &mut C,
    local_ip: Option<SpecifiedAddr<I::Addr>>,
    local_port: Option<NonZeroU16>,
    remote_ip: SpecifiedAddr<I::Addr>,
    remote_port: NonZeroU16,
    body: B,
) -> Result<(), (B, UdpSendError)> {
    // TODO(brunodalbo) this can be faster if we just perform the checks but
    // don't actually create a UDP connection.
    let tmp_conn = match connect_udp(ctx, local_ip, local_port, remote_ip, remote_port) {
        Ok(conn) => conn,
        Err(err) => return Err((body, UdpSendError::CreateSock(err))),
    };

    // Not using `?` here since we need to `remove_udp_conn` even in the case of failure.
    let ret =
        send_udp_conn(ctx, tmp_conn, body).map_err(|(body, err)| (body, UdpSendError::Send(err)));
    let info = remove_udp_conn(ctx, tmp_conn);
    if cfg!(debug_assertions) {
        assert_matches::assert_matches!(info, UdpConnInfo {
            local_ip: removed_local_ip,
            local_port: removed_local_port,
            remote_ip: removed_remote_ip,
            remote_port: removed_remote_port,
        } if local_ip.map(|local_ip| local_ip == removed_local_ip).unwrap_or(true) &&
            local_port.map(|local_port| local_port == removed_local_port).unwrap_or(true) &&
            removed_remote_ip == remote_ip && removed_remote_port == remote_port &&
            removed_remote_port == remote_port && removed_remote_port == remote_port
        );
    }

    ret
}

/// Sends a UDP packet on an existing connection.
///
/// # Errors
///
/// On error, the original `body` is returned unmodified so that it can be
/// reused by the caller.
pub fn send_udp_conn<I: IpExt, B: BufferMut, C: BufferUdpStateContext<I, B>>(
    ctx: &mut C,
    conn: UdpConnId<I>,
    body: B,
) -> Result<(), (B, IpSockSendError)> {
    let state = ctx.get_first_state();
    let ConnSocketEntry { sock, addr } = state
        .conn_state
        .conns
        .get_sock_by_id(conn.0)
        .expect("transport::udp::send_udp_conn: no such conn");
    let sock = sock.clone();
    let ConnAddr { local_ip, local_port, remote_ip, remote_port } = *addr;

    ctx.send_ip_packet(
        &sock,
        body.encapsulate(UdpPacketBuilder::new(
            local_ip.get(),
            remote_ip.get(),
            Some(local_port),
            remote_port,
        )),
    )
    .map_err(|(body, err)| (body.into_inner(), err))
}

/// An error encountered while sending a UDP packet on a listener socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum UdpSendListenerError {
    /// An error was encountered while trying to create a temporary IP socket
    /// to use for the send operation.
    #[error("could not create a temporary connection socket: {}", _0)]
    CreateSock(IpSockCreationError),
    /// A local IP address was specified, but it did not match any of the IP
    /// addresses associated with the listener socket.
    #[error("the provided local IP address is not associated with the socket")]
    LocalIpAddrMismatch,
    /// An MTU was exceeded.
    #[error("the maximum transmission unit (MTU) was exceeded")]
    Mtu,
}

/// Send a UDP packet on an existing listener.
///
/// `send_udp_listener` sends a UDP packet on an existing listener. The caller
/// must specify the local address in order to disambiguate in case the listener
/// is bound to multiple local addresses. If the listener is not bound to the
/// local address provided, `send_udp_listener` will fail.
///
/// # Panics
///
/// `send_udp_listener` panics if `listener` is not associated with a listener
/// for this IP version.
pub fn send_udp_listener<I: IpExt, B: BufferMut, C: BufferUdpStateContext<I, B>>(
    ctx: &mut C,
    listener: UdpListenerId<I>,
    local_ip: Option<SpecifiedAddr<I::Addr>>,
    remote_ip: SpecifiedAddr<I::Addr>,
    remote_port: NonZeroU16,
    body: B,
) -> Result<(), (B, UdpSendListenerError)> {
    // TODO(https://fxbug.dev/92447) If `local_ip` is `None`, and so
    // `new_ip_socket` picks a local IP address for us, it may cause problems
    // when we don't match the bound listener addresses. We should revisit
    // whether that check is actually necessary.
    //
    // Also, if the local IP address is a multicast address this function should
    // probably fail and `send_udp` must be used instead.
    let sock = match ctx.new_ip_socket(
        local_ip,
        remote_ip,
        IpProto::Udp.into(),
        UnroutableBehavior::Close,
        None,
    ) {
        Ok(sock) => sock,
        Err(err) => return Err((body, UdpSendListenerError::CreateSock(err))),
    };

    let state = ctx.get_first_state();
    let local_port = match listener.listener_type {
        ListenerType::Specified => {
            let addrs = state
                .conn_state
                .listeners
                .get_by_listener(listener.id)
                .expect("specified listener not found");
            // We found the listener. Make sure at least one of the addresses
            // associated with it is the local_ip the caller passed.
            match addrs.iter().find_map(|addr| {
                if &addr.addr == sock.local_ip() {
                    Some(addr.port)
                } else {
                    None
                }
            }) {
                Some(port) => port,
                None => return Err((body, UdpSendListenerError::LocalIpAddrMismatch)),
            }
        }
        ListenerType::Wildcard => {
            let ports = state
                .conn_state
                .wildcard_listeners
                .get_by_listener(listener.id)
                .expect("wildcard listener not found");
            ports[0]
        }
    };

    ctx.send_ip_packet(
        &sock,
        body.encapsulate(UdpPacketBuilder::new(
            sock.local_ip().get(),
            sock.remote_ip().get(),
            Some(local_port),
            remote_port,
        )),
    )
    .map_err(|(body, err)| (body.into_inner(), match err {
        IpSockSendError::Mtu => UdpSendListenerError::Mtu,
        IpSockSendError::Unroutable(err) => unreachable!("temporary IP socket which was created with `UnroutableBehavior::Close` should still be routable, but got error {:?}", err),
    }))
}

/// Create a UDP connection.
///
/// `connect_udp` binds `conn` as a connection to the remote address and port.
/// It is also bound to the local address and port, meaning that packets sent on
/// this connection will always come from that address and port. If `local_ip`
/// is `None`, then the local address will be chosen based on the route to the
/// remote address. If `local_port` is `None`, then one will be chosen from the
/// available local ports.
///
/// # Errors
///
/// `connect_udp` will fail in the following cases:
/// - If both `local_ip` and `local_port` are specified but conflict with an
///   existing connection or listener
/// - If one or both are left unspecified but there is still no way to satisfy
///   the request (e.g., `local_ip` is specified but there are no available
///   local ports for that address)
/// - If there is no route to `remote_ip`
pub fn connect_udp<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    local_ip: Option<SpecifiedAddr<I::Addr>>,
    local_port: Option<NonZeroU16>,
    remote_ip: SpecifiedAddr<I::Addr>,
    remote_port: NonZeroU16,
) -> Result<UdpConnId<I>, UdpSockCreationError> {
    let ip_sock = ctx
        .new_ip_socket(local_ip, remote_ip, IpProto::Udp.into(), UnroutableBehavior::StayOpen, None)
        .map_err(<IpSockCreationError as Into<UdpSockCreationError>>::into)?;

    let local_ip = *ip_sock.local_ip();
    let remote_ip = *ip_sock.remote_ip();

    let local_port = if let Some(local_port) = local_port {
        local_port
    } else {
        try_alloc_local_port(ctx, &ProtocolFlowId::new(local_ip, remote_ip, remote_port))
            .ok_or(UdpSockCreationError::CouldNotAllocateLocalPort)?
    };

    let c = ConnAddr { local_ip, local_port, remote_ip, remote_port };
    let listener = ListenerAddr { addr: local_ip, port: local_port };
    let state = ctx.get_first_state_mut();
    if state.conn_state.conns.get_id_by_addr(&c).is_some()
        || state.conn_state.listeners.get_by_addr(&listener).is_some()
    {
        return Err(UdpSockCreationError::SockAddrConflict);
    }
    Ok(UdpConnId::new(state.conn_state.conns.insert(c.clone(), ip_sock)))
}

/// Removes a previously registered UDP connection.
///
/// `remove_udp_conn` removes a previously registered UDP connection indexed by
/// the [`UpConnId`] `id`. It returns the [`UdpConnInfo`] information that was
/// associated with that UDP connection.
///
/// # Panics
///
/// `remove_udp_conn` panics if `id` is not a valid `UdpConnId`.
pub fn remove_udp_conn<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    id: UdpConnId<I>,
) -> UdpConnInfo<I::Addr> {
    let state = ctx.get_first_state_mut();
    state.conn_state.conns.remove_by_id(id.into()).expect("UDP connection not found").addr.into()
}

/// Gets the [`UdpConnInfo`] associated with the UDP connection referenced by [`id`].
///
/// # Panics
///
/// `get_udp_conn_info` panics if `id` is not a valid `UdpConnId`.
pub fn get_udp_conn_info<I: IpExt, C: UdpStateContext<I>>(
    ctx: &C,
    id: UdpConnId<I>,
) -> UdpConnInfo<I::Addr> {
    ctx.get_first_state()
        .conn_state
        .conns
        .get_sock_by_id(id.into())
        .expect("UDP connection not found")
        .addr
        .clone()
        .into()
}

/// Listen on for incoming UDP packets.
///
/// `listen_udp` registers `listener` as a listener for incoming UDP packets on
/// the given `port`. If `addr` is `None`, the listener is a "wildcard
/// listener", and is bound to all local addresses. See the `transport` module
/// documentation for more details.
///
/// If `addr` is `Some``, and `addr` is already bound on the given port (either
/// by a listener or a connection), `listen_udp` will fail. If `addr` is `None`,
/// and a wildcard listener is already bound to the given port, `listen_udp`
/// will fail.
///
/// # Panics
///
/// `listen_udp` panics if `listener` is already in use.
pub fn listen_udp<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    addr: Option<SpecifiedAddr<I::Addr>>,
    port: Option<NonZeroU16>,
) -> Result<UdpListenerId<I>, LocalAddressError> {
    let port = if let Some(port) = port {
        if !ctx.get_first_state().conn_state.is_listen_port_available(addr, port) {
            return Err(LocalAddressError::AddressInUse);
        }
        port
    } else {
        let used_ports = ctx
            .get_first_state_mut()
            .conn_state
            .collect_used_local_ports(addr.as_ref().into_iter());
        try_alloc_listen_port(ctx, &used_ports)
            .ok_or(LocalAddressError::FailedToAllocateLocalPort)?
    };
    match addr {
        None => {
            let state = ctx.get_first_state_mut();
            Ok(UdpListenerId::new_wildcard(
                state.conn_state.wildcard_listeners.insert(alloc::vec![port]),
            ))
        }
        Some(addr) => {
            if !ctx.is_assigned_local_addr(addr.get()) {
                return Err(LocalAddressError::CannotBindToAddress);
            }
            let state = ctx.get_first_state_mut();
            Ok(UdpListenerId::new_specified(
                state.conn_state.listeners.insert(alloc::vec![ListenerAddr { addr, port }]),
            ))
        }
    }
}

/// Removes a previously registered UDP listener.
///
/// `remove_udp_listener` removes a previously registered UDP listener indexed
/// by the [`UdpListenerId`] `id`. It returns the [`UdpListenerInfo`]
/// information that was associated with that UDP listener.
///
/// # Panics
///
/// `remove_listener` panics if `id` is not a valid `UdpListenerId`.
pub fn remove_udp_listener<I: IpExt, C: UdpStateContext<I>>(
    ctx: &mut C,
    id: UdpListenerId<I>,
) -> UdpListenerInfo<I::Addr> {
    let state = ctx.get_first_state_mut();
    match id.listener_type {
        ListenerType::Specified => state
            .conn_state
            .listeners
            .remove_by_listener(id.id)
            .expect("Invalid UDP listener ID")
            // NOTE(brunodalbo) ListenerSocketMap keeps vecs internally, but we
            // always only add a single address, so unwrap the first one.
            .first()
            .expect("Unexpected empty UDP listener")
            .clone()
            .into(),
        ListenerType::Wildcard => state
            .conn_state
            .wildcard_listeners
            .remove_by_listener(id.id)
            .expect("Invalid UDP listener ID")
            // NOTE(brunodalbo) ListenerSocketMap keeps vecs internally, but we
            // always only add a single address, so unwrap the first one.
            .first()
            .expect("Unexpected empty UDP listener")
            .clone()
            .into(),
    }
}

/// Gets the [`UdpListenerInfo`] associated with the UDP listener referenced by
/// [`id`].
///
/// # Panics
///
/// `get_udp_conn_info` panics if `id` is not a valid `UdpListenerId`.
pub fn get_udp_listener_info<I: IpExt, C: UdpStateContext<I>>(
    ctx: &C,
    id: UdpListenerId<I>,
) -> UdpListenerInfo<I::Addr> {
    let state = ctx.get_first_state();
    match id.listener_type {
        ListenerType::Specified => state
            .conn_state
            .listeners
            .get_by_listener(id.id)
            .expect("UDP listener not found")
            // NOTE(brunodalbo) ListenerSocketMap keeps vecs internally, but we
            // always only add a single address, so unwrap the first one.
            .first()
            .map(|l| l.clone().into())
            .expect("Unexpected empty UDP listener"),
        ListenerType::Wildcard => state
            .conn_state
            .wildcard_listeners
            .get_by_listener(id.id)
            .expect("UDP listener not found")
            // NOTE(brunodalbo) ListenerSocketMap keeps vecs internally, but we
            // always only add a single address, so unwrap the first one.
            .first()
            .map(|l| l.clone().into())
            .expect("Unexpected empty UDP listener"),
    }
}

/// An error when attempting to create a UDP socket.
#[derive(Error, Copy, Clone, Debug, Eq, PartialEq)]
pub enum UdpSockCreationError {
    /// An error was encountered creating an IP socket.
    #[error("{}", _0)]
    Ip(#[from] IpSockCreationError),
    /// No local port was specified, and none could be automatically allocated.
    #[error("a local port could not be allocated")]
    CouldNotAllocateLocalPort,
    /// The specified socket addresses (IP addresses and ports) conflict with an
    /// existing UDP socket.
    #[error("the socket's IP address and port conflict with an existing socket")]
    SockAddrConflict,
}

#[cfg(test)]
mod tests {
    use alloc::{borrow::ToOwned, vec, vec::Vec};

    use net_types::ip::{Ipv4, Ipv4Addr, Ipv6, Ipv6Addr, Ipv6SourceAddr};
    use packet::{Buf, InnerPacketBuilder, ParsablePacket, Serializer};
    use packet_formats::{
        icmp::{Icmpv4DestUnreachableCode, Icmpv6DestUnreachableCode},
        ip::{IpExtByteSlice, IpPacket, IpPacketBuilder},
        ipv4::{Ipv4Header, Ipv4PacketRaw},
        ipv6::{Ipv6Header, Ipv6PacketRaw},
    };
    use rand_xorshift::XorShiftRng;
    use specialize_ip_macro::ip_test;

    use super::*;
    use crate::{
        assert_empty,
        context::testutil::{DummyFrameCtx, DummyInstant},
        device::IpFrameMeta,
        ip::{
            device::state::IpDeviceStateIpExt,
            icmp::Icmpv6ErrorCode,
            socket::{testutil::DummyIpSocketCtx, BufferIpSocketHandler, IpSockUnroutableError},
            DummyDeviceId,
        },
        testutil::{set_logger_for_test, FakeCryptoRng},
    };

    /// The listener data sent through a [`DummyUdpCtx`].
    #[derive(Debug, PartialEq)]
    struct ListenData<I: Ip> {
        listener: UdpListenerId<I>,
        src_ip: I::Addr,
        dst_ip: I::Addr,
        src_port: Option<NonZeroU16>,
        body: Vec<u8>,
    }

    /// The UDP connection data sent through a [`DummyUdpCtx`].
    #[derive(Debug, PartialEq)]
    struct ConnData<I: Ip> {
        conn: UdpConnId<I>,
        body: Vec<u8>,
    }

    /// An ICMP error delivered to a [`DummyUdpCtx`].
    #[derive(Debug, Eq, PartialEq)]
    struct IcmpError<I: TestIpExt> {
        id: Result<UdpConnId<I>, UdpListenerId<I>>,
        err: I::ErrorCode,
    }

    struct DummyUdpCtx<I: TestIpExt> {
        state: UdpState<I, DummyDeviceId>,
        ip_socket_ctx: DummyIpSocketCtx<I>,
        listen_data: Vec<ListenData<I>>,
        conn_data: Vec<ConnData<I>>,
        icmp_errors: Vec<IcmpError<I>>,
        extra_local_addrs: Vec<I::Addr>,
    }

    impl<I: TestIpExt> Default for DummyUdpCtx<I>
    where
        DummyUdpCtx<I>: DummyUdpCtxExt<I>,
    {
        fn default() -> Self {
            DummyUdpCtx::with_local_remote_ip_addrs(vec![local_ip::<I>()], vec![remote_ip::<I>()])
        }
    }

    impl<I: TestIpExt> DummyUdpCtx<I> {
        fn with_ip_socket_ctx(ip_socket_ctx: DummyIpSocketCtx<I>) -> Self {
            DummyUdpCtx {
                state: Default::default(),
                ip_socket_ctx,
                listen_data: Default::default(),
                conn_data: Default::default(),
                icmp_errors: Default::default(),
                extra_local_addrs: Vec::new(),
            }
        }
    }

    trait DummyUdpCtxExt<I: Ip> {
        fn with_local_remote_ip_addrs(
            local_ips: Vec<SpecifiedAddr<I::Addr>>,
            remote_ips: Vec<SpecifiedAddr<I::Addr>>,
        ) -> Self;
    }

    impl DummyUdpCtxExt<Ipv4> for DummyUdpCtx<Ipv4> {
        fn with_local_remote_ip_addrs(
            local_ips: Vec<SpecifiedAddr<Ipv4Addr>>,
            remote_ips: Vec<SpecifiedAddr<Ipv4Addr>>,
        ) -> Self {
            DummyUdpCtx::with_ip_socket_ctx(DummyIpSocketCtx::new_ipv4(local_ips, remote_ips))
        }
    }

    impl DummyUdpCtxExt<Ipv6> for DummyUdpCtx<Ipv6> {
        fn with_local_remote_ip_addrs(
            local_ips: Vec<SpecifiedAddr<Ipv6Addr>>,
            remote_ips: Vec<SpecifiedAddr<Ipv6Addr>>,
        ) -> Self {
            DummyUdpCtx::with_ip_socket_ctx(DummyIpSocketCtx::new_ipv6(local_ips, remote_ips))
        }
    }

    impl<I: TestIpExt> AsRef<DummyIpSocketCtx<I>> for DummyUdpCtx<I> {
        fn as_ref(&self) -> &DummyIpSocketCtx<I> {
            &self.ip_socket_ctx
        }
    }

    impl<I: TestIpExt> AsMut<DummyIpSocketCtx<I>> for DummyUdpCtx<I> {
        fn as_mut(&mut self) -> &mut DummyIpSocketCtx<I> {
            &mut self.ip_socket_ctx
        }
    }

    type DummyCtx<I> = crate::context::testutil::DummyCtx<
        DummyUdpCtx<I>,
        (),
        IpFrameMeta<<I as Ip>::Addr, DummyDeviceId>,
    >;

    /// The trait bounds required of `DummyCtx<I>` in tests.
    trait DummyCtxBound<I: TestIpExt>:
        Default + BufferIpSocketHandler<I, Buf<Vec<u8>>, DeviceId = DummyDeviceId>
    {
    }
    impl<I: TestIpExt> DummyCtxBound<I> for DummyCtx<I> where
        DummyCtx<I>: Default + BufferIpSocketHandler<I, Buf<Vec<u8>>, DeviceId = DummyDeviceId>
    {
    }

    impl<I: TestIpExt> TransportIpContext<I> for DummyCtx<I>
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        fn is_assigned_local_addr(&self, addr: <I as Ip>::Addr) -> bool {
            local_ip::<I>().get() == addr || self.get_ref().extra_local_addrs.contains(&addr)
        }
    }

    impl<I: TestIpExt> DualStateContext<UdpState<I, DummyDeviceId>, FakeCryptoRng<XorShiftRng>>
        for DummyCtx<I>
    {
        fn get_states_with(
            &self,
            _id0: (),
            _id1: (),
        ) -> (&UdpState<I, DummyDeviceId>, &FakeCryptoRng<XorShiftRng>) {
            let (state, rng): (&DummyUdpCtx<I>, _) = self.get_states();
            (&state.state, rng)
        }

        fn get_states_mut_with(
            &mut self,
            _id0: (),
            _id1: (),
        ) -> (&mut UdpState<I, DummyDeviceId>, &mut FakeCryptoRng<XorShiftRng>) {
            let (state, rng): (&mut DummyUdpCtx<I>, _) = self.get_states_mut();
            (&mut state.state, rng)
        }
    }

    impl<I: TestIpExt> UdpContext<I> for DummyCtx<I> {
        fn receive_icmp_error(
            &mut self,
            id: Result<UdpConnId<I>, UdpListenerId<I>>,
            err: I::ErrorCode,
        ) {
            self.get_mut().icmp_errors.push(IcmpError { id, err })
        }
    }
    impl<I: TestIpExt, B: BufferMut> BufferUdpContext<I, B> for DummyCtx<I> {
        fn receive_udp_from_conn(
            &mut self,
            conn: UdpConnId<I>,
            _src_ip: <I as Ip>::Addr,
            _src_port: NonZeroU16,
            body: B,
        ) {
            self.get_mut().conn_data.push(ConnData { conn, body: body.as_ref().to_owned() })
        }

        fn receive_udp_from_listen(
            &mut self,
            listener: UdpListenerId<I>,
            src_ip: <I as Ip>::Addr,
            dst_ip: <I as Ip>::Addr,
            src_port: Option<NonZeroU16>,
            body: B,
        ) {
            self.get_mut().listen_data.push(ListenData {
                listener,
                src_ip,
                dst_ip,
                src_port,
                body: body.as_ref().to_owned(),
            })
        }
    }

    fn local_ip<I: TestIpExt>() -> SpecifiedAddr<I::Addr> {
        I::get_other_ip_address(1)
    }

    fn remote_ip<I: TestIpExt>() -> SpecifiedAddr<I::Addr> {
        I::get_other_ip_address(2)
    }

    trait TestIpExt: crate::testutil::TestIpExt + IpExt + IpDeviceStateIpExt<DummyInstant> {
        fn try_into_recv_src_addr(addr: Self::Addr) -> Option<Self::RecvSrcAddr>;
    }

    impl TestIpExt for Ipv4 {
        fn try_into_recv_src_addr(addr: Ipv4Addr) -> Option<Ipv4Addr> {
            Some(addr)
        }
    }

    impl TestIpExt for Ipv6 {
        fn try_into_recv_src_addr(addr: Ipv6Addr) -> Option<Ipv6SourceAddr> {
            Ipv6SourceAddr::new(addr)
        }
    }

    /// Helper function to inject an UDP packet with the provided parameters.
    fn receive_udp_packet<I: TestIpExt>(
        ctx: &mut DummyCtx<I>,
        src_ip: I::Addr,
        dst_ip: I::Addr,
        src_port: NonZeroU16,
        dst_port: NonZeroU16,
        body: &[u8],
    ) where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let builder = UdpPacketBuilder::new(src_ip, dst_ip, Some(src_port), dst_port);
        let buffer = Buf::new(body.to_owned(), ..)
            .encapsulate(builder)
            .serialize_vec_outer()
            .unwrap()
            .into_inner();
        UdpIpTransportContext::receive_ip_packet(
            ctx,
            Some(DummyDeviceId),
            I::try_into_recv_src_addr(src_ip).unwrap(),
            SpecifiedAddr::new(dst_ip).unwrap(),
            buffer,
        )
        .expect("Receive IP packet succeeds");
    }

    /// Tests UDP listeners over different IP versions.
    ///
    /// Tests that a listener can be created, that the context receives packet
    /// notifications for that listener, and that we can send data using that
    /// listener.
    #[ip_test]
    fn test_listen_udp<I: Ip + TestIpExt + for<'a> IpExtByteSlice<&'a [u8]>>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a listener on local port 100, bound to the local IP:
        let listener = listen_udp::<I, _>(&mut ctx, Some(local_ip), NonZeroU16::new(100))
            .expect("listen_udp failed");
        assert_eq!(listener.listener_type, ListenerType::Specified);

        // Inject a packet and check that the context receives it:
        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut ctx,
            remote_ip.get(),
            local_ip.get(),
            NonZeroU16::new(200).unwrap(),
            NonZeroU16::new(100).unwrap(),
            &body[..],
        );

        let listen_data = &ctx.get_ref().listen_data;
        assert_eq!(listen_data.len(), 1);
        let pkt = &listen_data[0];
        assert_eq!(pkt.listener, listener);
        assert_eq!(pkt.src_ip, remote_ip.get());
        assert_eq!(pkt.dst_ip, local_ip.get());
        assert_eq!(pkt.src_port.unwrap().get(), 200);
        assert_eq!(pkt.body, &body[..]);

        // Send a packet providing a local ip:
        send_udp_listener(
            &mut ctx,
            listener,
            Some(local_ip),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_udp_listener failed");
        // And send a packet that doesn't:
        send_udp_listener(
            &mut ctx,
            listener,
            None,
            remote_ip,
            NonZeroU16::new(200).unwrap(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_udp_listener failed");
        let frames = ctx.frames();
        assert_eq!(frames.len(), 2);
        let check_frame = |(meta, frame_body): &(IpFrameMeta<I::Addr, DummyDeviceId>, Vec<u8>)| {
            assert_eq!(meta.local_addr, remote_ip);
            let mut buf = &frame_body[..];
            let ip_packet = <I::Packet as ParsablePacket<_, _>>::parse(&mut buf, ())
                .expect("Parsed send IP packet");
            let (src_ip, dst_ip) = (ip_packet.src_ip(), ip_packet.dst_ip());
            assert_eq!(src_ip, local_ip.get());
            assert_eq!(dst_ip, remote_ip.get());
            assert_eq!(ip_packet.proto(), IpProto::Udp.into());
            mem::drop(ip_packet);
            let udp_packet = UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip, dst_ip))
                .expect("Parsed sent UDP packet");
            assert_eq!(udp_packet.src_port().unwrap().get(), 100);
            assert_eq!(udp_packet.dst_port().get(), 200);
            assert_eq!(udp_packet.body(), &body[..]);
        };
        check_frame(&frames[0]);
        check_frame(&frames[1]);
    }

    /// Tests that UDP packets without a connection are dropped.
    ///
    /// Tests that receiving a UDP packet on a port over which there isn't a
    /// listener causes the packet to be dropped correctly.
    #[ip_test]
    fn test_udp_drop<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();

        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut ctx,
            remote_ip.get(),
            local_ip.get(),
            NonZeroU16::new(200).unwrap(),
            NonZeroU16::new(100).unwrap(),
            &body[..],
        );
        assert_empty(ctx.get_ref().listen_data.iter());
        assert_empty(ctx.get_ref().conn_data.iter());
    }

    /// Tests that UDP connections can be created and data can be transmitted
    /// over it.
    ///
    /// Only tests with specified local port and address bounds.
    #[ip_test]
    fn test_udp_conn_basic<I: Ip + TestIpExt + for<'a> IpExtByteSlice<&'a [u8]>>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a UDP connection with a specified local port and local IP.
        let conn = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(NonZeroU16::new(100).unwrap()),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .expect("connect_udp failed");

        // Inject a UDP packet and see if we receive it on the context.
        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut ctx,
            remote_ip.get(),
            local_ip.get(),
            NonZeroU16::new(200).unwrap(),
            NonZeroU16::new(100).unwrap(),
            &body[..],
        );

        let conn_data = &ctx.get_ref().conn_data;
        assert_eq!(conn_data.len(), 1);
        let pkt = &conn_data[0];
        assert_eq!(pkt.conn, conn);
        assert_eq!(pkt.body, &body[..]);

        // Now try to send something over this new connection.
        send_udp_conn(&mut ctx, conn, Buf::new(body.to_vec(), ..))
            .expect("send_udp_conn returned an error");

        let frames = ctx.frames();
        assert_eq!(frames.len(), 1);

        // Check first frame.
        let (meta, frame_body) = &frames[0];
        assert_eq!(meta.local_addr, remote_ip);
        let mut buf = &frame_body[..];
        let ip_packet = <I::Packet as ParsablePacket<_, _>>::parse(&mut buf, ())
            .expect("Parsed send IP packet");
        let (src_ip, dst_ip) = (ip_packet.src_ip(), ip_packet.dst_ip());
        assert_eq!(src_ip, local_ip.get());
        assert_eq!(dst_ip, remote_ip.get());
        assert_eq!(ip_packet.proto(), IpProto::Udp.into());
        mem::drop(ip_packet);
        let udp_packet = UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip, dst_ip))
            .expect("Parsed sent UDP packet");
        assert_eq!(udp_packet.src_port().unwrap().get(), 100);
        assert_eq!(udp_packet.dst_port().get(), 200);
        assert_eq!(udp_packet.body(), &body[..]);
    }

    /// Tests that UDP connections fail with an appropriate error for
    /// non-routable remote addresses.
    #[ip_test]
    fn test_udp_conn_unroutable<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        // Set dummy context callback to treat all addresses as unroutable.
        let local_ip = local_ip::<I>();
        let remote_ip = I::get_other_ip_address(127);
        // Create a UDP connection with a specified local port and local IP.
        let conn_err = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(NonZeroU16::new(100).unwrap()),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .unwrap_err();

        assert_eq!(
            conn_err,
            UdpSockCreationError::Ip(IpSockUnroutableError::NoRouteToRemoteAddr.into())
        );
    }

    /// Tests that UDP connections fail with an appropriate error when local
    /// address is non-local.
    #[ip_test]
    fn test_udp_conn_cannot_bind<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();

        // Cse remote address to trigger IpSockCreationError::LocalAddrNotAssigned.
        let local_ip = remote_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a UDP connection with a specified local port and local ip:
        let conn_err = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(NonZeroU16::new(100).unwrap()),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .unwrap_err();

        assert_eq!(
            conn_err,
            UdpSockCreationError::Ip(IpSockUnroutableError::LocalAddrNotAssigned.into())
        );
    }

    /// Tests that UDP connections fail with an appropriate error when local
    /// ports are exhausted.
    #[ip_test]
    fn test_udp_conn_exhausted<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();

        let local_ip = local_ip::<I>();
        // Exhaust local ports to trigger FailedToAllocateLocalPort error.
        for port_num in UdpConnectionState::<I, IpSock<I, DummyDeviceId>>::EPHEMERAL_RANGE {
            let _: usize =
                DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state_mut(&mut ctx)
                    .conn_state
                    .listeners
                    .insert(vec![ListenerAddr {
                        addr: local_ip,
                        port: NonZeroU16::new(port_num).unwrap(),
                    }]);
        }

        let remote_ip = remote_ip::<I>();
        let conn_err = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            None,
            remote_ip,
            NonZeroU16::new(100).unwrap(),
        )
        .unwrap_err();

        assert_eq!(conn_err, UdpSockCreationError::CouldNotAllocateLocalPort);
    }

    /// Tests that UDP connections fail with an appropriate error when the
    /// connection is in use.
    #[ip_test]
    fn test_udp_conn_in_use<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();

        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();

        let local_port = NonZeroU16::new(100).unwrap();

        // Tie up the connection so the second call to `connect_udp` fails.
        let _ = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(local_port),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .expect("Initial call to connect_udp was expected to succeed");

        // Create a UDP connection with a specified local port and local ip:
        let conn_err = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(local_port),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .unwrap_err();

        assert_eq!(conn_err, UdpSockCreationError::SockAddrConflict);
    }

    #[ip_test]
    fn test_send_udp<I: Ip + TestIpExt + for<'a> IpExtByteSlice<&'a [u8]>>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();

        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();

        // UDP connection count should be zero before and after `send_udp` call.
        assert_empty(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .conns
                .iter_addrs(),
        );

        let body = [1, 2, 3, 4, 5];
        // Try to send something with send_udp
        send_udp(
            &mut ctx,
            Some(local_ip),
            NonZeroU16::new(100),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
            Buf::new(body.to_vec(), ..),
        )
        .expect("send_udp failed");

        // UDP connection count should be zero before and after `send_udp` call.
        assert_empty(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .conns
                .iter_addrs(),
        );
        let frames = ctx.frames();
        assert_eq!(frames.len(), 1);

        // Check first frame.
        let (meta, frame_body) = &frames[0];
        assert_eq!(meta.local_addr, remote_ip);
        let mut buf = &frame_body[..];
        let ip_packet = <I::Packet as ParsablePacket<_, _>>::parse(&mut buf, ())
            .expect("Parsed send IP packet");
        let (src_ip, dst_ip) = (ip_packet.src_ip(), ip_packet.dst_ip());
        assert_eq!(src_ip, local_ip.get());
        assert_eq!(dst_ip, remote_ip.get());
        assert_eq!(ip_packet.proto(), IpProto::Udp.into());
        mem::drop(ip_packet);
        let udp_packet = UdpPacket::parse(&mut buf, UdpParseArgs::new(src_ip, dst_ip))
            .expect("Parsed sent UDP packet");
        assert_eq!(udp_packet.src_port().unwrap().get(), 100);
        assert_eq!(udp_packet.dst_port().get(), 200);
        assert_eq!(udp_packet.body(), &body[..]);
    }

    /// Tests that `send_udp` propogates errors.
    #[ip_test]
    fn test_send_udp_errors<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();

        let mut ctx = DummyCtx::<I>::default();

        // Use invalid local IP to force a CannotBindToAddress error.
        let local_ip = remote_ip::<I>();
        let remote_ip = remote_ip::<I>();

        let body = [1, 2, 3, 4, 5];
        // Try to send something with send_udp.
        let (_, send_error) = send_udp(
            &mut ctx,
            Some(local_ip),
            NonZeroU16::new(100),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
            Buf::new(body.to_vec(), ..),
        )
        .expect_err("send_udp unexpectedly succeeded");

        assert_eq!(
            send_error,
            UdpSendError::CreateSock(UdpSockCreationError::Ip(
                IpSockUnroutableError::LocalAddrNotAssigned.into()
            ))
        );
    }

    /// Tests that `send_udp` cleans up after errors.
    #[ip_test]
    fn test_send_udp_errors_cleanup<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();

        let mut ctx = DummyCtx::<I>::default();

        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();

        // UDP connection count should be zero before and after `send_udp` call.
        assert_empty(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .conns
                .iter_addrs(),
        );

        // Instruct the dummy frame context to throw errors.
        let frames: &mut DummyFrameCtx<IpFrameMeta<I::Addr, DummyDeviceId>> = ctx.as_mut();
        frames.set_should_error_for_frame(|_frame_meta| true);

        let body = [1, 2, 3, 4, 5];
        // Try to send something with send_udp
        let (_, send_error) = send_udp(
            &mut ctx,
            Some(local_ip),
            NonZeroU16::new(100),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
            Buf::new(body.to_vec(), ..),
        )
        .expect_err("send_udp unexpectedly succeeded");

        assert_eq!(send_error, UdpSendError::Send(IpSockSendError::Mtu));

        // UDP connection count should be zero before and after `send_udp` call
        // (even in the case of errors).
        assert_empty(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .conns
                .iter_addrs(),
        );
    }

    /// Tests that UDP send failures are propagated as errors.
    ///
    /// Only tests with specified local port and address bounds.
    #[ip_test]
    fn test_send_udp_conn_failure<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a UDP connection with a specified local port and local IP.
        let conn = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(NonZeroU16::new(100).unwrap()),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .expect("connect_udp failed");

        // Instruct the dummy frame context to throw errors.
        let frames: &mut DummyFrameCtx<IpFrameMeta<I::Addr, DummyDeviceId>> = ctx.as_mut();
        frames.set_should_error_for_frame(|_frame_meta| true);

        // Now try to send something over this new connection:
        let (_, send_err) = send_udp_conn(&mut ctx, conn, Buf::new(Vec::new(), ..)).unwrap_err();
        assert_eq!(send_err, IpSockSendError::Mtu);
    }

    /// Tests that if we have multiple listeners and connections, demuxing the
    /// flows is performed correctly.
    #[ip_test]
    fn test_udp_demux<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
        DummyUdpCtx<I>: DummyUdpCtxExt<I>,
    {
        set_logger_for_test();
        let local_ip = local_ip::<I>();
        let remote_ip_a = I::get_other_ip_address(70);
        let remote_ip_b = I::get_other_ip_address(72);
        let local_port_a = NonZeroU16::new(100).unwrap();
        let local_port_b = NonZeroU16::new(101).unwrap();
        let local_port_c = NonZeroU16::new(102).unwrap();
        let local_port_d = NonZeroU16::new(103).unwrap();
        let remote_port_a = NonZeroU16::new(200).unwrap();

        let mut ctx = DummyCtx::<I>::with_state(DummyUdpCtx::with_local_remote_ip_addrs(
            vec![local_ip],
            vec![remote_ip_a, remote_ip_b],
        ));

        // Create some UDP connections and listeners:
        let conn1 = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(local_port_d),
            remote_ip_a,
            remote_port_a,
        )
        .expect("connect_udp failed");
        // conn2 has just a remote addr different than conn1
        let conn2 = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            Some(local_port_d),
            remote_ip_b,
            remote_port_a,
        )
        .expect("connect_udp failed");
        let list1 = listen_udp::<I, _>(&mut ctx, Some(local_ip), Some(local_port_a))
            .expect("listen_udp failed");
        let list2 = listen_udp::<I, _>(&mut ctx, Some(local_ip), Some(local_port_b))
            .expect("listen_udp failed");
        let wildcard_list =
            listen_udp::<I, _>(&mut ctx, None, Some(local_port_c)).expect("listen_udp failed");

        // Now inject UDP packets that each of the created connections should
        // receive.
        let body_conn1 = [1, 1, 1, 1];
        receive_udp_packet(
            &mut ctx,
            remote_ip_a.get(),
            local_ip.get(),
            remote_port_a,
            local_port_d,
            &body_conn1[..],
        );
        let body_conn2 = [2, 2, 2, 2];
        receive_udp_packet(
            &mut ctx,
            remote_ip_b.get(),
            local_ip.get(),
            remote_port_a,
            local_port_d,
            &body_conn2[..],
        );
        let body_list1 = [3, 3, 3, 3];
        receive_udp_packet(
            &mut ctx,
            remote_ip_a.get(),
            local_ip.get(),
            remote_port_a,
            local_port_a,
            &body_list1[..],
        );
        let body_list2 = [4, 4, 4, 4];
        receive_udp_packet(
            &mut ctx,
            remote_ip_a.get(),
            local_ip.get(),
            remote_port_a,
            local_port_b,
            &body_list2[..],
        );
        let body_wildcard_list = [5, 5, 5, 5];
        receive_udp_packet(
            &mut ctx,
            remote_ip_a.get(),
            local_ip.get(),
            remote_port_a,
            local_port_c,
            &body_wildcard_list[..],
        );
        // Check that we got everything in order.
        let conn_packets = &ctx.get_ref().conn_data;
        assert_eq!(conn_packets.len(), 2);
        let pkt = &conn_packets[0];
        assert_eq!(pkt.conn, conn1);
        assert_eq!(pkt.body, &body_conn1[..]);
        let pkt = &conn_packets[1];
        assert_eq!(pkt.conn, conn2);
        assert_eq!(pkt.body, &body_conn2[..]);

        let list_packets = &ctx.get_ref().listen_data;
        assert_eq!(list_packets.len(), 3);
        let pkt = &list_packets[0];
        assert_eq!(pkt.listener, list1);
        assert_eq!(pkt.src_ip, remote_ip_a.get());
        assert_eq!(pkt.dst_ip, local_ip.get());
        assert_eq!(pkt.src_port.unwrap(), remote_port_a);
        assert_eq!(pkt.body, &body_list1[..]);

        let pkt = &list_packets[1];
        assert_eq!(pkt.listener, list2);
        assert_eq!(pkt.src_ip, remote_ip_a.get());
        assert_eq!(pkt.dst_ip, local_ip.get());
        assert_eq!(pkt.src_port.unwrap(), remote_port_a);
        assert_eq!(pkt.body, &body_list2[..]);

        let pkt = &list_packets[2];
        assert_eq!(pkt.listener, wildcard_list);
        assert_eq!(pkt.src_ip, remote_ip_a.get());
        assert_eq!(pkt.dst_ip, local_ip.get());
        assert_eq!(pkt.src_port.unwrap(), remote_port_a);
        assert_eq!(pkt.body, &body_wildcard_list[..]);
    }

    /// Tests UDP wildcard listeners for different IP versions.
    #[ip_test]
    fn test_wildcard_listeners<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_ip_a = I::get_other_ip_address(1);
        let local_ip_b = I::get_other_ip_address(2);
        let remote_ip_a = I::get_other_ip_address(70);
        let remote_ip_b = I::get_other_ip_address(72);
        let listener_port = NonZeroU16::new(100).unwrap();
        let remote_port = NonZeroU16::new(200).unwrap();
        let listener =
            listen_udp::<I, _>(&mut ctx, None, Some(listener_port)).expect("listen_udp failed");
        assert_eq!(listener.listener_type, ListenerType::Wildcard);

        let body = [1, 2, 3, 4, 5];
        receive_udp_packet(
            &mut ctx,
            remote_ip_a.get(),
            local_ip_a.get(),
            remote_port,
            listener_port,
            &body[..],
        );
        // Receive into a different local IP.
        receive_udp_packet(
            &mut ctx,
            remote_ip_b.get(),
            local_ip_b.get(),
            remote_port,
            listener_port,
            &body[..],
        );

        // Check that we received both packets for the listener.
        let listen_packets = &ctx.get_ref().listen_data;
        assert_eq!(listen_packets.len(), 2);
        let pkt = &listen_packets[0];
        assert_eq!(pkt.listener, listener);
        assert_eq!(pkt.src_ip, remote_ip_a.get());
        assert_eq!(pkt.dst_ip, local_ip_a.get());
        assert_eq!(pkt.src_port.unwrap(), remote_port);
        assert_eq!(pkt.body, &body[..]);
        let pkt = &listen_packets[1];
        assert_eq!(pkt.listener, listener);
        assert_eq!(pkt.src_ip, remote_ip_b.get());
        assert_eq!(pkt.dst_ip, local_ip_b.get());
        assert_eq!(pkt.src_port.unwrap(), remote_port);
        assert_eq!(pkt.body, &body[..]);
    }

    /// Tests establishing a UDP connection without providing a local IP
    #[ip_test]
    fn test_conn_unspecified_local_ip<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        set_logger_for_test();
        let mut ctx = DummyCtx::<I>::default();
        let local_port = NonZeroU16::new(100).unwrap();
        let remote_port = NonZeroU16::new(200).unwrap();
        let conn =
            connect_udp::<I, _>(&mut ctx, None, Some(local_port), remote_ip::<I>(), remote_port)
                .expect("connect_udp failed");
        let ConnSocketEntry { sock: _, addr } =
            ctx.get_ref().state.conn_state.conns.get_sock_by_id(conn.into()).unwrap();

        assert_eq!(addr.local_ip, local_ip::<I>());
        assert_eq!(addr.local_port, local_port);
        assert_eq!(addr.remote_ip, remote_ip::<I>());
        assert_eq!(addr.remote_port, remote_port);
    }

    /// Tests local port allocation for [`connect_udp`].
    ///
    /// Tests that calling [`connect_udp`] causes a valid local port to be
    /// allocated when no local port is passed.
    #[ip_test]
    fn test_udp_local_port_alloc<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
        DummyUdpCtx<I>: DummyUdpCtxExt<I>,
    {
        let local_ip = local_ip::<I>();
        let ip_a = I::get_other_ip_address(100);
        let ip_b = I::get_other_ip_address(200);

        let mut ctx = DummyCtx::<I>::with_state(DummyUdpCtx::with_local_remote_ip_addrs(
            vec![local_ip],
            vec![ip_a, ip_b],
        ));

        let conn_a = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            None,
            ip_a,
            NonZeroU16::new(1010).unwrap(),
        )
        .expect("connect_udp failed");
        let conn_b = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            None,
            ip_b,
            NonZeroU16::new(1010).unwrap(),
        )
        .expect("connect_udp failed");
        let conn_c = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            None,
            ip_a,
            NonZeroU16::new(2020).unwrap(),
        )
        .expect("connect_udp failed");
        let conn_d = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            None,
            ip_a,
            NonZeroU16::new(1010).unwrap(),
        )
        .expect("connect_udp failed");
        let conns = &ctx.get_ref().state.conn_state.conns;
        let valid_range = &UdpConnectionState::<I, IpSock<I, DummyDeviceId>>::EPHEMERAL_RANGE;
        let port_a = conns.get_sock_by_id(conn_a.into()).unwrap().addr.local_port.get();
        assert!(valid_range.contains(&port_a));
        let port_b = conns.get_sock_by_id(conn_b.into()).unwrap().addr.local_port.get();
        assert!(valid_range.contains(&port_b));
        assert_ne!(port_a, port_b);
        let port_c = conns.get_sock_by_id(conn_c.into()).unwrap().addr.local_port.get();
        assert!(valid_range.contains(&port_c));
        assert_ne!(port_a, port_c);
        let port_d = conns.get_sock_by_id(conn_d.into()).unwrap().addr.local_port.get();
        assert!(valid_range.contains(&port_d));
        assert_ne!(port_a, port_d);
    }

    /// Tests [`UdpConnectionState::collect_used_local_ports`]
    #[ip_test]
    fn test_udp_collect_local_ports<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
        DummyUdpCtx<I>: DummyUdpCtxExt<I>,
    {
        let local_ip = local_ip::<I>();
        let local_ip_2 = I::get_other_ip_address(10);

        let mut ctx = DummyCtx::<I>::with_state(DummyUdpCtx::with_local_remote_ip_addrs(
            vec![local_ip, local_ip_2],
            vec![remote_ip::<I>()],
        ));

        let remote_ip = remote_ip::<I>();
        ctx.get_mut().extra_local_addrs.push(local_ip_2.get());

        let pa = NonZeroU16::new(10).unwrap();
        let pb = NonZeroU16::new(11).unwrap();
        let pc = NonZeroU16::new(12).unwrap();
        let pd = NonZeroU16::new(13).unwrap();
        let pe = NonZeroU16::new(14).unwrap();
        let pf = NonZeroU16::new(15).unwrap();
        let remote_port = NonZeroU16::new(100).unwrap();

        // Create some listeners and connections.

        // Wildcard listeners
        assert_eq!(
            listen_udp::<I, _>(&mut ctx, None, Some(pa)),
            Ok(UdpListenerId::new_wildcard(0))
        );
        assert_eq!(
            listen_udp::<I, _>(&mut ctx, None, Some(pb)),
            Ok(UdpListenerId::new_wildcard(1))
        );
        // Specified address listeners
        assert_eq!(
            listen_udp::<I, _>(&mut ctx, Some(local_ip), Some(pc)),
            Ok(UdpListenerId::new_specified(0))
        );
        assert_eq!(
            listen_udp::<I, _>(&mut ctx, Some(local_ip_2), Some(pd)),
            Ok(UdpListenerId::new_specified(1))
        );
        // Connections
        assert_eq!(
            connect_udp::<I, _>(&mut ctx, Some(local_ip), Some(pe), remote_ip, remote_port),
            Ok(UdpConnId::new(0))
        );
        assert_eq!(
            connect_udp::<I, _>(&mut ctx, Some(local_ip_2), Some(pf), remote_ip, remote_port),
            Ok(UdpConnId::new(1))
        );

        let conn_state =
            &DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx).conn_state;

        // Collect all used local ports.
        assert_eq!(
            conn_state.collect_used_local_ports(None.into_iter()),
            [pa, pb, pc, pd, pe, pf].iter().copied().collect()
        );
        // Collect all local ports for local_ip.
        assert_eq!(
            conn_state.collect_used_local_ports(Some(local_ip).iter()),
            [pa, pb, pc, pe].iter().copied().collect()
        );
        // Collect all local ports for local_ip_2.
        assert_eq!(
            conn_state.collect_used_local_ports(Some(local_ip_2).iter()),
            [pa, pb, pd, pf].iter().copied().collect()
        );
        // Collect all local ports for local_ip and local_ip_2.
        assert_eq!(
            conn_state.collect_used_local_ports(vec![local_ip, local_ip_2].iter()),
            [pa, pb, pc, pd, pe, pf].iter().copied().collect()
        );
    }

    /// Tests local port allocation for [`listen_udp`].
    ///
    /// Tests that calling [`listen_udp`] causes a valid local port to be
    /// allocated when no local port is passed.
    #[ip_test]
    fn test_udp_listen_port_alloc<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();

        let wildcard_list = listen_udp::<I, _>(&mut ctx, None, None).expect("listen_udp failed");
        let specified_list =
            listen_udp::<I, _>(&mut ctx, Some(local_ip), None).expect("listen_udp failed");

        let conn_state =
            &DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx).conn_state;
        let wildcard_port = conn_state
            .wildcard_listeners
            .get_by_listener(wildcard_list.id)
            .unwrap()
            .first()
            .unwrap()
            .clone();
        let specified_port =
            conn_state.listeners.get_by_listener(specified_list.id).unwrap().first().unwrap().port;
        assert!(UdpConnectionState::<I, IpSock<I, DummyDeviceId>>::EPHEMERAL_RANGE
            .contains(&wildcard_port.get()));
        assert!(UdpConnectionState::<I, IpSock<I, DummyDeviceId>>::EPHEMERAL_RANGE
            .contains(&specified_port.get()));
        assert_ne!(wildcard_port, specified_port);
    }

    /// Tests [`remove_udp_conn`]
    #[ip_test]
    fn test_remove_udp_conn<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        let local_port = NonZeroU16::new(100).unwrap();
        let remote_port = NonZeroU16::new(200).unwrap();
        let conn =
            connect_udp::<I, _>(&mut ctx, Some(local_ip), Some(local_port), remote_ip, remote_port)
                .expect("connect_udp failed");
        let info = remove_udp_conn(&mut ctx, conn);
        // Assert that the info gotten back matches what was expected.
        assert_eq!(info.local_ip, local_ip);
        assert_eq!(info.local_port, local_port);
        assert_eq!(info.remote_ip, remote_ip);
        assert_eq!(info.remote_port, remote_port);

        // Assert that that connection id was removed from the connections
        // state.
        assert_eq!(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .conns
                .get_sock_by_id(conn.0),
            None
        );
    }

    /// Tests [`remove_udp_listener`]
    #[ip_test]
    fn test_remove_udp_listener<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let local_port = NonZeroU16::new(100).unwrap();

        // Test removing a specified listener.
        let list = listen_udp::<I, _>(&mut ctx, Some(local_ip), Some(local_port))
            .expect("listen_udp failed");
        let info = remove_udp_listener(&mut ctx, list);
        assert_eq!(info.local_ip.unwrap(), local_ip);
        assert_eq!(info.local_port, local_port);
        assert_eq!(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .listeners
                .get_by_listener(list.id),
            None
        );

        // Test removing a wildcard listener.
        let list = listen_udp::<I, _>(&mut ctx, None, Some(local_port)).expect("listen_udp failed");
        let info = remove_udp_listener(&mut ctx, list);
        assert_eq!(info.local_ip, None);
        assert_eq!(info.local_port, local_port);
        assert_eq!(
            DualStateContext::<UdpState<I, DummyDeviceId>, _>::get_first_state(&ctx)
                .conn_state
                .wildcard_listeners
                .get_by_listener(list.id),
            None
        );
    }

    #[ip_test]
    fn test_get_conn_info<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();
        let remote_ip = remote_ip::<I>();
        // Create a UDP connection with a specified local port and local IP.
        let conn = connect_udp::<I, _>(
            &mut ctx,
            Some(local_ip),
            NonZeroU16::new(100),
            remote_ip,
            NonZeroU16::new(200).unwrap(),
        )
        .expect("connect_udp failed");
        let info = get_udp_conn_info(&ctx, conn);
        assert_eq!(info.local_ip, local_ip);
        assert_eq!(info.local_port.get(), 100);
        assert_eq!(info.remote_ip, remote_ip);
        assert_eq!(info.remote_port.get(), 200);
    }

    #[ip_test]
    fn test_get_listener_info<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let local_ip = local_ip::<I>();

        // Check getting info on specified listener.
        let list = listen_udp::<I, _>(&mut ctx, Some(local_ip), NonZeroU16::new(100))
            .expect("listen_udp failed");
        let info = get_udp_listener_info(&ctx, list);
        assert_eq!(info.local_ip.unwrap(), local_ip);
        assert_eq!(info.local_port.get(), 100);

        // Check getting info on wildcard listener.
        let list =
            listen_udp::<I, _>(&mut ctx, None, NonZeroU16::new(200)).expect("listen_udp failed");
        let info = get_udp_listener_info(&ctx, list);
        assert_eq!(info.local_ip, None);
        assert_eq!(info.local_port.get(), 200);
    }

    #[ip_test]
    fn test_listen_udp_forwards_errors<I: Ip + TestIpExt>()
    where
        DummyCtx<I>: DummyCtxBound<I>,
    {
        let mut ctx = DummyCtx::<I>::default();
        let remote_ip = remote_ip::<I>();

        // Check listening to a non-local IP fails.
        let listen_err = listen_udp::<I, _>(&mut ctx, Some(remote_ip), NonZeroU16::new(100))
            .expect_err("listen_udp unexpectedly succeeded");
        assert_eq!(listen_err, LocalAddressError::CannotBindToAddress);

        let _ =
            listen_udp::<I, _>(&mut ctx, None, NonZeroU16::new(200)).expect("listen_udp failed");
        let listen_err = listen_udp::<I, _>(&mut ctx, None, NonZeroU16::new(200))
            .expect_err("listen_udp unexpectedly succeeded");
        assert_eq!(listen_err, LocalAddressError::AddressInUse);
    }

    /// Tests that incoming ICMP errors are properly delivered to a connection,
    /// a listener, and a wildcard listener.
    #[test]
    fn test_icmp_error() {
        // Create a context with:
        // - A wildcard listener on port 1
        // - A listener on the local IP and port 2
        // - A connection from the local IP to the remote IP on local port 2 and
        //   remote port 3
        fn initialize_context<I: TestIpExt>() -> DummyCtx<I>
        where
            DummyCtx<I>: DummyCtxBound<I>,
        {
            let mut ctx = DummyCtx::default();
            assert_eq!(
                listen_udp(&mut ctx, None, Some(NonZeroU16::new(1).unwrap())).unwrap(),
                UdpListenerId::new_wildcard(0)
            );
            assert_eq!(
                listen_udp(&mut ctx, Some(local_ip::<I>()), Some(NonZeroU16::new(2).unwrap()))
                    .unwrap(),
                UdpListenerId::new_specified(0)
            );
            assert_eq!(
                connect_udp(
                    &mut ctx,
                    Some(local_ip::<I>()),
                    Some(NonZeroU16::new(3).unwrap()),
                    remote_ip::<I>(),
                    NonZeroU16::new(4).unwrap(),
                )
                .unwrap(),
                UdpConnId::new(0)
            );
            ctx
        }

        // Serialize a UDP-in-IP packet with the given values, and then receive
        // an ICMP error message with that packet as the original packet.
        fn receive_icmp_error<I: TestIpExt, F: Fn(&mut DummyCtx<I>, &[u8], I::ErrorCode)>(
            ctx: &mut DummyCtx<I>,
            src_ip: I::Addr,
            dst_ip: I::Addr,
            src_port: u16,
            dst_port: u16,
            err: I::ErrorCode,
            f: F,
        ) where
            I::PacketBuilder: core::fmt::Debug,
        {
            let packet = (&[0u8][..])
                .into_serializer()
                .encapsulate(UdpPacketBuilder::new(
                    src_ip,
                    dst_ip,
                    NonZeroU16::new(src_port),
                    NonZeroU16::new(dst_port).unwrap(),
                ))
                .encapsulate(I::PacketBuilder::new(src_ip, dst_ip, 64, IpProto::Udp.into()))
                .serialize_vec_outer()
                .unwrap();
            f(ctx, packet.as_ref(), err);
        }

        fn test<I: TestIpExt + PartialEq, F: Copy + Fn(&mut DummyCtx<I>, &[u8], I::ErrorCode)>(
            err: I::ErrorCode,
            f: F,
            other_remote_ip: I::Addr,
        ) where
            I::PacketBuilder: core::fmt::Debug,
            I::ErrorCode: Copy + core::fmt::Debug + PartialEq,
            DummyCtx<I>: DummyCtxBound<I>,
        {
            let mut ctx = initialize_context::<I>();

            let src_ip = local_ip::<I>();
            let dst_ip = remote_ip::<I>();

            // Test that we receive an error for the connection.
            receive_icmp_error(&mut ctx, src_ip.get(), dst_ip.get(), 3, 4, err, f);
            assert_eq!(
                ctx.get_ref().icmp_errors.as_slice(),
                [IcmpError { id: Ok(UdpConnId::new(0)), err }]
            );

            // Test that we receive an error for the listener.
            receive_icmp_error(&mut ctx, src_ip.get(), dst_ip.get(), 2, 4, err, f);
            assert_eq!(
                &ctx.get_ref().icmp_errors.as_slice()[1..],
                [IcmpError { id: Err(UdpListenerId::new_specified(0)), err }]
            );

            // Test that we receive an error for the wildcard listener.
            receive_icmp_error(&mut ctx, src_ip.get(), dst_ip.get(), 1, 4, err, f);
            assert_eq!(
                &ctx.get_ref().icmp_errors.as_slice()[2..],
                [IcmpError { id: Err(UdpListenerId::new_wildcard(0)), err }]
            );

            // Test that we receive an error for the wildcard listener even if
            // the original packet was sent to a different remote IP/port.
            receive_icmp_error(&mut ctx, src_ip.get(), other_remote_ip, 1, 5, err, f);
            assert_eq!(
                &ctx.get_ref().icmp_errors.as_slice()[3..],
                [IcmpError { id: Err(UdpListenerId::new_wildcard(0)), err }]
            );

            // Test that an error that doesn't correspond to any connection or
            // listener isn't received.
            receive_icmp_error(&mut ctx, src_ip.get(), dst_ip.get(), 3, 5, err, f);
            assert_eq!(ctx.get_ref().icmp_errors.len(), 4);
        }

        test(
            Icmpv4ErrorCode::DestUnreachable(Icmpv4DestUnreachableCode::DestNetworkUnreachable),
            |ctx: &mut DummyCtx<Ipv4>, mut packet, error_code| {
                let packet = packet.parse::<Ipv4PacketRaw<_>>().unwrap();
                let src_ip = SpecifiedAddr::new(packet.src_ip());
                let dst_ip = SpecifiedAddr::new(packet.dst_ip()).unwrap();
                let body = packet.body().into_inner();
                <UdpIpTransportContext as IpTransportContext<Ipv4, _>>::receive_icmp_error(
                    ctx, src_ip, dst_ip, body, error_code,
                )
            },
            Ipv4Addr::new([1, 2, 3, 4]),
        );

        test(
            Icmpv6ErrorCode::DestUnreachable(Icmpv6DestUnreachableCode::NoRoute),
            |ctx: &mut DummyCtx<Ipv6>, mut packet, error_code| {
                let packet = packet.parse::<Ipv6PacketRaw<_>>().unwrap();
                let src_ip = SpecifiedAddr::new(packet.src_ip());
                let dst_ip = SpecifiedAddr::new(packet.dst_ip()).unwrap();
                let body = packet.body().unwrap().into_inner();
                <UdpIpTransportContext as IpTransportContext<Ipv6, _>>::receive_icmp_error(
                    ctx, src_ip, dst_ip, body, error_code,
                )
            },
            Ipv6Addr::from_bytes([1, 2, 3, 4, 5, 6, 7, 8, 1, 2, 3, 4, 5, 6, 7, 8]),
        );
    }
}
