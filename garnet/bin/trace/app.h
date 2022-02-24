// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_TRACE_APP_H_
#define GARNET_BIN_TRACE_APP_H_

#include <map>
#include <memory>
#include <string>

#include "garnet/bin/trace/command.h"

namespace tracing {

class App : public Command {
 public:
  App(sys::ComponentContext* context);
  ~App();

 protected:
  void Start(const fxl::CommandLine& command_line) override;

 private:
  void RegisterCommand(Command::Info info);
  void PrintHelp();

  std::map<std::string, Command::Info> known_commands_;
  std::unique_ptr<Command> command_;

  App(const App&) = delete;
  App(App&&) = delete;
  App& operator=(const App&) = delete;
  App& operator=(App&&) = delete;
};

}  // namespace tracing

#endif  // GARNET_BIN_TRACE_APP_H_
