// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/macros.h>

#include <iostream>

#include "runner.h"
#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/log_settings_command_line.h"

using namespace netemul;

int main(int argc, const char** argv) {
  auto command_line = fxl::CommandLineFromArgcArgv(argc, argv);
  if (!fxl::SetLogSettingsFromCommandLine(command_line))
    return 1;

  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);

  FX_LOGS(INFO) << "Starting netemul runner";

  Runner r(loop.dispatcher());
  loop.Run();

  return 0;
}
