// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_LIB_ESCHER_VK_SHADER_PROGRAM_FACTORY_H_
#define SRC_UI_LIB_ESCHER_VK_SHADER_PROGRAM_FACTORY_H_

#include "src/ui/lib/escher/base/trait.h"
#include "src/ui/lib/escher/forward_declarations.h"
#include "src/ui/lib/escher/util/enum_count.h"
#include "src/ui/lib/escher/vk/shader_stage.h"
#include "src/ui/lib/escher/vk/shader_variant_args.h"

namespace escher {

// ShaderProgramFactory is a |Trait| that clients use to obtain ShaderPrograms.
// Subclasses must override GetProgram(), and will typically lazily-generate and
// cache these programs.
class ShaderProgramFactory : public Trait {
 public:
  // Return a compute program whose code is specified by |compute_shader_path|.
  ShaderProgramPtr GetComputeProgram(std::string compute_shader_path, ShaderVariantArgs args = {});

  // Return a graphics program which has only vertex and fragment shader stages;
  // The fragment shader path may be empty: this is used for depth-only passes.
  ShaderProgramPtr GetGraphicsProgram(std::string vertex_shader_path,
                                      std::string fragment_shader_path,
                                      ShaderVariantArgs args = {});

  // Return a graphics program containing all shader stages that a non-empty
  // path is provided for.
  ShaderProgramPtr GetGraphicsProgram(std::string vertex_shader_path,
                                      std::string tessellation_control_shader_path,
                                      std::string tessellation_evaluation_shader_path,
                                      std::string geometry_shader_path,
                                      std::string fragment_shader_path, ShaderVariantArgs args);

  // Convenience helper function that calls the pure virtual function |GetProgram|. Can be used
  // for both compute and graphics shaders.
  ShaderProgramPtr GetProgram(ShaderProgramData program_data);

 protected:
  virtual ~ShaderProgramFactory();

  // Subclasses must implement this.  The array index of each path corresponds
  // to a value in the ShaderStage enum; each non-empty path provides the source
  // code for the corresponding shader stage.
  virtual ShaderProgramPtr GetProgramImpl(const std::string shader_paths[EnumCount<ShaderStage>()],
                                          ShaderVariantArgs args) = 0;
};

}  // namespace escher

#endif  // SRC_UI_LIB_ESCHER_VK_SHADER_PROGRAM_FACTORY_H_
