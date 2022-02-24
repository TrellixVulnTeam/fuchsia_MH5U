// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_PHY_H_
#define SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_PHY_H_

#include <fuchsia/wlan/tap/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/ddk/device.h>
#include <lib/zx/channel.h>

namespace wlan {

zx_status_t CreatePhy(zx_device_t* wlantapctl, zx::channel user_channel,
                      std::unique_ptr<::fuchsia::wlan::tap::WlantapPhyConfig> phy_config_from_fidl,
                      async_dispatcher_t* loop);

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_TESTING_WLANTAP_DRIVER_WLANTAP_PHY_H_
