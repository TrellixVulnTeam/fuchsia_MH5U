// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_EXAMPLES_LAB_SCENIC_DEV_APP_APP_H_
#define SRC_UI_EXAMPLES_LAB_SCENIC_DEV_APP_APP_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/sys/cpp/component_context.h>

#include "lib/ui/scenic/cpp/resources.h"
#include "lib/ui/scenic/cpp/session.h"
#include "src/lib/fxl/command_line.h"

namespace scenic_dev_app {

class App {
 public:
  App(async::Loop* loop, const fxl::CommandLine& command_line);

 private:
  // Called asynchronously by constructor.
  void Init(fuchsia::ui::gfx::DisplayInfo display_info);

  // Updates and presents the scene.  Called first by Init().  Each invocation
  // schedules another call to Update() when the result of the previous
  // presentation is asynchronously received.
  void Update(uint64_t next_presentation_time);

  void CreateExampleScene(float display_width, float display_height);

  void ReleaseSessionResources();

  void InitCheckerboardMaterial(scenic::Material* uninitialized_material);

  std::unique_ptr<sys::ComponentContext> component_context_;
  async::Loop* const loop_;
  fuchsia::ui::scenic::ScenicPtr scenic_;

  std::unique_ptr<scenic::Session> session_;
  std::unique_ptr<scenic::DisplayCompositor> compositor_;
  std::unique_ptr<scenic::Camera> camera_;

  std::unique_ptr<scenic::ShapeNode> rrect_node_;
  std::unique_ptr<scenic::ShapeNode> clipper_1_;
  std::unique_ptr<scenic::ShapeNode> clipper_2_;
  std::unique_ptr<scenic::EntityNode> pane_2_contents_;

  // Time of the first update.  Animation of the "pane" content is based on the
  // time elapsed since this time.
  uint64_t start_time_ = 0;
  // The camera alternates between moving toward and away from the stage.  This
  // time is the timestamp that the last change of direction occurred.
  uint64_t camera_anim_start_time_;
  bool camera_anim_returning_ = false;

  fuchsia::ui::gfx::ShadowTechnique shadow_technique_ =
      fuchsia::ui::gfx::ShadowTechnique::UNSHADOWED;
};

}  // namespace scenic_dev_app

#endif  // SRC_UI_EXAMPLES_LAB_SCENIC_DEV_APP_APP_H_
