// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_KEY_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_KEY_H_

#include <fuchsia/hardware/wlan/associnfo/c/banjo.h>
#include <fuchsia/hardware/wlan/softmac/c/banjo.h>
#include <fuchsia/wlan/mlme/cpp/fidl.h>

#include <optional>

namespace wlan {

std::optional<wlan_key_config_t> ToKeyConfig(
    const ::fuchsia::wlan::mlme::SetKeyDescriptor& key_descriptor);

}  // namespace wlan
#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_KEY_H_
