// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <chrono>
#include <cstdlib>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "src/virtualization/tests/guest_test.h"

using testing::HasSubstr;

static constexpr size_t kVirtioBalloonPageCount = 256;
static constexpr size_t kVirtioConsoleMessageCount = 100;
static constexpr char kVirtioRngUtil[] = "virtio_rng_test_util";

template <class T>
using CoreGuestTest = GuestTest<T>;

// This test suite contains all guest tests that don't require a specific configuration of devices.
// They are grouped together so that they share guests and reduce the number of times guests are
// started, which is time consuming. Note that this means that some tests need to dynamically check
// the guest type in order to skip under certain conditions.
TYPED_TEST_SUITE(CoreGuestTest, AllGuestTypes);

TYPED_TEST(CoreGuestTest, VirtioBalloon) {
  // Zircon does not yet have a virtio balloon driver.
  if (this->GetGuestKernel() == GuestKernel::ZIRCON) {
    return;
  }

  std::string result;
  EXPECT_EQ(this->Execute({"echo", "test"}, &result), ZX_OK);
  EXPECT_EQ(result, "test\n");

  fuchsia::virtualization::BalloonControllerSyncPtr balloon_controller;
  this->ConnectToBalloon(balloon_controller.NewRequest());

  uint32_t initial_num_pages;
  zx_status_t status = balloon_controller->GetNumPages(&initial_num_pages);
  ASSERT_EQ(status, ZX_OK);

  // Request an increase to the number of pages in the balloon.
  status = balloon_controller->RequestNumPages(initial_num_pages + kVirtioBalloonPageCount);
  ASSERT_EQ(status, ZX_OK);

  // Verify that the number of pages eventually equals the requested number. The
  // guest may not respond to the request immediately so we call GetNumPages in
  // a loop.
  uint32_t num_pages;
  while (true) {
    status = balloon_controller->GetNumPages(&num_pages);
    ASSERT_EQ(status, ZX_OK);
    if (num_pages == initial_num_pages + kVirtioBalloonPageCount) {
      break;
    }
  }

  // Request a decrease to the number of pages in the balloon back to the
  // initial value.
  status = balloon_controller->RequestNumPages(initial_num_pages);
  ASSERT_EQ(status, ZX_OK);

  while (true) {
    status = balloon_controller->GetNumPages(&num_pages);
    ASSERT_EQ(status, ZX_OK);
    if (num_pages == initial_num_pages) {
      break;
    }
  }
}

TYPED_TEST(CoreGuestTest, VirtioConsole) {
  // Test many small packets.
  std::string result;
  for (size_t i = 0; i != kVirtioConsoleMessageCount; ++i) {
    EXPECT_EQ(this->Execute({"echo", "test"}, &result), ZX_OK);
    EXPECT_EQ(result, "test\n");
  }

  // Test large packets. Note that we must keep the total length below 4096,
  // which is the maximum line length for dash.
  std::string test_data = "";
  for (size_t i = 0; i != kVirtioConsoleMessageCount; ++i) {
    test_data.append("Lorem ipsum dolor sit amet consectetur");
  }
  EXPECT_EQ(this->Execute({"echo", test_data.c_str()}, &result), ZX_OK);
  test_data.append("\n");
  EXPECT_EQ(result, test_data);
}

TYPED_TEST(CoreGuestTest, VirtioRng) {
  std::string result;
  ASSERT_EQ(this->RunUtil(kVirtioRngUtil, {}, &result), ZX_OK);
  EXPECT_THAT(result, HasSubstr("PASS"));
}

TYPED_TEST(CoreGuestTest, RealTimeClock) {
  // Real time clock not functioning in Zircon guest at this time.
  //
  // TODO(fxbug.dev/75440): Fix clock in Zircon guest.
  if (this->GetGuestKernel() == GuestKernel::ZIRCON) {
    return;
  }

  // Print seconds since Unix epoch (1970-01-01), and parse the result.
  std::string result;
  ASSERT_EQ(this->Execute({"/bin/date", "+%s"}, {}, &result), ZX_OK);
  int64_t guest_timestamp = std::stol(result, /*pos=*/nullptr, /*base=*/10);
  ASSERT_TRUE(guest_timestamp > 0) << "Could not parse guest time.";

  // Get the system time.
  std::chrono::time_point now = std::chrono::system_clock::now();
  int64_t host_timestamp =
      std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch()).count();

  // Ensure the clock matches the system time, within a few minutes.
  std::cout << "Guest time is " << (host_timestamp - guest_timestamp)
            << " second(s) behind host time.\n";
  EXPECT_LT(std::abs(host_timestamp - guest_timestamp), std::chrono::minutes(5).count())
      << "Guest time (" << guest_timestamp << ") and host time (" << host_timestamp
      << ") differ by more than 5 minutes.";
}
