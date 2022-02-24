// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LOGGER_OPTIONS_H_
#define SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LOGGER_OPTIONS_H_

#include "logger_filter_options.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/json_parser/json_parser.h"

namespace netemul {
namespace config {

class LoggerOptions {
 public:
  LoggerOptions();
  LoggerOptions(LoggerOptions&& other) = default;

  bool ParseFromJSON(const rapidjson::Value& value, json::JSONParser* parser);
  void SetDefaults();

  void set_enabled(bool enabled);
  bool enabled() const;
  bool klogs_enabled() const;
  const LoggerFilterOptions& filters() const;

 private:
  bool enabled_;
  bool klogs_enabled_;
  LoggerFilterOptions filters_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LoggerOptions);
};

}  // namespace config
}  // namespace netemul

#endif  // SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LOGGER_OPTIONS_H_
