// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This application is intenteded to be used for manual testing of
// the Cobalt logger client on Fuchsia by Cobalt engineers.
//
// It also serves as an example of how to use the Cobalt FIDL API.
//
// It is also invoked by the cobalt_client CQ and CI.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/macros.h>

#include <memory>
#include <sstream>
#include <string>

#include "lib/fidl/cpp/binding.h"
#include "lib/fidl/cpp/synchronous_interface_ptr.h"
#include "src/cobalt/bin/testapp/cobalt_testapp.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"
#include "src/lib/fxl/macros.h"

// Command-line flags

// Don't use the network. Default=false (i.e. do use the network.)
constexpr std::string_view kNoNetworkForTesting = "no_network_for_testing";

// Use the prober project instead of the testapp project. Default=false (i.e.,
// use the testapp project).
constexpr std::string_view kTestForProber = "test_for_prober";

// If --test_for_prober was also passed, run the testapp in prober mode instead
// of printing a warning and exiting.
constexpr std::string_view kOverrideProberWarning = "override_prober_warning";

int main(int argc, const char** argv) {
  const auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  fxl::SetLogSettingsFromCommandLine(command_line, {"cobalt", "testapp"});
  bool use_network = !command_line.HasOption(kNoNetworkForTesting);

  FX_LOGS(INFO) << "The Cobalt testapp is starting.";

  bool test_for_prober = command_line.HasOption(kTestForProber);
  if (test_for_prober && !command_line.HasOption(kOverrideProberWarning)) {
    FX_LOGS(ERROR) << "Running the testapp in prober mode outside of CI will "
                      "corrupt prober test output. If you need to do this, "
                      "pass the flag --override_prober_warning.";
    return 1;
  }

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  cobalt::testapp::CobaltTestApp app(use_network, test_for_prober);

  if (!app.RunTests()) {
    FX_LOGS(ERROR) << "The Cobalt testapp exited with status: FAIL";
    return 1;
  }
  FX_LOGS(INFO) << "The Cobalt testapp exited with status: PASS";
  return 0;
}
