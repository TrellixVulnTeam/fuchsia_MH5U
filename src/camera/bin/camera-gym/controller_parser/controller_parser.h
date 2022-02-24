// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_CAMERA_BIN_CAMERA_GYM_CONTROLLER_PARSER_CONTROLLER_PARSER_H_
#define SRC_CAMERA_BIN_CAMERA_GYM_CONTROLLER_PARSER_CONTROLLER_PARSER_H_

#include <fuchsia/camera/gym/cpp/fidl.h>
#include <lib/fpromise/result.h>

namespace camera {

// ControllerParser supplies a parser for the control commands.
class ControllerParser {
 public:
  ControllerParser() = default;

  fpromise::result<std::vector<fuchsia::camera::gym::Command>> ParseArgcArgv(int argc,
                                                                             const char** argv);

 private:
  ControllerParser(const ControllerParser&) = delete;
  ControllerParser& operator=(const ControllerParser&) = delete;

  fpromise::result<fuchsia::camera::gym::Command> ParseOneCommand(const std::string& name,
                                                                  const std::string& value);

  // Parsing individual commands:
  fpromise::result<fuchsia::camera::gym::SetConfigCommand> ParseSetConfigCommand(
      const std::string& name, const std::string& value, bool async = false);
  fpromise::result<fuchsia::camera::gym::AddStreamCommand> ParseAddStreamCommand(
      const std::string& name, const std::string& value, bool async = false);
  fpromise::result<fuchsia::camera::gym::SetCropCommand> ParseSetCropCommand(
      const std::string& name, const std::string& value, bool async = false);
  fpromise::result<fuchsia::camera::gym::SetResolutionCommand> ParseSetResolutionCommand(
      const std::string& name, const std::string& value, bool async = false);
  fpromise::result<fuchsia::camera::gym::SetDescriptionCommand> ParseSetDescriptionCommand(
      const std::string& name, const std::string& value);
  fpromise::result<fuchsia::camera::gym::CaptureFrameCommand> ParseCaptureFrameCommand(
      const std::string& name, const std::string& value);

  // Maximum number of parameters returned by ParseValues.
  static constexpr size_t MAX_VALUES = 5;
  using ValuesArray = struct {
    uint32_t i[MAX_VALUES];
    float f[MAX_VALUES];
  };

  // Parse individual numeric parameters according to types[] array passed in.
  // types[] array may be up to MAX_VALUES types + 1 terminating '\0'.
  fpromise::result<camera::ControllerParser::ValuesArray> ParseValues(const std::string& args,
                                                                      const char* types);
};

}  // namespace camera

#endif  // SRC_CAMERA_BIN_CAMERA_GYM_CONTROLLER_PARSER_CONTROLLER_PARSER_H_
