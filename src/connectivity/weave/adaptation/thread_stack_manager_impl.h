// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SRC_CONNECTIVITY_WEAVE_ADAPTATION_THREAD_STACK_MANAGER_IMPL_H_
#define SRC_CONNECTIVITY_WEAVE_ADAPTATION_THREAD_STACK_MANAGER_IMPL_H_

#include <fuchsia/lowpan/device/cpp/fidl.h>

#include <nest/trait/network/TelemetryNetworkWpanTrait.h>

namespace nl {
namespace Weave {
namespace DeviceLayer {

class NL_DLL_EXPORT ThreadStackManagerImpl final : public ThreadStackManager {
  // Allow the ThreadStackManager interface class to delegate method calls to
  // the implementation methods provided by this class.
  friend class ThreadStackManager;

  // Allow the singleton accessors to access the instance.
  friend ThreadStackManager& ::nl::Weave::DeviceLayer::ThreadStackMgr();
  friend ThreadStackManagerImpl& ::nl::Weave::DeviceLayer::ThreadStackMgrImpl();

 private:
  ThreadStackManagerImpl() = default;

 public:
  /**
   * Delegate class to handle platform-specific implementations of the
   * ThreadStackManager API surface. This enables tests to swap out the
   * implementation of the static ThreadStackManager instance.
   */
  class Delegate {
   public:
    friend class ThreadStackManagerImpl;
    virtual ~Delegate() = default;

   private:
    // Initialize the implementation.
    virtual WEAVE_ERROR InitThreadStack() = 0;
    // Determine if the supplied IPAddress is accessible by a route through the
    // Thread interface.
    virtual bool HaveRouteToAddress(const IPAddress& destAddr) = 0;
    // Handle a DeviceLayer platform event.
    virtual void OnPlatformEvent(const WeaveDeviceEvent* event) = 0;
    // Determine if Thread is enabled/active.
    virtual bool IsThreadEnabled() = 0;
    // Attempt to set whether Thread is enabled/active.
    virtual WEAVE_ERROR SetThreadEnabled(bool val) = 0;
    // Determine if Thread is provisioned.
    virtual bool IsThreadProvisioned() = 0;
    // Determine if the Thread device is attached to the network.
    virtual bool IsThreadAttached() = 0;
    // Retrieve the Thread provision.
    virtual WEAVE_ERROR GetThreadProvision(Internal::DeviceNetworkInfo& netInfo,
                                           bool includeCredentials) = 0;
    // Set the Thread provision.
    virtual WEAVE_ERROR SetThreadProvision(const Internal::DeviceNetworkInfo& netInfo) = 0;
    // Clear/remove the Thread provision.
    virtual void ClearThreadProvision() = 0;
    // Determine the current device type of the Thread device.
    virtual ConnectivityManager::ThreadDeviceType GetThreadDeviceType() = 0;
    // Determine if there is mesh connectivity.
    virtual bool HaveMeshConnectivity() = 0;
    // Log a Weave event for the Thread statistics.
    virtual WEAVE_ERROR GetAndLogThreadStatsCounters() = 0;
    // Log a Weave event for a minimimal Thread topology.
    virtual WEAVE_ERROR GetAndLogThreadTopologyMinimal() = 0;
    // Log a Weave event for a full Thread topology.
    virtual WEAVE_ERROR GetAndLogThreadTopologyFull() = 0;
    // Get the name of the thread interface.
    virtual std::string GetInterfaceName() const = 0;
    // Determine if Thread is supported. If `false` all calls other than
    // InitThreadStack should return unsuccessfully with no side effects.
    virtual bool IsThreadSupported() const = 0;
    // Get the primary 802154 MAC address. Supplied buffer must be 8 bytes long.
    virtual WEAVE_ERROR GetPrimary802154MACAddress(uint8_t* mac_address) = 0;
    // Set whether Thread should be in joinable mode or not.
    virtual WEAVE_ERROR SetThreadJoinable(bool enable) = 0;

    // Log a NetworkWpanStatsEvent.
    virtual nl::Weave::Profiles::DataManagement::event_id_t LogNetworkWpanStatsEvent(
        Schema::Nest::Trait::Network::TelemetryNetworkWpanTrait::NetworkWpanStatsEvent* event) = 0;
  };

  // Sets the delegate containing the platform-specific implementation. It is
  // invalid to invoke the ThreadStackManager without setting a delegate
  // first. However, the OpenWeave surface requires a no-constructor
  // instantiation of this class, so it is up to the caller to enforce this.
  void SetDelegate(std::unique_ptr<Delegate> delegate);

  // Gets the delegate currently in use. This may return nullptr if no delegate
  // was set on this class.
  Delegate* GetDelegate();

  // ThreadStackManager implementations. Public for testing purposes only.
  WEAVE_ERROR _InitThreadStack();

  void _ProcessThreadActivity() {}
  WEAVE_ERROR _StartThreadTask() {
    return WEAVE_NO_ERROR;  // No thread task is managed here.
  }

  void _LockThreadStack() {}
  bool _TryLockThreadStack() { return true; }
  void _UnlockThreadStack() {}

  bool _HaveRouteToAddress(const IPAddress& destAddr);

  WEAVE_ERROR _GetPrimary802154MACAddress(uint8_t* mac_address);

  void _OnPlatformEvent(const WeaveDeviceEvent* event);

  bool _IsThreadEnabled();
  WEAVE_ERROR _SetThreadEnabled(bool val);
  bool _IsThreadProvisioned();
  bool _IsThreadAttached();

  WEAVE_ERROR _GetThreadProvision(Internal::DeviceNetworkInfo& netInfo, bool includeCredentials);
  WEAVE_ERROR _SetThreadProvision(const Internal::DeviceNetworkInfo& netInfo);
  void _ClearThreadProvision();

  ConnectivityManager::ThreadDeviceType _GetThreadDeviceType();
  WEAVE_ERROR _SetThreadDeviceType(ConnectivityManager::ThreadDeviceType threadRole) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  void _GetThreadPollingConfig(ConnectivityManager::ThreadPollingConfig& pollingConfig) {
    pollingConfig.Clear();  // GetThreadPollingConfig not supported.
  }
  WEAVE_ERROR _SetThreadPollingConfig(
      const ConnectivityManager::ThreadPollingConfig& pollingConfig) {
    return WEAVE_ERROR_UNSUPPORTED_WEAVE_FEATURE;
  }

  bool _HaveMeshConnectivity();

  void _OnMessageLayerActivityChanged(bool messageLayerIsActive) {}

  void _OnWoBLEAdvertisingStart() {}
  void _OnWoBLEAdvertisingStop() {}

  WEAVE_ERROR _GetAndLogThreadStatsCounters();
  WEAVE_ERROR _GetAndLogThreadTopologyMinimal();
  WEAVE_ERROR _GetAndLogThreadTopologyFull();

  // ThreadStackManagerImpl-specific functionality.
  std::string GetInterfaceName() const;
  bool IsThreadSupported() const;
  WEAVE_ERROR SetThreadJoinable(bool enable);

 private:
  static ThreadStackManagerImpl sInstance;
  std::unique_ptr<Delegate> delegate_;
};

inline ThreadStackManager& ThreadStackMgr() { return ThreadStackManagerImpl::sInstance; }
inline ThreadStackManagerImpl& ThreadStackMgrImpl() { return ThreadStackManagerImpl::sInstance; }

}  // namespace DeviceLayer
}  // namespace Weave
}  // namespace nl

#endif  // SRC_CONNECTIVITY_WEAVE_ADAPTATION_THREAD_STACK_MANAGER_IMPL_H_
