// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_PPP_LIB_HDLC_FCS_H_
#define SRC_CONNECTIVITY_PPP_LIB_HDLC_FCS_H_

#include <lib/stdcompat/span.h>

#include <cstddef>
#include <cstdint>

namespace ppp {

static constexpr uint16_t kFrameCheckSequenceInit = 0xffff;
static constexpr uint16_t kFrameCheckSequence = 0xf0b8;

uint16_t Fcs(uint16_t current, cpp20::span<const uint8_t> data);

}  // namespace ppp

#endif  // SRC_CONNECTIVITY_PPP_LIB_HDLC_FCS_H_
