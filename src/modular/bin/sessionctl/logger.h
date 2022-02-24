// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_BIN_SESSIONCTL_LOGGER_H_
#define SRC_MODULAR_BIN_SESSIONCTL_LOGGER_H_

#include <fuchsia/modular/cpp/fidl.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/async/cpp/task.h>

#include <iostream>
#include <string>

#include <rapidjson/document.h>

#include "src/lib/fxl/command_line.h"
#include "src/lib/fxl/strings/string_printf.h"
#include "src/modular/lib/async/cpp/future.h"

namespace modular {

class Logger {
 public:
  explicit Logger(bool json_out);

  void LogError(const std::string& command, const std::string& error) const;

  void Log(const std::string& command, const std::vector<std::string>& params) const;

  void Log(const std::string& command, const std::map<std::string, std::string>& params) const;

 private:
  // Returns a JSON formatted string of the executed |command| with respective
  // |params| to be logged.
  std::string GenerateJsonLogString(const std::string& command,
                                    const std::vector<std::string>& params) const;

  std::string GenerateJsonLogString(const std::string& command,
                                    const std::map<std::string, std::string>& params) const;

  rapidjson::Document GetDocument(const std::string& command) const;

  // Returns a string of the executed |command| with respective |params| to be
  // logged.
  std::string GenerateLogString(const std::string& command,
                                const std::map<std::string, std::string>& params) const;
  bool json_out_;
};

}  // namespace modular

#endif  // SRC_MODULAR_BIN_SESSIONCTL_LOGGER_H_
