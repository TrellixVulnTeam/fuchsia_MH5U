// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_LIB_NETWORK_REALM_SETUP_H_
#define SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_LIB_NETWORK_REALM_SETUP_H_

#include <lib/zx/status.h>

#include <fbl/unique_fd.h>

namespace netemul {

// Start the driver test realm and wait for the '/dev/sys/test/tapctl' path to
// be enumerated under '/dev'.
//
// If successful, returns a `fbl::unique_fd` handle to the '/dev' directory.
zx::status<fbl::unique_fd> StartDriverTestRealm();

}  // namespace netemul

#endif  // SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_LIB_NETWORK_REALM_SETUP_H_
