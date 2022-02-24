// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_HWSTRESS_LIGHT_STRESS_H_
#define GARNET_BIN_HWSTRESS_LIGHT_STRESS_H_

#include <fuchsia/hardware/light/cpp/fidl.h>
#include <lib/zx/status.h>
#include <lib/zx/time.h>

#include <string>

#include "args.h"
#include "status.h"

namespace hwstress {

// Start a stress on a device light / LED.
//
// Return true on success.
bool StressLight(StatusLine* status, const CommandLineArgs& args, zx::duration duration);

//
// Functions below exposed for testing.
//

// Details about a single LED / light.
struct LightInfo {
  std::string name;  // Name of the light
  uint32_t index;    // Index of the light
};
bool operator==(const LightInfo&, const LightInfo&);
bool operator!=(const LightInfo&, const LightInfo&);

// Query all lights on the given interface.
zx::status<std::vector<LightInfo>> GetLights(const fuchsia::hardware::light::LightSyncPtr& light);

// Turn on or off the light at the given index.
zx::status<> TurnOnLight(const fuchsia::hardware::light::LightSyncPtr& light, uint32_t light_num);
zx::status<> TurnOffLight(const fuchsia::hardware::light::LightSyncPtr& light, uint32_t light_num);

}  // namespace hwstress

#endif  // GARNET_BIN_HWSTRESS_LIGHT_STRESS_H_
