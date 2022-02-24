// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/guest_manager/host_vsock_endpoint.h"

#include <lib/async/cpp/time.h>
#include <lib/async/default.h>
#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>

HostVsockEndpoint::HostVsockEndpoint(async_dispatcher_t* dispatcher,
                                     AcceptorProvider acceptor_provider)
    : dispatcher_(dispatcher), acceptor_provider_(std::move(acceptor_provider)) {}

void HostVsockEndpoint::AddBinding(
    fidl::InterfaceRequest<fuchsia::virtualization::HostVsockEndpoint> request) {
  bindings_.AddBinding(this, std::move(request));
}

void HostVsockEndpoint::Connect(
    uint32_t src_cid, uint32_t src_port, uint32_t cid, uint32_t port,
    fuchsia::virtualization::HostVsockConnector::ConnectCallback callback) {
  if (cid == fuchsia::virtualization::HOST_CID) {
    // Guest to host connection.
    auto it = listeners_.find(port);
    if (it == listeners_.end()) {
      callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
      return;
    }
    it->second->Accept(src_cid, src_port, port, std::move(callback));
  } else {
    // Guest to guest connection.
    fuchsia::virtualization::GuestVsockAcceptor* acceptor = acceptor_provider_(cid);
    if (acceptor == nullptr) {
      callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
      return;
    }
    // Use a socket for direct guest to guest communication.
    zx::socket h1, h2;
    zx_status_t status = zx::socket::create(ZX_SOCKET_STREAM, &h1, &h2);
    if (status != ZX_OK) {
      callback(ZX_ERR_CONNECTION_REFUSED, zx::handle());
      return;
    }
    acceptor->Accept(src_cid, src_port, port, std::move(h1),
                     [callback = std::move(callback), h2 = std::move(h2)](
                         zx_status_t status) mutable { callback(status, std::move(h2)); });
  }
}

void HostVsockEndpoint::Listen(
    uint32_t port, fidl::InterfaceHandle<fuchsia::virtualization::HostVsockAcceptor> acceptor,
    ListenCallback callback) {
  if (port_bitmap_.GetOne(port)) {
    callback(ZX_ERR_ALREADY_BOUND);
    return;
  }
  bool inserted;
  auto acceptor_ptr = acceptor.Bind();
  acceptor_ptr.set_error_handler([this, port](zx_status_t status) {
    port_bitmap_.ClearOne(port);
    listeners_.erase(port);
  });
  std::tie(std::ignore, inserted) = listeners_.emplace(port, std::move(acceptor_ptr));
  if (!inserted) {
    callback(ZX_ERR_ALREADY_BOUND);
    return;
  }
  port_bitmap_.SetOne(port);
  callback(ZX_OK);
}

void HostVsockEndpoint::Connect(
    uint32_t cid, uint32_t port, zx::handle handle,
    fuchsia::virtualization::HostVsockEndpoint::ConnectCallback callback) {
  if (cid == fuchsia::virtualization::HOST_CID) {
    FX_LOGS(ERROR) << "Attempt to connect to host service from host";
    callback(ZX_ERR_CONNECTION_REFUSED);
    return;
  }
  fuchsia::virtualization::GuestVsockAcceptor* acceptor = acceptor_provider_(cid);
  if (acceptor == nullptr) {
    callback(ZX_ERR_CONNECTION_REFUSED);
    return;
  }
  uint32_t src_port;
  zx_status_t status = AllocEphemeralPort(&src_port);
  if (status != ZX_OK) {
    callback(status);
    return;
  }
  // Get access to the guests.
  acceptor->Accept(fuchsia::virtualization::HOST_CID, src_port, port, std::move(handle),
                   [this, src_port, callback = std::move(callback)](zx_status_t status) mutable {
                     ConnectCallback(status, src_port, std::move(callback));
                   });
}

void HostVsockEndpoint::ConnectCallback(
    zx_status_t status, uint32_t src_port,
    fuchsia::virtualization::HostVsockEndpoint::ConnectCallback callback) {
  if (status != ZX_OK) {
    FreeEphemeralPort(src_port);
  }
  callback(status);
}

void HostVsockEndpoint::OnShutdown(uint32_t port) {
  // If there are no listeners for this port then it was ephemeral and should
  // free it.
  if (listeners_.find(port) == listeners_.end()) {
    FreeEphemeralPort(port);
  }
}

zx_status_t HostVsockEndpoint::AllocEphemeralPort(uint32_t* port) {
  // Remove ephemeral ports that have been in quarantine long enough.
  zx::time now = async::Now(dispatcher_);
  while (!quarantined_ports_.empty() && now >= quarantined_ports_.front().available_time) {
    zx_status_t status = port_bitmap_.ClearOne(quarantined_ports_.front().port);
    FX_DCHECK(status == ZX_OK);
    quarantined_ports_.pop_front();
  }

  // Find an ephemeral port.
  uint32_t value;
  zx_status_t status = port_bitmap_.Find(false, kFirstEphemeralPort, kLastEphemeralPort, 1, &value);
  if (status != ZX_OK) {
    return ZX_ERR_NO_RESOURCES;
  }
  *port = value;
  return port_bitmap_.SetOne(value);
}

void HostVsockEndpoint::FreeEphemeralPort(uint32_t port) {
  FX_DCHECK(port_bitmap_.GetOne(port)) << "Attempted to free port that was unallocated: " << port;

  // Add the port to the quarantine list.
  quarantined_ports_.push_back(QuarantinedPort{
      .port = port, .available_time = async::Now(dispatcher_) + kPortQuarantineTime});
}
