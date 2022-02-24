// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_DRIVERS_CODECS_MAX98927_MAX98927_H_
#define SRC_MEDIA_AUDIO_DRIVERS_CODECS_MAX98927_MAX98927_H_

#include <fuchsia/hardware/i2c/c/banjo.h>
#include <zircon/types.h>

#include <ddktl/device.h>
#include <ddktl/protocol/empty-protocol.h>

namespace audio {
namespace max98927 {

class Max98927Device;
using DeviceType = ddk::Device<Max98927Device>;

class Max98927Device : public DeviceType {
 public:
  static zx_status_t Create(void* ctx, zx_device_t* parent);

  Max98927Device(zx_device_t* parent) : DeviceType(parent) {}
  ~Max98927Device() {}

  zx_status_t Bind();
  zx_status_t Initialize();

  // Methods required by the ddk mixins
  void DdkRelease();

 private:
  zx_status_t FidlSetEnabled(bool enable);

  // Play a test tone
  void Test();

  // Enable the device
  void Enable();

  // Disable the device
  void Disable();

  // Methods to read/write registers
  uint8_t ReadReg(uint16_t addr);
  void WriteReg(uint16_t addr, uint8_t val);

  // Debug
  void DumpRegs();

  i2c_protocol_t i2c_;
};

}  // namespace max98927
}  // namespace audio

#endif  // SRC_MEDIA_AUDIO_DRIVERS_CODECS_MAX98927_MAX98927_H_
