// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/vk/shader_program_factory.h"

#include "src/ui/lib/escher/vk/shader_program.h"

namespace escher {

ShaderProgramFactory::~ShaderProgramFactory() = default;

ShaderProgramPtr ShaderProgramFactory::GetComputeProgram(std::string compute_shader_path,
                                                         ShaderVariantArgs args) {
  // Array indices match the corresponding values of enum ShaderStage.  In this
  // case, only the path to the compute shader code is provided; all others are
  // empty.
  FX_DCHECK(!compute_shader_path.empty());
  std::string paths[EnumCount<ShaderStage>()] = {"", "", "",
                                                 "", "", std::move(compute_shader_path)};
  return GetProgramImpl(paths, std::move(args));
}

ShaderProgramPtr ShaderProgramFactory::GetGraphicsProgram(std::string vertex_shader_path,
                                                          std::string fragment_shader_path,
                                                          ShaderVariantArgs args) {
  // Array indices match the corresponding values of enum ShaderStage.  In this
  // case, only the paths to the vertex and fragment shader code are provided;
  // all others are empty.
  FX_DCHECK(!vertex_shader_path.empty());
  std::string paths[EnumCount<ShaderStage>()] = {std::move(vertex_shader_path),   "", "", "",
                                                 std::move(fragment_shader_path), ""};
  return GetProgramImpl(paths, std::move(args));
}

ShaderProgramPtr ShaderProgramFactory::GetProgram(ShaderProgramData program_data) {
  std::string paths[EnumCount<ShaderStage>()];
  for (const auto& iter : program_data.source_files) {
    paths[static_cast<uint8_t>(iter.first)] = std::move(iter.second);
  }

  return GetProgramImpl(paths, program_data.args);
}

ShaderProgramPtr ShaderProgramFactory::GetGraphicsProgram(
    std::string vertex_shader_path, std::string tessellation_control_shader_path,
    std::string tessellation_evaluation_shader_path, std::string geometry_shader_path,
    std::string fragment_shader_path, ShaderVariantArgs args) {
  // Array indices match the corresponding values of enum ShaderStage.  In this
  // case, paths are provided for all shader stages except the compute shader.
  FX_DCHECK(!vertex_shader_path.empty());
  static const std::string kEmpty("");
  std::string paths[EnumCount<ShaderStage>()] = {std::move(vertex_shader_path),
                                                 std::move(tessellation_control_shader_path),
                                                 std::move(tessellation_evaluation_shader_path),
                                                 std::move(geometry_shader_path),
                                                 std::move(fragment_shader_path),
                                                 kEmpty};
  return GetProgramImpl(paths, std::move(args));
}

}  // namespace escher
