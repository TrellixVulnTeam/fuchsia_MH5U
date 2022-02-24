// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_LIB_CODEC_IMPL_INCLUDE_LIB_MEDIA_CODEC_IMPL_CODEC_PORT_H_
#define SRC_MEDIA_LIB_CODEC_IMPL_INCLUDE_LIB_MEDIA_CODEC_IMPL_CODEC_PORT_H_

#include <stdint.h>

#include <limits>

// Constants for indexing into our own member variable arrays.
enum CodecPort {
  kFirstPort = 0,
  kInputPort = 0,
  kOutputPort = 1,
  kPortCount = 2,
  kInvalidPort = std::numeric_limits<uint32_t>::max(),
};

#endif  // SRC_MEDIA_LIB_CODEC_IMPL_INCLUDE_LIB_MEDIA_CODEC_IMPL_CODEC_PORT_H_
