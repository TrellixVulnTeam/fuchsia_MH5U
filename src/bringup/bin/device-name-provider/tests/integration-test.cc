// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// These tests should run without any network interface (except loopback).

#include <fidl/fuchsia.device/cpp/wire.h>
#include <lib/service/llcpp/service.h>
#include <sys/utsname.h>

#include <gtest/gtest.h>

#include "src/lib/testing/predicates/status.h"

namespace {

TEST(NameProviderTest, GetHostNameDefault) {
  char hostname[HOST_NAME_MAX];
  ASSERT_EQ(gethostname(hostname, sizeof(hostname)), 0) << strerror(errno);
  ASSERT_STREQ(hostname, fuchsia_device::wire::kDefaultDeviceName);
}

TEST(NameProviderTest, UnameDefault) {
  utsname uts;
  ASSERT_EQ(uname(&uts), 0) << strerror(errno);
  ASSERT_STREQ(uts.nodename, fuchsia_device::wire::kDefaultDeviceName);
}

TEST(NameProviderTest, GetDeviceName) {
  zx::status client_end = service::Connect<fuchsia_device::NameProvider>();
  ASSERT_OK(client_end.status_value());

  fidl::WireResult response = fidl::WireCall(client_end.value())->GetDeviceName();
  ASSERT_OK(response.status());
  fuchsia_device::wire::NameProviderGetDeviceNameResult& result = response.value().result;
  switch (result.Which()) {
    case fuchsia_device::wire::NameProviderGetDeviceNameResult::Tag::kErr:
      FAIL() << zx_status_get_string(result.err());
    case fuchsia_device::wire::NameProviderGetDeviceNameResult::Tag::kResponse: {
      const fidl::StringView& name = result.response().name;
      // regression test: ensure that no additional data is present past the last null byte
      EXPECT_EQ(name.size(), strlen(fuchsia_device::wire::kDefaultDeviceName));
      EXPECT_EQ(memcmp(name.data(), fuchsia_device::wire::kDefaultDeviceName, name.size()), 0);
    }
  }
}

}  // namespace
