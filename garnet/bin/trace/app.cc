// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/trace/app.h"

#include <lib/syslog/cpp/macros.h>

#include "garnet/bin/trace/commands/list_categories.h"
#include "garnet/bin/trace/commands/record.h"
#include "garnet/bin/trace/commands/time.h"

namespace tracing {

App::App(sys::ComponentContext* context) : Command(context) {
  RegisterCommand(ListCategoriesCommand::Describe());
  RegisterCommand(RecordCommand::Describe());
  RegisterCommand(TimeCommand::Describe());
}

App::~App() {}

void App::Start(const fxl::CommandLine& command_line) {
  if (command_line.HasOption("help")) {
    PrintHelp();
    Done(EXIT_SUCCESS);
    return;
  }

  const auto& positional_args = command_line.positional_args();

  if (positional_args.empty()) {
    FX_LOGS(ERROR) << "Command missing - aborting";
    PrintHelp();
    Done(EXIT_FAILURE);
    return;
  }

  auto it = known_commands_.find(positional_args.front());
  if (it == known_commands_.end()) {
    FX_LOGS(ERROR) << "Unknown command '" << positional_args.front() << "' - aborting";
    PrintHelp();
    Done(EXIT_FAILURE);
    return;
  }

  command_ = it->second.factory(context());
  command_->Run(fxl::CommandLineFromIteratorsWithArgv0(
                    positional_args.front(), positional_args.begin() + 1, positional_args.end()),
                [this](int32_t return_code) { Done(return_code); });
}

void App::RegisterCommand(Command::Info info) { known_commands_[info.name] = std::move(info); }

void App::PrintHelp() {
  out() << "trace [options] command [command-specific options]" << std::endl;
  out() << "  --help: Produce this help message" << std::endl << std::endl;
  for (const auto& pair : known_commands_) {
    out() << "  " << pair.second.name << " - " << pair.second.usage << std::endl;
    for (const auto& option : pair.second.options)
      out() << "    --" << option.first << ": " << option.second << std::endl;
  }
}

}  // namespace tracing
