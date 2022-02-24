// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain
// +build !build_with_native_toolchain

package netstack

import (
	"context"
	"errors"
	"fmt"
	"strings"
	"syscall/zx"
	"syscall/zx/fidl"
	"time"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/fidlconv"
	"go.fuchsia.dev/fuchsia/src/lib/component"
	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"

	"fidl/fuchsia/net"
	"fidl/fuchsia/net/neighbor"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
)

const (
	nudTag = "NUD"
)

type nudDispatcher struct {
	ns *Netstack
}

var _ stack.NUDDispatcher = (*nudDispatcher)(nil)

func (d *nudDispatcher) log(verb string, nicID tcpip.NICID, entry stack.NeighborEntry) {
	family := func() string {
		switch l := len(entry.Addr); l {
		case header.IPv4AddressSize:
			return "v4"
		case header.IPv6AddressSize:
			return "v6"
		default:
			return fmt.Sprintf("l=%d", l)
		}
	}()
	flags := func() string {
		var defaultGateway, onLink bool
		for _, route := range d.ns.stack.GetRouteTable() {
			if route.NIC != nicID {
				continue
			}
			if len(route.Destination.ID()) != len(entry.Addr) {
				continue
			}
			if route.Destination.Prefix() == 0 {
				if route.Gateway == entry.Addr {
					defaultGateway = true
				}
			} else if route.Destination.Contains(entry.Addr) {
				if len(route.Gateway) == 0 {
					onLink = true
				}
			}
		}
		var b strings.Builder
		if defaultGateway {
			b.WriteByte('G')
		}
		if onLink {
			b.WriteByte('L')
		}
		if b.Len() != 0 {
			return b.String()
		}
		return "U"
	}()

	// TODO(https://fxbug.dev/62788): Change log level to Debug once the neighbor table
	// is able to be inspected.
	_ = syslog.InfoTf(nudTag, "%s %s (%s|%s) NIC=%d LinkAddress=%s %s", verb, entry.Addr, family, flags, nicID, entry.LinkAddr, entry.State)
}

// OnNeighborAdded implements stack.NUDDispatcher.
func (d *nudDispatcher) OnNeighborAdded(nicID tcpip.NICID, entry stack.NeighborEntry) {
	d.log("ADD", nicID, entry)
}

// OnNeighborChanged implements stack.NUDDispatcher.
func (d *nudDispatcher) OnNeighborChanged(nicID tcpip.NICID, entry stack.NeighborEntry) {
	d.log("MOD", nicID, entry)
}

// OnNeighborRemoved implements stack.NUDDispatcher.
func (d *nudDispatcher) OnNeighborRemoved(nicID tcpip.NICID, entry stack.NeighborEntry) {
	d.log("DEL", nicID, entry)
}

type neighborImpl struct {
	stack *stack.Stack
}

var _ neighbor.ViewWithCtx = (*neighborImpl)(nil)

func (n *neighborImpl) OpenEntryIterator(ctx fidl.Context, it neighbor.EntryIteratorWithCtxInterfaceRequest, _ neighbor.EntryIteratorOptions) error {
	// TODO(https://fxbug.dev/59425): Watch for changes.
	var items []neighbor.EntryIteratorItem

	for nicID := range n.stack.NICInfo() {
		for _, network := range []tcpip.NetworkProtocolNumber{header.IPv4ProtocolNumber, header.IPv6ProtocolNumber} {
			neighbors, err := n.stack.Neighbors(nicID, network)
			switch err.(type) {
			case nil:
			case *tcpip.ErrNotSupported:
				// This NIC does not use a neighbor table.
				continue
			case *tcpip.ErrUnknownNICID:
				// This NIC was removed since stack.NICInfo() was called.
				continue
			default:
				_ = syslog.ErrorTf(neighbor.ViewName, "EntryIterator received unexpected error from Neighbors(%d): %s", nicID, err)
				_ = it.Close()
				return WrapTcpIpError(err)
			}

			for _, n := range neighbors {
				if netEntry, ok := toNeighborEntry(nicID, n); ok {
					items = append(items, neighbor.EntryIteratorItemWithExisting(netEntry))
				}
			}
		}
	}

	// End the list with a special item to indicate the end of existing entries.
	items = append(items, neighbor.EntryIteratorItemWithIdle(neighbor.IdleEvent{}))

	stub := neighbor.EntryIteratorWithCtxStub{
		Impl: &neighborEntryIterator{
			items: items,
		},
	}
	go component.Serve(context.Background(), &stub, it.Channel, component.ServeOptions{
		OnError: func(err error) {
			_ = syslog.WarnTf(neighbor.ViewName, "EntryIterator: %s", err)
		},
	})

	return nil
}

func (n *neighborImpl) GetUnreachabilityConfig(_ fidl.Context, interfaceID uint64, ipVersion net.IpVersion) (neighbor.ViewGetUnreachabilityConfigResult, error) {
	netProto, ok := fidlconv.ToTCPIPNetProto(ipVersion)
	if !ok {
		return neighbor.ViewGetUnreachabilityConfigResultWithErr(int32(zx.ErrInvalidArgs)), nil
	}

	config, err := n.stack.NUDConfigurations(tcpip.NICID(interfaceID), netProto)
	if err != nil {
		return neighbor.ViewGetUnreachabilityConfigResultWithErr(int32(WrapTcpIpError(err).ToZxStatus())), nil
	}

	var resp neighbor.UnreachabilityConfig
	resp.SetBaseReachableTime(config.BaseReachableTime.Nanoseconds())
	resp.SetLearnBaseReachableTime(config.LearnBaseReachableTime)
	resp.SetMinRandomFactor(config.MinRandomFactor)
	resp.SetMaxRandomFactor(config.MaxRandomFactor)
	resp.SetRetransmitTimer(config.RetransmitTimer.Nanoseconds())
	resp.SetLearnRetransmitTimer(config.LearnRetransmitTimer)
	resp.SetDelayFirstProbeTime(config.DelayFirstProbeTime.Nanoseconds())
	resp.SetMaxMulticastProbes(config.MaxMulticastProbes)
	resp.SetMaxUnicastProbes(config.MaxUnicastProbes)
	resp.SetMaxAnycastDelayTime(config.MaxAnycastDelayTime.Nanoseconds())
	resp.SetMaxReachabilityConfirmations(config.MaxReachabilityConfirmations)

	return neighbor.ViewGetUnreachabilityConfigResultWithResponse(neighbor.ViewGetUnreachabilityConfigResponse{
		Config: resp,
	}), nil
}

var _ neighbor.ControllerWithCtx = (*neighborImpl)(nil)

func (n *neighborImpl) AddEntry(_ fidl.Context, interfaceID uint64, neighborIP net.IpAddress, mac net.MacAddress) (neighbor.ControllerAddEntryResult, error) {
	address, network := fidlconv.ToTCPIPAddressAndProtocolNumber(neighborIP)
	if err := n.stack.AddStaticNeighbor(tcpip.NICID(interfaceID), network, address, fidlconv.ToTCPIPLinkAddress(mac)); err != nil {
		return neighbor.ControllerAddEntryResultWithErr(int32(WrapTcpIpError(err).ToZxStatus())), nil
	}
	return neighbor.ControllerAddEntryResultWithResponse(neighbor.ControllerAddEntryResponse{}), nil
}

func (n *neighborImpl) RemoveEntry(_ fidl.Context, interfaceID uint64, neighborIP net.IpAddress) (neighbor.ControllerRemoveEntryResult, error) {
	address, network := fidlconv.ToTCPIPAddressAndProtocolNumber(neighborIP)
	if err := n.stack.RemoveNeighbor(tcpip.NICID(interfaceID), network, address); err != nil {
		var zxErr zx.Status
		switch err.(type) {
		case *tcpip.ErrBadAddress:
			zxErr = zx.ErrNotFound
		default:
			zxErr = WrapTcpIpError(err).ToZxStatus()
		}
		return neighbor.ControllerRemoveEntryResultWithErr(int32(zxErr)), nil
	}
	return neighbor.ControllerRemoveEntryResultWithResponse(neighbor.ControllerRemoveEntryResponse{}), nil
}

func (n *neighborImpl) ClearEntries(_ fidl.Context, interfaceID uint64, ipVersion net.IpVersion) (neighbor.ControllerClearEntriesResult, error) {
	netProto, ok := fidlconv.ToTCPIPNetProto(ipVersion)
	if !ok {
		return neighbor.ControllerClearEntriesResultWithErr(int32(zx.ErrInvalidArgs)), nil
	}

	if err := n.stack.ClearNeighbors(tcpip.NICID(interfaceID), netProto); err != nil {
		return neighbor.ControllerClearEntriesResultWithErr(int32(WrapTcpIpError(err).ToZxStatus())), nil
	}
	return neighbor.ControllerClearEntriesResultWithResponse(neighbor.ControllerClearEntriesResponse{}), nil
}

func (n *neighborImpl) UpdateUnreachabilityConfig(_ fidl.Context, interfaceID uint64, ipVersion net.IpVersion, config neighbor.UnreachabilityConfig) (neighbor.ControllerUpdateUnreachabilityConfigResult, error) {
	if !n.stack.HasNIC(tcpip.NICID(interfaceID)) {
		return neighbor.ControllerUpdateUnreachabilityConfigResultWithErr(int32(zx.ErrNotFound)), nil
	}

	netProto, ok := fidlconv.ToTCPIPNetProto(ipVersion)
	if !ok {
		return neighbor.ControllerUpdateUnreachabilityConfigResultWithErr(int32(zx.ErrInvalidArgs)), nil
	}

	currentConfig, err := n.stack.NUDConfigurations(tcpip.NICID(interfaceID), netProto)
	if err != nil {
		return neighbor.ControllerUpdateUnreachabilityConfigResultWithErr(int32(WrapTcpIpError(err).ToZxStatus())), nil
	}

	invalidArgsResult := neighbor.ControllerUpdateUnreachabilityConfigResultWithErr(int32(zx.ErrInvalidArgs))

	// See fuchsia.net.neighbor/UnreachabilityConfig for the list of constraints.
	if config.HasBaseReachableTime() {
		if v := config.GetBaseReachableTime(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `base_reachable_time` %d: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.BaseReachableTime = time.Duration(config.GetBaseReachableTime())
	}
	if config.HasLearnBaseReachableTime() {
		currentConfig.LearnBaseReachableTime = config.GetLearnBaseReachableTime()
	}
	if config.HasMinRandomFactor() {
		if v := config.GetMinRandomFactor(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `min_random_factor` %f: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.MinRandomFactor = config.GetMinRandomFactor()
	}
	if config.HasMaxRandomFactor() {
		if v := config.GetMaxRandomFactor(); v < currentConfig.MinRandomFactor {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `max_random_factor` %f: must be >= `min_random_factor` %f", v, currentConfig.MinRandomFactor)
			return invalidArgsResult, nil
		}
		currentConfig.MaxRandomFactor = config.GetMaxRandomFactor()
	}
	if config.HasRetransmitTimer() {
		if v := config.GetRetransmitTimer(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `retransmit_timer` %d: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.RetransmitTimer = time.Duration(config.GetRetransmitTimer())
	}
	if config.HasLearnRetransmitTimer() {
		currentConfig.LearnRetransmitTimer = config.GetLearnRetransmitTimer()
	}
	if config.HasDelayFirstProbeTime() {
		if v := config.GetDelayFirstProbeTime(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `delay_first_probe_time` %d: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.DelayFirstProbeTime = time.Duration(config.GetDelayFirstProbeTime())
	}
	if config.HasMaxMulticastProbes() {
		if v := config.GetMaxMulticastProbes(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `max_multicast_probes` %d: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.MaxMulticastProbes = config.GetMaxMulticastProbes()
	}
	if config.HasMaxUnicastProbes() {
		if v := config.GetMaxUnicastProbes(); v <= 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `max_unicast_probes` %d: must be > 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.MaxUnicastProbes = config.GetMaxUnicastProbes()
	}
	if config.HasMaxAnycastDelayTime() {
		if v := config.GetMaxAnycastDelayTime(); v < 0 {
			_ = syslog.WarnTf(neighbor.ControllerName, "UpdateUnreachabilityConfig: invalid `max_anycast_delay_time` %d: must be >= 0", v)
			return invalidArgsResult, nil
		}
		currentConfig.MaxAnycastDelayTime = time.Duration(config.GetMaxAnycastDelayTime())
	}
	if config.HasMaxReachabilityConfirmations() {
		currentConfig.MaxReachabilityConfirmations = config.GetMaxReachabilityConfirmations()
	}

	if err := n.stack.SetNUDConfigurations(tcpip.NICID(interfaceID), netProto, currentConfig); err != nil {
		return neighbor.ControllerUpdateUnreachabilityConfigResultWithErr(int32(WrapTcpIpError(err).ToZxStatus())), nil
	}
	return neighbor.ControllerUpdateUnreachabilityConfigResultWithResponse(neighbor.ControllerUpdateUnreachabilityConfigResponse{}), nil
}

// neighborEntryIterator queues events received from the neighbor table for
// consumption by a FIDL client.
type neighborEntryIterator struct {
	// items contains neighbor entry notifications waiting for client consumption.
	items []neighbor.EntryIteratorItem
}

var _ neighbor.EntryIteratorWithCtx = (*neighborEntryIterator)(nil)

// GetNext implements neighbor.EntryIteratorWithCtx.GetNext.
func (it *neighborEntryIterator) GetNext(fidl.Context) ([]neighbor.EntryIteratorItem, error) {
	if len(it.items) == 0 {
		// TODO(https://fxbug.dev/59425): Watch for changes instead of closing the
		// connection. This was deferred to unblock listing entries.
		return nil, errors.New("watching for changes not supported")
	}

	items := it.items
	if uint64(len(it.items)) > neighbor.MaxItemBatchSize {
		// There are too many items to send; only send the max amount and leave the
		// rest for subsequent calls.
		items = items[:neighbor.MaxItemBatchSize]
	}
	it.items = it.items[len(items):]

	// Avoid memory leak on always-appended slice.
	if len(it.items) == 0 {
		it.items = nil
	}

	return items, nil
}

// toNeighborEntry converts a stack.NeighborEntry to a
// fuchsia.net.neighbor/Entry. Returns the converted entry and true if the
// conversion was successful, false otherwise.
func toNeighborEntry(nicID tcpip.NICID, n stack.NeighborEntry) (neighbor.Entry, bool) {
	e := neighbor.Entry{}
	e.SetInterface(uint64(nicID))
	e.SetUpdatedAt(n.UpdatedAt.UnixNano())

	if len(n.Addr) != 0 {
		e.SetNeighbor(fidlconv.ToNetIpAddress(n.Addr))
	}
	if len(n.LinkAddr) != 0 {
		e.SetMac(fidlconv.ToNetMacAddress(n.LinkAddr))
	}

	switch n.State {
	case stack.Unknown:
		// Unknown is an internal state used by the netstack to represent a newly
		// created or deleted entry. Clients do not need to be concerned with this
		// in-between state; all transitions into and out of the Unknown state
		// trigger an event.
		return e, false
	case stack.Incomplete:
		e.SetState(neighbor.EntryStateIncomplete)
	case stack.Reachable:
		e.SetState(neighbor.EntryStateReachable)
	case stack.Stale:
		e.SetState(neighbor.EntryStateStale)
	case stack.Delay:
		e.SetState(neighbor.EntryStateDelay)
	case stack.Probe:
		e.SetState(neighbor.EntryStateProbe)
	case stack.Static:
		e.SetState(neighbor.EntryStateStatic)
	case stack.Unreachable:
		e.SetState(neighbor.EntryStateUnreachable)
	default:
		panic(fmt.Sprintf("invalid NeighborState = %d: %#v", n.State, n))
	}

	return e, true
}
