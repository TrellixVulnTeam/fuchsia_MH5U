// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/sys/appmgr/integration_tests/sandbox/namespace_test.h"

#include <errno.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <fuchsia/sys/cpp/fidl.h>
#include <lib/fdio/directory.h>
#include <string.h>
#include <unistd.h>
#include <zircon/errors.h>

#include <string>

#include <fbl/unique_fd.h>

namespace {

namespace fio = fuchsia_io;

// Note that these are the rights that the ExpectPathSupportsRights call itself supports generally.
// The rights that are checked for a specific path are provided by the caller and must be <= these.
constexpr uint32_t kKnownFsRights =
    fio::wire::kOpenRightReadable | fio::wire::kOpenRightWritable | fio::wire::kOpenRightExecutable;

std::string rights_str(uint32_t rights) {
  std::string str;
  if (rights & fio::wire::kOpenRightReadable) {
    str.push_back('r');
  }
  if (rights & fio::wire::kOpenRightWritable) {
    str.push_back('w');
  }
  if (rights & fio::wire::kOpenRightExecutable) {
    str.push_back('x');
  }
  return str;
}

}  // namespace

bool NamespaceTest::Exists(const char* path) {
  struct stat stat_;
  return stat(path, &stat_) == 0;
}

void NamespaceTest::ExpectExists(const char* path) {
  EXPECT_TRUE(Exists(path)) << "Can't find " << path << ": " << strerror(errno);
}

void NamespaceTest::ExpectDoesNotExist(const char* path) {
  EXPECT_FALSE(Exists(path)) << "Unexpectedly found " << path;
}

void NamespaceTest::ExpectPathSupportsRights(const char* path, uint32_t rights) {
  ASSERT_FALSE(rights & ~kKnownFsRights) << "Unsupported rights in ExpectPathSupportsRights call";

  fbl::unique_fd fd;
  EXPECT_EQ(ZX_OK, fdio_open_fd(path, rights, fd.reset_and_get_address()))
      << "Failed to open " << path << " with rights: " << rights_str(rights);
  EXPECT_GE(fd.get(), 0);
}

void NamespaceTest::ExpectPathSupportsStrictRights(const char* path, uint32_t rights,
                                                   bool require_access_denied) {
  ExpectPathSupportsRights(path, rights);

  // Check that the path can't be opened with rights other than the ones passed in 'rights'.
  for (uint32_t r = 1; r < kKnownFsRights; r = r << 1) {
    uint32_t rights_bit = kKnownFsRights & r;
    if (!rights_bit || (rights & rights_bit)) {
      continue;
    }

    fbl::unique_fd fd;
    zx_status_t status = fdio_open_fd(path, rights_bit, fd.reset_and_get_address());
    EXPECT_NE(ZX_OK, status) << "Opening " << path << " with '" << rights_str(rights_bit)
                             << "' right unexpectedly succeeded";
    if (status != ZX_OK && require_access_denied) {
      EXPECT_EQ(ZX_ERR_ACCESS_DENIED, status)
          << "Opening " << path << " with '" << rights_str(rights_bit)
          << "' right failed with wrong/unexpected status";
    }
  }
}

TEST_F(NamespaceTest, SanityCheck) {
  ExpectExists("/svc/");
  ExpectDoesNotExist("/this_should_not_exist");
}
