// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/wlan/common/c/banjo.h>
#include <fuchsia/wlan/ieee80211/cpp/fidl.h>

#include "src/connectivity/wlan/drivers/testing/lib/sim-fake-ap/sim-fake-ap.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/fwil.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/sim.h"
#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"

namespace wlan::brcmfmac {

constexpr uint16_t kDefaultCh = 149;
constexpr wlan_channel_t kDefaultChannel = {
    .primary = kDefaultCh, .cbw = CHANNEL_BANDWIDTH_CBW20, .secondary80 = 0};
const common::MacAddr kDefaultBssid({0x12, 0x34, 0x56, 0x78, 0x9a, 0xbc});
constexpr cssid_t kDefaultSsid = {.len = 15, .data = "Fuchsia Fake AP"};
const common::MacAddr kFakeMac({0xde, 0xad, 0xbe, 0xef, 0x00, 0x02});
constexpr simulation::WlanTxInfo kDefaultTxInfo = {.channel = kDefaultChannel};

class MfgTest : public SimTest {
 public:
  static constexpr zx::duration kTestDuration = zx::sec(100);
  // How many devices have been registered by the fake devhost
  uint32_t DeviceCountByProtocolId(uint32_t proto_id);
  void CreateIF(wlan_mac_role_t role);
  void DelIF(SimInterface* ifc);
  void StartSoftAP();
  void TxAuthAndAssocReq();

 protected:
  SimInterface client_ifc_;
  SimInterface softap_ifc_;
};

uint32_t MfgTest::DeviceCountByProtocolId(uint32_t proto_id) {
  return dev_mgr_->DeviceCountByProtocolId(proto_id);
}

void MfgTest::CreateIF(wlan_mac_role_t role) {
  switch (role) {
    case WLAN_MAC_ROLE_CLIENT:
      ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);
      break;
    case WLAN_MAC_ROLE_AP:
      ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_), ZX_OK);
      break;
  }
}
void MfgTest::DelIF(SimInterface* ifc) { EXPECT_EQ(DeleteInterface(ifc), ZX_OK); }

void MfgTest::StartSoftAP() {
  softap_ifc_.StartSoftAp(SimInterface::kDefaultSoftApSsid, kDefaultChannel);
}

void MfgTest::TxAuthAndAssocReq() {
  // Get the mac address of the SoftAP
  common::MacAddr soft_ap_mac;
  softap_ifc_.GetMacAddr(&soft_ap_mac);
  cssid_t ssid = {.len = 6, .data = "Sim_AP"};
  // Pass the auth stop for softAP iface before assoc.
  simulation::SimAuthFrame auth_req_frame(kFakeMac, soft_ap_mac, 1, simulation::AUTH_TYPE_OPEN,
                                          ::fuchsia::wlan::ieee80211::StatusCode::SUCCESS);
  env_->Tx(auth_req_frame, kDefaultTxInfo, this);
  simulation::SimAssocReqFrame assoc_req_frame(kFakeMac, soft_ap_mac, ssid);
  env_->Tx(assoc_req_frame, kDefaultTxInfo, this);
}

// Check to make sure only one IF can be active at anytime with MFG FW.
TEST_F(MfgTest, BasicTest) {
  Init();
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);

  // SoftAP If creation should fail as Client IF has already been created.
  ASSERT_NE(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_, std::nullopt, kDefaultBssid), ZX_OK);

  // Now delete the Client IF and SoftAP creation should pass
  EXPECT_EQ(DeleteInterface(&client_ifc_), ZX_OK);
  EXPECT_EQ(DeviceCountByProtocolId(ZX_PROTOCOL_WLAN_FULLMAC_IMPL), 0u);
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_AP, &softap_ifc_, std::nullopt, kDefaultBssid), ZX_OK);
  // Now that SoftAP IF is created, Client IF creation should fail
  ASSERT_NE(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_), ZX_OK);
  EXPECT_EQ(DeleteInterface(&softap_ifc_), ZX_OK);
}

// Start client and SoftAP interfaces and check if
// the client can associate to a FakeAP and a fake client can associate to the
// SoftAP.
TEST_F(MfgTest, CheckConnections) {
  Init();
  StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc_);
  // Start up our fake AP
  simulation::FakeAp ap(env_.get(), kDefaultBssid, kDefaultSsid, kDefaultChannel);

  // Associate to FakeAp
  client_ifc_.AssociateWith(ap, zx::msec(10));
  env_->ScheduleNotification(std::bind(&MfgTest::DelIF, this, &client_ifc_), zx::msec(100));
  env_->ScheduleNotification(std::bind(&MfgTest::CreateIF, this, WLAN_MAC_ROLE_AP), zx::msec(200));
  env_->ScheduleNotification(std::bind(&MfgTest::StartSoftAP, this), zx::msec(300));
  // Associate to SoftAP
  env_->ScheduleNotification(std::bind(&MfgTest::TxAuthAndAssocReq, this), zx::msec(400));
  env_->ScheduleNotification(std::bind(&MfgTest::DelIF, this, &softap_ifc_), zx::msec(500));

  env_->Run(kTestDuration);

  // Check if the client's assoc with FakeAP succeeded
  EXPECT_EQ(client_ifc_.stats_.assoc_attempts, 1u);
  EXPECT_EQ(client_ifc_.stats_.assoc_successes, 1u);
  // Deletion of the client IF should have resulted in disassoc of the
  // client (cleanup during IF delete).
  EXPECT_EQ(client_ifc_.stats_.disassoc_indications.size(), 1u);
  // Verify Assoc with SoftAP succeeded
  ASSERT_EQ(softap_ifc_.stats_.assoc_indications.size(), 1u);
  ASSERT_EQ(softap_ifc_.stats_.auth_indications.size(), 1u);
}
}  // namespace wlan::brcmfmac
