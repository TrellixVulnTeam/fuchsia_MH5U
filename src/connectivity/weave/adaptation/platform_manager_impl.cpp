// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/PlatformManager.h>

#include "generic_platform_manager_impl_fuchsia.ipp"
#include "configuration_manager_delegate_impl.h"
#include "connectivity_manager_delegate_impl.h"
#include "network_provisioning_server_delegate_impl.h"
#include "thread_stack_manager_delegate_impl.h"
// clang-format on

#include <lib/async/cpp/task.h>
#include <lib/async/default.h>
#include <lib/syslog/cpp/macros.h>

namespace nl::Weave::DeviceLayer {

using Internal::NetworkProvisioningSvrImpl;

PlatformManagerImpl PlatformManagerImpl::sInstance;

WEAVE_ERROR PlatformManagerImpl::_InitWeaveStack() {
  FX_CHECK(ConfigurationMgrImpl().GetDelegate() != nullptr)
      << "ConfigurationManager delegate must be set before InitWeaveStack is called.";
  FX_CHECK(ConnectivityMgrImpl().GetDelegate() != nullptr)
      << "ConnectivityManager delegate must be set before InitWeaveStack is called.";
  FX_CHECK(NetworkProvisioningSvrImpl().GetDelegate() != nullptr)
      << "NetworkProvisioningServer delegate must be set before InitWeaveStack is called.";
  FX_CHECK(ThreadStackMgrImpl().GetDelegate() != nullptr)
      << "ThreadStackManager delegate must be set before InitWeaveStack is called.";
  return Internal::GenericPlatformManagerImpl_Fuchsia<PlatformManagerImpl>::_InitWeaveStack();
}

sys::ComponentContext *PlatformManagerImpl::GetComponentContextForProcess() {
  if (!context_) {
    context_ = sys::ComponentContext::CreateAndServeOutgoingDirectory();
  }
  return context_.get();
}

void PlatformManagerImpl::SetComponentContextForProcess(
    std::unique_ptr<sys::ComponentContext> context) {
  context_ = std::move(context);
}

void PlatformManagerImpl::SetDispatcher(async_dispatcher_t *dispatcher) {
  ZX_ASSERT(dispatcher != nullptr);
  dispatcher_ = dispatcher;
}

async_dispatcher_t *PlatformManagerImpl::GetDispatcher() {
  ZX_ASSERT(dispatcher_ != nullptr);
  return dispatcher_;
}

void PlatformManagerImpl::_PostEvent(const WeaveDeviceEvent *event) {
  ZX_ASSERT(dispatcher_ != nullptr);
  async::PostTask(dispatcher_, [ev = *event] { PlatformMgr().DispatchEvent(&ev); });
  GetSystemLayer().WakeSelect();
}

void PlatformManagerImpl::ShutdownWeaveStack() {
  Internal::GenericPlatformManagerImpl_Fuchsia<PlatformManagerImpl>::_ShutdownWeaveStack();
  ThreadStackMgrImpl().SetDelegate(nullptr);
  NetworkProvisioningSvrImpl().SetDelegate(nullptr);
  ConnectivityMgrImpl().SetDelegate(nullptr);
  ConfigurationMgrImpl().SetDelegate(nullptr);
  context_.reset();
}

InetLayer::FuchsiaPlatformData *PlatformManagerImpl::GetPlatformData() {
  platform_data_.ctx = GetComponentContextForProcess();
  platform_data_.dispatcher = dispatcher_;
  return &platform_data_;
}

}  // namespace nl::Weave::DeviceLayer
