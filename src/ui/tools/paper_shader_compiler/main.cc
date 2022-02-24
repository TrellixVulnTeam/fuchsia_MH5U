// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/syslog/cpp/macros.h>

#include "src/ui/lib/escher/defaults/default_shader_program_factory.h"
#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/escher_process_init.h"
#include "src/ui/lib/escher/flatland/flatland_static_config.h"
#include "src/ui/lib/escher/forward_declarations.h"
#include "src/ui/lib/escher/fs/hack_filesystem.h"
#include "src/ui/lib/escher/hmd/pose_buffer_latching_shader.h"
#include "src/ui/lib/escher/paper/paper_renderer_config.h"
#include "src/ui/lib/escher/paper/paper_renderer_static_config.h"
#include "src/ui/lib/escher/shaders/util/spirv_file_util.h"
#include "src/ui/lib/escher/vk/shader_program.h"

#include "third_party/shaderc/libshaderc/include/shaderc/shaderc.hpp"

namespace escher {

// Compiles all of the provided shader modules and writes out their spirv
// to disk in the source tree.
bool CompileAndWriteShader(HackFilesystemPtr filesystem, ShaderProgramData program_data) {
  std::string abs_root = *filesystem->base_path() + "/shaders/spirv/";

  static auto compiler = std::make_unique<shaderc::Compiler>();

  // Loop over all the shader stages.
  for (const auto& iter : program_data.source_files) {
    // Skip if path is empty.
    if (iter.second.length() == 0) {
      continue;
    }

    FX_LOGS(INFO) << "Processing shader " << iter.second;

    auto shader = fxl::MakeRefCounted<ShaderModuleTemplate>(vk::Device(), compiler.get(),
                                                            iter.first, iter.second, filesystem);

    std::vector<uint32_t> spirv;
    if (!shader->CompileVariantToSpirv(program_data.args, &spirv)) {
      FX_LOGS(ERROR) << "could not compile shader " << iter.second;
      return false;
    }

    // As per above, only write out the spirv if there has been a change.
    if (shader_util::SpirvExistsOnDisk(program_data.args, abs_root, iter.second, spirv)) {
      if (!shader_util::WriteSpirvToDisk(spirv, program_data.args, abs_root, iter.second)) {
        FX_LOGS(ERROR) << "could not write shader " << iter.second << " to disk.";
        return false;
      }
    } else {
      FX_LOGS(INFO) << "Shader already exists on disk.";
    }
  }
  return true;
}

}  // namespace escher

int main(int argc, const char** argv) {
  // Register all the shader files, along with include files, that are used by Escher.
  auto filesystem = escher::HackFilesystem::New();

  // The binary for this is expected to be in ./out/default/host_x64.

  auto paths = escher::kPaperRendererShaderPaths;
  paths.insert(paths.end(), escher::kFlatlandShaderPaths.begin(),
               escher::kFlatlandShaderPaths.end());
  paths.insert(paths.end(), escher::hmd::kPoseBufferLatchingPaths.begin(),
               escher::hmd::kPoseBufferLatchingPaths.end());
  bool success = filesystem->InitializeWithRealFiles(paths, "./../../../../src/ui/lib/escher/");
  FX_CHECK(success);
  FX_CHECK(filesystem->base_path());

  // Ambient light program.
  if (!CompileAndWriteShader(filesystem, escher::kAmbientLightProgramData)) {
    return EXIT_FAILURE;
  }

  // No lighting program.
  if (!CompileAndWriteShader(filesystem, escher::kNoLightingProgramData)) {
    return EXIT_FAILURE;
  }

  if (!CompileAndWriteShader(filesystem, escher::kPointLightProgramData)) {
    return EXIT_FAILURE;
  }

  if (!CompileAndWriteShader(filesystem, escher::kShadowVolumeGeometryProgramData)) {
    return EXIT_FAILURE;
  }

  if (!CompileAndWriteShader(filesystem, escher::kShadowVolumeGeometryDebugProgramData)) {
    return EXIT_FAILURE;
  }

  if (!CompileAndWriteShader(filesystem, escher::hmd::kPoseBufferLatchingProgramData)) {
    return EXIT_FAILURE;
  }

  // Flatland shader.
  if (!CompileAndWriteShader(filesystem, escher::kFlatlandStandardProgram)) {
    return EXIT_FAILURE;
  }

  // Test shaders from escher/test/vk/shader_program_unittest.cc
  const escher::ShaderProgramData test_variant1 = {
      .source_files = {{escher::ShaderStage::kVertex, "shaders/model_renderer/main.vert"},
                       {escher::ShaderStage::kFragment, "shaders/test/main.frag"}},
      .args = escher::ShaderVariantArgs({{"USE_ATTRIBUTE_UV", "1"},
                                         {"USE_PAPER_SHADER_PUSH_CONSTANTS", "1"},
                                         {"NO_SHADOW_LIGHTING_PASS", "1"}}),
  };
  const escher::ShaderProgramData test_variant2 = {
      .source_files = {{escher::ShaderStage::kVertex, "shaders/model_renderer/main.vert"},
                       {escher::ShaderStage::kFragment, "shaders/test/main.frag"}},
      .args = escher::ShaderVariantArgs({{"USE_ATTRIBUTE_UV", "0"},
                                         {"USE_PAPER_SHADER_PUSH_CONSTANTS", "1"},
                                         {"NO_SHADOW_LIGHTING_PASS", "1"}}),
  };

  if (!CompileAndWriteShader(filesystem, test_variant1)) {
    return EXIT_FAILURE;
  }

  if (!CompileAndWriteShader(filesystem, test_variant2)) {
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
