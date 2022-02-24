// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

//go:build !build_with_native_toolchain
// +build !build_with_native_toolchain

package netstack

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"math"
	"runtime"
	"sort"
	"strings"
	"sync/atomic"
	"syscall/zx"
	"syscall/zx/fidl"
	"syscall/zx/zxsocket"
	"syscall/zx/zxwait"
	"time"
	"unsafe"

	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/fidlconv"
	"go.fuchsia.dev/fuchsia/src/connectivity/network/netstack/sync"
	"go.fuchsia.dev/fuchsia/src/lib/component"
	syslog "go.fuchsia.dev/fuchsia/src/lib/syslog/go"

	fidlio "fidl/fuchsia/io"
	fidlnet "fidl/fuchsia/net"
	"fidl/fuchsia/posix"
	"fidl/fuchsia/posix/socket"
	packetsocket "fidl/fuchsia/posix/socket/packet"
	rawsocket "fidl/fuchsia/posix/socket/raw"

	"gvisor.dev/gvisor/pkg/tcpip"
	"gvisor.dev/gvisor/pkg/tcpip/header"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv4"
	"gvisor.dev/gvisor/pkg/tcpip/network/ipv6"
	"gvisor.dev/gvisor/pkg/tcpip/stack"
	"gvisor.dev/gvisor/pkg/tcpip/transport/icmp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/tcp"
	"gvisor.dev/gvisor/pkg/tcpip/transport/udp"
	"gvisor.dev/gvisor/pkg/waiter"
)

const (
	// NB: these constants are defined in gVisor as well, but not exported.
	ccReno  = "reno"
	ccCubic = "cubic"

	// Max values for sockopt TCP_KEEPIDLE and TCP_KEEPINTVL in Linux.
	//
	// https://github.com/torvalds/linux/blob/f2850dd5ee015bd7b77043f731632888887689c7/include/net/tcp.h#L156-L158
	maxTCPKeepIdle  = 32767
	maxTCPKeepIntvl = 32767
	maxTCPKeepCnt   = 127
)

func optionalUint8ToInt(v socket.OptionalUint8, unset int) (int, tcpip.Error) {
	switch v.Which() {
	case socket.OptionalUint8Value:
		return int(v.Value), nil
	case socket.OptionalUint8Unset:
		return unset, nil
	default:
		return -1, &tcpip.ErrInvalidOptionValue{}
	}
}

func optionalUint32ToInt(v socket.OptionalUint32, unset int) (int, tcpip.Error) {
	switch v.Which() {
	case socket.OptionalUint32Value:
		return int(v.Value), nil
	case socket.OptionalUint32Unset:
		return unset, nil
	default:
		return -1, &tcpip.ErrInvalidOptionValue{}
	}
}

var _ io.Writer = (*socketWriter)(nil)

type socketWriter struct {
	socket    zx.Socket
	lastError error
}

func (w *socketWriter) Write(p []byte) (int, error) {
	n, err := w.socket.Write(p, 0)
	if err == nil && n != len(p) {
		err = &zx.Error{Status: zx.ErrShouldWait}
	}
	w.lastError = err
	return n, err
}

var _ tcpip.Payloader = (*socketReader)(nil)

type socketReader struct {
	socket    zx.Socket
	lastError error
	lastRead  int
}

func (r *socketReader) Read(p []byte) (int, error) {
	n, err := r.socket.Read(p, 0)
	if err == nil && n != len(p) {
		err = &zx.Error{Status: zx.ErrShouldWait}
	}
	r.lastError = err
	r.lastRead = n
	return n, err
}

func (r *socketReader) Len() int {
	n, err := func() (int, error) {
		var info zx.InfoSocket
		if err := r.socket.Handle().GetInfo(zx.ObjectInfoSocket, unsafe.Pointer(&info), uint(unsafe.Sizeof(info))); err != nil {
			return 0, err
		}
		return info.RXBufAvailable, nil
	}()
	if err == nil && n == 0 {
		err = &zx.Error{Status: zx.ErrShouldWait}
	}
	r.lastError = err
	r.lastRead = n
	return n
}

func eventsToDatagramSignals(events waiter.EventMask) zx.Signals {
	signals := zx.SignalNone
	if events&waiter.EventIn != 0 {
		signals |= zxsocket.SignalDatagramIncoming
		events ^= waiter.EventIn
	}
	if events&waiter.EventErr != 0 {
		signals |= zxsocket.SignalDatagramError
		events ^= waiter.EventErr
	}
	if events != 0 {
		panic(fmt.Sprintf("unexpected events=%b", events))
	}
	return signals
}

func eventsToStreamSignals(events waiter.EventMask) zx.Signals {
	signals := zx.SignalNone
	if events&waiter.EventIn != 0 {
		signals |= zxsocket.SignalStreamIncoming
		events ^= waiter.EventIn
	}
	if events != 0 {
		panic(fmt.Sprintf("unexpected events=%b", events))
	}
	return signals
}

type signaler struct {
	supported       waiter.EventMask
	eventsToSignals func(waiter.EventMask) zx.Signals
	readiness       func(waiter.EventMask) waiter.EventMask
	signalPeer      func(zx.Signals, zx.Signals) error

	mu struct {
		sync.Mutex
		asserted waiter.EventMask
	}
}

func (s *signaler) update() error {
	// We lock to ensure that no incoming event changes readiness while we maybe
	// set the signals.
	s.mu.Lock()
	defer s.mu.Unlock()

	// Consult the present readiness of the events we are interested in while
	// we're locked, as they may have changed already.
	observed := s.readiness(s.supported)
	// readiness may return events that were not requested so only keep the events
	// we explicitly requested. For example, Readiness implementation of the UDP
	// endpoint in gvisor may return EventErr whether or not it was set in the
	// mask:
	// https://github.com/google/gvisor/blob/8ee4a3f/pkg/tcpip/transport/udp/endpoint.go#L1252
	observed &= s.supported

	if observed == s.mu.asserted {
		// No events changed since last time.
		return nil
	}

	set := s.eventsToSignals(observed &^ s.mu.asserted)
	clear := s.eventsToSignals(s.mu.asserted &^ observed)
	if set == 0 && clear == 0 {
		return nil
	}
	if err := s.signalPeer(clear, set); err != nil {
		return err
	}
	s.mu.asserted = observed
	return nil
}

func (s *signaler) mustUpdate() {
	err := s.update()
	switch err := err.(type) {
	case nil:
		return
	case *zx.Error:
		switch err.Status {
		case zx.ErrBadHandle, zx.ErrPeerClosed:
			return
		}
	}
	panic(err)
}

type terminalError struct {
	mu struct {
		sync.Mutex
		ch  <-chan tcpip.Error
		err tcpip.Error
	}
}

func (t *terminalError) setLocked(err tcpip.Error) {
	switch previous := t.setLockedInner(err); previous {
	case nil, err:
	default:
		switch previous.(type) {
		case *tcpip.ErrNetworkUnreachable:
		case *tcpip.ErrNoRoute:
		// At the time of writing, these errors are triggered by inbound
		// ICMP Destination Unreachable messages; if another error is to
		// be set on the endpoint, treat that one as the true error
		// because ICMP Destination Unreachable messages must not be
		// treated as a hard error as required by RFC 1122:
		//
		// A Destination Unreachable message that is received with code
		// 0 (Net), 1 (Host), or 5 (Bad Source Route) may result from a
		// routing transient and MUST therefore be interpreted as only
		// a hint, not proof, that the specified destination is
		// unreachable [IP:11].
		//
		// https://datatracker.ietf.org/doc/html/rfc1122#page-40
		default:
			panic(fmt.Sprintf("previous=%#v err=%#v", previous, err))
		}
	}
}

func (t *terminalError) setLockedInner(err tcpip.Error) tcpip.Error {
	switch err.(type) {
	case
		*tcpip.ErrConnectionAborted,
		*tcpip.ErrConnectionRefused,
		*tcpip.ErrConnectionReset,
		*tcpip.ErrNetworkUnreachable,
		*tcpip.ErrNoRoute,
		*tcpip.ErrTimeout:
		previous := t.mu.err
		_ = syslog.DebugTf("setLockedInner", "previous=%#v err=%#v", previous, err)
		if previous == nil {
			ch := make(chan tcpip.Error, 1)
			ch <- err
			close(ch)
			t.mu.ch = ch
			t.mu.err = err
		}
		return previous
	default:
		return nil
	}
}

// setConsumedLocked is used to set errors that are about to be returned to the
// client; since errors can only be returned once, this is used only for its
// side effect of causing subsequent reads to treat the error is consumed.
//
// This method is needed because gVisor sometimes double-reports errors:
//
// Double set: https://github.com/google/gvisor/blob/949c814/pkg/tcpip/transport/tcp/connect.go#L1357-L1361
//
// Retrieval: https://github.com/google/gvisor/blob/949c814/pkg/tcpip/transport/tcp/endpoint.go#L1273-L1281
func (t *terminalError) setConsumedLocked(err tcpip.Error) {
	if ch := t.setConsumedLockedInner(err); ch != nil {
		switch consumed := <-ch; consumed {
		case nil, err:
		default:
			panic(fmt.Sprintf("consumed=%#v err=%#v", consumed, err))
		}
	}
}

func (t *terminalError) setConsumedLockedInner(err tcpip.Error) <-chan tcpip.Error {
	if previous := t.setLockedInner(err); previous != nil {
		// Was already set; let the caller decide whether to consume.
		return t.mu.ch
	}
	ch := t.mu.ch
	if ch != nil {
		// Wasn't set, became set. Consume since we're returning the error.
		if consumed := <-ch; consumed != err {
			panic(fmt.Sprintf("consumed=%#v err=%#v", consumed, err))
		}
	}
	return ch
}

// endpoint is the base structure that models all network sockets.
type endpoint struct {
	wq *waiter.Queue
	ep tcpip.Endpoint

	mu struct {
		sync.RWMutex
		refcount         uint32
		sockOptTimestamp socket.TimestampOption
	}

	transProto tcpip.TransportProtocolNumber
	netProto   tcpip.NetworkProtocolNumber

	key uint64

	ns *Netstack

	pending signaler

	terminal terminalError
}

func (ep *endpoint) incRef() {
	ep.mu.Lock()
	ep.mu.refcount++
	ep.mu.Unlock()
}

func (ep *endpoint) decRef() bool {
	ep.mu.Lock()
	doubleClose := ep.mu.refcount == 0
	ep.mu.refcount--
	doClose := ep.mu.refcount == 0
	ep.mu.Unlock()
	if doubleClose {
		panic(fmt.Sprintf("%p: double close", ep))
	}
	return doClose
}

func (ep *endpoint) SyncDeprecated(fidl.Context) (int32, error) {
	_ = syslog.DebugTf("Sync", "%p", ep)

	return 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) Sync(fidl.Context) (fidlio.Node2SyncResult, error) {
	_ = syslog.DebugTf("Sync", "%p", ep)

	return fidlio.Node2SyncResultWithErr(int32(zx.ErrNotSupported)), nil
}

func (ep *endpoint) GetAttr(fidl.Context) (int32, fidlio.NodeAttributes, error) {
	_ = syslog.DebugTf("GetAttr", "%p", ep)

	return 0, fidlio.NodeAttributes{}, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) GetAttributes(_ fidl.Context, query fidlio.NodeAttributesQuery) (fidlio.Node2GetAttributesResult, error) {
	_ = syslog.DebugTf("GetAttributes", "%p: query=%#v", ep, query)
	return fidlio.Node2GetAttributesResultWithErr(int32(zx.ErrNotSupported)), nil
}

func (ep *endpoint) SetAttr(fidl.Context, uint32, fidlio.NodeAttributes) (int32, error) {
	_ = syslog.DebugTf("SetAttr", "%p", ep)

	return 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) UpdateAttributes(_ fidl.Context, attributes fidlio.NodeAttributes2) (fidlio.Node2UpdateAttributesResult, error) {
	_ = syslog.DebugTf("UpdateAttributes", "%p: attributes=%#v", ep, attributes)
	return fidlio.Node2UpdateAttributesResultWithErr(int32(zx.ErrNotSupported)), nil
}

func (ep *endpoint) GetFlags(fidl.Context) (int32, uint32, error) {
	_ = syslog.DebugTf("GetFlags", "%p", ep)

	return 0, 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) SetFlags(_ fidl.Context, flags uint32) (int32, error) {
	_ = syslog.DebugTf("GetFlags", "%p", ep)

	return 0, &zx.Error{Status: zx.ErrNotSupported, Text: fmt.Sprintf("%T", ep)}
}

func (ep *endpoint) QueryFilesystem(fidl.Context) (int32, *fidlio.FilesystemInfo, error) {
	return int32(zx.ErrNotSupported), nil, nil
}

func (ep *endpoint) Bind(_ fidl.Context, sockaddr fidlnet.SocketAddress) (socket.BaseNetworkSocketBindResult, error) {
	addr, err := toTCPIPFullAddress(sockaddr)
	if err != nil {
		return socket.BaseNetworkSocketBindResultWithErr(tcpipErrorToCode(&tcpip.ErrBadAddress{})), nil
	}
	if err := ep.ep.Bind(addr); err != nil {
		return socket.BaseNetworkSocketBindResultWithErr(tcpipErrorToCode(err)), nil
	}

	{
		localAddr, err := ep.ep.GetLocalAddress()
		if err != nil {
			panic(err)
		}
		_ = syslog.DebugTf("bind", "%p: local=%+v", ep, localAddr)
	}

	return socket.BaseNetworkSocketBindResultWithResponse(socket.BaseNetworkSocketBindResponse{}), nil
}

func (ep *endpoint) connect(address fidlnet.SocketAddress) tcpip.Error {
	addr, err := toTCPIPFullAddress(address)
	if err != nil {
		return &tcpip.ErrBadAddress{}
	}
	if l := len(addr.Addr); l > 0 {
		if ep.netProto == ipv4.ProtocolNumber && l != header.IPv4AddressSize {
			_ = syslog.DebugTf("connect", "%p: unsupported address %s", ep, addr.Addr)
			return &tcpip.ErrAddressFamilyNotSupported{}
		}
	}

	{
		ep.terminal.mu.Lock()
		err := ep.ep.Connect(addr)
		ch := ep.terminal.setConsumedLockedInner(err)
		ep.terminal.mu.Unlock()
		if err != nil {
			switch err.(type) {
			case *tcpip.ErrConnectStarted:
				localAddr, err := ep.ep.GetLocalAddress()
				if err != nil {
					panic(err)
				}
				_ = syslog.DebugTf("connect", "%p: started, local=%+v, addr=%+v", ep, localAddr, addr)
			case *tcpip.ErrConnectionAborted:
				// For TCP endpoints, gVisor Connect() returns this error when the
				// endpoint is in an error state and the specific error has already been
				// consumed.
				//
				// If the endpoint is in an error state, that means that loop{Read,Write}
				// must be shutting down, and the only way to consume the error correctly
				// is to get it from them.
				if ch == nil {
					panic(fmt.Sprintf("nil terminal error channel after Connect(%#v)=%#v", addr, err))
				}
				if terminal := <-ch; terminal != nil {
					err = terminal
				}
			}
			return err
		}
	}

	{
		localAddr, err := ep.ep.GetLocalAddress()
		if err != nil {
			panic(err)
		}

		remoteAddr, err := ep.ep.GetRemoteAddress()
		if err != nil {
			if _, ok := err.(*tcpip.ErrNotConnected); !ok {
				panic(err)
			}
			_ = syslog.DebugTf("connect", "%p: local=%+v, remote=disconnected", ep, localAddr)
		} else {
			_ = syslog.DebugTf("connect", "%p: local=%+v, remote=%+v", ep, localAddr, remoteAddr)
		}
	}

	return nil
}

func (ep *endpoint) Disconnect(fidl.Context) (socket.BaseNetworkSocketDisconnectResult, error) {
	if err := ep.ep.Disconnect(); err != nil {
		return socket.BaseNetworkSocketDisconnectResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketDisconnectResultWithResponse(socket.BaseNetworkSocketDisconnectResponse{}), nil
}

func (ep *endpoint) GetSockName(fidl.Context) (socket.BaseNetworkSocketGetSockNameResult, error) {
	addr, err := ep.ep.GetLocalAddress()
	if err != nil {
		return socket.BaseNetworkSocketGetSockNameResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketGetSockNameResultWithResponse(socket.BaseNetworkSocketGetSockNameResponse{
		Addr: toNetSocketAddress(ep.netProto, addr),
	}), nil
}

func (ep *endpoint) GetPeerName(fidl.Context) (socket.BaseNetworkSocketGetPeerNameResult, error) {
	addr, err := ep.ep.GetRemoteAddress()
	if err != nil {
		return socket.BaseNetworkSocketGetPeerNameResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketGetPeerNameResultWithResponse(socket.BaseNetworkSocketGetPeerNameResponse{
		Addr: toNetSocketAddress(ep.netProto, addr),
	}), nil
}

// TODO(https://fxbug.dev/86944): Remove after ABI transition.
func (ep *endpoint) GetTimestamp(fidl.Context) (socket.BaseSocketGetTimestampResult, error) {
	ep.mu.RLock()
	value := ep.mu.sockOptTimestamp == socket.TimestampOptionMicrosecond
	ep.mu.RUnlock()
	return socket.BaseSocketGetTimestampResultWithResponse(socket.BaseSocketGetTimestampResponse{Value: value}), nil
}

// TODO(https://fxbug.dev/86944): Remove after ABI transition.
func (ep *endpoint) SetTimestamp(_ fidl.Context, value bool) (socket.BaseSocketSetTimestampResult, error) {
	ep.mu.Lock()
	if value {
		ep.mu.sockOptTimestamp = socket.TimestampOptionMicrosecond
	} else {
		ep.mu.sockOptTimestamp = socket.TimestampOptionDisabled
	}
	ep.mu.Unlock()
	return socket.BaseSocketSetTimestampResultWithResponse(socket.BaseSocketSetTimestampResponse{}), nil
}

// TODO(https://fxbug.dev/86944): Rename into GetTimestamp after ABI transition.
func (ep *endpoint) GetTimestamp2(_ fidl.Context) (socket.BaseSocketGetTimestamp2Result, error) {
	ep.mu.RLock()
	value := ep.mu.sockOptTimestamp
	ep.mu.RUnlock()
	return socket.BaseSocketGetTimestamp2ResultWithResponse(socket.BaseSocketGetTimestamp2Response{Value: value}), nil
}

func (ep *endpoint) SetTimestamp2(_ fidl.Context, value socket.TimestampOption) (socket.BaseSocketSetTimestamp2Result, error) {
	ep.mu.Lock()
	ep.mu.sockOptTimestamp = value
	ep.mu.Unlock()
	return socket.BaseSocketSetTimestamp2ResultWithResponse(socket.BaseSocketSetTimestamp2Response{}), nil
}

func (ep *endpoint) domain() (socket.Domain, tcpip.Error) {
	switch ep.netProto {
	case ipv4.ProtocolNumber:
		return socket.DomainIpv4, nil
	case ipv6.ProtocolNumber:
		return socket.DomainIpv6, nil
	}
	return 0, &tcpip.ErrNotSupported{}
}

func (ep *endpoint) GetError(fidl.Context) (socket.BaseSocketGetErrorResult, error) {
	err := func() tcpip.Error {
		ep.terminal.mu.Lock()
		defer ep.terminal.mu.Unlock()
		if ch := ep.terminal.mu.ch; ch != nil {
			err := <-ch
			_ = syslog.DebugTf("GetError", "%p: err=%#v", ep, err)
			return err
		}
		err := ep.ep.LastError()
		ep.terminal.setConsumedLocked(err)
		_ = syslog.DebugTf("GetError", "%p: err=%#v", ep, err)
		return err
	}()
	ep.pending.mustUpdate()
	if err != nil {
		return socket.BaseSocketGetErrorResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseSocketGetErrorResultWithResponse(socket.BaseSocketGetErrorResponse{}), nil
}

func setBufferSize(size uint64, set func(int64, bool), limits func() (min, max int64)) {
	if size > math.MaxInt64 {
		size = math.MaxInt64
	}

	{
		size := int64(size)

		// packetOverheadFactor is used to multiply the value provided by the user on
		// a setsockopt(2) for setting the send/receive buffer sizes sockets.
		const packetOverheadFactor = 2
		if size > math.MaxInt64/packetOverheadFactor {
			size = math.MaxInt64
		} else {
			size *= packetOverheadFactor
		}

		min, max := limits()
		if size > max {
			size = max
		}
		if size < min {
			size = min
		}

		set(size, true /* notify */)
	}
}

func (ep *endpoint) SetSendBuffer(_ fidl.Context, size uint64) (socket.BaseSocketSetSendBufferResult, error) {
	opts := ep.ep.SocketOptions()
	setBufferSize(size, opts.SetSendBufferSize, opts.SendBufferLimits)
	return socket.BaseSocketSetSendBufferResultWithResponse(socket.BaseSocketSetSendBufferResponse{}), nil
}

func (ep *endpoint) GetSendBuffer(fidl.Context) (socket.BaseSocketGetSendBufferResult, error) {
	size := ep.ep.SocketOptions().GetSendBufferSize()
	return socket.BaseSocketGetSendBufferResultWithResponse(socket.BaseSocketGetSendBufferResponse{ValueBytes: uint64(size)}), nil
}

func (ep *endpoint) SetReceiveBuffer(_ fidl.Context, size uint64) (socket.BaseSocketSetReceiveBufferResult, error) {
	opts := ep.ep.SocketOptions()
	setBufferSize(size, opts.SetReceiveBufferSize, opts.ReceiveBufferLimits)
	return socket.BaseSocketSetReceiveBufferResultWithResponse(socket.BaseSocketSetReceiveBufferResponse{}), nil
}

func (ep *endpoint) GetReceiveBuffer(fidl.Context) (socket.BaseSocketGetReceiveBufferResult, error) {
	size := ep.ep.SocketOptions().GetReceiveBufferSize()
	return socket.BaseSocketGetReceiveBufferResultWithResponse(socket.BaseSocketGetReceiveBufferResponse{ValueBytes: uint64(size)}), nil
}

func (ep *endpoint) SetReuseAddress(_ fidl.Context, value bool) (socket.BaseSocketSetReuseAddressResult, error) {
	ep.ep.SocketOptions().SetReuseAddress(value)
	return socket.BaseSocketSetReuseAddressResultWithResponse(socket.BaseSocketSetReuseAddressResponse{}), nil
}

func (ep *endpoint) GetReuseAddress(fidl.Context) (socket.BaseSocketGetReuseAddressResult, error) {
	value := ep.ep.SocketOptions().GetReuseAddress()
	return socket.BaseSocketGetReuseAddressResultWithResponse(socket.BaseSocketGetReuseAddressResponse{Value: value}), nil
}

func (ep *endpoint) SetReusePort(_ fidl.Context, value bool) (socket.BaseSocketSetReusePortResult, error) {
	ep.ep.SocketOptions().SetReusePort(value)
	return socket.BaseSocketSetReusePortResultWithResponse(socket.BaseSocketSetReusePortResponse{}), nil
}

func (ep *endpoint) GetReusePort(fidl.Context) (socket.BaseSocketGetReusePortResult, error) {
	value := ep.ep.SocketOptions().GetReusePort()
	return socket.BaseSocketGetReusePortResultWithResponse(socket.BaseSocketGetReusePortResponse{Value: value}), nil
}

func (ep *endpoint) GetAcceptConn(fidl.Context) (socket.BaseSocketGetAcceptConnResult, error) {
	value := false
	if ep.transProto == tcp.ProtocolNumber {
		value = tcp.EndpointState(ep.ep.State()) == tcp.StateListen
	}
	return socket.BaseSocketGetAcceptConnResultWithResponse(socket.BaseSocketGetAcceptConnResponse{Value: value}), nil
}

func (ep *endpoint) SetBindToDevice(_ fidl.Context, value string) (socket.BaseSocketSetBindToDeviceResult, error) {
	if err := func() tcpip.Error {
		if len(value) == 0 {
			return ep.ep.SocketOptions().SetBindToDevice(0)
		}
		for id, info := range ep.ns.stack.NICInfo() {
			if value == info.Name {
				return ep.ep.SocketOptions().SetBindToDevice(int32(id))
			}
		}
		return &tcpip.ErrUnknownDevice{}
	}(); err != nil {
		return socket.BaseSocketSetBindToDeviceResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseSocketSetBindToDeviceResultWithResponse(socket.BaseSocketSetBindToDeviceResponse{}), nil
}

func (ep *endpoint) GetBindToDevice(fidl.Context) (socket.BaseSocketGetBindToDeviceResult, error) {
	id := ep.ep.SocketOptions().GetBindToDevice()
	if id == 0 {
		return socket.BaseSocketGetBindToDeviceResultWithResponse(socket.BaseSocketGetBindToDeviceResponse{}), nil
	}
	if name := ep.ns.stack.FindNICNameFromID(tcpip.NICID(id)); len(name) != 0 {
		return socket.BaseSocketGetBindToDeviceResultWithResponse(
			socket.BaseSocketGetBindToDeviceResponse{
				Value: name,
			}), nil
	}
	return socket.BaseSocketGetBindToDeviceResultWithErr(posix.ErrnoEnodev), nil
}

func (ep *endpoint) SetBroadcast(_ fidl.Context, value bool) (socket.BaseSocketSetBroadcastResult, error) {
	ep.ep.SocketOptions().SetBroadcast(value)
	return socket.BaseSocketSetBroadcastResultWithResponse(socket.BaseSocketSetBroadcastResponse{}), nil
}

func (ep *endpoint) GetBroadcast(fidl.Context) (socket.BaseSocketGetBroadcastResult, error) {
	value := ep.ep.SocketOptions().GetBroadcast()
	return socket.BaseSocketGetBroadcastResultWithResponse(socket.BaseSocketGetBroadcastResponse{Value: value}), nil
}

func (ep *endpoint) SetKeepAlive(_ fidl.Context, value bool) (socket.BaseSocketSetKeepAliveResult, error) {
	ep.ep.SocketOptions().SetKeepAlive(value)
	return socket.BaseSocketSetKeepAliveResultWithResponse(socket.BaseSocketSetKeepAliveResponse{}), nil
}

func (ep *endpoint) GetKeepAlive(fidl.Context) (socket.BaseSocketGetKeepAliveResult, error) {
	value := ep.ep.SocketOptions().GetKeepAlive()
	return socket.BaseSocketGetKeepAliveResultWithResponse(socket.BaseSocketGetKeepAliveResponse{Value: value}), nil
}

func (ep *endpoint) SetLinger(_ fidl.Context, linger bool, seconds uint32) (socket.BaseSocketSetLingerResult, error) {
	ep.ep.SocketOptions().SetLinger(tcpip.LingerOption{
		Enabled: linger,
		Timeout: time.Second * time.Duration(seconds),
	})
	return socket.BaseSocketSetLingerResultWithResponse(socket.BaseSocketSetLingerResponse{}), nil
}

func (ep *endpoint) GetLinger(fidl.Context) (socket.BaseSocketGetLingerResult, error) {
	value := ep.ep.SocketOptions().GetLinger()
	return socket.BaseSocketGetLingerResultWithResponse(
		socket.BaseSocketGetLingerResponse{
			Linger:     value.Enabled,
			LengthSecs: uint32(value.Timeout / time.Second),
		},
	), nil
}

func (ep *endpoint) SetOutOfBandInline(_ fidl.Context, value bool) (socket.BaseSocketSetOutOfBandInlineResult, error) {
	ep.ep.SocketOptions().SetOutOfBandInline(value)
	return socket.BaseSocketSetOutOfBandInlineResultWithResponse(socket.BaseSocketSetOutOfBandInlineResponse{}), nil
}

func (ep *endpoint) GetOutOfBandInline(fidl.Context) (socket.BaseSocketGetOutOfBandInlineResult, error) {
	value := ep.ep.SocketOptions().GetOutOfBandInline()
	return socket.BaseSocketGetOutOfBandInlineResultWithResponse(
		socket.BaseSocketGetOutOfBandInlineResponse{
			Value: value,
		},
	), nil
}

func (ep *endpoint) SetNoCheck(_ fidl.Context, value bool) (socket.BaseSocketSetNoCheckResult, error) {
	ep.ep.SocketOptions().SetNoChecksum(value)
	return socket.BaseSocketSetNoCheckResultWithResponse(socket.BaseSocketSetNoCheckResponse{}), nil
}

func (ep *endpoint) GetNoCheck(fidl.Context) (socket.BaseSocketGetNoCheckResult, error) {
	value := ep.ep.SocketOptions().GetNoChecksum()
	return socket.BaseSocketGetNoCheckResultWithResponse(socket.BaseSocketGetNoCheckResponse{Value: value}), nil
}

func (ep *endpoint) SetIpv6Only(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpv6OnlyResult, error) {
	ep.ep.SocketOptions().SetV6Only(value)
	return socket.BaseNetworkSocketSetIpv6OnlyResultWithResponse(socket.BaseNetworkSocketSetIpv6OnlyResponse{}), nil
}

func (ep *endpoint) GetIpv6Only(fidl.Context) (socket.BaseNetworkSocketGetIpv6OnlyResult, error) {
	value := ep.ep.SocketOptions().GetV6Only()
	return socket.BaseNetworkSocketGetIpv6OnlyResultWithResponse(socket.BaseNetworkSocketGetIpv6OnlyResponse{Value: value}), nil
}

func (ep *endpoint) SetIpv6TrafficClass(_ fidl.Context, value socket.OptionalUint8) (socket.BaseNetworkSocketSetIpv6TrafficClassResult, error) {
	v, err := optionalUint8ToInt(value, 0)
	if err != nil {
		return socket.BaseNetworkSocketSetIpv6TrafficClassResultWithErr(tcpipErrorToCode(err)), nil
	}
	if err := ep.ep.SetSockOptInt(tcpip.IPv6TrafficClassOption, v); err != nil {
		return socket.BaseNetworkSocketSetIpv6TrafficClassResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpv6TrafficClassResultWithResponse(socket.BaseNetworkSocketSetIpv6TrafficClassResponse{}), nil
}

func (ep *endpoint) GetIpv6TrafficClass(fidl.Context) (socket.BaseNetworkSocketGetIpv6TrafficClassResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.IPv6TrafficClassOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpv6TrafficClassResultWithErr(tcpipErrorToCode(err)), nil
	}

	return socket.BaseNetworkSocketGetIpv6TrafficClassResultWithResponse(
		socket.BaseNetworkSocketGetIpv6TrafficClassResponse{
			Value: uint8(value),
		},
	), nil
}

func (ep *endpoint) SetIpv6MulticastInterface(_ fidl.Context, value uint64) (socket.BaseNetworkSocketSetIpv6MulticastInterfaceResult, error) {
	opt := tcpip.MulticastInterfaceOption{
		NIC: tcpip.NICID(value),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketSetIpv6MulticastInterfaceResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpv6MulticastInterfaceResultWithResponse(socket.BaseNetworkSocketSetIpv6MulticastInterfaceResponse{}), nil
}

func (ep *endpoint) GetIpv6MulticastInterface(fidl.Context) (socket.BaseNetworkSocketGetIpv6MulticastInterfaceResult, error) {
	var value tcpip.MulticastInterfaceOption
	if err := ep.ep.GetSockOpt(&value); err != nil {
		return socket.BaseNetworkSocketGetIpv6MulticastInterfaceResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketGetIpv6MulticastInterfaceResultWithResponse(socket.BaseNetworkSocketGetIpv6MulticastInterfaceResponse{Value: uint64(value.NIC)}), nil
}

func (ep *endpoint) SetIpv6MulticastHops(_ fidl.Context, value socket.OptionalUint8) (socket.BaseNetworkSocketSetIpv6MulticastHopsResult, error) {
	v, err := optionalUint8ToInt(value, 1)
	if err != nil {
		return socket.BaseNetworkSocketSetIpv6MulticastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	if err := ep.ep.SetSockOptInt(tcpip.MulticastTTLOption, v); err != nil {
		return socket.BaseNetworkSocketSetIpv6MulticastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpv6MulticastHopsResultWithResponse(socket.BaseNetworkSocketSetIpv6MulticastHopsResponse{}), nil
}

func (ep *endpoint) GetIpv6MulticastHops(fidl.Context) (socket.BaseNetworkSocketGetIpv6MulticastHopsResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.MulticastTTLOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpv6MulticastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketGetIpv6MulticastHopsResultWithResponse(
		socket.BaseNetworkSocketGetIpv6MulticastHopsResponse{
			Value: uint8(value),
		},
	), nil
}

func (ep *endpoint) SetIpv6UnicastHops(_ fidl.Context, value socket.OptionalUint8) (socket.BaseNetworkSocketSetIpv6UnicastHopsResult, error) {
	v, err := optionalUint8ToInt(value, -1)
	if err != nil {
		return socket.BaseNetworkSocketSetIpv6UnicastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	if err := ep.ep.SetSockOptInt(tcpip.IPv6HopLimitOption, v); err != nil {
		return socket.BaseNetworkSocketSetIpv6UnicastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpv6UnicastHopsResultWithResponse(socket.BaseNetworkSocketSetIpv6UnicastHopsResponse{}), nil
}

func (ep *endpoint) GetIpv6UnicastHops(fidl.Context) (socket.BaseNetworkSocketGetIpv6UnicastHopsResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.IPv6HopLimitOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpv6UnicastHopsResultWithErr(tcpipErrorToCode(err)), nil
	}
	if value == -1 {
		var defaultHopLimit tcpip.DefaultTTLOption
		if err := ep.ns.stack.NetworkProtocolOption(header.IPv6ProtocolNumber, &defaultHopLimit); err != nil {
			panic(fmt.Sprintf("stack.NetworkProtocolOption(header.IPv6ProtocolNumber, _): %s", err))
		}
		value = int(defaultHopLimit)
	}
	return socket.BaseNetworkSocketGetIpv6UnicastHopsResultWithResponse(
		socket.BaseNetworkSocketGetIpv6UnicastHopsResponse{
			Value: uint8(value),
		},
	), nil
}

func (ep *endpoint) SetIpv6MulticastLoopback(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpv6MulticastLoopbackResult, error) {
	ep.ep.SocketOptions().SetMulticastLoop(value)
	return socket.BaseNetworkSocketSetIpv6MulticastLoopbackResultWithResponse(socket.BaseNetworkSocketSetIpv6MulticastLoopbackResponse{}), nil
}

func (ep *endpoint) GetIpv6MulticastLoopback(fidl.Context) (socket.BaseNetworkSocketGetIpv6MulticastLoopbackResult, error) {
	value := ep.ep.SocketOptions().GetMulticastLoop()
	return socket.BaseNetworkSocketGetIpv6MulticastLoopbackResultWithResponse(socket.BaseNetworkSocketGetIpv6MulticastLoopbackResponse{Value: value}), nil
}

func (ep *endpoint) SetIpTtl(_ fidl.Context, value socket.OptionalUint8) (socket.BaseNetworkSocketSetIpTtlResult, error) {
	v, err := optionalUint8ToInt(value, -1)
	if err != nil {
		return socket.BaseNetworkSocketSetIpTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	switch v {
	case -1:
		// Unset maps to default value
		v = 0
	case 0:
		return socket.BaseNetworkSocketSetIpTtlResultWithErr(posix.ErrnoEinval), nil
	}
	if err := ep.ep.SetSockOptInt(tcpip.IPv4TTLOption, v); err != nil {
		return socket.BaseNetworkSocketSetIpTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpTtlResultWithResponse(socket.BaseNetworkSocketSetIpTtlResponse{}), nil
}

func (ep *endpoint) GetIpTtl(fidl.Context) (socket.BaseNetworkSocketGetIpTtlResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.IPv4TTLOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	if value == 0 {
		var defaultTtl tcpip.DefaultTTLOption
		if err := ep.ns.stack.NetworkProtocolOption(header.IPv4ProtocolNumber, &defaultTtl); err != nil {
			panic(fmt.Sprintf("stack.NetworkProtocolOption(header.IPv4ProtocolNumber, _): %s", err))
		}
		value = int(defaultTtl)
	}
	return socket.BaseNetworkSocketGetIpTtlResultWithResponse(socket.BaseNetworkSocketGetIpTtlResponse{Value: uint8(value)}), nil
}

func (ep *endpoint) SetIpMulticastTtl(_ fidl.Context, value socket.OptionalUint8) (socket.BaseNetworkSocketSetIpMulticastTtlResult, error) {
	// Linux translates -1 (unset) to 1
	v, err := optionalUint8ToInt(value, 1)
	if err != nil {
		return socket.BaseNetworkSocketSetIpMulticastTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	if err := ep.ep.SetSockOptInt(tcpip.MulticastTTLOption, v); err != nil {
		return socket.BaseNetworkSocketSetIpMulticastTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpMulticastTtlResultWithResponse(socket.BaseNetworkSocketSetIpMulticastTtlResponse{}), nil
}

func (ep *endpoint) GetIpMulticastTtl(fidl.Context) (socket.BaseNetworkSocketGetIpMulticastTtlResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.MulticastTTLOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpMulticastTtlResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketGetIpMulticastTtlResultWithResponse(socket.BaseNetworkSocketGetIpMulticastTtlResponse{Value: uint8(value)}), nil
}

func (ep *endpoint) SetIpMulticastInterface(_ fidl.Context, iface uint64, value fidlnet.Ipv4Address) (socket.BaseNetworkSocketSetIpMulticastInterfaceResult, error) {
	opt := tcpip.MulticastInterfaceOption{
		NIC:           tcpip.NICID(iface),
		InterfaceAddr: toTcpIpAddressDroppingUnspecifiedv4(value),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketSetIpMulticastInterfaceResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpMulticastInterfaceResultWithResponse(socket.BaseNetworkSocketSetIpMulticastInterfaceResponse{}), nil
}

func (ep *endpoint) GetIpMulticastInterface(fidl.Context) (socket.BaseNetworkSocketGetIpMulticastInterfaceResult, error) {
	var v tcpip.MulticastInterfaceOption
	if err := ep.ep.GetSockOpt(&v); err != nil {
		return socket.BaseNetworkSocketGetIpMulticastInterfaceResultWithErr(tcpipErrorToCode(err)), nil
	}
	var addr fidlnet.Ipv4Address
	if len(v.InterfaceAddr) == header.IPv4AddressSize {
		copy(addr.Addr[:], v.InterfaceAddr)
	}
	return socket.BaseNetworkSocketGetIpMulticastInterfaceResultWithResponse(
		socket.BaseNetworkSocketGetIpMulticastInterfaceResponse{
			Value: addr,
		},
	), nil
}

func (ep *endpoint) SetIpMulticastLoopback(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpMulticastLoopbackResult, error) {
	ep.ep.SocketOptions().SetMulticastLoop(value)
	return socket.BaseNetworkSocketSetIpMulticastLoopbackResultWithResponse(socket.BaseNetworkSocketSetIpMulticastLoopbackResponse{}), nil
}

func (ep *endpoint) GetIpMulticastLoopback(fidl.Context) (socket.BaseNetworkSocketGetIpMulticastLoopbackResult, error) {
	value := ep.ep.SocketOptions().GetMulticastLoop()
	return socket.BaseNetworkSocketGetIpMulticastLoopbackResultWithResponse(socket.BaseNetworkSocketGetIpMulticastLoopbackResponse{Value: value}), nil
}

func (ep *endpoint) SetIpTypeOfService(_ fidl.Context, value uint8) (socket.BaseNetworkSocketSetIpTypeOfServiceResult, error) {
	if err := ep.ep.SetSockOptInt(tcpip.IPv4TOSOption, int(value)); err != nil {
		return socket.BaseNetworkSocketSetIpTypeOfServiceResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketSetIpTypeOfServiceResultWithResponse(socket.BaseNetworkSocketSetIpTypeOfServiceResponse{}), nil
}

func (ep *endpoint) GetIpTypeOfService(fidl.Context) (socket.BaseNetworkSocketGetIpTypeOfServiceResult, error) {
	value, err := ep.ep.GetSockOptInt(tcpip.IPv4TOSOption)
	if err != nil {
		return socket.BaseNetworkSocketGetIpTypeOfServiceResultWithErr(tcpipErrorToCode(err)), nil
	}
	if value < 0 || value > math.MaxUint8 {
		value = 0
	}
	return socket.BaseNetworkSocketGetIpTypeOfServiceResultWithResponse(socket.BaseNetworkSocketGetIpTypeOfServiceResponse{Value: uint8(value)}), nil
}

func (ep *endpoint) AddIpMembership(_ fidl.Context, membership socket.IpMulticastMembership) (socket.BaseNetworkSocketAddIpMembershipResult, error) {
	opt := tcpip.AddMembershipOption{
		NIC:           tcpip.NICID(membership.Iface),
		InterfaceAddr: toTcpIpAddressDroppingUnspecifiedv4(membership.LocalAddr),
		MulticastAddr: toTcpIpAddressDroppingUnspecifiedv4(membership.McastAddr),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketAddIpMembershipResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketAddIpMembershipResultWithResponse(socket.BaseNetworkSocketAddIpMembershipResponse{}), nil
}

func (ep *endpoint) DropIpMembership(_ fidl.Context, membership socket.IpMulticastMembership) (socket.BaseNetworkSocketDropIpMembershipResult, error) {
	opt := tcpip.RemoveMembershipOption{
		NIC:           tcpip.NICID(membership.Iface),
		InterfaceAddr: toTcpIpAddressDroppingUnspecifiedv4(membership.LocalAddr),
		MulticastAddr: toTcpIpAddressDroppingUnspecifiedv4(membership.McastAddr),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketDropIpMembershipResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketDropIpMembershipResultWithResponse(socket.BaseNetworkSocketDropIpMembershipResponse{}), nil
}

func (ep *endpoint) AddIpv6Membership(_ fidl.Context, membership socket.Ipv6MulticastMembership) (socket.BaseNetworkSocketAddIpv6MembershipResult, error) {
	opt := tcpip.AddMembershipOption{
		NIC:           tcpip.NICID(membership.Iface),
		MulticastAddr: toTcpIpAddressDroppingUnspecifiedv6(membership.McastAddr),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketAddIpv6MembershipResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketAddIpv6MembershipResultWithResponse(socket.BaseNetworkSocketAddIpv6MembershipResponse{}), nil
}

func (ep *endpoint) DropIpv6Membership(_ fidl.Context, membership socket.Ipv6MulticastMembership) (socket.BaseNetworkSocketDropIpv6MembershipResult, error) {
	opt := tcpip.RemoveMembershipOption{
		NIC:           tcpip.NICID(membership.Iface),
		MulticastAddr: toTcpIpAddressDroppingUnspecifiedv6(membership.McastAddr),
	}
	if err := ep.ep.SetSockOpt(&opt); err != nil {
		return socket.BaseNetworkSocketDropIpv6MembershipResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketDropIpv6MembershipResultWithResponse(socket.BaseNetworkSocketDropIpv6MembershipResponse{}), nil
}

func (ep *endpoint) SetIpv6ReceiveTrafficClass(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpv6ReceiveTrafficClassResult, error) {
	ep.ep.SocketOptions().SetReceiveTClass(value)
	return socket.BaseNetworkSocketSetIpv6ReceiveTrafficClassResultWithResponse(socket.BaseNetworkSocketSetIpv6ReceiveTrafficClassResponse{}), nil
}

func (ep *endpoint) GetIpv6ReceiveTrafficClass(fidl.Context) (socket.BaseNetworkSocketGetIpv6ReceiveTrafficClassResult, error) {
	value := ep.ep.SocketOptions().GetReceiveTClass()
	return socket.BaseNetworkSocketGetIpv6ReceiveTrafficClassResultWithResponse(socket.BaseNetworkSocketGetIpv6ReceiveTrafficClassResponse{Value: value}), nil
}

func (ep *endpoint) SetIpReceiveTypeOfService(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpReceiveTypeOfServiceResult, error) {
	ep.ep.SocketOptions().SetReceiveTOS(value)
	return socket.BaseNetworkSocketSetIpReceiveTypeOfServiceResultWithResponse(socket.BaseNetworkSocketSetIpReceiveTypeOfServiceResponse{}), nil
}

func (ep *endpoint) GetIpReceiveTypeOfService(fidl.Context) (socket.BaseNetworkSocketGetIpReceiveTypeOfServiceResult, error) {
	value := ep.ep.SocketOptions().GetReceiveTOS()
	return socket.BaseNetworkSocketGetIpReceiveTypeOfServiceResultWithResponse(socket.BaseNetworkSocketGetIpReceiveTypeOfServiceResponse{Value: value}), nil
}

func (ep *endpoint) SetIpPacketInfo(_ fidl.Context, value bool) (socket.BaseNetworkSocketSetIpPacketInfoResult, error) {
	ep.ep.SocketOptions().SetReceivePacketInfo(value)
	return socket.BaseNetworkSocketSetIpPacketInfoResultWithResponse(socket.BaseNetworkSocketSetIpPacketInfoResponse{}), nil
}

func (ep *endpoint) GetIpPacketInfo(fidl.Context) (socket.BaseNetworkSocketGetIpPacketInfoResult, error) {
	value := ep.ep.SocketOptions().GetReceivePacketInfo()
	return socket.BaseNetworkSocketGetIpPacketInfoResultWithResponse(socket.BaseNetworkSocketGetIpPacketInfoResponse{Value: value}), nil
}

// endpointWithSocket implements a network socket that uses a zircon socket for
// its data plane. This structure creates a pair of goroutines which are
// responsible for moving data and signals between the underlying
// tcpip.Endpoint and the zircon socket.
type endpointWithSocket struct {
	endpoint

	local, peer zx.Socket

	// These channels enable coordination of orderly shutdown of loops, handles,
	// and endpoints. See the comment on `close` for more information.
	mu struct {
		sync.Mutex

		// loop{Read,Write}Done are signaled iff loop{Read,Write} have exited,
		// respectively.
		loopReadDone, loopWriteDone <-chan struct{}
	}

	// closing is signaled iff close has been called.
	closing chan struct{}

	// This is used to make sure that endpoint.close only cleans up its
	// resources once - the first time it was closed.
	closeOnce sync.Once

	// Used to unblock waiting to write when SO_LINGER is enabled.
	linger chan struct{}

	onHUpOnce sync.Once

	// onHUp is used to register callback for closing events.
	onHUp waiter.Entry

	// onListen is used to register callbacks for listening sockets.
	onListen sync.Once

	// onConnect is used to register callbacks for connected sockets.
	onConnect sync.Once
}

func newEndpointWithSocket(ep tcpip.Endpoint, wq *waiter.Queue, transProto tcpip.TransportProtocolNumber, netProto tcpip.NetworkProtocolNumber, ns *Netstack) (*endpointWithSocket, error) {
	localS, peerS, err := zx.NewSocket(zx.SocketStream)
	if err != nil {
		return nil, err
	}

	eps := &endpointWithSocket{
		endpoint: endpoint{
			ep:         ep,
			wq:         wq,
			transProto: transProto,
			netProto:   netProto,
			ns:         ns,
			pending: signaler{
				eventsToSignals: eventsToStreamSignals,
				readiness:       ep.Readiness,
				signalPeer:      localS.Handle().SignalPeer,
			},
		},
		local:   localS,
		peer:    peerS,
		closing: make(chan struct{}),
		linger:  make(chan struct{}),
	}

	// Add the endpoint before registering callback for hangup event.
	// The callback could be called soon after registration, where the
	// endpoint is attempted to be removed from the map.
	ns.onAddEndpoint(&eps.endpoint)

	eps.onHUp = waiter.NewFunctionEntry(waiter.EventHUp, func(waiter.EventMask) {
		eps.HUp()
	})
	eps.wq.EventRegister(&eps.onHUp)

	return eps, nil
}

func (eps *endpointWithSocket) HUp() {
	eps.onHUpOnce.Do(func() {
		if !eps.endpoint.ns.onRemoveEndpoint(eps.endpoint.key) {
			_ = syslog.Errorf("endpoint map delete error, endpoint with key %d does not exist", eps.endpoint.key)
		}
		// Run this in a separate goroutine to avoid deadlock.
		//
		// The waiter.Queue lock is held by the caller of this callback.
		// close() blocks on completions of `loop*`, which
		// depend on acquiring waiter.Queue lock to unregister events.
		go func() {
			eps.wq.EventUnregister(&eps.onHUp)
			eps.close()
		}()
	})
}

type endpointWithEvent struct {
	endpoint

	local, peer zx.Handle

	entry waiter.Entry
}

func (epe *endpointWithEvent) describe() (zx.Handle, error) {
	// TODO(https://fxbug.dev/77623): The rights on this handle should be capped at the connection's.
	event, err := epe.peer.Duplicate(zx.RightTransfer | zx.RightWait)
	_ = syslog.DebugTf("Describe", "%p: err=%v", epe, err)
	return event, err
}

func (epe *endpointWithEvent) Connect(_ fidl.Context, address fidlnet.SocketAddress) (socket.BaseNetworkSocketConnectResult, error) {
	if err := epe.endpoint.connect(address); err != nil {
		return socket.BaseNetworkSocketConnectResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketConnectResultWithResponse(socket.BaseNetworkSocketConnectResponse{}), nil
}

func (epe *endpointWithEvent) shutdown(how socket.ShutdownMode) (posix.Errno, error) {
	var signals zx.Signals
	var flags tcpip.ShutdownFlags

	if how&socket.ShutdownModeRead != 0 {
		signals |= zxsocket.SignalDatagramShutdownRead
		flags |= tcpip.ShutdownRead
	}
	if how&socket.ShutdownModeWrite != 0 {
		signals |= zxsocket.SignalDatagramShutdownWrite
		flags |= tcpip.ShutdownWrite
	}
	if flags == 0 {
		return posix.ErrnoEinval, nil
	}

	if err := epe.ep.Shutdown(flags); err != nil {
		return tcpipErrorToCode(err), nil
	}
	if flags&tcpip.ShutdownRead != 0 {
		epe.wq.EventUnregister(&epe.entry)
	}
	if err := epe.local.SignalPeer(0, signals); err != nil {
		return 0, err
	}
	return 0, nil
}

func (epe *endpointWithEvent) Shutdown(_ fidl.Context, how socket.ShutdownMode) (socket.BaseNetworkSocketShutdownResult, error) {
	errno, err := epe.shutdown(how)
	_ = syslog.DebugTf("shutdown", "%p: how=%s errno=%s err=%v", epe, how, errno, err)
	if err != nil {
		return socket.BaseNetworkSocketShutdownResult{}, err
	}
	if errno != 0 {
		return socket.BaseNetworkSocketShutdownResultWithErr(errno), nil
	}
	return socket.BaseNetworkSocketShutdownResultWithResponse(socket.BaseNetworkSocketShutdownResponse{}), nil
}

const localSignalClosing = zx.SignalUser1

// close destroys the endpoint and releases associated resources.
//
// When called, close signals loopRead and loopWrite (via closing and
// local) to exit, and then blocks until its arguments are signaled. close
// is typically called with loop{Read,Write}Done.
//
// Note, calling close on an endpoint that has already been closed is safe as
// the cleanup work will only be done once.
func (eps *endpointWithSocket) close() {
	eps.closeOnce.Do(func() {
		// Interrupt waits on notification channels. Notification reads
		// are always combined with closing in a select statement.
		close(eps.closing)

		// Interrupt waits on endpoint.local. Handle waits always
		// include localSignalClosing.
		if err := eps.local.Handle().Signal(0, localSignalClosing); err != nil {
			panic(err)
		}

		// Grab the loop channels _after_ having closed `eps.closing` to avoid a
		// race in which the loops are allowed to start without guaranteeing that
		// this routine will wait for them to return.
		eps.mu.Lock()
		channels := []<-chan struct{}{
			eps.mu.loopReadDone,
			eps.mu.loopWriteDone,
		}
		eps.mu.Unlock()

		// The interruptions above cause our loops to exit. Wait until
		// they do before releasing resources they may be using.
		for _, ch := range channels {
			if ch != nil {
				for range ch {
				}
			}
		}

		if err := eps.local.Close(); err != nil {
			panic(err)
		}

		eps.ep.Close()

		_ = syslog.DebugTf("close", "%p", eps)
	})
}

func (eps *endpointWithSocket) Listen(_ fidl.Context, backlog int16) (socket.StreamSocketListenResult, error) {
	if backlog < 0 {
		backlog = 0
	}

	// Accept one more than the configured listen backlog to keep in parity with
	// Linux. Ref, because of missing equality check here:
	// https://github.com/torvalds/linux/blob/7acac4b3196/include/net/sock.h#L937
	backlog++

	if err := eps.ep.Listen(int(backlog)); err != nil {
		return socket.StreamSocketListenResultWithErr(tcpipErrorToCode(err)), nil
	}

	// It is possible to call `listen` on a connected socket - such a call would
	// fail above, so we register the callback only in the success case to avoid
	// incorrectly handling events on connected sockets.
	eps.onListen.Do(func() {
		eps.pending.supported = waiter.EventIn
		var entry waiter.Entry
		cb := func() {
			err := eps.pending.update()
			switch err := err.(type) {
			case nil:
				return
			case *zx.Error:
				switch err.Status {
				case zx.ErrBadHandle, zx.ErrPeerClosed:
					// The endpoint is closing -- this is possible when an incoming
					// connection races with the listening endpoint being closed.
					go eps.wq.EventUnregister(&entry)
					return
				}
			}
			panic(err)
		}
		entry = waiter.NewFunctionEntry(eps.pending.supported, func(waiter.EventMask) {
			cb()
		})
		eps.wq.EventRegister(&entry)

		// We're registering after calling Listen, so we might've missed an event.
		// Call the callback once to check for already-present incoming
		// connections.
		cb()
	})

	_ = syslog.DebugTf("listen", "%p: backlog=%d", eps, backlog)

	return socket.StreamSocketListenResultWithResponse(socket.StreamSocketListenResponse{}), nil
}

func (eps *endpointWithSocket) startReadWriteLoops() {
	eps.mu.Lock()
	defer eps.mu.Unlock()
	select {
	case <-eps.closing:
	default:
		if err := eps.local.Handle().SignalPeer(0, zxsocket.SignalStreamConnected); err != nil {
			panic(err)
		}
		for _, m := range []struct {
			done *<-chan struct{}
			fn   func(chan<- struct{})
		}{
			{&eps.mu.loopReadDone, eps.loopRead},
			{&eps.mu.loopWriteDone, eps.loopWrite},
		} {
			ch := make(chan struct{})
			*m.done = ch
			go m.fn(ch)
		}
	}
}

func (eps *endpointWithSocket) describe() (zx.Handle, error) {
	// TODO(https://fxbug.dev/77623): The rights on this handle should be capped at the connection's.
	socket, err := eps.peer.Handle().Duplicate(zx.RightTransfer | zx.RightsIO | zx.RightWait | zx.RightInspect)
	_ = syslog.DebugTf("Describe", "%p: err=%v", eps, err)
	return socket, err
}

func (eps *endpointWithSocket) Connect(_ fidl.Context, address fidlnet.SocketAddress) (socket.BaseNetworkSocketConnectResult, error) {
	err := eps.endpoint.connect(address)

	switch err.(type) {
	case *tcpip.ErrConnectStarted, nil:
		// It is possible to call `connect` on a listening socket - such a call
		// would fail above, so we register the callback only in the success case
		// to avoid incorrectly handling events on connected sockets.
		eps.onConnect.Do(func() {
			var (
				once  sync.Once
				entry waiter.Entry
			)
			cb := func(m waiter.EventMask) {
				once.Do(func() {
					go eps.wq.EventUnregister(&entry)
					if m&waiter.EventErr == 0 {
						eps.startReadWriteLoops()
					} else {
						eps.HUp()
					}
				})
			}
			entry = waiter.NewFunctionEntry(waiter.EventOut|waiter.EventErr, cb)
			eps.wq.EventRegister(&entry)

			// We're registering after calling Connect, so we might've missed an
			// event. Call the callback once to check for an already-complete (even
			// with error) handshake.
			if m := eps.ep.Readiness(waiter.EventOut | waiter.EventErr); m != 0 {
				cb(m)
			}
		})
	}

	if err != nil {
		return socket.BaseNetworkSocketConnectResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.BaseNetworkSocketConnectResultWithResponse(socket.BaseNetworkSocketConnectResponse{}), nil
}

func (eps *endpointWithSocket) Accept(wantAddr bool) (posix.Errno, *tcpip.FullAddress, *endpointWithSocket, error) {
	var addr *tcpip.FullAddress
	if wantAddr {
		addr = new(tcpip.FullAddress)
	}
	ep, wq, err := eps.endpoint.ep.Accept(addr)
	if err != nil {
		return tcpipErrorToCode(err), nil, nil, nil
	}
	{
		if err := eps.pending.update(); err != nil {
			panic(err)
		}
	}

	switch localAddr, err := ep.GetLocalAddress(); err.(type) {
	case *tcpip.ErrNotConnected:
		// This should never happen as of writing as GetLocalAddress does not
		// actually return any errors. However, we handle the tcpip.ErrNotConnected
		// case now for the same reasons as mentioned below for the
		// ep.GetRemoteAddress case.
		_ = syslog.DebugTf("accept", "%p: disconnected", eps)
	case nil:
		switch remoteAddr, err := ep.GetRemoteAddress(); err.(type) {
		case *tcpip.ErrNotConnected:
			// GetRemoteAddress returns a tcpip.ErrNotConnected error if ep is no
			// longer connected. This can happen if the endpoint was closed after the
			// call to Accept returned, but before this point. A scenario this was
			// actually witnessed was when a TCP RST was received after the call to
			// Accept returned, but before this point. If GetRemoteAddress returns
			// other (unexpected) errors, panic.
			_ = syslog.DebugTf("accept", "%p: local=%+v, disconnected", eps, localAddr)
		case nil:
			_ = syslog.DebugTf("accept", "%p: local=%+v, remote=%+v", eps, localAddr, remoteAddr)
		default:
			panic(err)
		}
	default:
		panic(err)
	}

	{
		eps, err := newEndpointWithSocket(ep, wq, eps.transProto, eps.netProto, eps.endpoint.ns)
		if err != nil {
			return 0, nil, nil, err
		}

		// NB: signal connectedness before handling any error below to ensure
		// correct interpretation in fdio.
		//
		// See //sdk/lib/fdio/socket.cc:stream_socket::wait_begin/wait_end for
		// details on how fdio infers the error code from asserted signals.
		eps.onConnect.Do(func() { eps.startReadWriteLoops() })

		// Check if the endpoint has already encountered an error since
		// our installed callback will not fire in this case.
		if ep.Readiness(waiter.EventErr)&waiter.EventErr != 0 {
			eps.HUp()
		}

		return 0, addr, eps, nil
	}
}

// loopWrite shuttles signals and data from the zircon socket to the tcpip.Endpoint.
func (eps *endpointWithSocket) loopWrite(ch chan<- struct{}) {
	defer close(ch)

	const sigs = zx.SignalSocketReadable | zx.SignalSocketPeerWriteDisabled | localSignalClosing

	waitEntry, notifyCh := waiter.NewChannelEntry(waiter.EventOut)
	eps.wq.EventRegister(&waitEntry)
	defer eps.wq.EventUnregister(&waitEntry)

	reader := socketReader{
		socket: eps.local,
	}
	for {
		reader.lastError = nil
		reader.lastRead = 0

		eps.terminal.mu.Lock()
		n, err := eps.ep.Write(&reader, tcpip.WriteOptions{
			// We must write atomically in order to guarantee all the data fetched
			// from the zircon socket is consumed by the endpoint.
			Atomic: true,
		})
		eps.terminal.setLocked(err)
		eps.terminal.mu.Unlock()

		if n != int64(reader.lastRead) {
			panic(fmt.Sprintf("partial write into endpoint (%s); got %d, want %d", err, n, reader.lastRead))
		}
		// TODO(https://fxbug.dev/35006): Handle all transport write errors.
		switch err.(type) {
		case nil, *tcpip.ErrBadBuffer:
			switch err := reader.lastError.(type) {
			case nil:
				continue
			case *zx.Error:
				switch err.Status {
				case zx.ErrShouldWait:
					obs, err := zxwait.WaitContext(context.Background(), zx.Handle(eps.local), sigs)
					if err != nil {
						panic(err)
					}
					switch {
					case obs&zx.SignalSocketReadable != 0:
						// The client might have written some data into the socket.
						// Always continue to the loop below and try to read even if the
						// signals show the client has closed the socket.
						continue
					case obs&localSignalClosing != 0:
						// We're closing the endpoint.
						return
					case obs&zx.SignalSocketPeerWriteDisabled != 0:
						// Fallthrough.
					default:
						panic(fmt.Sprintf("impossible signals observed: %b/%b", obs, sigs))
					}
					fallthrough
				case zx.ErrBadState:
					// Reading has been disabled for this socket endpoint.
					switch err := eps.ep.Shutdown(tcpip.ShutdownWrite); err.(type) {
					case nil, *tcpip.ErrNotConnected:
						// Shutdown can return ErrNotConnected if the endpoint was
						// connected but no longer is.
					default:
						panic(err)
					}
					return
				}
			}
			panic(err)
		case *tcpip.ErrNotConnected:
			// Write never returns ErrNotConnected except for endpoints that were
			// never connected. Such endpoints should never reach this loop.
			panic(fmt.Sprintf("connected endpoint returned %s", err))
		case *tcpip.ErrWouldBlock:
			// NB: we can't select on closing here because the client may have
			// written some data into the buffer and then immediately closed the
			// socket.
			//
			// We must wait until the linger timeout.
			select {
			case <-eps.linger:
				return
			case <-notifyCh:
				continue
			}
		case *tcpip.ErrConnectionRefused:
			// TODO(https://fxbug.dev/61594): Allow the socket to be reused for
			// another connection attempt to match Linux.
			return
		case *tcpip.ErrClosedForSend:
			// Shut the endpoint down *only* if it is not already in an error
			// state; an endpoint in an error state will soon be fully closed down,
			// and shutting it down here would cause signals to be asserted twice,
			// which can produce races in the client.
			if eps.ep.Readiness(waiter.EventErr)&waiter.EventErr == 0 {
				if err := eps.local.SetDisposition(0, zx.SocketDispositionWriteDisabled); err != nil {
					panic(err)
				}
				_ = syslog.DebugTf("zx_socket_set_disposition", "%p: disposition=0, disposition_peer=ZX_SOCKET_DISPOSITION_WRITE_DISABLED", eps)
			}
			return
		case *tcpip.ErrConnectionAborted, *tcpip.ErrConnectionReset, *tcpip.ErrNetworkUnreachable, *tcpip.ErrNoRoute:
			return
		case *tcpip.ErrTimeout:
			// The maximum duration of missing ACKs was reached, or the maximum
			// number of unacknowledged keepalives was reached.
			return
		default:
			_ = syslog.Errorf("TCP Endpoint.Write(): %s", err)
		}
	}
}

// loopRead shuttles signals and data from the tcpip.Endpoint to the zircon socket.
func (eps *endpointWithSocket) loopRead(ch chan<- struct{}) {
	defer close(ch)

	const sigs = zx.SignalSocketWritable | zx.SignalSocketWriteDisabled | localSignalClosing

	inEntry, inCh := waiter.NewChannelEntry(waiter.EventIn)
	eps.wq.EventRegister(&inEntry)
	defer eps.wq.EventUnregister(&inEntry)

	writer := socketWriter{
		socket: eps.local,
	}
	for {
		eps.terminal.mu.Lock()
		res, err := eps.ep.Read(&writer, tcpip.ReadOptions{})
		eps.terminal.setLocked(err)
		eps.terminal.mu.Unlock()
		// TODO(https://fxbug.dev/35006): Handle all transport read errors.
		switch err.(type) {
		case *tcpip.ErrNotConnected:
			// Read never returns ErrNotConnected except for endpoints that were
			// never connected. Such endpoints should never reach this loop.
			panic(fmt.Sprintf("connected endpoint returned %s", err))
		case *tcpip.ErrTimeout:
			// At the time of writing, this error indicates that a TCP connection
			// has failed. This can occur during the TCP handshake if the peer
			// fails to respond to a SYN within 60 seconds, or if the retransmit
			// logic gives up after 60 seconds of missing ACKs from the peer, or if
			// the maximum number of unacknowledged keepalives is reached.
			//
			// The connection was alive but now is dead - this is equivalent to
			// having received a TCP RST.
			return
		case *tcpip.ErrConnectionRefused:
			// TODO(https://fxbug.dev/61594): Allow the socket to be reused for
			// another connection attempt to match Linux.
			return
		case *tcpip.ErrWouldBlock:
			select {
			case <-inCh:
				continue
			case <-eps.closing:
				// We're shutting down.
				return
			}
		case *tcpip.ErrClosedForReceive:
			// Shut the endpoint down *only* if it is not already in an error
			// state; an endpoint in an error state will soon be fully closed down,
			// and shutting it down here would cause signals to be asserted twice,
			// which can produce races in the client.
			if eps.ep.Readiness(waiter.EventErr)&waiter.EventErr == 0 {
				if err := eps.local.SetDisposition(zx.SocketDispositionWriteDisabled, 0); err != nil {
					panic(err)
				}
				_ = syslog.DebugTf("zx_socket_set_disposition", "%p: disposition=ZX_SOCKET_DISPOSITION_WRITE_DISABLED, disposition_peer=0", eps)
			}
			return
		case *tcpip.ErrConnectionAborted, *tcpip.ErrConnectionReset, *tcpip.ErrNetworkUnreachable, *tcpip.ErrNoRoute:
			return
		case nil, *tcpip.ErrBadBuffer:
			if err == nil {
				eps.ep.ModerateRecvBuf(res.Count)
			}
			// `tcpip.Endpoint.Read` returns a nil error if _anything_ was written
			// - even if the writer returned an error - we always want to handle
			// those errors.
			switch err := writer.lastError.(type) {
			case nil:
				continue
			case *zx.Error:
				switch err.Status {
				case zx.ErrShouldWait:
					obs, err := zxwait.WaitContext(context.Background(), zx.Handle(eps.local), sigs)
					if err != nil {
						panic(err)
					}
					switch {
					case obs&zx.SignalSocketWritable != 0:
						continue
					case obs&localSignalClosing != 0:
						// We're shutting down.
						return
					case obs&zx.SignalSocketWriteDisabled != 0:
						// Fallthrough.
					default:
						panic(fmt.Sprintf("impossible signals observed: %b/%b", obs, sigs))
					}
					fallthrough
				case zx.ErrBadState:
					// Writing has been disabled for this zircon socket endpoint. This
					// happens when the client called the Shutdown FIDL method, with
					// socket.ShutdownModeRead: because the client will not read from this
					// zircon socket endpoint anymore, writes are disabled.
					//
					// Clients can't revert a shutdown, so we can terminate this infinite
					// loop here.
					return
				}
			}
			panic(err)
		default:
			_ = syslog.Errorf("Endpoint.Read(): %s", err)
		}
	}
}

func (eps *endpointWithSocket) shutdown(how socket.ShutdownMode) (posix.Errno, error) {
	var disposition, dispositionPeer uint32

	// Typically, a Shutdown(Write) is equivalent to disabling the writes on the
	// local zircon socket. Similarly, a Shutdown(Read) translates to
	// disabling the writes on the peer zircon socket.
	// Here, the client wants to shutdown its endpoint but we are calling
	// setDisposition on our local zircon socket (eg, the client's peer), which is
	// why disposition and dispositionPeer seem backwards.
	if how&socket.ShutdownModeRead != 0 {
		disposition = zx.SocketDispositionWriteDisabled
		how ^= socket.ShutdownModeRead
	}
	if how&socket.ShutdownModeWrite != 0 {
		dispositionPeer = zx.SocketDispositionWriteDisabled
		how ^= socket.ShutdownModeWrite
	}
	if how != 0 || (disposition == 0 && dispositionPeer == 0) {
		return posix.ErrnoEinval, nil
	}

	if h := eps.local.Handle(); !h.IsValid() {
		return tcpipErrorToCode(&tcpip.ErrNotConnected{}), nil
	}

	if err := eps.local.SetDisposition(disposition, dispositionPeer); err != nil {
		return 0, err
	}

	// Only handle a shutdown read on the endpoint here. Shutdown write is
	// performed by the loopWrite goroutine: it ensures that it happens *after*
	// all the buffered data in the zircon socket was read.
	if disposition&zx.SocketDispositionWriteDisabled != 0 {
		switch err := eps.ep.Shutdown(tcpip.ShutdownRead); err.(type) {
		case nil, *tcpip.ErrNotConnected:
			// Shutdown can return ErrNotConnected if the endpoint was connected but
			// no longer is, in which case the loopRead is also expected to be
			// terminating.
			eps.mu.Lock()
			ch := eps.mu.loopReadDone
			eps.mu.Unlock()
			if ch != nil {
				for range ch {
				}
			}
		default:
			panic(err)
		}
	}

	return 0, nil
}

func (eps *endpointWithSocket) Shutdown(_ fidl.Context, how socket.ShutdownMode) (socket.BaseNetworkSocketShutdownResult, error) {
	errno, err := eps.shutdown(how)
	_ = syslog.DebugTf("shutdown", "%p: how=%s errno=%s err=%v", eps, how, errno, err)
	if err != nil {
		return socket.BaseNetworkSocketShutdownResult{}, err
	}
	if errno != 0 {
		return socket.BaseNetworkSocketShutdownResultWithErr(errno), nil
	}
	return socket.BaseNetworkSocketShutdownResultWithResponse(socket.BaseNetworkSocketShutdownResponse{}), nil
}

type datagramSocket struct {
	*endpointWithEvent

	cancel context.CancelFunc
}

type networkDatagramSocket struct {
	datagramSocket
}

type datagramSocketImpl struct {
	networkDatagramSocket
}

var _ socket.DatagramSocketWithCtx = (*datagramSocketImpl)(nil)

func (s *datagramSocketImpl) Describe(fidl.Context) (fidlio.NodeInfo, error) {
	event, err := s.describe()
	if err != nil {
		return fidlio.NodeInfo{}, err
	}
	return fidlio.NodeInfoWithDatagramSocket(fidlio.DatagramSocket{Event: event}), nil
}

func (s *datagramSocketImpl) Describe2(_ fidl.Context, query fidlio.ConnectionInfoQuery) (fidlio.ConnectionInfo, error) {
	var connectionInfo fidlio.ConnectionInfo
	if query&fidlio.ConnectionInfoQueryRepresentation != 0 {
		event, err := s.describe()
		if err != nil {
			return connectionInfo, err
		}
		var datagramSocket fidlio.DatagramSocketInfo
		datagramSocket.SetEvent(event)
		connectionInfo.SetRepresentation(fidlio.RepresentationWithDatagramSocket(datagramSocket))
	}
	// TODO(https://fxbug.dev/77623): Populate the rights requested by the client at connection.
	rights := fidlio.RStarDir
	if query&fidlio.ConnectionInfoQueryRights != 0 {
		connectionInfo.SetRights(rights)
	}
	if query&fidlio.ConnectionInfoQueryAvailableOperations != 0 {
		abilities := fidlio.OperationsReadBytes | fidlio.OperationsWriteBytes | fidlio.OperationsGetAttributes
		connectionInfo.SetAvailableOperations(abilities & rights)
	}
	return connectionInfo, nil
}

func (s *datagramSocket) socketControlMessagesToFIDL(cmsg tcpip.ControlMessages) socket.SocketRecvControlData {
	s.mu.RLock()
	sockOptTimestamp := s.endpoint.mu.sockOptTimestamp
	s.mu.RUnlock()

	var controlData socket.SocketRecvControlData
	switch sockOptTimestamp {
	case socket.TimestampOptionDisabled:
	case socket.TimestampOptionNanosecond:
		controlData.SetTimestamp(socket.TimestampWithNanoseconds(cmsg.Timestamp.UnixNano()))
		// TODO(https://fxbug.dev/86944): Remove after ABI transition.
		controlData.SetTimestampNs(cmsg.Timestamp.UnixNano())
	case socket.TimestampOptionMicrosecond:
		controlData.SetTimestamp(socket.TimestampWithMicroseconds(cmsg.Timestamp.UnixMicro()))
	default:
		panic(fmt.Sprintf("unknown timestamp option: %d", sockOptTimestamp))
	}

	return controlData
}

func (s *datagramSocket) ipControlMessagesToFIDL(cmsg tcpip.ControlMessages) socket.IpRecvControlData {
	var controlData socket.IpRecvControlData
	if s.ep.SocketOptions().GetReceiveTOS() && cmsg.HasTOS {
		controlData.SetTos(cmsg.TOS)
	}
	return controlData
}

func (s *datagramSocket) networkSocketControlMessagesToFIDL(cmsg tcpip.ControlMessages) socket.NetworkSocketRecvControlData {
	var controlData socket.NetworkSocketRecvControlData
	if socketControlData := s.socketControlMessagesToFIDL(cmsg); socketControlData != (socket.SocketRecvControlData{}) {
		controlData.SetSocket(socketControlData)
	}
	if ipControlData := s.ipControlMessagesToFIDL(cmsg); ipControlData != (socket.IpRecvControlData{}) {
		controlData.SetIp(ipControlData)
	}
	return controlData
}

func (s *datagramSocketImpl) controlMessagesToFIDL(cmsg tcpip.ControlMessages) socket.DatagramSocketRecvControlData {
	var controlData socket.DatagramSocketRecvControlData
	if networkSocketControlData := s.networkSocketControlMessagesToFIDL(cmsg); networkSocketControlData != (socket.NetworkSocketRecvControlData{}) {
		controlData.SetNetwork(networkSocketControlData)
	}
	return controlData
}

func (s *datagramSocket) close() {
	if s.endpoint.decRef() {
		s.wq.EventUnregister(&s.entry)

		if err := s.local.Close(); err != nil {
			panic(fmt.Sprintf("local.Close() = %s", err))
		}

		if err := s.peer.Close(); err != nil {
			panic(fmt.Sprintf("peer.Close() = %s", err))
		}

		if !s.ns.onRemoveEndpoint(s.endpoint.key) {
			_ = syslog.Errorf("endpoint map delete error, endpoint with key %d does not exist", s.endpoint.key)
		}

		s.ep.Close()

		_ = syslog.DebugTf("close", "%p", s.endpointWithEvent)
	}
	s.cancel()
}

func (s *datagramSocket) CloseDeprecated(fidl.Context) (int32, error) {
	_ = syslog.DebugTf("Close", "%p", s.endpointWithEvent)
	s.close()
	return int32(zx.ErrOk), nil
}

func (s *datagramSocket) Close(fidl.Context) (fidlio.Node2CloseResult, error) {
	_ = syslog.DebugTf("Close", "%p", s.endpointWithEvent)
	s.close()
	return fidlio.Node2CloseResultWithResponse(fidlio.Node2CloseResponse{}), nil
}

func (s *datagramSocketImpl) addConnection(_ fidl.Context, object fidlio.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		// NB: this protocol is not discoverable, so the bindings do not include its name.
		s.datagramSocket.addConnection("fuchsia.posix.socket.DatagramSocket", object, &socket.DatagramSocketWithCtxStub{Impl: s})
	}
}

func (s *datagramSocket) addConnection(prefix string, object fidlio.NodeWithCtxInterfaceRequest, stub fidl.Stub) {
	s.ns.stats.SocketCount.Increment()
	s.endpoint.incRef()
	go func() {
		defer s.ns.stats.SocketCount.Decrement()

		ctx, cancel := context.WithCancel(context.Background())
		s.cancel = cancel
		defer func() {
			// Avoid double close when the peer calls Close and then hangs up.
			if ctx.Err() == nil {
				s.close()
			}
		}()

		component.Serve(ctx, stub, object.Channel, component.ServeOptions{
			OnError: func(err error) {
				// NB: this protocol is not discoverable, so the bindings do not include its name.
				_ = syslog.WarnTf(prefix, "%s", err)
			},
		})
	}()
}

func (s *datagramSocketImpl) Clone(ctx fidl.Context, flags uint32, object fidlio.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	_ = syslog.DebugTf("Clone", "%p: flags=%b", s.endpointWithEvent, flags)

	return nil
}

func (s *datagramSocketImpl) Reopen(ctx fidl.Context, options fidlio.ConnectionOptions, channel zx.Channel) error {
	// TODO(https://fxbug.dev/77623): Implement.
	_ = channel.Close()

	_ = syslog.DebugTf("Clone", "%p: options=%#v", s.endpointWithEvent, options)

	return nil
}

func (s *datagramSocket) recvMsg(opts tcpip.ReadOptions, dataLen uint32) ([]byte, tcpip.ReadResult, tcpip.Error) {
	var b bytes.Buffer
	dst := tcpip.LimitedWriter{
		W: &b,
		N: int64(dataLen),
	}
	res, err := s.ep.Read(&dst, opts)
	if _, ok := err.(*tcpip.ErrBadBuffer); ok && dataLen == 0 {
		err = nil
	}
	if err := s.pending.update(); err != nil {
		panic(err)
	}

	return b.Bytes(), res, err
}

func (s *networkDatagramSocket) recvMsg(wantAddr bool, dataLen uint32, peek bool) (fidlnet.SocketAddress, []byte, uint32, tcpip.ControlMessages, tcpip.Error) {
	bytes, res, err := s.datagramSocket.recvMsg(tcpip.ReadOptions{
		Peek:           peek,
		NeedRemoteAddr: wantAddr,
	}, dataLen)
	if err != nil {
		return fidlnet.SocketAddress{}, nil, 0, tcpip.ControlMessages{}, err
	}

	var addr fidlnet.SocketAddress
	if wantAddr {
		sockaddr := toNetSocketAddress(s.netProto, res.RemoteAddr)
		addr = sockaddr
	}
	return addr, bytes, uint32(res.Total - res.Count), res.ControlMessages, nil
}

func (s *datagramSocketImpl) RecvMsg(_ fidl.Context, wantAddr bool, dataLen uint32, wantControl bool, flags socket.RecvMsgFlags) (socket.DatagramSocketRecvMsgResult, error) {
	addr, data, truncated, cmsg, err := s.recvMsg(wantAddr, dataLen, flags&socket.RecvMsgFlagsPeek != 0)
	if err != nil {
		return socket.DatagramSocketRecvMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	var pAddr *fidlnet.SocketAddress
	if wantAddr {
		pAddr = &addr
	}

	var controlData socket.DatagramSocketRecvControlData
	if wantControl {
		controlData = s.controlMessagesToFIDL(cmsg)
	}

	return socket.DatagramSocketRecvMsgResultWithResponse(socket.DatagramSocketRecvMsgResponse{
		Addr:      pAddr,
		Data:      data,
		Control:   controlData,
		Truncated: truncated,
	}), nil
}

func (s *datagramSocket) sendMsg(to *tcpip.FullAddress, data []uint8) (int64, tcpip.Error) {
	var r bytes.Reader
	r.Reset(data)
	n, err := s.ep.Write(&r, tcpip.WriteOptions{To: to})
	if err != nil {
		if err := s.pending.update(); err != nil {
			panic(err)
		}
		return 0, err
	}
	return n, nil
}

func (s *networkDatagramSocket) sendMsg(addr *fidlnet.SocketAddress, data []uint8) (int64, tcpip.Error) {
	var fullAddr tcpip.FullAddress
	var to *tcpip.FullAddress
	if addr != nil {
		var err error
		fullAddr, err = toTCPIPFullAddress(*addr)
		if err != nil {
			return 0, &tcpip.ErrBadAddress{}
		}
		if s.endpoint.netProto == ipv4.ProtocolNumber && len(fullAddr.Addr) == header.IPv6AddressSize {
			return 0, &tcpip.ErrAddressFamilyNotSupported{}
		}
		to = &fullAddr
	}

	return s.datagramSocket.sendMsg(to, data)
}

func (s *datagramSocketImpl) SendMsg(_ fidl.Context, addr *fidlnet.SocketAddress, data []uint8, control socket.DatagramSocketSendControlData, _ socket.SendMsgFlags) (socket.DatagramSocketSendMsgResult, error) {
	// TODO(https://fxbug.dev/21106): do something with control.
	_ = control

	n, err := s.sendMsg(addr, data)
	if err != nil {
		return socket.DatagramSocketSendMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.DatagramSocketSendMsgResultWithResponse(socket.DatagramSocketSendMsgResponse{Len: n}), nil
}

func (s *datagramSocketImpl) GetInfo(fidl.Context) (socket.DatagramSocketGetInfoResult, error) {
	domain, err := s.domain()
	if err != nil {
		return socket.DatagramSocketGetInfoResultWithErr(tcpipErrorToCode(err)), nil
	}
	var proto socket.DatagramSocketProtocol
	switch transProto := s.transProto; transProto {
	case udp.ProtocolNumber:
		proto = socket.DatagramSocketProtocolUdp
	case header.ICMPv4ProtocolNumber, header.ICMPv6ProtocolNumber:
		proto = socket.DatagramSocketProtocolIcmpEcho
	default:
		panic(fmt.Sprintf("unhandled transport protocol: %d", transProto))
	}
	return socket.DatagramSocketGetInfoResultWithResponse(socket.DatagramSocketGetInfoResponse{
		Domain: domain,
		Proto:  proto,
	}), nil
}

type streamSocketImpl struct {
	*endpointWithSocket

	cancel context.CancelFunc
}

var _ socket.StreamSocketWithCtx = (*streamSocketImpl)(nil)

func newStreamSocket(eps *endpointWithSocket) (socket.StreamSocketWithCtxInterface, error) {
	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return socket.StreamSocketWithCtxInterface{}, err
	}
	s := &streamSocketImpl{
		endpointWithSocket: eps,
	}
	s.addConnection(context.Background(), fidlio.NodeWithCtxInterfaceRequest{Channel: localC})
	_ = syslog.DebugTf("NewStream", "%p", s.endpointWithSocket)
	return socket.StreamSocketWithCtxInterface{Channel: peerC}, nil
}

func (s *streamSocketImpl) close() {
	defer s.cancel()

	if s.endpoint.decRef() {
		linger := s.ep.SocketOptions().GetLinger()

		doClose := func() {
			s.endpointWithSocket.close()

			if err := s.peer.Close(); err != nil {
				panic(err)
			}
		}

		if linger.Enabled {
			// `man 7 socket`:
			//
			//  When enabled, a close(2) or shutdown(2) will not return until all
			//  queued messages for the socket have been successfully sent or the
			//  linger timeout has been reached. Otherwise, the call returns
			//  immediately and the closing is done in the background. When the
			//  socket is closed as part of exit(2), it always lingers in the
			//  background.
			//
			// Thus we must allow linger-amount-of-time for pending writes to flush,
			// and do so synchronously if linger is enabled.
			time.AfterFunc(linger.Timeout, func() { close(s.linger) })
			doClose()
		} else {
			// Here be dragons.
			//
			// Normally, with linger disabled, the socket is immediately closed to
			// the application (accepting no more writes) and lingers for TCP_LINGER2
			// duration. However, because of the use of a zircon socket in front of
			// the netstack endpoint, we can't be sure that all writes have flushed
			// from the zircon socket to the netstack endpoint when we observe
			// `close(3)`. This in turn means we can't close the netstack endpoint
			// (which would start the TCP_LINGER2 timer), because there may still be
			// data pending in the zircon socket (e.g. when the netstack endpoint's
			// send buffer is full). We need *some* condition to break us out of this
			// deadlock.
			//
			// We pick TCP_LINGER2 somewhat arbitrarily. In the worst case, this
			// means that our true TCP linger time will be twice the configured
			// value, but this is the best we can do without rethinking the
			// interfaces.

			// If no data is in the buffer, close synchronously. This is an important
			// optimization that prevents flakiness when a socket is closed and
			// another socket is immediately bound to the port.
			if reader := (socketReader{socket: s.endpointWithSocket.local}); reader.Len() == 0 {
				doClose()
			} else {
				var linger tcpip.TCPLingerTimeoutOption
				if err := s.ep.GetSockOpt(&linger); err != nil {
					panic(fmt.Sprintf("GetSockOpt(%T): %s", linger, err))
				}
				time.AfterFunc(time.Duration(linger), func() { close(s.linger) })

				go doClose()
			}
		}
	}
}

func (s *streamSocketImpl) CloseDeprecated(fidl.Context) (int32, error) {
	_ = syslog.DebugTf("Close", "%p", s.endpointWithSocket)
	s.close()
	return int32(zx.ErrOk), nil
}

func (s *streamSocketImpl) Close(fidl.Context) (fidlio.Node2CloseResult, error) {
	_ = syslog.DebugTf("Close", "%p", s.endpointWithSocket)
	s.close()
	return fidlio.Node2CloseResultWithResponse(fidlio.Node2CloseResponse{}), nil
}

func (s *streamSocketImpl) addConnection(_ fidl.Context, object fidlio.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		s.ns.stats.SocketCount.Increment()
		s.endpoint.incRef()
		go func() {
			defer s.ns.stats.SocketCount.Decrement()

			ctx, cancel := context.WithCancel(context.Background())
			s.cancel = cancel
			defer func() {
				// Avoid double close when the peer calls Close and then hangs up.
				if ctx.Err() == nil {
					s.close()
				}
			}()

			stub := socket.StreamSocketWithCtxStub{Impl: s}
			component.Serve(ctx, &stub, object.Channel, component.ServeOptions{
				OnError: func(err error) {
					// NB: this protocol is not discoverable, so the bindings do not include its name.
					_ = syslog.WarnTf("fuchsia.posix.socket.StreamSocket", "%s", err)
				},
			})
		}()
	}
}

func (s *streamSocketImpl) Clone(ctx fidl.Context, flags uint32, object fidlio.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	_ = syslog.DebugTf("Clone", "%p: flags=%b", s.endpointWithSocket, flags)

	return nil
}

func (s *streamSocketImpl) Reopen(ctx fidl.Context, options fidlio.ConnectionOptions, channel zx.Channel) error {
	// TODO(https://fxbug.dev/77623): Implement.
	_ = channel.Close()

	_ = syslog.DebugTf("Clone", "%p: options=%#v", s.endpointWithSocket, options)

	return nil
}

func (s *streamSocketImpl) Describe(fidl.Context) (fidlio.NodeInfo, error) {
	socket, err := s.describe()
	if err != nil {
		return fidlio.NodeInfo{}, err
	}
	return fidlio.NodeInfoWithStreamSocket(fidlio.StreamSocket{Socket: zx.Socket(socket)}), nil
}

func (s *streamSocketImpl) Describe2(_ fidl.Context, query fidlio.ConnectionInfoQuery) (fidlio.ConnectionInfo, error) {
	var connectionInfo fidlio.ConnectionInfo
	if query&fidlio.ConnectionInfoQueryRepresentation != 0 {
		socket, err := s.describe()
		if err != nil {
			return connectionInfo, err
		}
		var streamSocket fidlio.StreamSocketInfo
		streamSocket.SetSocket(zx.Socket(socket))
		connectionInfo.SetRepresentation(fidlio.RepresentationWithStreamSocket(streamSocket))
	}
	// TODO(https://fxbug.dev/77623): Populate the rights requested by the client at connection.
	rights := fidlio.RStarDir
	if query&fidlio.ConnectionInfoQueryRights != 0 {
		connectionInfo.SetRights(rights)
	}
	if query&fidlio.ConnectionInfoQueryAvailableOperations != 0 {
		abilities := fidlio.OperationsReadBytes | fidlio.OperationsWriteBytes | fidlio.OperationsGetAttributes
		connectionInfo.SetAvailableOperations(abilities & rights)
	}
	return connectionInfo, nil
}

func (s *streamSocketImpl) Accept(_ fidl.Context, wantAddr bool) (socket.StreamSocketAcceptResult, error) {
	code, addr, eps, err := s.endpointWithSocket.Accept(wantAddr)
	if err != nil {
		return socket.StreamSocketAcceptResult{}, err
	}
	if code != 0 {
		return socket.StreamSocketAcceptResultWithErr(code), nil
	}
	streamSocketInterface, err := newStreamSocket(eps)
	if err != nil {
		return socket.StreamSocketAcceptResult{}, err
	}
	// TODO(https://fxbug.dev/67600): this copies a lock; avoid this when FIDL bindings are better.
	response := socket.StreamSocketAcceptResponse{
		S: streamSocketInterface,
	}
	if addr != nil {
		sockaddr := toNetSocketAddress(s.netProto, *addr)
		response.Addr = &sockaddr
	}
	return socket.StreamSocketAcceptResultWithResponse(response), nil
}

func (s *streamSocketImpl) GetInfo(fidl.Context) (socket.StreamSocketGetInfoResult, error) {
	domain, err := s.domain()
	if err != nil {
		return socket.StreamSocketGetInfoResultWithErr(tcpipErrorToCode(err)), nil
	}
	var proto socket.StreamSocketProtocol
	switch transProto := s.transProto; transProto {
	case tcp.ProtocolNumber:
		proto = socket.StreamSocketProtocolTcp
	default:
		panic(fmt.Sprintf("unhandled transport protocol: %d", transProto))
	}
	return socket.StreamSocketGetInfoResultWithResponse(socket.StreamSocketGetInfoResponse{
		Domain: domain,
		Proto:  proto,
	}), nil
}

func (s *streamSocketImpl) SetTcpNoDelay(_ fidl.Context, value bool) (socket.StreamSocketSetTcpNoDelayResult, error) {
	s.ep.SocketOptions().SetDelayOption(!value)
	return socket.StreamSocketSetTcpNoDelayResultWithResponse(socket.StreamSocketSetTcpNoDelayResponse{}), nil
}

func (s *streamSocketImpl) GetTcpNoDelay(fidl.Context) (socket.StreamSocketGetTcpNoDelayResult, error) {
	value := s.ep.SocketOptions().GetDelayOption()
	return socket.StreamSocketGetTcpNoDelayResultWithResponse(
		socket.StreamSocketGetTcpNoDelayResponse{
			Value: !value,
		},
	), nil
}

func (s *streamSocketImpl) SetTcpCork(_ fidl.Context, value bool) (socket.StreamSocketSetTcpCorkResult, error) {
	s.ep.SocketOptions().SetCorkOption(value)
	return socket.StreamSocketSetTcpCorkResultWithResponse(socket.StreamSocketSetTcpCorkResponse{}), nil
}

func (s *streamSocketImpl) GetTcpCork(fidl.Context) (socket.StreamSocketGetTcpCorkResult, error) {
	value := s.ep.SocketOptions().GetCorkOption()
	return socket.StreamSocketGetTcpCorkResultWithResponse(socket.StreamSocketGetTcpCorkResponse{Value: value}), nil
}

func (s *streamSocketImpl) SetTcpQuickAck(_ fidl.Context, value bool) (socket.StreamSocketSetTcpQuickAckResult, error) {
	s.ep.SocketOptions().SetQuickAck(value)
	return socket.StreamSocketSetTcpQuickAckResultWithResponse(socket.StreamSocketSetTcpQuickAckResponse{}), nil
}

func (s *streamSocketImpl) GetTcpQuickAck(fidl.Context) (socket.StreamSocketGetTcpQuickAckResult, error) {
	value := s.ep.SocketOptions().GetQuickAck()
	return socket.StreamSocketGetTcpQuickAckResultWithResponse(socket.StreamSocketGetTcpQuickAckResponse{Value: value}), nil
}

func (s *streamSocketImpl) SetTcpMaxSegment(_ fidl.Context, valueBytes uint32) (socket.StreamSocketSetTcpMaxSegmentResult, error) {
	if err := s.ep.SetSockOptInt(tcpip.MaxSegOption, int(valueBytes)); err != nil {
		return socket.StreamSocketSetTcpMaxSegmentResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpMaxSegmentResultWithResponse(socket.StreamSocketSetTcpMaxSegmentResponse{}), nil
}

func (s *streamSocketImpl) GetTcpMaxSegment(fidl.Context) (socket.StreamSocketGetTcpMaxSegmentResult, error) {
	value, err := s.ep.GetSockOptInt(tcpip.MaxSegOption)
	if err != nil {
		return socket.StreamSocketGetTcpMaxSegmentResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpMaxSegmentResultWithResponse(socket.StreamSocketGetTcpMaxSegmentResponse{
		ValueBytes: uint32(value),
	}), nil
}

func (s *streamSocketImpl) SetTcpKeepAliveIdle(_ fidl.Context, valueSecs uint32) (socket.StreamSocketSetTcpKeepAliveIdleResult, error) {
	// https://github.com/torvalds/linux/blob/f2850dd5ee015bd7b77043f731632888887689c7/net/ipv4/tcp.c#L2991
	if valueSecs < 1 || valueSecs > maxTCPKeepIdle {
		return socket.StreamSocketSetTcpKeepAliveIdleResultWithErr(posix.ErrnoEinval), nil
	}
	opt := tcpip.KeepaliveIdleOption(time.Second * time.Duration(valueSecs))
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpKeepAliveIdleResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpKeepAliveIdleResultWithResponse(socket.StreamSocketSetTcpKeepAliveIdleResponse{}), nil
}

func (s *streamSocketImpl) GetTcpKeepAliveIdle(fidl.Context) (socket.StreamSocketGetTcpKeepAliveIdleResult, error) {
	var value tcpip.KeepaliveIdleOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpKeepAliveIdleResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpKeepAliveIdleResultWithResponse(socket.StreamSocketGetTcpKeepAliveIdleResponse{
		ValueSecs: uint32(time.Duration(value).Seconds()),
	}), nil
}

func (s *streamSocketImpl) SetTcpKeepAliveInterval(_ fidl.Context, valueSecs uint32) (socket.StreamSocketSetTcpKeepAliveIntervalResult, error) {
	// https://github.com/torvalds/linux/blob/f2850dd5ee015bd7b77043f731632888887689c7/net/ipv4/tcp.c#L3008
	if valueSecs < 1 || valueSecs > maxTCPKeepIntvl {
		return socket.StreamSocketSetTcpKeepAliveIntervalResultWithErr(posix.ErrnoEinval), nil
	}
	opt := tcpip.KeepaliveIntervalOption(time.Second * time.Duration(valueSecs))
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpKeepAliveIntervalResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpKeepAliveIntervalResultWithResponse(socket.StreamSocketSetTcpKeepAliveIntervalResponse{}), nil
}

func (s *streamSocketImpl) GetTcpKeepAliveInterval(fidl.Context) (socket.StreamSocketGetTcpKeepAliveIntervalResult, error) {
	var value tcpip.KeepaliveIntervalOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpKeepAliveIntervalResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpKeepAliveIntervalResultWithResponse(socket.StreamSocketGetTcpKeepAliveIntervalResponse{
		ValueSecs: uint32(time.Duration(value).Seconds()),
	}), nil
}

func (s *streamSocketImpl) SetTcpKeepAliveCount(_ fidl.Context, value uint32) (socket.StreamSocketSetTcpKeepAliveCountResult, error) {
	// https://github.com/torvalds/linux/blob/f2850dd5ee015bd7b77043f731632888887689c7/net/ipv4/tcp.c#L3014
	if value < 1 || value > maxTCPKeepCnt {
		return socket.StreamSocketSetTcpKeepAliveCountResultWithErr(posix.ErrnoEinval), nil
	}
	if err := s.ep.SetSockOptInt(tcpip.KeepaliveCountOption, int(value)); err != nil {
		return socket.StreamSocketSetTcpKeepAliveCountResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpKeepAliveCountResultWithResponse(socket.StreamSocketSetTcpKeepAliveCountResponse{}), nil
}

func (s *streamSocketImpl) GetTcpKeepAliveCount(fidl.Context) (socket.StreamSocketGetTcpKeepAliveCountResult, error) {
	value, err := s.ep.GetSockOptInt(tcpip.KeepaliveCountOption)
	if err != nil {
		return socket.StreamSocketGetTcpKeepAliveCountResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpKeepAliveCountResultWithResponse(socket.StreamSocketGetTcpKeepAliveCountResponse{Value: uint32(value)}), nil
}

func (s *streamSocketImpl) SetTcpUserTimeout(_ fidl.Context, valueMillis uint32) (socket.StreamSocketSetTcpUserTimeoutResult, error) {
	opt := tcpip.TCPUserTimeoutOption(time.Millisecond * time.Duration(valueMillis))
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpUserTimeoutResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpUserTimeoutResultWithResponse(socket.StreamSocketSetTcpUserTimeoutResponse{}), nil
}

func (s *streamSocketImpl) GetTcpUserTimeout(fidl.Context) (socket.StreamSocketGetTcpUserTimeoutResult, error) {
	var value tcpip.TCPUserTimeoutOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpUserTimeoutResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpUserTimeoutResultWithResponse(socket.StreamSocketGetTcpUserTimeoutResponse{
		ValueMillis: uint32(time.Duration(value).Milliseconds()),
	}), nil
}

func (s *streamSocketImpl) SetTcpCongestion(_ fidl.Context, value socket.TcpCongestionControl) (socket.StreamSocketSetTcpCongestionResult, error) {
	var cc string
	switch value {
	case socket.TcpCongestionControlReno:
		cc = ccReno
	case socket.TcpCongestionControlCubic:
		cc = ccCubic
	default:
		// Linux returns ENOENT when an invalid congestion
		// control algorithm is specified.
		return socket.StreamSocketSetTcpCongestionResultWithErr(posix.ErrnoEnoent), nil
	}
	opt := tcpip.CongestionControlOption(cc)
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpCongestionResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpCongestionResultWithResponse(socket.StreamSocketSetTcpCongestionResponse{}), nil
}

func (s *streamSocketImpl) GetTcpCongestion(fidl.Context) (socket.StreamSocketGetTcpCongestionResult, error) {
	var value tcpip.CongestionControlOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpCongestionResultWithErr(tcpipErrorToCode(err)), nil
	}
	var cc socket.TcpCongestionControl
	switch string(value) {
	case ccReno:
		cc = socket.TcpCongestionControlReno
	case ccCubic:
		cc = socket.TcpCongestionControlCubic
	default:
		return socket.StreamSocketGetTcpCongestionResultWithErr(posix.ErrnoEopnotsupp), nil
	}
	return socket.StreamSocketGetTcpCongestionResultWithResponse(socket.StreamSocketGetTcpCongestionResponse{Value: cc}), nil
}

func (s *streamSocketImpl) SetTcpDeferAccept(_ fidl.Context, valueSecs uint32) (socket.StreamSocketSetTcpDeferAcceptResult, error) {
	opt := tcpip.TCPDeferAcceptOption(time.Second * time.Duration(valueSecs))
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpDeferAcceptResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpDeferAcceptResultWithResponse(socket.StreamSocketSetTcpDeferAcceptResponse{}), nil
}

func (s *streamSocketImpl) GetTcpDeferAccept(fidl.Context) (socket.StreamSocketGetTcpDeferAcceptResult, error) {
	var value tcpip.TCPDeferAcceptOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpDeferAcceptResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpDeferAcceptResultWithResponse(socket.StreamSocketGetTcpDeferAcceptResponse{
		ValueSecs: uint32(time.Duration(value).Seconds()),
	}), nil
}

func (s *streamSocketImpl) GetTcpInfo(fidl.Context) (socket.StreamSocketGetTcpInfoResult, error) {
	var value tcpip.TCPInfoOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpInfoResultWithErr(tcpipErrorToCode(err)), nil
	}
	var info socket.TcpInfo
	info.SetCaState(func() socket.TcpCongestionControlState {
		switch state := value.CcState; state {
		case tcpip.Open:
			return socket.TcpCongestionControlStateOpen
		case tcpip.RTORecovery:
			return socket.TcpCongestionControlStateLoss
		case tcpip.FastRecovery, tcpip.SACKRecovery:
			return socket.TcpCongestionControlStateRecovery
		case tcpip.Disorder:
			return socket.TcpCongestionControlStateDisorder
		default:
			panic(fmt.Sprintf("unknown congestion control state: %d", state))
		}
	}())
	info.SetState(func() socket.TcpState {
		switch state := tcp.EndpointState(value.State); state {
		case tcp.StateEstablished:
			return socket.TcpStateEstablished
		case tcp.StateSynSent:
			return socket.TcpStateSynSent
		case tcp.StateSynRecv:
			return socket.TcpStateSynRecv
		case tcp.StateFinWait1:
			return socket.TcpStateFinWait1
		case tcp.StateFinWait2:
			return socket.TcpStateFinWait2
		case tcp.StateTimeWait:
			return socket.TcpStateTimeWait
		case tcp.StateClose:
			return socket.TcpStateClose
		case tcp.StateCloseWait:
			return socket.TcpStateCloseWait
		case tcp.StateLastAck:
			return socket.TcpStateLastAck
		case tcp.StateListen:
			return socket.TcpStateListen
		case tcp.StateClosing:
			return socket.TcpStateClosing
		// Endpoint states internal to netstack.
		case tcp.StateInitial, tcp.StateBound, tcp.StateConnecting, tcp.StateError:
			return socket.TcpStateClose
		default:
			panic(fmt.Sprintf("unknown state: %d", state))
		}
	}())
	info.SetRtoUsec(uint32(value.RTO.Microseconds()))
	info.SetRttUsec(uint32(value.RTT.Microseconds()))
	info.SetRttVarUsec(uint32(value.RTTVar.Microseconds()))
	info.SetSndSsthresh(value.SndSsthresh)
	info.SetSndCwnd(value.SndCwnd)
	info.SetReorderSeen(value.ReorderSeen)
	return socket.StreamSocketGetTcpInfoResultWithResponse(socket.StreamSocketGetTcpInfoResponse{
		Info: info,
	}), nil
}

func (s *streamSocketImpl) SetTcpSynCount(_ fidl.Context, value uint32) (socket.StreamSocketSetTcpSynCountResult, error) {
	if err := s.ep.SetSockOptInt(tcpip.TCPSynCountOption, int(value)); err != nil {
		return socket.StreamSocketSetTcpSynCountResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpSynCountResultWithResponse(socket.StreamSocketSetTcpSynCountResponse{}), nil
}

func (s *streamSocketImpl) GetTcpSynCount(fidl.Context) (socket.StreamSocketGetTcpSynCountResult, error) {
	value, err := s.ep.GetSockOptInt(tcpip.TCPSynCountOption)
	if err != nil {
		return socket.StreamSocketGetTcpSynCountResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpSynCountResultWithResponse(socket.StreamSocketGetTcpSynCountResponse{Value: uint32(value)}), nil
}

func (s *streamSocketImpl) SetTcpWindowClamp(_ fidl.Context, value uint32) (socket.StreamSocketSetTcpWindowClampResult, error) {
	if err := s.ep.SetSockOptInt(tcpip.TCPWindowClampOption, int(value)); err != nil {
		return socket.StreamSocketSetTcpWindowClampResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpWindowClampResultWithResponse(socket.StreamSocketSetTcpWindowClampResponse{}), nil
}

func (s *streamSocketImpl) GetTcpWindowClamp(fidl.Context) (socket.StreamSocketGetTcpWindowClampResult, error) {
	value, err := s.ep.GetSockOptInt(tcpip.TCPWindowClampOption)
	if err != nil {
		return socket.StreamSocketGetTcpWindowClampResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketGetTcpWindowClampResultWithResponse(socket.StreamSocketGetTcpWindowClampResponse{Value: uint32(value)}), nil
}

func (s *streamSocketImpl) SetTcpLinger(_ fidl.Context, valueSecs socket.OptionalUint32) (socket.StreamSocketSetTcpLingerResult, error) {
	v, err := optionalUint32ToInt(valueSecs, -1)
	if err != nil {
		return socket.StreamSocketSetTcpLingerResultWithErr(tcpipErrorToCode(err)), nil
	}
	opt := tcpip.TCPLingerTimeoutOption(time.Second * time.Duration(v))
	if err := s.ep.SetSockOpt(&opt); err != nil {
		return socket.StreamSocketSetTcpLingerResultWithErr(tcpipErrorToCode(err)), nil
	}
	return socket.StreamSocketSetTcpLingerResultWithResponse(socket.StreamSocketSetTcpLingerResponse{}), nil
}

func (s *streamSocketImpl) GetTcpLinger(fidl.Context) (socket.StreamSocketGetTcpLingerResult, error) {
	var value tcpip.TCPLingerTimeoutOption
	if err := s.ep.GetSockOpt(&value); err != nil {
		return socket.StreamSocketGetTcpLingerResultWithErr(tcpipErrorToCode(err)), nil
	}
	v := socket.OptionalUint32WithUnset(socket.Empty{})
	if seconds := time.Duration(value) / time.Second; seconds != 0 {
		v = socket.OptionalUint32WithValue(uint32(seconds))
	}
	return socket.StreamSocketGetTcpLingerResultWithResponse(socket.StreamSocketGetTcpLingerResponse{
		ValueSecs: v,
	}), nil
}

func (ns *Netstack) onAddEndpoint(e *endpoint) {
	ns.stats.SocketsCreated.Increment()
	var key uint64
	// Reserve key value 0 to indicate that the endpoint was never
	// added to the endpoints map.
	for key == 0 {
		key = atomic.AddUint64(&ns.endpoints.nextKey, 1)
	}
	// Check if the key exists in the map already. The key is a uint64 value
	// and we skip adding the endpoint to the map in the unlikely wrap around
	// case for now.
	if ep, loaded := ns.endpoints.LoadOrStore(key, e.ep); loaded {
		var info stack.TransportEndpointInfo
		switch t := ep.Info().(type) {
		case *stack.TransportEndpointInfo:
			info = *t
		}
		_ = syslog.Errorf("endpoint map store error, key %d exists for endpoint %+v", key, info)
	} else {
		e.key = key
	}
}

func (ns *Netstack) onRemoveEndpoint(key uint64) bool {
	ns.stats.SocketsDestroyed.Increment()
	// Key value 0 would indicate that the endpoint was never
	// added to the endpoints map.
	if key == 0 {
		return false
	}
	_, deleted := ns.endpoints.LoadAndDelete(key)
	return deleted
}

type providerImpl struct {
	ns *Netstack
}

var _ socket.ProviderWithCtx = (*providerImpl)(nil)

func toTransProtoStream(_ socket.Domain, proto socket.StreamSocketProtocol) (posix.Errno, tcpip.TransportProtocolNumber) {
	switch proto {
	case socket.StreamSocketProtocolTcp:
		return 0, tcp.ProtocolNumber
	}
	return posix.ErrnoEprotonosupport, 0
}

func toTransProtoDatagram(domain socket.Domain, proto socket.DatagramSocketProtocol) (posix.Errno, tcpip.TransportProtocolNumber) {
	switch proto {
	case socket.DatagramSocketProtocolUdp:
		return 0, udp.ProtocolNumber
	case socket.DatagramSocketProtocolIcmpEcho:
		switch domain {
		case socket.DomainIpv4:
			return 0, icmp.ProtocolNumber4
		case socket.DomainIpv6:
			return 0, icmp.ProtocolNumber6
		}
	}
	return posix.ErrnoEprotonosupport, 0
}

func toNetProto(domain socket.Domain) (posix.Errno, tcpip.NetworkProtocolNumber) {
	switch domain {
	case socket.DomainIpv4:
		return 0, ipv4.ProtocolNumber
	case socket.DomainIpv6:
		return 0, ipv6.ProtocolNumber
	default:
		return posix.ErrnoEpfnosupport, 0
	}
}

func makeDatagramSocket(ep tcpip.Endpoint, netProto tcpip.NetworkProtocolNumber, transProto tcpip.TransportProtocolNumber, wq *waiter.Queue, ns *Netstack) (datagramSocket, error) {
	var localE, peerE zx.Handle
	if status := zx.Sys_eventpair_create(0, &localE, &peerE); status != zx.ErrOk {
		return datagramSocket{}, &zx.Error{Status: status, Text: "zx.EventPair"}
	}

	s := datagramSocket{
		endpointWithEvent: &endpointWithEvent{
			endpoint: endpoint{
				ep:         ep,
				wq:         wq,
				transProto: transProto,
				netProto:   netProto,
				ns:         ns,
				pending: signaler{
					supported:       waiter.EventIn | waiter.EventErr,
					eventsToSignals: eventsToDatagramSignals,
					readiness:       ep.Readiness,
					signalPeer:      localE.SignalPeer,
				},
			},
			local: localE,
			peer:  peerE,
		},
	}

	s.entry = waiter.NewFunctionEntry(s.pending.supported, func(waiter.EventMask) {
		if err := s.pending.update(); err != nil {
			panic(err)
		}
	})

	s.wq.EventRegister(&s.entry)

	return s, nil
}

func (sp *providerImpl) DatagramSocket(ctx fidl.Context, domain socket.Domain, proto socket.DatagramSocketProtocol) (socket.ProviderDatagramSocketResult, error) {
	code, netProto := toNetProto(domain)
	if code != 0 {
		return socket.ProviderDatagramSocketResultWithErr(code), nil
	}
	code, transProto := toTransProtoDatagram(domain, proto)
	if code != 0 {
		return socket.ProviderDatagramSocketResultWithErr(code), nil
	}

	wq := new(waiter.Queue)
	var ep tcpip.Endpoint
	{
		var err tcpip.Error
		ep, err = sp.ns.stack.NewEndpoint(transProto, netProto, wq)
		if err != nil {
			return socket.ProviderDatagramSocketResultWithErr(tcpipErrorToCode(err)), nil
		}
	}

	datagramSocket, err := makeDatagramSocket(ep, netProto, transProto, wq, sp.ns)
	if err != nil {
		return socket.ProviderDatagramSocketResult{}, err
	}

	s := datagramSocketImpl{
		networkDatagramSocket: networkDatagramSocket{
			datagramSocket: datagramSocket,
		},
	}

	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return socket.ProviderDatagramSocketResult{}, err
	}

	s.addConnection(ctx, fidlio.NodeWithCtxInterfaceRequest{Channel: localC})
	_ = syslog.DebugTf("NewDatagram", "%p", s.endpointWithEvent)
	sp.ns.onAddEndpoint(&s.endpoint)

	if err := s.endpointWithEvent.local.SignalPeer(0, zxsocket.SignalDatagramOutgoing); err != nil {
		panic(fmt.Sprintf("local.SignalPeer(0, zxsocket.SignalDatagramOutgoing) = %s", err))
	}

	return socket.ProviderDatagramSocketResultWithResponse(socket.ProviderDatagramSocketResponse{
		S: socket.DatagramSocketWithCtxInterface{Channel: peerC},
	}), nil
}

func (sp *providerImpl) StreamSocket(_ fidl.Context, domain socket.Domain, proto socket.StreamSocketProtocol) (socket.ProviderStreamSocketResult, error) {
	code, netProto := toNetProto(domain)
	if code != 0 {
		return socket.ProviderStreamSocketResultWithErr(code), nil
	}
	code, transProto := toTransProtoStream(domain, proto)
	if code != 0 {
		return socket.ProviderStreamSocketResultWithErr(code), nil
	}

	wq := new(waiter.Queue)
	var ep tcpip.Endpoint
	{
		var err tcpip.Error
		ep, err = sp.ns.stack.NewEndpoint(transProto, netProto, wq)
		if err != nil {
			return socket.ProviderStreamSocketResultWithErr(tcpipErrorToCode(err)), nil
		}
	}

	socketEp, err := newEndpointWithSocket(ep, wq, transProto, netProto, sp.ns)
	if err != nil {
		return socket.ProviderStreamSocketResult{}, err
	}
	streamSocketInterface, err := newStreamSocket(socketEp)
	if err != nil {
		return socket.ProviderStreamSocketResult{}, err
	}
	return socket.ProviderStreamSocketResultWithResponse(socket.ProviderStreamSocketResponse{
		S: socket.StreamSocketWithCtxInterface{Channel: streamSocketInterface.Channel},
	}), nil
}

func (sp *providerImpl) InterfaceIndexToName(_ fidl.Context, index uint64) (socket.ProviderInterfaceIndexToNameResult, error) {
	if info, ok := sp.ns.stack.NICInfo()[tcpip.NICID(index)]; ok {
		return socket.ProviderInterfaceIndexToNameResultWithResponse(socket.ProviderInterfaceIndexToNameResponse{
			Name: info.Name,
		}), nil
	}
	return socket.ProviderInterfaceIndexToNameResultWithErr(int32(zx.ErrNotFound)), nil
}

func (sp *providerImpl) InterfaceNameToIndex(_ fidl.Context, name string) (socket.ProviderInterfaceNameToIndexResult, error) {
	for id, info := range sp.ns.stack.NICInfo() {
		if info.Name == name {
			return socket.ProviderInterfaceNameToIndexResultWithResponse(socket.ProviderInterfaceNameToIndexResponse{
				Index: uint64(id),
			}), nil
		}
	}
	return socket.ProviderInterfaceNameToIndexResultWithErr(int32(zx.ErrNotFound)), nil
}

type rawProviderImpl struct {
	ns *Netstack
}

var _ rawsocket.ProviderWithCtx = (*rawProviderImpl)(nil)

func (sp *rawProviderImpl) Socket(ctx fidl.Context, domain socket.Domain, proto rawsocket.ProtocolAssociation) (rawsocket.ProviderSocketResult, error) {
	code, netProto := toNetProto(domain)
	if code != 0 {
		return rawsocket.ProviderSocketResultWithErr(code), nil
	}

	// If the endpoint is unassociated, then the protocol number doesn't matter.
	//
	// 255 is a reserved protocol number as per
	// https://www.iana.org/assignments/protocol-numbers/protocol-numbers.xhtml.
	transProto := tcpip.TransportProtocolNumber(255)
	associated := true
	switch tag := proto.Which(); tag {
	case rawsocket.ProtocolAssociationUnassociated:
		associated = false
	case rawsocket.ProtocolAssociationAssociated:
		transProto = tcpip.TransportProtocolNumber(proto.Associated)
	default:
		panic(fmt.Sprintf("unhandled association = %d; %#v", tag, proto))
	}

	wq := new(waiter.Queue)
	var ep tcpip.Endpoint
	{
		var err tcpip.Error
		ep, err = sp.ns.stack.NewRawEndpoint(transProto, netProto, wq, associated)
		if err != nil {
			return rawsocket.ProviderSocketResultWithErr(tcpipErrorToCode(err)), nil
		}
	}

	datagramSocket, err := makeDatagramSocket(ep, netProto, transProto, wq, sp.ns)
	if err != nil {
		return rawsocket.ProviderSocketResult{}, err
	}

	s := rawSocketImpl{
		networkDatagramSocket: networkDatagramSocket{
			datagramSocket: datagramSocket,
		},
		proto: proto,
	}

	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return rawsocket.ProviderSocketResult{}, err
	}

	s.addConnection(ctx, fidlio.NodeWithCtxInterfaceRequest{Channel: localC})
	_ = syslog.DebugTf("NewRawSocket", "%p", s.endpointWithEvent)
	sp.ns.onAddEndpoint(&s.endpoint)

	if err := s.endpointWithEvent.local.SignalPeer(0, zxsocket.SignalDatagramOutgoing); err != nil {
		panic(fmt.Sprintf("local.SignalPeer(0, zxsocket.SignalDatagramOutgoing) = %s", err))
	}

	return rawsocket.ProviderSocketResultWithResponse(rawsocket.ProviderSocketResponse{
		S: rawsocket.SocketWithCtxInterface{Channel: peerC},
	}), nil
}

type rawSocketImpl struct {
	networkDatagramSocket

	proto rawsocket.ProtocolAssociation
}

var _ rawsocket.SocketWithCtx = (*rawSocketImpl)(nil)

func (s *rawSocketImpl) Describe(fidl.Context) (fidlio.NodeInfo, error) {
	event, err := s.describe()
	if err != nil {
		return fidlio.NodeInfo{}, err
	}
	return fidlio.NodeInfoWithRawSocket(fidlio.RawSocket{Event: event}), nil
}

func (s *rawSocketImpl) Describe2(_ fidl.Context, query fidlio.ConnectionInfoQuery) (fidlio.ConnectionInfo, error) {
	var connectionInfo fidlio.ConnectionInfo
	if query&fidlio.ConnectionInfoQueryRepresentation != 0 {
		event, err := s.describe()
		if err != nil {
			return connectionInfo, err
		}
		var rawSocket fidlio.RawSocketInfo
		rawSocket.SetEvent(event)
		connectionInfo.SetRepresentation(fidlio.RepresentationWithRawSocket(rawSocket))
	}
	// TODO(https://fxbug.dev/77623): Populate the rights requested by the client at connection.
	rights := fidlio.RStarDir
	if query&fidlio.ConnectionInfoQueryRights != 0 {
		connectionInfo.SetRights(rights)
	}
	if query&fidlio.ConnectionInfoQueryAvailableOperations != 0 {
		abilities := fidlio.OperationsReadBytes | fidlio.OperationsWriteBytes | fidlio.OperationsGetAttributes
		connectionInfo.SetAvailableOperations(abilities & rights)
	}
	return connectionInfo, nil
}

func (s *rawSocketImpl) addConnection(_ fidl.Context, object fidlio.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		// NB: this protocol is not discoverable, so the bindings do not include its name.
		s.datagramSocket.addConnection("fuchsia.posix.socket.raw.Socket", object, &rawsocket.SocketWithCtxStub{Impl: s})
	}
}

func (s *rawSocketImpl) Clone(ctx fidl.Context, flags uint32, object fidlio.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	_ = syslog.DebugTf("Clone", "%p: flags=%b", s.endpointWithEvent, flags)

	return nil
}

func (s *rawSocketImpl) Reopen(ctx fidl.Context, options fidlio.ConnectionOptions, channel zx.Channel) error {
	// TODO(https://fxbug.dev/77623): Implement.
	_ = channel.Close()

	_ = syslog.DebugTf("Clone", "%p: options=%#v", s.endpointWithEvent, options)

	return nil
}

func (s *rawSocketImpl) RecvMsg(_ fidl.Context, wantAddr bool, dataLen uint32, wantControl bool, flags socket.RecvMsgFlags) (rawsocket.SocketRecvMsgResult, error) {
	addr, data, truncated, cmsg, err := s.recvMsg(wantAddr, dataLen, flags&socket.RecvMsgFlagsPeek != 0)
	if err != nil {
		return rawsocket.SocketRecvMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	var pAddr *fidlnet.SocketAddress
	if wantAddr {
		pAddr = &addr
	}

	var controlData socket.NetworkSocketRecvControlData
	if wantControl {
		controlData = s.networkSocketControlMessagesToFIDL(cmsg)
	}

	return rawsocket.SocketRecvMsgResultWithResponse(rawsocket.SocketRecvMsgResponse{
		Addr:      pAddr,
		Data:      data,
		Control:   controlData,
		Truncated: truncated,
	}), nil
}

func (s *rawSocketImpl) SendMsg(_ fidl.Context, addr *fidlnet.SocketAddress, data []uint8, control socket.NetworkSocketSendControlData, _ socket.SendMsgFlags) (rawsocket.SocketSendMsgResult, error) {
	// TODO(https://fxbug.dev/21106): do something with control.
	_ = control

	n, err := s.sendMsg(addr, data)
	if err != nil {
		return rawsocket.SocketSendMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	if want := int64(len(data)); n != want {
		panic(fmt.Sprintf("got sendMsg(..) = %d, want = %d", n, want))
	}
	return rawsocket.SocketSendMsgResultWithResponse(rawsocket.SocketSendMsgResponse{}), nil
}

func (s *rawSocketImpl) GetInfo(fidl.Context) (rawsocket.SocketGetInfoResult, error) {
	domain, err := s.domain()
	if err != nil {
		return rawsocket.SocketGetInfoResultWithErr(tcpipErrorToCode(err)), nil
	}
	return rawsocket.SocketGetInfoResultWithResponse(rawsocket.SocketGetInfoResponse{
		Domain: domain,
		Proto:  s.proto,
	}), nil
}

func (s *rawSocketImpl) SetIpHeaderIncluded(_ fidl.Context, value bool) (rawsocket.SocketSetIpHeaderIncludedResult, error) {
	s.ep.SocketOptions().SetHeaderIncluded(value)
	return rawsocket.SocketSetIpHeaderIncludedResultWithResponse(rawsocket.SocketSetIpHeaderIncludedResponse{}), nil
}

func (s *rawSocketImpl) GetIpHeaderIncluded(fidl.Context) (rawsocket.SocketGetIpHeaderIncludedResult, error) {
	value := s.ep.SocketOptions().GetHeaderIncluded()
	return rawsocket.SocketGetIpHeaderIncludedResultWithResponse(rawsocket.SocketGetIpHeaderIncludedResponse{Value: value}), nil
}

func (s *rawSocketImpl) SetIcmpv6Filter(_ fidl.Context, value rawsocket.Icmpv6Filter) (rawsocket.SocketSetIcmpv6FilterResult, error) {
	if err := s.ep.SetSockOpt(&tcpip.ICMPv6Filter{DenyType: value.BlockedTypes}); err != nil {
		return rawsocket.SocketSetIcmpv6FilterResultWithErr(tcpipErrorToCode(err)), nil
	}
	return rawsocket.SocketSetIcmpv6FilterResultWithResponse(rawsocket.SocketSetIcmpv6FilterResponse{}), nil
}

func (s *rawSocketImpl) GetIcmpv6Filter(fidl.Context) (rawsocket.SocketGetIcmpv6FilterResult, error) {
	var filter tcpip.ICMPv6Filter
	if err := s.ep.GetSockOpt(&filter); err != nil {
		return rawsocket.SocketGetIcmpv6FilterResultWithErr(tcpipErrorToCode(err)), nil
	}
	return rawsocket.SocketGetIcmpv6FilterResultWithResponse(rawsocket.SocketGetIcmpv6FilterResponse{
		Filter: rawsocket.Icmpv6Filter{
			BlockedTypes: filter.DenyType,
		},
	}), nil
}

func (s *rawSocketImpl) SetIpv6Checksum(_ fidl.Context, value rawsocket.Ipv6ChecksumConfiguration) (rawsocket.SocketSetIpv6ChecksumResult, error) {
	var v int
	switch value.Which() {
	case rawsocket.Ipv6ChecksumConfigurationDisabled:
		v = -1
	case rawsocket.Ipv6ChecksumConfigurationOffset:
		v = int(value.Offset)
	default:
		panic(fmt.Sprintf("unhandled variant = %#v", value))
	}

	if err := s.ep.SetSockOptInt(tcpip.IPv6Checksum, v); err != nil {
		return rawsocket.SocketSetIpv6ChecksumResultWithErr(tcpipErrorToCode(err)), nil
	}
	return rawsocket.SocketSetIpv6ChecksumResultWithResponse(rawsocket.SocketSetIpv6ChecksumResponse{}), nil
}

func (s *rawSocketImpl) GetIpv6Checksum(fidl.Context) (rawsocket.SocketGetIpv6ChecksumResult, error) {
	v, err := s.ep.GetSockOptInt(tcpip.IPv6Checksum)
	if err != nil {
		return rawsocket.SocketGetIpv6ChecksumResultWithErr(tcpipErrorToCode(err)), nil
	}
	var config rawsocket.Ipv6ChecksumConfiguration
	if v < 0 {
		config.SetDisabled(rawsocket.Empty{})
	} else {
		config.SetOffset(int32(v))
	}
	return rawsocket.SocketGetIpv6ChecksumResultWithResponse(rawsocket.SocketGetIpv6ChecksumResponse{
		Config: config,
	}), nil
}

// Adapted from helper function `nicStateFlagsToLinux` in gvisor's
// sentry/socket/netstack package.
func nicInfoFlagsToFIDL(info stack.NICInfo) socket.InterfaceFlags {
	ifs := info.Context.(*ifState)
	var bits socket.InterfaceFlags
	flags := info.Flags
	if flags.Loopback {
		bits |= socket.InterfaceFlagsLoopback
	}
	if flags.Running {
		bits |= socket.InterfaceFlagsRunning
	}
	if flags.Promiscuous {
		bits |= socket.InterfaceFlagsPromisc
	}
	// Check `IsUpLocked` because netstack interfaces are always defined to be
	// `Up` in gVisor.
	ifs.mu.Lock()
	if ifs.IsUpLocked() {
		bits |= socket.InterfaceFlagsUp
	}
	ifs.mu.Unlock()
	// Approximate that all interfaces support multicasting.
	bits |= socket.InterfaceFlagsMulticast
	return bits
}

func (sp *providerImpl) InterfaceNameToFlags(_ fidl.Context, name string) (socket.ProviderInterfaceNameToFlagsResult, error) {
	for _, info := range sp.ns.stack.NICInfo() {
		if info.Name == name {
			return socket.ProviderInterfaceNameToFlagsResultWithResponse(socket.ProviderInterfaceNameToFlagsResponse{
				Flags: nicInfoFlagsToFIDL(info),
			}), nil
		}
	}
	return socket.ProviderInterfaceNameToFlagsResultWithErr(int32(zx.ErrNotFound)), nil
}

func (sp *providerImpl) GetInterfaceAddresses(fidl.Context) ([]socket.InterfaceAddresses, error) {
	nicInfos := sp.ns.stack.NICInfo()

	resultInfos := make([]socket.InterfaceAddresses, 0, len(nicInfos))
	for id, info := range nicInfos {
		// Ensure deterministic API response.
		sort.Slice(info.ProtocolAddresses, func(i, j int) bool {
			x, y := info.ProtocolAddresses[i], info.ProtocolAddresses[j]
			if x.Protocol != y.Protocol {
				return x.Protocol < y.Protocol
			}
			ax, ay := x.AddressWithPrefix, y.AddressWithPrefix
			if ax.Address != ay.Address {
				return ax.Address < ay.Address
			}
			return ax.PrefixLen < ay.PrefixLen
		})

		addrs := make([]fidlnet.Subnet, 0, len(info.ProtocolAddresses))
		for _, a := range info.ProtocolAddresses {
			if a.Protocol != ipv4.ProtocolNumber && a.Protocol != ipv6.ProtocolNumber {
				continue
			}
			addrs = append(addrs, fidlnet.Subnet{
				Addr:      fidlconv.ToNetIpAddress(a.AddressWithPrefix.Address),
				PrefixLen: uint8(a.AddressWithPrefix.PrefixLen),
			})
		}

		var resultInfo socket.InterfaceAddresses
		resultInfo.SetId(uint64(id))
		resultInfo.SetName(info.Name)
		resultInfo.SetAddresses(addrs)

		// gVisor assumes interfaces are always up, which is not the case on Fuchsia,
		// so overwrite it with Fuchsia's interface state.
		bits := nicInfoFlagsToFIDL(info)
		// TODO(https://fxbug.dev/64758): don't `SetFlags` once all clients are
		// transitioned to use `interface_flags`.
		resultInfo.SetFlags(uint32(bits))
		resultInfo.SetInterfaceFlags(bits)

		resultInfos = append(resultInfos, resultInfo)
	}

	// Ensure deterministic API response.
	sort.Slice(resultInfos, func(i, j int) bool {
		return resultInfos[i].Id < resultInfos[j].Id
	})
	return resultInfos, nil
}

func tcpipErrorToCode(err tcpip.Error) posix.Errno {
	if _, ok := err.(*tcpip.ErrConnectStarted); !ok {
		if pc, file, line, ok := runtime.Caller(1); ok {
			if i := strings.LastIndexByte(file, '/'); i != -1 {
				file = file[i+1:]
			}
			_ = syslog.Debugf("%s: %s:%d: %s", runtime.FuncForPC(pc).Name(), file, line, err)
		} else {
			_ = syslog.Debugf("%s", err)
		}
	}
	switch err.(type) {
	case *tcpip.ErrUnknownProtocol:
		return posix.ErrnoEinval
	case *tcpip.ErrUnknownNICID:
		return posix.ErrnoEinval
	case *tcpip.ErrUnknownDevice:
		return posix.ErrnoEnodev
	case *tcpip.ErrUnknownProtocolOption:
		return posix.ErrnoEnoprotoopt
	case *tcpip.ErrDuplicateNICID:
		return posix.ErrnoEexist
	case *tcpip.ErrDuplicateAddress:
		return posix.ErrnoEexist
	case *tcpip.ErrNoRoute:
		return posix.ErrnoEhostunreach
	case *tcpip.ErrAlreadyBound:
		return posix.ErrnoEinval
	case *tcpip.ErrInvalidEndpointState:
		return posix.ErrnoEinval
	case *tcpip.ErrAlreadyConnecting:
		return posix.ErrnoEalready
	case *tcpip.ErrAlreadyConnected:
		return posix.ErrnoEisconn
	case *tcpip.ErrNoPortAvailable:
		return posix.ErrnoEagain
	case *tcpip.ErrPortInUse:
		return posix.ErrnoEaddrinuse
	case *tcpip.ErrBadLocalAddress:
		return posix.ErrnoEaddrnotavail
	case *tcpip.ErrClosedForSend:
		return posix.ErrnoEpipe
	case *tcpip.ErrClosedForReceive:
		return posix.ErrnoEagain
	case *tcpip.ErrWouldBlock:
		return posix.Ewouldblock
	case *tcpip.ErrConnectionRefused:
		return posix.ErrnoEconnrefused
	case *tcpip.ErrTimeout:
		return posix.ErrnoEtimedout
	case *tcpip.ErrAborted:
		return posix.ErrnoEpipe
	case *tcpip.ErrConnectStarted:
		return posix.ErrnoEinprogress
	case *tcpip.ErrDestinationRequired:
		return posix.ErrnoEdestaddrreq
	case *tcpip.ErrNotSupported:
		return posix.ErrnoEopnotsupp
	case *tcpip.ErrQueueSizeNotSupported:
		return posix.ErrnoEnotty
	case *tcpip.ErrNotConnected:
		return posix.ErrnoEnotconn
	case *tcpip.ErrConnectionReset:
		return posix.ErrnoEconnreset
	case *tcpip.ErrConnectionAborted:
		return posix.ErrnoEconnaborted
	case *tcpip.ErrNoSuchFile:
		return posix.ErrnoEnoent
	case *tcpip.ErrInvalidOptionValue:
		return posix.ErrnoEinval
	case *tcpip.ErrBadAddress:
		return posix.ErrnoEfault
	case *tcpip.ErrNetworkUnreachable:
		return posix.ErrnoEnetunreach
	case *tcpip.ErrMessageTooLong:
		return posix.ErrnoEmsgsize
	case *tcpip.ErrNoBufferSpace:
		return posix.ErrnoEnobufs
	case *tcpip.ErrBroadcastDisabled, *tcpip.ErrNotPermitted:
		return posix.ErrnoEacces
	case *tcpip.ErrAddressFamilyNotSupported:
		return posix.ErrnoEafnosupport
	case *tcpip.ErrMalformedHeader:
		return posix.ErrnoEinval
	default:
		panic(fmt.Sprintf("unknown error %v", err))
	}
}

var _ packetsocket.SocketWithCtx = (*packetSocketImpl)(nil)

type packetSocketImpl struct {
	datagramSocket

	kind packetsocket.Kind
}

func (s *packetSocketImpl) Describe(fidl.Context) (fidlio.NodeInfo, error) {
	event, err := s.describe()
	if err != nil {
		return fidlio.NodeInfo{}, err
	}
	return fidlio.NodeInfoWithPacketSocket(fidlio.PacketSocket{Event: event}), nil
}

func (s *packetSocketImpl) Describe2(_ fidl.Context, query fidlio.ConnectionInfoQuery) (fidlio.ConnectionInfo, error) {
	var connectionInfo fidlio.ConnectionInfo
	if query&fidlio.ConnectionInfoQueryRepresentation != 0 {
		event, err := s.describe()
		if err != nil {
			return connectionInfo, err
		}
		var packetSocket fidlio.PacketSocketInfo
		packetSocket.SetEvent(event)
		connectionInfo.SetRepresentation(fidlio.RepresentationWithPacketSocket(packetSocket))
	}
	// TODO(https://fxbug.dev/77623): Populate the rights requested by the client at connection.
	rights := fidlio.RStarDir
	if query&fidlio.ConnectionInfoQueryRights != 0 {
		connectionInfo.SetRights(rights)
	}
	if query&fidlio.ConnectionInfoQueryAvailableOperations != 0 {
		abilities := fidlio.OperationsReadBytes | fidlio.OperationsWriteBytes | fidlio.OperationsGetAttributes
		connectionInfo.SetAvailableOperations(abilities & rights)
	}
	return connectionInfo, nil
}

func (s *packetSocketImpl) Clone(ctx fidl.Context, flags uint32, object fidlio.NodeWithCtxInterfaceRequest) error {
	s.addConnection(ctx, object)

	_ = syslog.DebugTf("Clone", "%p: flags=%b", s.endpointWithEvent, flags)

	return nil
}

func (s *packetSocketImpl) Reopen(ctx fidl.Context, options fidlio.ConnectionOptions, channel zx.Channel) error {
	// TODO(https://fxbug.dev/77623): Implement.
	_ = channel.Close()

	_ = syslog.DebugTf("Clone", "%p: options=%#v", s.endpointWithEvent, options)

	return nil
}

func (s *packetSocketImpl) addConnection(_ fidl.Context, object fidlio.NodeWithCtxInterfaceRequest) {
	{
		sCopy := *s
		s := &sCopy

		// NB: this protocol is not discoverable, so the bindings do not include its name.
		s.datagramSocket.addConnection("fuchsia.posix.socket.packet.Socket", object, &packetsocket.SocketWithCtxStub{Impl: s})
	}
}

func (s *packetSocketImpl) Bind(_ fidl.Context, proto *packetsocket.ProtocolAssociation, interface_id packetsocket.BoundInterfaceId) (packetsocket.SocketBindResult, error) {
	var addr tcpip.FullAddress

	if proto != nil {
		switch tag := proto.Which(); tag {
		case packetsocket.ProtocolAssociationAll:
			addr.Port = uint16(header.EthernetProtocolAll)
		case packetsocket.ProtocolAssociationSpecified:
			addr.Port = proto.Specified
		default:
			panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d; %#[2]v", tag, proto))
		}
	}

	switch w := interface_id.Which(); w {
	case packetsocket.BoundInterfaceIdAll:
	case packetsocket.BoundInterfaceIdSpecified:
		addr.NIC = tcpip.NICID(interface_id.Specified)
	default:
		panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d; %#[2]v", w, interface_id))
	}

	if err := s.ep.Bind(addr); err != nil {
		if _, ok := err.(*tcpip.ErrUnknownNICID); ok {
			return packetsocket.SocketBindResultWithErr(posix.ErrnoEnodev), nil
		}
		return packetsocket.SocketBindResultWithErr(tcpipErrorToCode(err)), nil
	}

	return packetsocket.SocketBindResultWithResponse(packetsocket.SocketBindResponse{}), nil
}

func (s *packetSocketImpl) GetInfo(fidl.Context) (packetsocket.SocketGetInfoResult, error) {
	addr, err := s.ep.GetLocalAddress()
	if err != nil {
		return packetsocket.SocketGetInfoResultWithErr(tcpipErrorToCode(err)), nil
	}

	var protoStorage packetsocket.ProtocolAssociation
	var proto *packetsocket.ProtocolAssociation
	switch addr.Port {
	case 0:
		// The protocol is only specified if the socket is bound to a protocol.
	case uint16(header.EthernetProtocolAll):
		protoStorage = packetsocket.ProtocolAssociationWithAll(packetsocket.Empty{})
		proto = &protoStorage
	default:
		protoStorage = packetsocket.ProtocolAssociationWithSpecified(addr.Port)
		proto = &protoStorage
	}

	var boundInterface packetsocket.BoundInterface
	switch addr.NIC {
	case 0:
		boundInterface = packetsocket.BoundInterfaceWithAll(packetsocket.Empty{})
	default:
		nicsInfo := s.ns.stack.NICInfo()
		nicInfo, ok := nicsInfo[addr.NIC]
		if !ok {
			return packetsocket.SocketGetInfoResultWithErr(posix.ErrnoEnodev), nil
		}

		var hwType packetsocket.HardwareType
		switch nicInfo.ARPHardwareType {
		case header.ARPHardwareNone:
			hwType = packetsocket.HardwareTypeNetworkOnly
		case header.ARPHardwareEther:
			hwType = packetsocket.HardwareTypeEthernet
		case header.ARPHardwareLoopback:
			hwType = packetsocket.HardwareTypeLoopback
		default:
			panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d", nicInfo.ARPHardwareType))
		}

		boundInterface = packetsocket.BoundInterfaceWithSpecified(packetsocket.InterfaceProperties{
			Id:   uint64(addr.NIC),
			Addr: tcpipLinkAddressToFidlHWAddr(nicInfo.LinkAddress),
			Type: hwType,
		})
	}

	return packetsocket.SocketGetInfoResultWithResponse(packetsocket.SocketGetInfoResponse{
		Kind:           s.kind,
		Protocol:       proto,
		BoundInterface: boundInterface,
	}), nil
}

func tcpipPacketTypeToFidl(v tcpip.PacketType) packetsocket.PacketType {
	switch v {
	case tcpip.PacketHost:
		return packetsocket.PacketTypeHost
	case tcpip.PacketOtherHost:
		return packetsocket.PacketTypeOtherHost
	case tcpip.PacketOutgoing:
		return packetsocket.PacketTypeOutgoing
	case tcpip.PacketBroadcast:
		return packetsocket.PacketTypeBroadcast
	case tcpip.PacketMulticast:
		return packetsocket.PacketTypeMulticast
	default:
		panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d", v))
	}
}

func tcpipLinkAddressToFidlHWAddr(v tcpip.LinkAddress) packetsocket.HardwareAddress {
	switch l := len(v); l {
	case 0:
		return packetsocket.HardwareAddressWithNone(packetsocket.Empty{})
	case header.EthernetAddressSize:
		return packetsocket.HardwareAddressWithEui48(fidlconv.ToNetMacAddress(v))
	default:
		panic(fmt.Sprintf("unhandled link address = %s with length = %d", v, l))
	}
}

func (s *packetSocketImpl) controlMessagesToFIDL(cmsg tcpip.ControlMessages) packetsocket.RecvControlData {
	var controlData packetsocket.RecvControlData
	if socketControlData := s.socketControlMessagesToFIDL(cmsg); socketControlData != (socket.SocketRecvControlData{}) {
		controlData.SetSocket(socketControlData)
	}
	return controlData
}

func (s *packetSocketImpl) RecvMsg(_ fidl.Context, wantPacketInfo bool, dataLen uint32, wantControl bool, flags socket.RecvMsgFlags) (packetsocket.SocketRecvMsgResult, error) {
	// TODO(https://fxbug.dev/21106): do something with control messages.
	_ = wantControl

	bytes, res, err := s.datagramSocket.recvMsg(tcpip.ReadOptions{
		Peek:               flags&socket.RecvMsgFlagsPeek != 0,
		NeedRemoteAddr:     wantPacketInfo,
		NeedLinkPacketInfo: wantPacketInfo,
	}, dataLen)
	if err != nil {
		return packetsocket.SocketRecvMsgResultWithErr(tcpipErrorToCode(err)), nil
	}

	resp := packetsocket.SocketRecvMsgResponse{
		Data:      bytes,
		Truncated: uint32(res.Total - res.Count),
	}
	if wantPacketInfo {
		resp.PacketInfo = &packetsocket.RecvPacketInfo{
			PacketInfo: packetsocket.PacketInfo{
				Protocol:    uint16(res.LinkPacketInfo.Protocol),
				InterfaceId: uint64(res.RemoteAddr.NIC),
				Addr:        tcpipLinkAddressToFidlHWAddr(tcpip.LinkAddress(res.RemoteAddr.Addr)),
			},
			PacketType:    tcpipPacketTypeToFidl(res.LinkPacketInfo.PktType),
			InterfaceType: packetsocket.HardwareTypeEthernet,
		}
	}

	if wantControl {
		resp.Control = s.controlMessagesToFIDL(res.ControlMessages)
	}

	return packetsocket.SocketRecvMsgResultWithResponse(resp), nil
}

func (s *packetSocketImpl) SendMsg(_ fidl.Context, addr *packetsocket.PacketInfo, data []uint8, control packetsocket.SendControlData, _ socket.SendMsgFlags) (packetsocket.SocketSendMsgResult, error) {
	// TODO(https://fxbug.dev/21106): do something with control.
	_ = control

	var fullAddr tcpip.FullAddress
	var to *tcpip.FullAddress
	if addr != nil {
		fullAddr.NIC = tcpip.NICID(addr.InterfaceId)
		fullAddr.Port = addr.Protocol
		switch w := addr.Addr.Which(); w {
		case packetsocket.HardwareAddressNone:
		case packetsocket.HardwareAddressEui48:
			fullAddr.Addr = tcpip.Address(fidlconv.ToTCPIPLinkAddress(addr.Addr.Eui48))
		default:
			panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d; %#[2]v", w, addr.Addr))
		}
		to = &fullAddr
	}

	n, err := s.datagramSocket.sendMsg(to, data)
	if err != nil {
		return packetsocket.SocketSendMsgResultWithErr(tcpipErrorToCode(err)), nil
	}
	if want := int64(len(data)); n != want {
		panic(fmt.Sprintf("got sendMsg(..) = %d, want = %d", n, want))
	}
	return packetsocket.SocketSendMsgResultWithResponse(packetsocket.SocketSendMsgResponse{}), nil
}

var _ packetsocket.ProviderWithCtx = (*packetProviderImpl)(nil)

type packetProviderImpl struct {
	ns *Netstack
}

func (sp *packetProviderImpl) Socket(ctx fidl.Context, kind packetsocket.Kind) (packetsocket.ProviderSocketResult, error) {
	cooked := false
	switch kind {
	case packetsocket.KindNetwork:
		cooked = true
	case packetsocket.KindLink:
	default:
		panic(fmt.Sprintf("unhandled %[1]T variant = %[1]d", kind))
	}

	wq := new(waiter.Queue)
	var ep tcpip.Endpoint
	{
		var err tcpip.Error
		ep, err = sp.ns.stack.NewPacketEndpoint(cooked, 0 /* netProto */, wq)
		if err != nil {
			return packetsocket.ProviderSocketResultWithErr(tcpipErrorToCode(err)), nil
		}
	}

	datagramSocket, err := makeDatagramSocket(ep, 0 /* netProto */, 0 /* transProto */, wq, sp.ns)
	if err != nil {
		return packetsocket.ProviderSocketResult{}, err
	}

	s := packetSocketImpl{
		datagramSocket: datagramSocket,
		kind:           kind,
	}

	localC, peerC, err := zx.NewChannel(0)
	if err != nil {
		return packetsocket.ProviderSocketResult{}, err
	}

	s.addConnection(ctx, fidlio.NodeWithCtxInterfaceRequest{Channel: localC})
	_ = syslog.DebugTf("NewPacketSocket", "%p", s.endpointWithEvent)
	sp.ns.onAddEndpoint(&s.endpoint)

	if err := s.endpointWithEvent.local.SignalPeer(0, zxsocket.SignalDatagramOutgoing); err != nil {
		panic(fmt.Sprintf("local.SignalPeer(0, zxsocket.SignalDatagramOutgoing) = %s", err))
	}

	return packetsocket.ProviderSocketResultWithResponse(packetsocket.ProviderSocketResponse{
		Socket: packetsocket.SocketWithCtxInterface{Channel: peerC},
	}), nil
}
