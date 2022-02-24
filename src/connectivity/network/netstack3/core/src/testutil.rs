// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//! Testing-related utilities.

use alloc::{
    borrow::ToOwned,
    collections::{BinaryHeap, HashMap},
    string::{String, ToString},
    vec,
    vec::Vec,
};
use core::{fmt::Debug, hash::Hash, time::Duration};

use assert_matches::assert_matches;
use log::{debug, trace};
use net_types::{
    ethernet::Mac,
    ip::{AddrSubnet, Ip, IpAddr, IpAddress, Ipv4, Ipv4Addr, Ipv6, Ipv6Addr, Subnet, SubnetEither},
    SpecifiedAddr, UnicastAddr, Witness,
};
use packet::{Buf, BufferMut, Serializer};
use packet_formats::ip::IpProto;
use rand::{self, CryptoRng, Rng as _, RngCore, SeedableRng};
use rand_xorshift::XorShiftRng;

use crate::{
    context::{
        testutil::{DummyInstant, InstantAndData},
        InstantContext, RngContext, TimerContext,
    },
    device::{DeviceId, DeviceLayerEventDispatcher},
    handle_timer,
    ip::{
        icmp::{BufferIcmpContext, IcmpConnId, IcmpContext, IcmpIpExt},
        socket::IpSockCreationError,
    },
    transport::udp::{BufferUdpContext, UdpContext},
    Ctx, EntryDest, EntryEither, EventDispatcher, Instant, StackStateBuilder, TimerId,
};

/// Utilities to allow running benchmarks as tests.
///
/// Our benchmarks rely on the unstable `test` feature, which is disallowed in
/// Fuchsia's build system. In order to ensure that our benchmarks are always
/// compiled and tested, this module provides mocks that allow us to run our
/// benchmarks as normal tests when the `benchmark` feature is disabled.
///
/// See the `bench!` macro for details on how this module is used.
pub(crate) mod benchmarks {
    /// A trait to allow mocking of the `test::Bencher` type.
    pub(crate) trait Bencher {
        fn iter<T, F: FnMut() -> T>(&mut self, inner: F);
    }

    #[cfg(feature = "benchmark")]
    impl Bencher for test::Bencher {
        fn iter<T, F: FnMut() -> T>(&mut self, inner: F) {
            test::Bencher::iter(self, inner)
        }
    }

    /// A `Bencher` whose `iter` method runs the provided argument once.
    #[cfg(not(feature = "benchmark"))]
    pub(crate) struct TestBencher;

    #[cfg(not(feature = "benchmark"))]
    impl Bencher for TestBencher {
        fn iter<T, F: FnMut() -> T>(&mut self, mut inner: F) {
            super::set_logger_for_test();
            let _: T = inner();
        }
    }

    #[inline(always)]
    pub(crate) fn black_box<T>(dummy: T) -> T {
        #[cfg(feature = "benchmark")]
        return test::black_box(dummy);
        #[cfg(not(feature = "benchmark"))]
        return dummy;
    }
}

pub(crate) type DummyCtx = Ctx<DummyEventDispatcher>;

/// A wrapper which implements `RngCore` and `CryptoRng` for any `RngCore`.
///
/// This is used to satisfy [`EventDispatcher`]'s requirement that the
/// associated `Rng` type implements `CryptoRng`.
///
/// # Security
///
/// This is obviously insecure. Don't use it except in testing!
#[derive(Clone)]
pub(crate) struct FakeCryptoRng<R>(R);

impl Default for FakeCryptoRng<XorShiftRng> {
    fn default() -> FakeCryptoRng<XorShiftRng> {
        FakeCryptoRng::new_xorshift(12957992561116578403)
    }
}

impl FakeCryptoRng<XorShiftRng> {
    /// Creates a new [`FakeCryptoRng<XorShiftRng>`] from a seed.
    pub(crate) fn new_xorshift(seed: u128) -> FakeCryptoRng<XorShiftRng> {
        FakeCryptoRng(new_rng(seed))
    }
}

impl<R: RngCore> RngCore for FakeCryptoRng<R> {
    fn next_u32(&mut self) -> u32 {
        self.0.next_u32()
    }
    fn next_u64(&mut self) -> u64 {
        self.0.next_u64()
    }
    fn fill_bytes(&mut self, dest: &mut [u8]) {
        self.0.fill_bytes(dest)
    }
    fn try_fill_bytes(&mut self, dest: &mut [u8]) -> Result<(), rand::Error> {
        self.0.try_fill_bytes(dest)
    }
}

impl<R: RngCore> CryptoRng for FakeCryptoRng<R> {}

impl<R: RngCore> crate::context::RngContext for FakeCryptoRng<R> {
    type Rng = Self;

    fn rng(&self) -> &Self::Rng {
        self
    }

    fn rng_mut(&mut self) -> &mut Self::Rng {
        self
    }
}

/// Create a new deterministic RNG from a seed.
pub(crate) fn new_rng(mut seed: u128) -> XorShiftRng {
    if seed == 0 {
        // XorShiftRng can't take 0 seeds
        seed = 1;
    }
    XorShiftRng::from_seed(seed.to_ne_bytes())
}

/// Creates `iterations` fake RNGs.
///
/// `with_fake_rngs` will create `iterations` different [`FakeCryptoRng`]s and
/// call the function `f` for each one of them.
///
/// This function can be used for tests that weed out weirdness that can
/// happen with certain random number sequences.
pub(crate) fn with_fake_rngs<F: Fn(FakeCryptoRng<XorShiftRng>)>(iterations: u128, f: F) {
    for seed in 0..iterations {
        f(FakeCryptoRng::new_xorshift(seed))
    }
}

/// Invokes a function multiple times with different RNG seeds.
pub(crate) fn run_with_many_seeds<F: FnMut(u128)>(mut f: F) {
    // Arbitrary seed.
    let mut rng = new_rng(0x0fe50fae6c37593d71944697f1245847);
    for _ in 0..64 {
        f(rng.gen());
    }
}

#[derive(Default, Debug)]
pub(crate) struct TestCounters {
    data: HashMap<String, usize>,
}

impl TestCounters {
    pub(crate) fn increment(&mut self, key: &str) {
        *(self.data.entry(key.to_string()).or_insert(0)) += 1;
    }

    pub(crate) fn get(&self, key: &str) -> &usize {
        self.data.get(key).unwrap_or(&0)
    }
}

/// log::Log implementation that uses stdout.
///
/// Useful when debugging tests.
struct Logger;

impl log::Log for Logger {
    fn enabled(&self, _metadata: &log::Metadata<'_>) -> bool {
        true
    }

    fn log(&self, record: &log::Record<'_>) {
        teststd::println!("{}", record.args())
    }

    fn flush(&self) {}
}

static LOGGER: Logger = Logger;

static LOGGER_ONCE: core::sync::atomic::AtomicBool = core::sync::atomic::AtomicBool::new(true);

/// Install a logger for tests.
///
/// Call this method at the beginning of the test for which logging is desired.
/// This function sets global program state, so all tests that run after this
/// function is called will use the logger.
pub(crate) fn set_logger_for_test() {
    // log::set_logger will panic if called multiple times.
    if LOGGER_ONCE.swap(false, core::sync::atomic::Ordering::AcqRel) {
        log::set_logger(&LOGGER).unwrap();
        log::set_max_level(log::LevelFilter::Trace);
    }
}

/// Skip current time forward to trigger the next timer event.
///
/// Returns the `TimerId` if a timer was triggered, `None` if there were no
/// timers waiting to be triggered.
pub(crate) fn trigger_next_timer(ctx: &mut DummyCtx) -> Option<TimerId> {
    match ctx.dispatcher.timer_events.pop() {
        Some(InstantAndData(t, id)) => {
            ctx.dispatcher.current_time = t;
            handle_timer(ctx, id);
            Some(id)
        }
        None => None,
    }
}

/// Skip current time forward by `duration`, triggering all timer events until
/// then, inclusive.
///
/// Returns the `TimerId` of the timer events triggered.
pub(crate) fn run_for(ctx: &mut DummyCtx, duration: Duration) -> Vec<TimerId> {
    let end_time = ctx.dispatcher.now() + duration;
    let mut timer_ids = Vec::new();

    while let Some(tmr) = ctx.dispatcher.timer_events.peek() {
        if tmr.0 > end_time {
            break;
        }

        let timer_id = trigger_next_timer(ctx);
        assert_matches!(timer_id, Some(_));
        timer_ids.push(timer_id.unwrap());
    }

    assert!(ctx.dispatcher.now() <= end_time);
    ctx.dispatcher.current_time = end_time;

    timer_ids
}

/// Trigger timer events until`f` callback returns true or passes the max
/// number of iterations.
///
/// `trigger_timers_until` always calls `f` on the first timer event, as the
/// timer_events is dynamically updated. As soon as `f` returns true or
/// 1,000,000 timer events have been triggered, `trigger_timers_until` will
/// exit.
///
/// Please note, the caller is expected to pass in an `f` which could return
/// true to exit `trigger_timer_until`. 1,000,000 limit is set to avoid an
/// endless loop.
pub(crate) fn trigger_timers_until<F: Fn(&TimerId) -> bool>(ctx: &mut DummyCtx, f: F) {
    for _ in 0..1_000_000 {
        let InstantAndData(t, id) = if let Some(t) = ctx.dispatcher.timer_events.pop() {
            t
        } else {
            return;
        };

        ctx.dispatcher.current_time = t;
        handle_timer(ctx, id);
        if f(&id) {
            break;
        }
    }
}

/// Get the counter value for a `key`.
pub(crate) fn get_counter_val(ctx: &DummyCtx, key: &str) -> usize {
    *ctx.state.test_counters.borrow().get(key)
}

/// An extension trait for `Ip` providing test-related functionality.
pub(crate) trait TestIpExt: Ip {
    /// Either [`DUMMY_CONFIG_V4`] or [`DUMMY_CONFIG_V6`].
    const DUMMY_CONFIG: DummyEventDispatcherConfig<Self::Addr>;

    /// Get an IP address in the same subnet as `Self::DUMMY_CONFIG`.
    ///
    /// `last` is the value to be put in the last octet of the IP address.
    fn get_other_ip_address(last: u8) -> SpecifiedAddr<Self::Addr>;

    /// Get an IP address in a different subnet from `Self::DUMMY_CONFIG`.
    ///
    /// `last` is the value to be put in the last octet of the IP address.
    fn get_other_remote_ip_address(last: u8) -> SpecifiedAddr<Self::Addr>;
}

impl TestIpExt for Ipv4 {
    const DUMMY_CONFIG: DummyEventDispatcherConfig<Ipv4Addr> = DUMMY_CONFIG_V4;

    fn get_other_ip_address(last: u8) -> SpecifiedAddr<Ipv4Addr> {
        let mut bytes = Self::DUMMY_CONFIG.local_ip.get().ipv4_bytes();
        bytes[bytes.len() - 1] = last;
        SpecifiedAddr::new(Ipv4Addr::new(bytes)).unwrap()
    }

    fn get_other_remote_ip_address(last: u8) -> SpecifiedAddr<Self::Addr> {
        let mut bytes = Self::DUMMY_CONFIG.local_ip.get().ipv4_bytes();
        bytes[bytes.len() - 3] += 1;
        bytes[bytes.len() - 1] = last;
        SpecifiedAddr::new(Ipv4Addr::new(bytes)).unwrap()
    }
}

impl TestIpExt for Ipv6 {
    const DUMMY_CONFIG: DummyEventDispatcherConfig<Ipv6Addr> = DUMMY_CONFIG_V6;

    fn get_other_ip_address(last: u8) -> SpecifiedAddr<Ipv6Addr> {
        let mut bytes = Self::DUMMY_CONFIG.local_ip.get().ipv6_bytes();
        bytes[bytes.len() - 1] = last;
        SpecifiedAddr::new(Ipv6Addr::from(bytes)).unwrap()
    }

    fn get_other_remote_ip_address(last: u8) -> SpecifiedAddr<Self::Addr> {
        let mut bytes = Self::DUMMY_CONFIG.local_ip.get().ipv6_bytes();
        bytes[bytes.len() - 3] += 1;
        bytes[bytes.len() - 1] = last;
        SpecifiedAddr::new(Ipv6Addr::from(bytes)).unwrap()
    }
}

/// A configuration for a simple network.
///
/// `DummyEventDispatcherConfig` describes a simple network with two IP hosts
/// - one remote and one local - both on the same Ethernet network.
#[derive(Clone)]
pub(crate) struct DummyEventDispatcherConfig<A: IpAddress> {
    /// The subnet of the local Ethernet network.
    pub(crate) subnet: Subnet<A>,
    /// The IP address of our interface to the local network (must be in
    /// subnet).
    pub(crate) local_ip: SpecifiedAddr<A>,
    /// The MAC address of our interface to the local network.
    pub(crate) local_mac: UnicastAddr<Mac>,
    /// The remote host's IP address (must be in subnet if provided).
    pub(crate) remote_ip: SpecifiedAddr<A>,
    /// The remote host's MAC address.
    pub(crate) remote_mac: UnicastAddr<Mac>,
}

/// A `DummyEventDispatcherConfig` with reasonable values for an IPv4 network.
pub(crate) const DUMMY_CONFIG_V4: DummyEventDispatcherConfig<Ipv4Addr> = unsafe {
    DummyEventDispatcherConfig {
        subnet: Subnet::new_unchecked(Ipv4Addr::new([192, 168, 0, 0]), 16),
        local_ip: SpecifiedAddr::new_unchecked(Ipv4Addr::new([192, 168, 0, 1])),
        local_mac: UnicastAddr::new_unchecked(Mac::new([0, 1, 2, 3, 4, 5])),
        remote_ip: SpecifiedAddr::new_unchecked(Ipv4Addr::new([192, 168, 0, 2])),
        remote_mac: UnicastAddr::new_unchecked(Mac::new([6, 7, 8, 9, 10, 11])),
    }
};

/// A `DummyEventDispatcherConfig` with reasonable values for an IPv6 network.
pub(crate) const DUMMY_CONFIG_V6: DummyEventDispatcherConfig<Ipv6Addr> = unsafe {
    DummyEventDispatcherConfig {
        subnet: Subnet::new_unchecked(
            Ipv6Addr::from_bytes([0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 0]),
            112,
        ),
        local_ip: SpecifiedAddr::new_unchecked(Ipv6Addr::from_bytes([
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 1,
        ])),
        local_mac: UnicastAddr::new_unchecked(Mac::new([0, 1, 2, 3, 4, 5])),
        remote_ip: SpecifiedAddr::new_unchecked(Ipv6Addr::from_bytes([
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 192, 168, 0, 2,
        ])),
        remote_mac: UnicastAddr::new_unchecked(Mac::new([6, 7, 8, 9, 10, 11])),
    }
};

impl<A: IpAddress> DummyEventDispatcherConfig<A> {
    /// Creates a copy of `self` with all the remote and local fields reversed.
    pub(crate) fn swap(&self) -> Self {
        Self {
            subnet: self.subnet,
            local_ip: self.remote_ip,
            local_mac: self.remote_mac,
            remote_ip: self.local_ip,
            remote_mac: self.local_mac,
        }
    }
}

/// A builder for `DummyEventDispatcher`s.
///
/// A `DummyEventDispatcherBuilder` is capable of storing the configuration of a
/// network stack including forwarding table entries, devices and their assigned
/// IP addresses, ARP table entries, etc. It can be built using `build`,
/// producing a `Context<DummyEventDispatcher>` with all of the appropriate
/// state configured.
#[derive(Clone, Default)]
pub(crate) struct DummyEventDispatcherBuilder {
    devices: Vec<(UnicastAddr<Mac>, Option<(IpAddr, SubnetEither)>)>,
    arp_table_entries: Vec<(usize, Ipv4Addr, UnicastAddr<Mac>)>,
    ndp_table_entries: Vec<(usize, UnicastAddr<Ipv6Addr>, UnicastAddr<Mac>)>,
    // usize refers to index into devices Vec.
    device_routes: Vec<(SubnetEither, usize)>,
    routes: Vec<(SubnetEither, SpecifiedAddr<IpAddr>)>,
}

impl DummyEventDispatcherBuilder {
    /// Construct a `DummyEventDispatcherBuilder` from a
    /// `DummyEventDispatcherConfig`.
    pub(crate) fn from_config<A: IpAddress>(
        cfg: DummyEventDispatcherConfig<A>,
    ) -> DummyEventDispatcherBuilder {
        assert!(cfg.subnet.contains(&cfg.local_ip));
        assert!(cfg.subnet.contains(&cfg.remote_ip));

        let mut builder = DummyEventDispatcherBuilder::default();
        builder.devices.push((cfg.local_mac, Some((cfg.local_ip.get().into(), cfg.subnet.into()))));

        match cfg.remote_ip.get().into() {
            IpAddr::V4(ip) => builder.arp_table_entries.push((0, ip, cfg.remote_mac)),
            IpAddr::V6(ip) => {
                builder.ndp_table_entries.push((0, UnicastAddr::new(ip).unwrap(), cfg.remote_mac))
            }
        };

        // Even with fixed ipv4 address we can have IPv6 link local addresses
        // pre-cached.
        builder.ndp_table_entries.push((
            0,
            cfg.remote_mac.to_ipv6_link_local().addr().get(),
            cfg.remote_mac,
        ));

        builder.device_routes.push((cfg.subnet.into(), 0));
        builder
    }

    /// Add a device.
    ///
    /// `add_device` returns a key which can be used to refer to the device in
    /// future calls to `add_arp_table_entry` and `add_device_route`.
    pub(crate) fn add_device(&mut self, mac: UnicastAddr<Mac>) -> usize {
        let idx = self.devices.len();
        self.devices.push((mac, None));
        idx
    }

    /// Add a device with an associated IP address.
    ///
    /// `add_device_with_ip` is like `add_device`, except that it takes an
    /// associated IP address and subnet to assign to the device.
    pub(crate) fn add_device_with_ip<A: IpAddress>(
        &mut self,
        mac: UnicastAddr<Mac>,
        ip: A,
        subnet: Subnet<A>,
    ) {
        let idx = self.devices.len();
        self.devices.push((mac, Some((ip.into(), subnet.into()))));
        self.device_routes.push((subnet.into(), idx));
    }

    /// Add an ARP table entry for a device's ARP table.
    pub(crate) fn add_arp_table_entry(
        &mut self,
        device: usize,
        ip: Ipv4Addr,
        mac: UnicastAddr<Mac>,
    ) {
        self.arp_table_entries.push((device, ip, mac));
    }

    /// Add an NDP table entry for a device's NDP table.
    pub(crate) fn add_ndp_table_entry(
        &mut self,
        device: usize,
        ip: UnicastAddr<Ipv6Addr>,
        mac: UnicastAddr<Mac>,
    ) {
        self.ndp_table_entries.push((device, ip, mac));
    }

    /// Build a `Ctx` from the present configuration with a default dispatcher,
    /// and stack state set to disable NDP's Duplicate Address Detection by
    /// default.
    pub(crate) fn build(self) -> DummyCtx {
        self.build_with_modifications(|_| {})
    }

    /// `build_with_modifications` is equivalent to `build`, except that after
    /// the `StackStateBuilder` is initialized, it is passed to `f` for further
    /// modification before the `Ctx` is constructed.
    pub(crate) fn build_with_modifications<F: FnOnce(&mut StackStateBuilder)>(
        self,
        f: F,
    ) -> DummyCtx {
        let mut stack_builder = StackStateBuilder::default();

        // Most tests do not need NDP's DAD or router solicitation so disable it
        // here.
        let mut ndp_config = crate::device::ndp::NdpConfiguration::default();
        ndp_config.set_max_router_solicitations(None);
        stack_builder.device_builder().set_default_ndp_config(ndp_config);
        let mut ipv6_config = crate::ip::device::state::Ipv6DeviceConfiguration::default();
        ipv6_config.dad_transmits = None;
        stack_builder.device_builder().set_default_ipv6_config(ipv6_config);

        f(&mut stack_builder);
        self.build_with(stack_builder, Default::default())
    }

    /// Build a `Ctx` from the present configuration with a caller-provided
    /// dispatcher and `StackStateBuilder`.
    pub(crate) fn build_with<D: EventDispatcher>(
        self,
        state_builder: StackStateBuilder,
        dispatcher: D,
    ) -> Ctx<D> {
        let mut ctx = Ctx::new(state_builder.build(), dispatcher);

        let DummyEventDispatcherBuilder {
            devices,
            arp_table_entries,
            ndp_table_entries,
            device_routes,
            routes,
        } = self;
        let idx_to_device_id: HashMap<_, _> = devices
            .into_iter()
            .enumerate()
            .map(|(idx, (mac, ip_subnet))| {
                let id = ctx.state.add_ethernet_device(mac, Ipv6::MINIMUM_LINK_MTU.into());
                crate::device::initialize_device(&mut ctx, id);
                match ip_subnet {
                    Some((IpAddr::V4(ip), SubnetEither::V4(subnet))) => {
                        let addr_sub = AddrSubnet::new(ip, subnet.prefix()).unwrap();
                        crate::device::add_ip_addr_subnet(&mut ctx, id, addr_sub).unwrap();
                    }
                    Some((IpAddr::V6(ip), SubnetEither::V6(subnet))) => {
                        let addr_sub = AddrSubnet::new(ip, subnet.prefix()).unwrap();
                        crate::device::add_ip_addr_subnet(&mut ctx, id, addr_sub).unwrap();
                    }
                    None => {}
                    _ => unreachable!(),
                }
                (idx, id)
            })
            .collect();
        for (idx, ip, mac) in arp_table_entries {
            let device = *idx_to_device_id.get(&idx).unwrap();
            crate::device::insert_static_arp_table_entry(&mut ctx, device, ip, mac)
                .expect("error inserting static ARP entry");
        }
        for (idx, ip, mac) in ndp_table_entries {
            let device = *idx_to_device_id.get(&idx).unwrap();
            crate::device::insert_ndp_table_entry(&mut ctx, device, ip, mac.get())
                .expect("error inserting static NDP entry");
        }
        for (subnet, idx) in device_routes {
            let device = *idx_to_device_id.get(&idx).unwrap();
            crate::add_route(
                &mut ctx,
                EntryEither::new(subnet, EntryDest::Local { device })
                    .expect("valid forwarding table entry"),
            )
            .expect("add device route");
        }
        for (subnet, next_hop) in routes {
            crate::add_route(
                &mut ctx,
                EntryEither::new(subnet, EntryDest::Remote { next_hop })
                    .expect("valid forwarding table entry"),
            )
            .expect("add remote route");
        }

        ctx
    }
}

/// Add either an NDP entry (if IPv6) or ARP entry (if IPv4) to a
/// `DummyEventDispatcherBuilder`.
pub(crate) fn add_arp_or_ndp_table_entry<A: IpAddress>(
    builder: &mut DummyEventDispatcherBuilder,
    device: usize,
    ip: A,
    mac: UnicastAddr<Mac>,
) {
    match ip.into() {
        IpAddr::V4(ip) => builder.add_arp_table_entry(device, ip, mac),
        IpAddr::V6(ip) => builder.add_ndp_table_entry(device, UnicastAddr::new(ip).unwrap(), mac),
    }
}

type PendingTimer = InstantAndData<TimerId>;

/// A dummy `EventDispatcher` used for testing.
///
/// A `DummyEventDispatcher` implements the `EventDispatcher` interface for
/// testing purposes. It provides facilities to inspect the history of what
/// events have been emitted to the system.
pub(crate) struct DummyEventDispatcher {
    frames_sent: Vec<(DeviceId, Vec<u8>)>,
    timer_events: BinaryHeap<PendingTimer>,
    current_time: DummyInstant,
    rng: FakeCryptoRng<XorShiftRng>,
    icmpv4_replies: HashMap<IcmpConnId<Ipv4>, Vec<(u16, Vec<u8>)>>,
    icmpv6_replies: HashMap<IcmpConnId<Ipv6>, Vec<(u16, Vec<u8>)>>,
}

impl Default for DummyEventDispatcher {
    fn default() -> DummyEventDispatcher {
        DummyEventDispatcher {
            frames_sent: Default::default(),
            timer_events: Default::default(),
            current_time: Default::default(),
            rng: FakeCryptoRng(new_rng(0)),
            icmpv4_replies: Default::default(),
            icmpv6_replies: Default::default(),
        }
    }
}

impl DummyEventDispatcher {
    pub(crate) fn rng(&self) -> &FakeCryptoRng<XorShiftRng> {
        &self.rng
    }
}

pub(crate) trait TestutilIpExt: Ip {
    fn icmp_replies(
        evt: &mut DummyEventDispatcher,
    ) -> &mut HashMap<IcmpConnId<Self>, Vec<(u16, Vec<u8>)>>;
}

impl TestutilIpExt for Ipv4 {
    fn icmp_replies(
        evt: &mut DummyEventDispatcher,
    ) -> &mut HashMap<IcmpConnId<Ipv4>, Vec<(u16, Vec<u8>)>> {
        &mut evt.icmpv4_replies
    }
}

impl TestutilIpExt for Ipv6 {
    fn icmp_replies(
        evt: &mut DummyEventDispatcher,
    ) -> &mut HashMap<IcmpConnId<Ipv6>, Vec<(u16, Vec<u8>)>> {
        &mut evt.icmpv6_replies
    }
}

impl DummyEventDispatcher {
    pub(crate) fn frames_sent(&self) -> &[(DeviceId, Vec<u8>)] {
        &self.frames_sent
    }

    /// Get an ordered list of all scheduled timer events
    pub(crate) fn timer_events(&self) -> impl Iterator<Item = (&'_ DummyInstant, &'_ TimerId)> {
        // TODO(joshlf): `iter` doesn't actually guarantee an ordering, so this
        // is a bug. We plan on removing this soon once we migrate to using the
        // utilities in the `context` module, so this is left as-is.
        self.timer_events.iter().map(|t| (&t.0, &t.1))
    }

    /// Takes all the received ICMP replies for a given `conn`.
    pub(crate) fn take_icmp_replies<I: TestutilIpExt>(
        &mut self,
        conn: IcmpConnId<I>,
    ) -> Vec<(u16, Vec<u8>)> {
        I::icmp_replies(self).remove(&conn).unwrap_or_else(Vec::default)
    }
}

impl<I: IcmpIpExt> UdpContext<I> for DummyEventDispatcher {}

impl<I: crate::ip::IpExt, B: BufferMut> BufferUdpContext<I, B> for DummyEventDispatcher {}

impl<I: IcmpIpExt> IcmpContext<I> for DummyEventDispatcher {
    fn receive_icmp_error(&mut self, _conn: IcmpConnId<I>, _seq_num: u16, _err: I::ErrorCode) {
        unimplemented!()
    }

    fn close_icmp_connection(&mut self, _conn: IcmpConnId<I>, _err: IpSockCreationError) {
        unimplemented!()
    }
}

impl<B: BufferMut> BufferIcmpContext<Ipv4, B> for DummyEventDispatcher {
    fn receive_icmp_echo_reply(
        &mut self,
        conn: IcmpConnId<Ipv4>,
        _src_ip: Ipv4Addr,
        _dst_ip: Ipv4Addr,
        _id: u16,
        seq_num: u16,
        data: B,
    ) {
        let replies = self.icmpv4_replies.entry(conn).or_insert_with(Vec::default);
        replies.push((seq_num, data.as_ref().to_owned()))
    }
}

impl<B: BufferMut> BufferIcmpContext<Ipv6, B> for DummyEventDispatcher {
    fn receive_icmp_echo_reply(
        &mut self,
        conn: IcmpConnId<Ipv6>,
        _src_ip: Ipv6Addr,
        _dst_ip: Ipv6Addr,
        _id: u16,
        seq_num: u16,
        data: B,
    ) {
        let replies = self.icmpv6_replies.entry(conn).or_insert_with(Vec::default);
        replies.push((seq_num, data.as_ref().to_owned()))
    }
}

impl<B: BufferMut> DeviceLayerEventDispatcher<B> for DummyEventDispatcher {
    fn send_frame<S: Serializer<Buffer = B>>(
        &mut self,
        device: DeviceId,
        frame: S,
    ) -> Result<(), S> {
        let frame = frame.serialize_vec_outer().map_err(|(_, ser)| ser)?;
        self.frames_sent.push((device, frame.as_ref().to_vec()));
        Ok(())
    }
}

impl InstantContext for DummyEventDispatcher {
    type Instant = DummyInstant;

    fn now(&self) -> DummyInstant {
        self.current_time
    }
}

impl RngContext for DummyEventDispatcher {
    type Rng = FakeCryptoRng<XorShiftRng>;

    fn rng(&self) -> &FakeCryptoRng<XorShiftRng> {
        &self.rng
    }

    fn rng_mut(&mut self) -> &mut FakeCryptoRng<XorShiftRng> {
        &mut self.rng
    }
}

impl TimerContext<TimerId> for DummyEventDispatcher {
    fn schedule_timer(&mut self, duration: Duration, id: TimerId) -> Option<DummyInstant> {
        self.schedule_timer_instant(self.current_time + duration, id)
    }

    fn schedule_timer_instant(&mut self, time: DummyInstant, id: TimerId) -> Option<DummyInstant> {
        let ret = self.cancel_timer(id);
        self.timer_events.push(PendingTimer::new(time, id));
        ret
    }

    fn cancel_timer(&mut self, id: TimerId) -> Option<DummyInstant> {
        let mut r: Option<DummyInstant> = None;
        // NOTE(brunodalbo): cancelling timers can be made a faster than this
        //  if we kept two data structures and TimerId was Hashable.
        self.timer_events = self
            .timer_events
            .drain()
            .filter(|t| {
                if t.1 == id {
                    r = Some(t.0);
                    false
                } else {
                    true
                }
            })
            .collect::<Vec<_>>()
            .into();
        r
    }

    fn cancel_timers_with<F: FnMut(&TimerId) -> bool>(&mut self, mut f: F) {
        self.timer_events =
            self.timer_events.drain().filter(|t| !f(&t.1)).collect::<Vec<_>>().into();
    }

    fn scheduled_instant(&self, id: TimerId) -> Option<DummyInstant> {
        self.timer_events.iter().find_map(|x| if x.1 == id { Some(x.0) } else { None })
    }
}

#[derive(Debug)]
struct PendingFrameData<N> {
    data: Vec<u8>,
    dst_context: N,
    dst_device: DeviceId,
}

type PendingFrame<N> = InstantAndData<PendingFrameData<N>>;

/// A dummy network, composed of many `Ctx`s backed by
/// `DummyEventDispatcher`s.
///
/// Provides a quick utility to have many contexts keyed by `N` that can
/// exchange frames between their interfaces, which are mapped by `mapper`.
/// `mapper` also provides the option to return a `Duration` parameter that is
/// interpreted as a delivery latency for a given packet.
pub(crate) struct DummyNetwork<
    N: Eq + Hash + Clone,
    F: Fn(&N, DeviceId) -> Vec<(N, DeviceId, Option<Duration>)>,
> {
    contexts: HashMap<N, DummyCtx>,
    mapper: F,
    current_time: DummyInstant,
    pending_frames: BinaryHeap<PendingFrame<N>>,
}

/// The result of a single step in a `DummyNetwork`
#[derive(Debug)]
pub(crate) struct StepResult {
    _time_delta: Duration,
    timers_fired: usize,
    frames_sent: usize,
}

impl StepResult {
    fn new(time_delta: Duration, timers_fired: usize, frames_sent: usize) -> Self {
        Self { _time_delta: time_delta, timers_fired, frames_sent }
    }

    fn new_idle() -> Self {
        Self::new(Duration::from_millis(0), 0, 0)
    }

    /// Returns `true` if the last step did not perform any operations.
    pub(crate) fn is_idle(&self) -> bool {
        return self.timers_fired == 0 && self.frames_sent == 0;
    }

    /// Returns the number of frames dispatched to their destinations in the
    /// last step.
    pub(crate) fn frames_sent(&self) -> usize {
        self.frames_sent
    }

    /// Returns the number of timers fired in the last step.
    pub(crate) fn timers_fired(&self) -> usize {
        self.timers_fired
    }
}

/// Error type that marks that one of the `run_until` family of functions
/// reached a maximum number of iterations.
#[derive(Debug)]
pub(crate) struct LoopLimitReachedError;

impl<N, F> DummyNetwork<N, F>
where
    N: Eq + Hash + Clone + core::fmt::Debug,
    F: Fn(&N, DeviceId) -> Vec<(N, DeviceId, Option<Duration>)>,
{
    /// Creates a new `DummyNetwork`.
    ///
    /// Creates a new `DummyNetwork` with the collection of `Ctx`s in
    /// `contexts`. `Ctx`s are named by type parameter `N`. `mapper` is used
    /// to route frames from one pair of (named `Ctx`, `DeviceId`) to
    /// another.
    ///
    /// # Panics
    ///
    /// `mapper` must map to a valid name, otherwise calls to `step` will panic.
    ///
    /// Calls to `new` will panic if given a `Ctx` with timer events.
    /// `Ctx`s given to `DummyNetwork` **must not** have any timer events
    /// already attached to them, because `DummyNetwork` maintains all the
    /// internal timers in dispatchers in sync to enable synchronous simulation
    /// steps.
    pub(crate) fn new<I: Iterator<Item = (N, DummyCtx)>>(contexts: I, mapper: F) -> Self {
        let mut ret = Self {
            contexts: contexts.collect(),
            mapper,
            current_time: DummyInstant::default(),
            pending_frames: BinaryHeap::new(),
        };

        // We can't guarantee that all contexts are safely running their timers
        // together if we receive a context with any timers already set.
        assert!(
            !ret.contexts.iter().any(|(_n, ctx)| { !ctx.dispatcher.timer_events.is_empty() }),
            "can't start network with contexts that already have timers set"
        );

        // Synchronize all dispatchers' current time to the same value.
        for (_, ctx) in ret.contexts.iter_mut() {
            ctx.dispatcher.current_time = ret.current_time;
        }

        ret
    }

    /// Retrieves a `Ctx` named `context`.
    pub(crate) fn context<K: Into<N>>(&mut self, context: K) -> &mut DummyCtx {
        self.contexts.get_mut(&context.into()).unwrap()
    }

    /// Performs a single step in network simulation.
    ///
    /// `step` performs a single logical step in the collection of `Ctx`s
    /// held by this `DummyNetwork`. A single step consists of the following
    /// operations:
    ///
    /// - All pending frames, kept in `frames_sent` of `DummyEventDispatcher`
    /// are mapped to their destination `Ctx`/`DeviceId` pairs and moved to
    /// an internal collection of pending frames.
    /// - The collection of pending timers and scheduled frames is inspected and
    /// a simulation time step is retrieved, which will cause a next event
    /// to trigger. The simulation time is updated to the new time.
    /// - All scheduled frames whose deadline is less than or equal to the new
    /// simulation time are sent to their destinations.
    /// - All timer events whose deadline is less than or equal to the new
    /// simulation time are fired.
    ///
    /// If any new events are created during the operation of frames or timers,
    /// they **will not** be taken into account in the current `step`. That is,
    /// `step` collects all the pending events before dispatching them, ensuring
    /// that an infinite loop can't be created as a side effect of calling
    /// `step`.
    ///
    /// The return value of `step` indicates which of the operations were
    /// performed.
    ///
    /// # Panics
    ///
    /// If `DummyNetwork` was set up with a bad `mapper`, calls to `step` may
    /// panic when trying to route frames to their `Ctx`/`DeviceId`
    /// destinations.
    pub(crate) fn step(&mut self) -> StepResult {
        self.collect_frames();

        let next_step = if let Some(t) = self.next_step() {
            t
        } else {
            return StepResult::new_idle();
        };

        // This assertion holds the contract that `next_step` does not return
        // a time in the past.
        assert!(next_step >= self.current_time);
        let mut ret = StepResult::new(next_step.duration_since(self.current_time), 0, 0);

        // Move time forward.
        trace!("testutil::DummyNetwork::step: current time = {:?}", next_step);
        self.current_time = next_step;
        for (_, ctx) in self.contexts.iter_mut() {
            ctx.dispatcher.current_time = next_step;
        }

        // Dispatch all pending frames.
        while let Some(InstantAndData(t, _)) = self.pending_frames.peek() {
            // TODO(brunodalbo): remove this break once let_chains is stable
            if *t > self.current_time {
                break;
            }

            // We can unwrap because we just peeked.
            let mut frame = self.pending_frames.pop().unwrap().1;
            crate::receive_frame(
                self.context(frame.dst_context),
                frame.dst_device,
                Buf::new(&mut frame.data, ..),
            )
            .expect("error receiving frame");
            ret.frames_sent += 1;
        }

        // Dispatch all pending timers.
        for (_n, ctx) in self.contexts.iter_mut() {
            // We have to collect the timers before dispatching them, to avoid
            // an infinite loop in case handle_timer schedules another timer
            // for the same or older DummyInstant.
            let mut timers = Vec::<TimerId>::new();
            while let Some(InstantAndData(t, id)) = ctx.dispatcher.timer_events.peek() {
                // TODO(brunodalbo): remove this break once let_chains is stable
                if *t > self.current_time {
                    break;
                }
                timers.push(*id);
                assert_ne!(ctx.dispatcher.timer_events.pop(), None);
            }

            for t in timers {
                crate::handle_timer(ctx, t);
                ret.timers_fired += 1;
            }
        }

        ret
    }

    /// Collects all queued frames.
    ///
    /// Collects all pending frames and schedules them for delivery to the
    /// destination `Ctx`/`DeviceId` based on the result of `mapper`. The
    /// collected frames are queued for dispatching in the `DummyNetwork`,
    /// ordered by their scheduled delivery time given by the latency result
    /// provided by `mapper`.
    fn collect_frames(&mut self) {
        let all_frames: Vec<(N, Vec<(DeviceId, Vec<u8>)>)> = self
            .contexts
            .iter_mut()
            .filter_map(|(n, ctx)| {
                if ctx.dispatcher.frames_sent.is_empty() {
                    None
                } else {
                    Some((n.clone(), ctx.dispatcher.frames_sent.drain(..).collect()))
                }
            })
            .collect();

        for (n, frames) in all_frames.into_iter() {
            for (device_id, frame) in frames.into_iter() {
                for (dst_context, dst_device, latency) in (self.mapper)(&n, device_id) {
                    self.pending_frames.push(PendingFrame::new(
                        self.current_time + latency.unwrap_or(Duration::from_millis(0)),
                        PendingFrameData::<N> { data: frame.clone(), dst_context, dst_device },
                    ));
                }
            }
        }
    }

    /// Calculates the next `DummyInstant` when events are available.
    ///
    /// Returns the smallest `DummyInstant` greater than or equal to
    /// `current_time` for which an event is available. If no events are
    /// available, returns `None`.
    fn next_step(&mut self) -> Option<DummyInstant> {
        self.collect_frames();

        // Get earliest timer in all contexts.
        let next_timer = self
            .contexts
            .iter()
            .filter_map(|(_n, ctx)| match ctx.dispatcher.timer_events.peek() {
                Some(tmr) => Some(tmr.0),
                None => None,
            })
            .min();
        // Get the instant for the next packet.
        let next_packet_due = self.pending_frames.peek().map(|t| t.0);

        // Return the earliest of them both, and protect against returning a
        // time in the past.
        match next_timer {
            Some(t) if next_packet_due.is_some() => Some(t).min(next_packet_due),
            Some(t) => Some(t),
            None => next_packet_due,
        }
        .map(|t| t.max(self.current_time))
    }

    /// Runs the dummy network simulation until it is starved of events.
    ///
    /// Runs `step` until it returns a `StepResult` where `is_idle` is `true` or
    /// a total of 1,000,000 steps is performed. The imposed limit in steps is
    /// there to prevent the call from blocking; reaching that limit should be
    /// considered a logic error.
    ///
    /// # Panics
    ///
    /// See [`step`] for possible panic conditions.
    pub(crate) fn run_until_idle(&mut self) -> Result<(), LoopLimitReachedError> {
        for _ in 0..1_000_000 {
            if self.step().is_idle() {
                return Ok(());
            }
        }
        debug!("DummyNetwork seems to have gotten stuck in a loop.");
        Err(LoopLimitReachedError)
    }

    /// Runs the dummy network simulation until it is starved of events or
    /// `stop` returns `true`.
    ///
    /// Runs `step` until it returns a `StepResult` where `is_idle` is `true` or
    /// the provided function `stop` returns `true`, or a total of 1,000,000
    /// steps is performed. The imposed limit in steps is there to prevent the
    /// call from blocking; reaching that limit should be considered a logic
    /// error.
    ///
    /// # Panics
    ///
    /// See [`step`] for possible panic conditions.
    pub(crate) fn run_until_idle_or<S: Fn(&mut Self) -> bool>(
        &mut self,
        stop: S,
    ) -> Result<(), LoopLimitReachedError> {
        for _ in 0..1_000_000 {
            if self.step().is_idle() {
                return Ok(());
            } else if stop(self) {
                return Ok(());
            }
        }
        debug!("DummyNetwork seems to have gotten stuck in a loop.");
        Err(LoopLimitReachedError)
    }
}

/// Convenience function to create `DummyNetwork`s
///
/// `new_dummy_network_from_config` creates a `DummyNetwork` with two `Ctx`s
/// named `a` and `b`. `Ctx` `a` is created from the configuration provided
/// in `cfg`, and `Ctx` `b` is created from the symmetric configuration
/// generated by `DummyEventDispatcherConfig::swap`. A default `mapper` function
/// is provided that maps all frames from (`a`, ethernet device `1`) to
/// (`b`, ethernet device `1`) and vice-versa.
pub(crate) fn new_dummy_network_from_config_with_latency<A: IpAddress, N>(
    a: N,
    b: N,
    cfg: DummyEventDispatcherConfig<A>,
    latency: Option<Duration>,
) -> DummyNetwork<N, impl Fn(&N, DeviceId) -> Vec<(N, DeviceId, Option<Duration>)>>
where
    N: Eq + Hash + Clone + core::fmt::Debug,
{
    let bob = DummyEventDispatcherBuilder::from_config(cfg.swap()).build();
    let alice = DummyEventDispatcherBuilder::from_config(cfg).build();
    let contexts = vec![(a.clone(), alice), (b.clone(), bob)].into_iter();
    DummyNetwork::<N, _>::new(contexts, move |net, _device_id| {
        if *net == a {
            vec![(b.clone(), DeviceId::new_ethernet(0), latency)]
        } else {
            vec![(a.clone(), DeviceId::new_ethernet(0), latency)]
        }
    })
}

/// Convenience function to create `DummyNetwork`s with no latency
///
/// Creates a `DummyNetwork` by calling
/// [`new_dummy_network_from_config_with_latency`] with `latency` set to `None`.
pub(crate) fn new_dummy_network_from_config<A: IpAddress, N>(
    a: N,
    b: N,
    cfg: DummyEventDispatcherConfig<A>,
) -> DummyNetwork<N, impl Fn(&N, DeviceId) -> Vec<(N, DeviceId, Option<Duration>)>>
where
    N: Eq + Hash + Clone + core::fmt::Debug,
{
    new_dummy_network_from_config_with_latency(a, b, cfg, None)
}

#[cfg(test)]
mod tests {
    use packet::{Buf, Serializer};
    use packet_formats::{
        icmp::{IcmpEchoRequest, IcmpPacketBuilder, IcmpUnusedCode},
        ip::Ipv4Proto,
    };
    use specialize_ip_macro::{ip_test, specialize_ip_address};

    use super::*;
    use crate::{assert_empty, ip::socket::BufferIpSocketHandler, TimerIdInner};

    #[test]
    fn test_dummy_network_transmits_packets() {
        set_logger_for_test();
        let mut net = new_dummy_network_from_config("alice", "bob", DUMMY_CONFIG_V4);

        // Alice sends Bob a ping.

        BufferIpSocketHandler::<Ipv4, _>::send_oneshot_ip_packet(
            net.context("alice"),
            None, // local_ip
            DUMMY_CONFIG_V4.remote_ip,
            Ipv4Proto::Icmp,
            None, // builder
            |_| {
                let req = IcmpEchoRequest::new(0, 0);
                let req_body = &[1, 2, 3, 4];
                Buf::new(req_body.to_vec(), ..).encapsulate(
                    IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                        DUMMY_CONFIG_V4.local_ip,
                        DUMMY_CONFIG_V4.remote_ip,
                        IcmpUnusedCode,
                        req,
                    ),
                )
            },
        )
        .unwrap();

        // Send from Alice to Bob.
        assert_eq!(net.step().frames_sent(), 1);
        // Respond from Bob to Alice.
        assert_eq!(net.step().frames_sent(), 1);
        // Should've starved all events.
        assert!(net.step().is_idle());
    }

    #[test]
    fn test_dummy_network_timers() {
        set_logger_for_test();
        let mut net = new_dummy_network_from_config(1, 2, DUMMY_CONFIG_V4);

        assert_eq!(
            net.context(1)
                .dispatcher
                .schedule_timer(Duration::from_secs(1), TimerId(TimerIdInner::Nop(1))),
            None
        );
        assert_eq!(
            net.context(2)
                .dispatcher
                .schedule_timer(Duration::from_secs(2), TimerId(TimerIdInner::Nop(2))),
            None
        );
        assert_eq!(
            net.context(2)
                .dispatcher
                .schedule_timer(Duration::from_secs(3), TimerId(TimerIdInner::Nop(3))),
            None
        );
        assert_eq!(
            net.context(1)
                .dispatcher
                .schedule_timer(Duration::from_secs(4), TimerId(TimerIdInner::Nop(4))),
            None
        );
        assert_eq!(
            net.context(1)
                .dispatcher
                .schedule_timer(Duration::from_secs(5), TimerId(TimerIdInner::Nop(5))),
            None
        );
        assert_eq!(
            net.context(2)
                .dispatcher
                .schedule_timer(Duration::from_secs(5), TimerId(TimerIdInner::Nop(6))),
            None
        );

        // No timers fired before.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 0);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 0);
        assert_eq!(net.step().timers_fired(), 1);
        // Only timer in context 1 should have fired.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 1);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 0);
        assert_eq!(net.step().timers_fired(), 1);
        // Only timer in context 2 should have fired.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 1);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 1);
        assert_eq!(net.step().timers_fired(), 1);
        // Only timer in context 2 should have fired.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 1);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 2);
        assert_eq!(net.step().timers_fired(), 1);
        // Only timer in context 1 should have fired.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 2);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 2);
        assert_eq!(net.step().timers_fired(), 2);
        // Both timers have fired at the same time.
        assert_eq!(get_counter_val(net.context(1), "timer::nop"), 3);
        assert_eq!(get_counter_val(net.context(2), "timer::nop"), 3);

        assert!(net.step().is_idle());
        // Check that current time on contexts tick together.
        let t1 = net.context(1).dispatcher.current_time;
        let t2 = net.context(2).dispatcher.current_time;
        assert_eq!(t1, t2);
    }

    #[test]
    fn test_dummy_network_until_idle() {
        set_logger_for_test();
        let mut net = new_dummy_network_from_config(1, 2, DUMMY_CONFIG_V4);
        assert_eq!(
            net.context(1)
                .dispatcher
                .schedule_timer(Duration::from_secs(1), TimerId(TimerIdInner::Nop(1))),
            None
        );
        assert_eq!(
            net.context(2)
                .dispatcher
                .schedule_timer(Duration::from_secs(2), TimerId(TimerIdInner::Nop(2))),
            None
        );
        assert_eq!(
            net.context(2)
                .dispatcher
                .schedule_timer(Duration::from_secs(3), TimerId(TimerIdInner::Nop(3))),
            None
        );

        net.run_until_idle_or(|net| {
            get_counter_val(net.context(1), "timer::nop") == 1
                && get_counter_val(net.context(2), "timer::nop") == 1
        })
        .unwrap();
        // Assert that we stopped before all times were fired, meaning we can
        // step again.
        assert_eq!(net.step().timers_fired(), 1);
    }

    #[test]
    fn test_instant_and_data() {
        // Verify implementation of InstantAndData to be used as a complex type
        // in a BinaryHeap.
        let mut heap = BinaryHeap::<InstantAndData<usize>>::new();
        let now = DummyInstant::default();

        fn new_data(time: DummyInstant, id: usize) -> InstantAndData<usize> {
            InstantAndData::new(time, id)
        }

        heap.push(new_data(now + Duration::from_secs(1), 1));
        heap.push(new_data(now + Duration::from_secs(2), 2));

        // Earlier timer is popped first.
        assert_eq!(heap.pop().unwrap().1, 1);
        assert_eq!(heap.pop().unwrap().1, 2);
        assert_eq!(heap.pop(), None);

        heap.push(new_data(now + Duration::from_secs(1), 1));
        heap.push(new_data(now + Duration::from_secs(1), 1));

        // Can pop twice with identical data.
        assert_eq!(heap.pop().unwrap().1, 1);
        assert_eq!(heap.pop().unwrap().1, 1);
        assert_eq!(heap.pop(), None);
    }

    #[test]
    fn test_delayed_packets() {
        set_logger_for_test();
        // Create a network that takes 5ms to get any packet to go through.
        let mut net = new_dummy_network_from_config_with_latency(
            "alice",
            "bob",
            DUMMY_CONFIG_V4,
            Some(Duration::from_millis(5)),
        );

        // Alice sends Bob a ping.
        BufferIpSocketHandler::<Ipv4, _>::send_oneshot_ip_packet(
            net.context("alice"),
            None, // local_ip
            DUMMY_CONFIG_V4.remote_ip,
            Ipv4Proto::Icmp,
            None, // builder
            |_| {
                let req = IcmpEchoRequest::new(0, 0);
                let req_body = &[1, 2, 3, 4];
                Buf::new(req_body.to_vec(), ..).encapsulate(
                    IcmpPacketBuilder::<Ipv4, &[u8], _>::new(
                        DUMMY_CONFIG_V4.local_ip,
                        DUMMY_CONFIG_V4.remote_ip,
                        IcmpUnusedCode,
                        req,
                    ),
                )
            },
        )
        .unwrap();

        assert_eq!(
            net.context("alice")
                .dispatcher
                .schedule_timer(Duration::from_millis(3), TimerId(TimerIdInner::Nop(1))),
            None
        );
        assert_eq!(
            net.context("bob")
                .dispatcher
                .schedule_timer(Duration::from_millis(7), TimerId(TimerIdInner::Nop(2))),
            None
        );
        assert_eq!(
            net.context("bob")
                .dispatcher
                .schedule_timer(Duration::from_millis(10), TimerId(TimerIdInner::Nop(1))),
            None
        );

        // Order of expected events is as follows:
        // - Alice's timer expires at t = 3
        // - Bob receives Alice's packet at t = 5
        // - Bob's timer expires at t = 7
        // - Alice receives Bob's response and Bob's last timer fires at t = 10

        fn assert_full_state<F>(
            net: &mut DummyNetwork<&'static str, F>,
            alice_nop: usize,
            bob_nop: usize,
            bob_echo_request: usize,
            alice_echo_response: usize,
        ) where
            F: Fn(&&'static str, DeviceId) -> Vec<(&'static str, DeviceId, Option<Duration>)>,
        {
            let alice = net.context("alice");
            assert_eq!(get_counter_val(alice, "timer::nop"), alice_nop);
            assert_eq!(get_counter_val(alice, "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::echo_reply"),
                alice_echo_response
            );

            let bob = net.context("bob");
            assert_eq!(get_counter_val(bob, "timer::nop"), bob_nop);
            assert_eq!(get_counter_val(bob, "<IcmpIpTransportContext as BufferIpTransportContext<Ipv4>>::receive_ip_packet::echo_request"),
                bob_echo_request
            );
        }

        assert_eq!(net.step().timers_fired(), 1);
        assert_full_state(&mut net, 1, 0, 0, 0);
        assert_eq!(net.step().frames_sent(), 1);
        assert_full_state(&mut net, 1, 0, 1, 0);
        assert_eq!(net.step().timers_fired(), 1);
        assert_full_state(&mut net, 1, 1, 1, 0);
        let step = net.step();
        assert_eq!(step.frames_sent(), 1);
        assert_eq!(step.timers_fired(), 1);
        assert_full_state(&mut net, 1, 2, 1, 1);

        // Should've starved all events.
        assert!(net.step().is_idle());
    }

    #[ip_test]
    fn test_send_to_many<I: Ip + TestIpExt>() {
        #[specialize_ip_address]
        fn send_packet<A: IpAddress>(
            ctx: &mut DummyCtx,
            src_ip: SpecifiedAddr<A>,
            dst_ip: SpecifiedAddr<A>,
            device: DeviceId,
        ) {
            #[ipv4addr]
            crate::ip::send_ipv4_packet_from_device(
                ctx,
                device,
                src_ip.get(),
                dst_ip.get(),
                dst_ip,
                IpProto::Udp.into(),
                Buf::new(vec![1, 2, 3, 4], ..),
                None,
            )
            .unwrap();

            #[ipv6addr]
            crate::ip::send_ipv6_packet_from_device(
                ctx,
                device,
                src_ip.get(),
                dst_ip.get(),
                dst_ip,
                IpProto::Udp.into(),
                Buf::new(vec![1, 2, 3, 4], ..),
                None,
            )
            .unwrap();
        }

        let device_builder_id = 0;
        let device = DeviceId::new_ethernet(device_builder_id);
        let a = "alice";
        let b = "bob";
        let c = "calvin";
        let mac_a = UnicastAddr::new(Mac::new([2, 3, 4, 5, 6, 7])).unwrap();
        let mac_b = UnicastAddr::new(Mac::new([2, 3, 4, 5, 6, 8])).unwrap();
        let mac_c = UnicastAddr::new(Mac::new([2, 3, 4, 5, 6, 9])).unwrap();
        let ip_a = I::get_other_ip_address(1);
        let ip_b = I::get_other_ip_address(2);
        let ip_c = I::get_other_ip_address(3);
        let subnet = Subnet::new(I::get_other_ip_address(0).get(), I::Addr::BYTES * 8 - 8).unwrap();
        let mut alice = DummyEventDispatcherBuilder::default();
        alice.add_device_with_ip(mac_a, ip_a.get(), subnet);
        let mut bob = DummyEventDispatcherBuilder::default();
        bob.add_device_with_ip(mac_b, ip_b.get(), subnet);
        let mut calvin = DummyEventDispatcherBuilder::default();
        calvin.add_device_with_ip(mac_c, ip_c.get(), subnet);
        add_arp_or_ndp_table_entry(&mut alice, device_builder_id, ip_b.get(), mac_b);
        add_arp_or_ndp_table_entry(&mut alice, device_builder_id, ip_c.get(), mac_c);
        add_arp_or_ndp_table_entry(&mut bob, device_builder_id, ip_a.get(), mac_a);
        add_arp_or_ndp_table_entry(&mut bob, device_builder_id, ip_c.get(), mac_c);
        add_arp_or_ndp_table_entry(&mut calvin, device_builder_id, ip_a.get(), mac_a);
        add_arp_or_ndp_table_entry(&mut calvin, device_builder_id, ip_b.get(), mac_b);
        let contexts =
            vec![(a.clone(), alice.build()), (b.clone(), bob.build()), (c.clone(), calvin.build())]
                .into_iter();
        let mut net = DummyNetwork::new(contexts, move |net, _| match *net {
            "alice" => vec![(b.clone(), device, None), (c.clone(), device, None)],
            "bob" => vec![(a.clone(), device, None)],
            "calvin" => Vec::new(),
            _ => unreachable!(),
        });

        net.collect_frames();
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_empty(net.pending_frames.iter());

        // Bob and Calvin should get any packet sent by Alice.

        send_packet(net.context("alice"), ip_a, ip_b, device);
        assert_eq!(net.context("alice").dispatcher.frames_sent().len(), 1);
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_empty(net.pending_frames.iter());
        net.collect_frames();
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_eq!(net.pending_frames.len(), 2);
        assert!(net
            .pending_frames
            .iter()
            .any(|InstantAndData(_, x)| (x.dst_context == b) && (x.dst_device == device)));
        assert!(net
            .pending_frames
            .iter()
            .any(|InstantAndData(_, x)| (x.dst_context == c) && (x.dst_device == device)));

        // Only Alice should get packets sent by Bob.

        net.pending_frames = BinaryHeap::new();
        send_packet(net.context("bob"), ip_b, ip_a, device);
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_eq!(net.context("bob").dispatcher.frames_sent().len(), 1);
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_empty(net.pending_frames.iter());
        net.collect_frames();
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_eq!(net.pending_frames.len(), 1);
        assert!(net
            .pending_frames
            .iter()
            .any(|InstantAndData(_, x)| (x.dst_context == a) && (x.dst_device == device)));

        // No one gets packets sent by Calvin.

        net.pending_frames = BinaryHeap::new();
        send_packet(net.context("calvin"), ip_c, ip_a, device);
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_eq!(net.context("calvin").dispatcher.frames_sent().len(), 1);
        assert_empty(net.pending_frames.iter());
        net.collect_frames();
        assert_empty(net.context("alice").dispatcher.frames_sent().iter());
        assert_empty(net.context("bob").dispatcher.frames_sent().iter());
        assert_empty(net.context("calvin").dispatcher.frames_sent().iter());
        assert_empty(net.pending_frames.iter());
    }
}
