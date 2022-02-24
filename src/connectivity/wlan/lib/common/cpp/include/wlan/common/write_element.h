// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_WRITE_ELEMENT_H_
#define SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_WRITE_ELEMENT_H_

#include <wlan/common/buffer_writer.h>
#include <wlan/common/element.h>

namespace wlan {
namespace common {

void WriteSsid(BufferWriter* w, cpp20::span<const uint8_t> ssid);
void WriteSupportedRates(BufferWriter* w, cpp20::span<const SupportedRate> supported_rates);
void WriteDsssParamSet(BufferWriter* w, uint8_t current_channel);
void WriteCfParamSet(BufferWriter* w, CfParamSet param_set);
void WriteTim(BufferWriter* w, TimHeader header, cpp20::span<const uint8_t> bitmap);
void WriteCountry(BufferWriter* w, Country country, cpp20::span<SubbandTriplet> triplets);
void WriteExtendedSupportedRates(BufferWriter* w,
                                 cpp20::span<const SupportedRate> ext_supported_rates);
void WriteMeshConfiguration(BufferWriter* w, MeshConfiguration mesh_config);
void WriteMeshId(BufferWriter* w, cpp20::span<const uint8_t> mesh_id);
void WriteQosCapability(BufferWriter* w, QosInfo qos_info);
void WriteGcrGroupAddress(BufferWriter* w, common::MacAddr gcr_group_addr);
void WriteHtCapabilities(BufferWriter* w, const HtCapabilities& ht_caps);
void WriteHtOperation(BufferWriter* w, const HtOperation& ht_op);
void WriteVhtCapabilities(BufferWriter* w, const VhtCapabilities& vht_caps);
void WriteVhtOperation(BufferWriter* w, const VhtOperation& vht_op);
void WriteMpmOpen(BufferWriter* w, MpmHeader mpm_header, const MpmPmk* pmk);
void WriteMpmConfirm(BufferWriter* w, MpmHeader mpm_header, uint16_t peer_link_id,
                     const MpmPmk* pmk);
void WritePreq(BufferWriter* w, const PreqHeader& header,
               const common::MacAddr* originator_external_addr, const PreqMiddle& middle,
               cpp20::span<const PreqPerTarget> per_target);
void WritePrep(BufferWriter* w, const PrepHeader& header,
               const common::MacAddr* target_external_addr, const PrepTail& tail);
void WritePerr(BufferWriter* w, const PerrHeader& header, cpp20::span<const uint8_t> destinations);

}  // namespace common
}  // namespace wlan

#endif  // SRC_CONNECTIVITY_WLAN_LIB_COMMON_CPP_INCLUDE_WLAN_COMMON_WRITE_ELEMENT_H_
