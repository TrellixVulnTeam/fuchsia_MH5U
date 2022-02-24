// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef TEST_PLATFORM_DEVICE_H
#define TEST_PLATFORM_DEVICE_H

#include <memory>

#include "platform_device.h"
#include "platform_pci_device.h"

class TestPlatformPciDevice {
 public:
  static magma::PlatformPciDevice* GetInstance() { return g_instance; }

  static void SetInstance(magma::PlatformPciDevice* platform_device) {
    g_instance = platform_device;
  }

  static magma::PlatformPciDevice* g_instance;
  static void* core_device_;
};

class TestPlatformDevice {
 public:
  static magma::PlatformDevice* GetInstance() { return g_instance.get(); }

  static void SetInstance(std::unique_ptr<magma::PlatformDevice> platform_device) {
    g_instance = std::move(platform_device);
  }

  static std::unique_ptr<magma::PlatformDevice> g_instance;
};

// Sets the driver handle that tests pass in to msd_create_device.
void SetTestDeviceHandle(void* driver_device);

// Gets the handle set in SetTestDeviceHandle
void* GetTestDeviceHandle();

#endif  // TEST_PLATFORM_DEVICE_H
