// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// clang-format off
#pragma GCC diagnostic push
#include <Weave/DeviceLayer/internal/WeaveDeviceLayerInternal.h>
#include <Weave/DeviceLayer/internal/BLEManager.h>
#pragma GCC diagnostic pop
#include "src/connectivity/weave/adaptation/ble_manager_impl.h"
// clang-format on

#include <lib/gtest/test_loop_fixture.h>
#include <lib/sys/cpp/testing/component_context_provider.h>
#include <lib/syslog/cpp/macros.h>

#include <gtest/gtest.h>

#include "connectivity_manager_delegate_impl.h"
#include "fake_ble_peripheral.h"
#include "fake_gatt_server.h"
#include "test_configuration_manager.h"
#include "test_thread_stack_manager.h"
#include "weave_test_fixture.h"

namespace nl::Weave::DeviceLayer::Internal::testing {
namespace {
using nl::Weave::DeviceLayer::ConnectivityManager;
using nl::Weave::DeviceLayer::Internal::BLEManager;
using nl::Weave::DeviceLayer::Internal::BLEManagerImpl;
using weave::adaptation::testing::FakeBLEPeripheral;
using weave::adaptation::testing::FakeGATTService;
using weave::adaptation::testing::TestConfigurationManager;
using weave::adaptation::testing::TestThreadStackManager;
}  // namespace

class BLEManagerTest : public WeaveTestFixture<> {
 public:
  BLEManagerTest() {
    context_provider_.service_directory_provider()->AddService(
        fake_gatt_server_.GetHandler(dispatcher()));
    context_provider_.service_directory_provider()->AddService(
        fake_ble_peripheral_.GetHandler(dispatcher()));
  }

  void SetUp() override {
    WeaveTestFixture<>::SetUp();
    WeaveTestFixture<>::RunFixtureLoop();

    PlatformMgrImpl().SetComponentContextForProcess(context_provider_.TakeContext());
    PlatformMgrImpl().SetDispatcher(event_loop_.dispatcher());
    PlatformMgrImpl().GetSystemLayer().Init(nullptr);

    ThreadStackMgrImpl().SetDelegate(std::make_unique<TestThreadStackManager>());
    ConfigurationMgrImpl().SetDelegate(std::make_unique<TestConfigurationManager>());
    ConnectivityMgrImpl().SetDelegate(std::make_unique<ConnectivityManagerDelegateImpl>());
    EXPECT_EQ(ConfigurationMgrImpl().IsWoBLEEnabled(), true);

    ble_mgr_ = std::make_unique<BLEManagerImpl>();
    InitBleMgr();
  }

  void TearDown() override {
    event_loop_.Quit();
    WeaveTestFixture<>::StopFixtureLoop();
    WeaveTestFixture<>::TearDown();

    ThreadStackMgrImpl().SetDelegate(nullptr);
    ConfigurationMgrImpl().SetDelegate(nullptr);
    ConnectivityMgrImpl().SetDelegate(nullptr);
  }

 protected:
  void InitBleMgr() {
    EXPECT_EQ(ble_mgr_->_Init(), WEAVE_NO_ERROR);
    event_loop_.RunUntilIdle();
    EXPECT_EQ(GetBLEMgrServiceMode(), ConnectivityManager::kWoBLEServiceMode_Enabled);
    if (ConfigurationMgrImpl().IsWoBLEAdvertisementEnabled()) {
      EXPECT_EQ(IsBLEMgrAdvertising(), true);
    } else {
      EXPECT_EQ(IsBLEMgrAdvertising(), false);
    }
  }

  BLEManager::WoBLEServiceMode GetBLEMgrServiceMode() { return ble_mgr_->_GetWoBLEServiceMode(); }

  uint16_t IsBLEMgrAdvertising() { return ble_mgr_->_IsAdvertising(); }

  WEAVE_ERROR GetBLEMgrDeviceName(char* device_name, size_t device_name_size) {
    return ble_mgr_->_GetDeviceName(device_name, device_name_size);
  }

  WEAVE_ERROR SetBLEMgrDeviceName(const char* device_name) {
    return ble_mgr_->_SetDeviceName(device_name);
  }

  void SetWoBLEAdvertising(bool enabled) {
    EXPECT_EQ(ble_mgr_->_SetAdvertisingEnabled(enabled), WEAVE_NO_ERROR);
    event_loop_.RunUntilIdle();
  }

  void WeaveConnect() {
    EXPECT_EQ(fake_gatt_server_.WriteRequest(), fuchsia::bluetooth::gatt::ErrorCode::NO_ERROR);
    event_loop_.RunUntilIdle();
    EXPECT_EQ(fake_gatt_server_.WeaveConnectionConfirmed(), false);
    fake_gatt_server_.OnCharacteristicConfiguration();
    // Event loop will be idle and waiting for subscribe request(characteristic configuration)
    // on timer. So we need to wait until either subscribe request is received or timeout.
    event_loop_.Run(zx::time::infinite(), true /*once*/);

    // Stop fixture loop before waiting for FakeGATTLocalService::NotifyValue
    // on dispatcher().
    WeaveTestFixture<>::StopFixtureLoop();
    // Wait until FakeGATTLocalService::NotifyValue is called.
    RunLoopUntil([&]() {
      bool res = fake_gatt_server_.WeaveConnectionConfirmed();
      return res;
    });
    // Wait for FakeGATTLocalService::NotifyValue completed. Restart fixture loop.
    WeaveTestFixture<>::RunFixtureLoop();

    bool is_confirmed = fake_gatt_server_.WeaveConnectionConfirmed();
    EXPECT_EQ(is_confirmed, true);
  }

 private:
  sys::testing::ComponentContextProvider context_provider_;
  std::unique_ptr<BLEManagerImpl> ble_mgr_;

  FakeGATTService fake_gatt_server_;
  FakeBLEPeripheral fake_ble_peripheral_;
  async::Loop event_loop_{&kAsyncLoopConfigNoAttachToCurrentThread};
};

TEST_F(BLEManagerTest, SetAndGetDeviceName) {
  constexpr char kLargeDeviceName[] = "TOO_LARGE_DEVICE_NAME_FUCHSIA";
  constexpr char kDeviceName[] = "FUCHSIATEST";
  char read_value[kMaxDeviceNameLength + 1];
  EXPECT_EQ(SetBLEMgrDeviceName(kLargeDeviceName), WEAVE_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(SetBLEMgrDeviceName(kDeviceName), WEAVE_NO_ERROR);
  EXPECT_EQ(GetBLEMgrDeviceName(read_value, 1), WEAVE_ERROR_BUFFER_TOO_SMALL);
  EXPECT_EQ(GetBLEMgrDeviceName(read_value, sizeof(read_value)), WEAVE_NO_ERROR);
  EXPECT_STREQ(kDeviceName, read_value);
}

TEST_F(BLEManagerTest, EnableAndDisableAdvertising) {
  // Disable Weave service advertising
  SetWoBLEAdvertising(false);
  EXPECT_EQ(IsBLEMgrAdvertising(), false);
  // Enable Weave service advertising
  SetWoBLEAdvertising(true);
  EXPECT_EQ(IsBLEMgrAdvertising(), true);
  // Re-enable Weave service advertising
  SetWoBLEAdvertising(true);
  EXPECT_EQ(IsBLEMgrAdvertising(), true);
}

TEST_F(BLEManagerTest, TestWeaveConnect) { WeaveConnect(); }

}  // namespace nl::Weave::DeviceLayer::Internal::testing
