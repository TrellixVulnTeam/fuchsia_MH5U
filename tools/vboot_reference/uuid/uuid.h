// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TOOLS_VBOOT_REFERENCE_UUID_UUID_H_
#define TOOLS_VBOOT_REFERENCE_UUID_UUID_H_

#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Bare minimal uuid_generate implementation.
void uuid_generate(uint8_t out[16]);
#ifdef __cplusplus
}
#endif

#endif  // TOOLS_VBOOT_REFERENCE_UUID_UUID_H_
