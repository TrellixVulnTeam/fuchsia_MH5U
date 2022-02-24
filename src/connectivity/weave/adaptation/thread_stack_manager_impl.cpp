// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/macros.h>

// clang-format off
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/internal/DeviceNetworkInfo.h>
#include <Weave/DeviceLayer/ThreadStackManager.h>
// clang-format on

namespace nl {
namespace Weave {
namespace DeviceLayer {

ThreadStackManagerImpl ThreadStackManagerImpl::sInstance;

void ThreadStackManagerImpl::SetDelegate(std::unique_ptr<Delegate> delegate) {
  FX_CHECK(!(delegate && delegate_)) << "Attempt to set an already set delegate. Must explicitly "
                                        "clear the existing delegate first.";
  delegate_ = std::move(delegate);
}

ThreadStackManagerImpl::Delegate* ThreadStackManagerImpl::GetDelegate() { return delegate_.get(); }

WEAVE_ERROR ThreadStackManagerImpl::_InitThreadStack() {
  if (!delegate_) {
    FX_LOGS(ERROR) << "InitThreadStack called without initializing with a delegate";
    return WEAVE_ERROR_INCORRECT_STATE;
  }

  return delegate_->InitThreadStack();
}

WEAVE_ERROR ThreadStackManagerImpl::_GetPrimary802154MACAddress(uint8_t* mac_address) {
  return delegate_->GetPrimary802154MACAddress(mac_address);
}

bool ThreadStackManagerImpl::_HaveRouteToAddress(const IPAddress& destAddr) {
  return delegate_->HaveRouteToAddress(destAddr);
}

void ThreadStackManagerImpl::_OnPlatformEvent(const WeaveDeviceEvent* event) {
  delegate_->OnPlatformEvent(event);
}

bool ThreadStackManagerImpl::_IsThreadEnabled() { return delegate_->IsThreadEnabled(); }

WEAVE_ERROR ThreadStackManagerImpl::_SetThreadEnabled(bool val) {
  return delegate_->SetThreadEnabled(val);
}

bool ThreadStackManagerImpl::_IsThreadProvisioned() { return delegate_->IsThreadProvisioned(); }

bool ThreadStackManagerImpl::_IsThreadAttached() { return delegate_->IsThreadAttached(); }

WEAVE_ERROR ThreadStackManagerImpl::_GetThreadProvision(Internal::DeviceNetworkInfo& netInfo,
                                                        bool includeCredentials) {
  return delegate_->GetThreadProvision(netInfo, includeCredentials);
}

WEAVE_ERROR ThreadStackManagerImpl::_SetThreadProvision(
    const Internal::DeviceNetworkInfo& netInfo) {
  return delegate_->SetThreadProvision(netInfo);
}

void ThreadStackManagerImpl::_ClearThreadProvision() { delegate_->ClearThreadProvision(); }

ConnectivityManager::ThreadDeviceType ThreadStackManagerImpl::_GetThreadDeviceType() {
  return delegate_->GetThreadDeviceType();
}

bool ThreadStackManagerImpl::_HaveMeshConnectivity() { return delegate_->HaveMeshConnectivity(); }

WEAVE_ERROR ThreadStackManagerImpl::_GetAndLogThreadStatsCounters() {
  return delegate_->GetAndLogThreadStatsCounters();
}

WEAVE_ERROR ThreadStackManagerImpl::_GetAndLogThreadTopologyMinimal() {
  return delegate_->GetAndLogThreadTopologyMinimal();
}

WEAVE_ERROR ThreadStackManagerImpl::_GetAndLogThreadTopologyFull() {
  return delegate_->GetAndLogThreadTopologyFull();
}

std::string ThreadStackManagerImpl::GetInterfaceName() const {
  return delegate_->GetInterfaceName();
}

bool ThreadStackManagerImpl::IsThreadSupported() const { return delegate_->IsThreadSupported(); }

WEAVE_ERROR ThreadStackManagerImpl::SetThreadJoinable(bool enable) {
  return delegate_->SetThreadJoinable(enable);
}

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl
