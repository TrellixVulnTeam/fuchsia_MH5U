// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_
#define SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_

#include <lib/stdcompat/span.h>

#include <memory>

#include <src/connectivity/wlan/lib/mlme/rust/c-binding/bindings.h>
#include <wlan/common/macaddr.h>

namespace wlan {

using SequenceManager =
    std::unique_ptr<mlme_sequence_manager_t, void (*)(mlme_sequence_manager_t*)>;
using RustClientMlme = std::unique_ptr<wlan_mlme_handle_t, void (*)(wlan_mlme_handle_t*)>;
using ApStation = std::unique_ptr<wlan_mlme_handle_t, void (*)(wlan_mlme_handle_t*)>;

SequenceManager NewSequenceManager();

static inline constexpr wlan_span_t AsWlanSpan(cpp20::span<const uint8_t> span) {
  return wlan_span_t{.data = span.data(), .size = span.size_bytes()};
}

}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_MLME_CPP_INCLUDE_WLAN_MLME_RUST_UTILS_H_
