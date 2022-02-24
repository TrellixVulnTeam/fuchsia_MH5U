// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/ConnectivityManager.h>
#include <Weave/DeviceLayer/internal/GenericConnectivityManagerImpl_Thread.ipp>
// clang-format on

#include <lib/syslog/cpp/macros.h>

namespace nl {
namespace Weave {
namespace DeviceLayer {

ConnectivityManagerImpl ConnectivityManagerImpl::sInstance;

void ConnectivityManagerImpl::SetDelegate(std::unique_ptr<Delegate> delegate) {
  FX_CHECK(!(delegate && delegate_)) << "Attempt to set an already set delegate. Must explicitly "
                                        "clear the existing delegate first.";
  delegate_ = std::move(delegate);
  if (delegate_) {
    delegate_->SetConnectivityManagerImpl(this);
  }
}

ConnectivityManagerImpl::Delegate* ConnectivityManagerImpl::GetDelegate() {
  return delegate_.get();
}

WEAVE_ERROR ConnectivityManagerImpl::_Init(void) {
  FX_CHECK(delegate_ != nullptr) << "ConnectivityManager delegate not set before Init.";
  return delegate_->Init();
}

bool ConnectivityManagerImpl::_IsServiceTunnelConnected(void) {
  return delegate_->IsServiceTunnelConnected();
}

bool ConnectivityManagerImpl::_IsServiceTunnelRestricted(void) {
  return delegate_->IsServiceTunnelRestricted();
}

void ConnectivityManagerImpl::_OnPlatformEvent(const WeaveDeviceEvent* event) {
  return delegate_->OnPlatformEvent(event);
}

bool ConnectivityManagerImpl::_HaveServiceConnectivityViaTunnel(void) {
  return delegate_->IsServiceTunnelConnected() && !delegate_->IsServiceTunnelRestricted();
}

ConnectivityManager::ServiceTunnelMode ConnectivityManagerImpl::_GetServiceTunnelMode(void) {
  return delegate_->GetServiceTunnelMode();
}

bool ConnectivityManagerImpl::_HaveIPv4InternetConnectivity(void) {
  return delegate_->HaveIPv4InternetConnectivity();
}

bool ConnectivityManagerImpl::_HaveIPv6InternetConnectivity(void) {
  return delegate_->HaveIPv6InternetConnectivity();
}

ConnectivityManager::ServiceTunnelMode ConnectivityManagerImpl::Delegate::GetServiceTunnelMode(
    void) {
  return service_tunnel_mode_;
}

bool ConnectivityManagerImpl::Delegate::HaveIPv4InternetConnectivity(void) {
  return ::nl::GetFlag(flags_, kFlag_HaveIPv4InternetConnectivity);
}

bool ConnectivityManagerImpl::Delegate::HaveIPv6InternetConnectivity(void) {
  return ::nl::GetFlag(flags_, kFlag_HaveIPv6InternetConnectivity);
}

std::optional<std::string> ConnectivityManagerImpl::GetWiFiInterfaceName() {
  return delegate_->GetWiFiInterfaceName();
}

ConnectivityManager::ThreadMode ConnectivityManagerImpl::_GetThreadMode(void) {
  return delegate_->GetThreadMode();
}

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl
