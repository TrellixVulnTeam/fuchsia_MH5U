// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LAUNCH_SERVICE_H_
#define SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LAUNCH_SERVICE_H_

#include "launch_app.h"
#include "src/lib/fxl/macros.h"
#include "src/lib/json_parser/json_parser.h"

namespace netemul {
namespace config {

class LaunchService {
 public:
  explicit LaunchService(std::string name);
  LaunchService(LaunchService&& other) = default;
  LaunchService(std::string name, std::string url, std::vector<std::string> arguments)
      : name_(std::move(name)), launch_(std::move(url), std::move(arguments)) {}

  bool ParseFromJSON(const rapidjson::Value& value, json::JSONParser* parser);

  const std::string& name() const;
  const LaunchApp& launch() const;

 private:
  std::string name_;
  LaunchApp launch_;

  FXL_DISALLOW_COPY_AND_ASSIGN(LaunchService);
};

}  // namespace config
}  // namespace netemul

#endif  // SRC_CONNECTIVITY_NETWORK_TESTING_NETEMUL_RUNNER_MODEL_LAUNCH_SERVICE_H_
