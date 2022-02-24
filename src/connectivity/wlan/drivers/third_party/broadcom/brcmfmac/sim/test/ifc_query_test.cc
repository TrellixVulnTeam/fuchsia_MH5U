// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#include <fuchsia/hardware/wlan/fullmac/c/banjo.h>
#include <fuchsia/hardware/wlan/phyinfo/c/banjo.h>
#include <fuchsia/wlan/ieee80211/c/banjo.h>
#include <zircon/errors.h>

#include <gtest/gtest.h>

#include "src/connectivity/wlan/drivers/third_party/broadcom/brcmfmac/sim/test/sim_test.h"
#include "src/connectivity/wlan/lib/common/cpp/include/wlan/common/macaddr.h"

namespace wlan::brcmfmac {
namespace {

constexpr zx::duration kSimulatedClockDuration = zx::sec(10);

}  // namespace

const common::MacAddr kDefaultMac({0x12, 0x34, 0x56, 0x65, 0x43, 0x21});

// Verify that a query operation works on a client interface
TEST_F(SimTest, ClientIfcQuery) {
  ASSERT_EQ(Init(), ZX_OK);

  SimInterface client_ifc;
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc, std::nullopt, kDefaultMac), ZX_OK);

  wlan_fullmac_query_info_t ifc_query_result;
  env_->ScheduleNotification(std::bind(&SimInterface::Query, &client_ifc, &ifc_query_result),
                             zx::sec(1));
  env_->Run(kSimulatedClockDuration);

  // Mac address returned should match the one we specified when we created the interface
  ASSERT_EQ(fuchsia_wlan_ieee80211_MAC_ADDR_LEN, common::kMacAddrLen);
  EXPECT_EQ(
      0, memcmp(kDefaultMac.byte, ifc_query_result.sta_addr, fuchsia_wlan_ieee80211_MAC_ADDR_LEN));

  EXPECT_EQ(ifc_query_result.role, WLAN_MAC_ROLE_CLIENT);

  // Number of bands shouldn't exceed the maximum allowable
  ASSERT_LE(ifc_query_result.band_cap_count, (size_t)fuchsia_wlan_common_MAX_BANDS);

  for (size_t band = 0; band < ifc_query_result.band_cap_count; band++) {
    wlan_fullmac_band_capability* band_cap = &ifc_query_result.band_cap_list[band];

    // Band id should be in valid range
    EXPECT_LE(band_cap->band, fuchsia_wlan_common_MAX_BANDS);

    // Number of channels shouldn't exceed the maximum allowable
    ASSERT_LE(band_cap->operating_channel_count,
              (size_t)fuchsia_wlan_ieee80211_MAX_UNIQUE_CHANNEL_NUMBERS);
  }

  // Verify driver features from if query.
  EXPECT_NE(0U, ifc_query_result.driver_features & WLAN_INFO_DRIVER_FEATURE_DFS);
  EXPECT_NE(0U, ifc_query_result.driver_features & WLAN_INFO_DRIVER_FEATURE_SAE_SME_AUTH);
}

// Verify that we can retrieve interface attributes even if the nchain iovar value is too large
TEST_F(SimTest, BadNchainIovar) {
  ASSERT_EQ(Init(), ZX_OK);

  SimInterface client_ifc;
  ASSERT_EQ(StartInterface(WLAN_MAC_ROLE_CLIENT, &client_ifc), ZX_OK);

  // This invalid value of rxchain data has the potential to overflow the driver's internal
  // data structures
  const std::vector<uint8_t> alt_rxchain_data = {0xff, 0xff, 0xff, 0xff};
  brcmf_simdev* sim = device_->GetSim();
  sim->sim_fw->err_inj_.AddErrInjIovar("rxstreams_cap", ZX_OK, BCME_OK, client_ifc.iface_id_,
                                       &alt_rxchain_data);

  wlan_fullmac_query_info_t ifc_query_result;
  env_->ScheduleNotification(std::bind(&SimInterface::Query, &client_ifc, &ifc_query_result),
                             zx::sec(1));
  env_->Run(kSimulatedClockDuration);

  // This test just verifies that we don't crash when the iovar is retrieved
}

}  // namespace wlan::brcmfmac
