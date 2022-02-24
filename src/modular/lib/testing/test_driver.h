// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_LIB_TESTING_TEST_DRIVER_H_
#define SRC_MODULAR_LIB_TESTING_TEST_DRIVER_H_

// Header for definitions for Link communication with a test driver module.
namespace modular_testing {

// This URL is used in a test driver module as the path in a link. Its value is
// used to determine the URL for a sub-module to launch for integration testing.
constexpr char kModuleUnderTestPath[] = "module_under_test";

// This URL is used in a test driver module as the path in a link.  Its value is
// used to determine the URL for a test driver script that will be used for
// integration testing against the mod run in |kModuleUnderTestPath|
constexpr char kTestDriverPath[] = "test_driver_url";

constexpr char kTestDriverLinkName[] = "test_fuchsia_driver";

}  // namespace modular_testing

#endif  // SRC_MODULAR_LIB_TESTING_TEST_DRIVER_H_
