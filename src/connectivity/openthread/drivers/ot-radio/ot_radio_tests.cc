// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/driver-unit-test/utils.h>
#include <lib/sync/completion.h>

#include <zxtest/zxtest.h>

#include "ot_radio.h"
#include "ot_radio_bootloader.h"

namespace {

#ifdef INTERNAL_ACCESS

TEST(OtRadioTestCase, BootloaderGetVersionTest) {
  std::unique_ptr<ot::OtRadioDevice> dev;
  // Init device
  ASSERT_OK(ot::OtRadioDevice::Create(nullptr, driver_unit_test::GetParent(), &dev));
  // Bind port to interrupt
  ASSERT_OK(dev->CreateAndBindPortToIntr());
  // Create a bootloader device object:
  ot::OtRadioDeviceBootloader dev_bl(dev.get());
  // Put in Bootloader mode:
  ASSERT_OK(dev_bl.PutRcpInBootloader());
  // Get version in a string:
  std::string bl_version;
  ot::OtRadioBlResult result = dev_bl.GetBootloaderVersion(bl_version);
  // Exit the bootloader mode by resetting the device:
  ASSERT_OK(dev->Reset());

  // Ensure that command succeeded
  ASSERT_EQ(result, ot::BL_RET_SUCCESS);
  // Ensure that version contains string 'Bootloader'
  ASSERT_NE(bl_version.find("Bootloader"), std::string::npos);
  // Teardown
  ASSERT_OK(dev->ShutDown());
}

// Note: This test flashes the rcp with whatever firmware is bundled with the
// build. In most cases, it is same firmware as what is already on the radio
// chip. But if the firmware is changed in the build, new firmware is flashed
// on the radio chip. This test should be run before SpinelFramerTest and
// InitTest so that they use the updated firmware.
TEST(OtRadioTestCase, BootloaderUpdateFirmwareTest) {
  std::unique_ptr<ot::OtRadioDevice> dev;
  // Init device
  ASSERT_OK(ot::OtRadioDevice::Create(nullptr, driver_unit_test::GetParent(), &dev));
  // Bind port to interrupt
  ASSERT_OK(dev->CreateAndBindPortToIntr());
  // Create a bootloader device object:
  ot::OtRadioDeviceBootloader dev_bl(dev.get());
  // Update firmware
  ot::OtRadioBlResult result = dev_bl.UpdateRadioFirmware();
  ASSERT_EQ(result, ot::BL_RET_SUCCESS);
  // Teardown
  ASSERT_OK(dev->ShutDown());
}

#endif  // INTERNAL_ACCESS

TEST(OtRadioTestCase, InitTest) {
  std::unique_ptr<ot::OtRadioDevice> dev;
  // Init device
  ASSERT_OK(ot::OtRadioDevice::Create(nullptr, driver_unit_test::GetParent(), &dev));
  // Bind port to interrupt
  ASSERT_OK(dev->CreateAndBindPortToIntr());
  // Starts the radio thread
  dev->StartRadioThread();
  // Trigger a reset so the radio send us something
  ASSERT_OK(dev->DriverUnitTestGetResetEvent());
  // Wait for interrupt to fire and for SPI transaction to complete
  ASSERT_OK(sync_completion_wait(&dev->spi_rx_complete_, ZX_SEC(30)));
  // Verify that a valid byte was sent by the radio
  ASSERT_NE(dev->spi_rx_buffer_[0], 0);
  // Teardown
  ASSERT_OK(dev->ShutDown());
}

TEST(OtRadioTestCase, SpinelFramerTest) {
  std::unique_ptr<ot::OtRadioDevice> dev;
  // Init device
  ASSERT_OK(ot::OtRadioDevice::Create(nullptr, driver_unit_test::GetParent(), &dev));
  // Bind port to interrupt
  ASSERT_OK(dev->CreateAndBindPortToIntr());
  // Starts the thread
  dev->StartRadioThread();
  // Trigger a reset so the radio send us something to ensure a clean state
  ASSERT_OK(dev->DriverUnitTestGetResetEvent());
  // Wait for interrupt to fire and for SPI transaction to complete
  ASSERT_OK(sync_completion_wait(&dev->spi_rx_complete_, ZX_SEC(30)));
  // Reset the completion signal
  sync_completion_reset(&dev->spi_rx_complete_);
  // Send get version command
  ASSERT_OK(dev->DriverUnitTestGetNCPVersion());

  const int kMaxAttempts = 5;
  int attempt;

  // Keep polling responses until we get response for GetNCPVersion
  for (attempt = 0; attempt < kMaxAttempts; attempt++) {
    // Wait for interrupt to fire and for SPI transaction to complete
    ASSERT_OK(sync_completion_wait(&dev->spi_rx_complete_, ZX_SEC(30)));

    // Reset the completion signal
    sync_completion_reset(&dev->spi_rx_complete_);

    // Update inbound allowance
    dev->SetMaxInboundAllowance();

    // Break if we find matching TID, so we have received a response
    if ((dev->spi_rx_buffer_[0] & 0xf) == ot::kGetNcpVersionTID) {
      break;
    }
  }

  // Ensure that we didn't use all attempts without getting expected response:
  ASSERT_NE(attempt, kMaxAttempts);

  // Verify that a valid version response containing the string 'OPENTHREAD' is received
  uint8_t version_openthread_ascii[] = {0x4f, 0x50, 0x45, 0x4E, 0x54, 0x48, 0x52, 0x45, 0x41, 0x44};
  ASSERT_EQ(
      memcmp(version_openthread_ascii, &dev->spi_rx_buffer_[3], sizeof(version_openthread_ascii)),
      0);

  // Teardown
  ASSERT_OK(dev->ShutDown());
}
}  // namespace
