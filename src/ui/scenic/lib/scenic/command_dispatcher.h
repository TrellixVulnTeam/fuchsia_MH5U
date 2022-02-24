// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_SCENIC_COMMAND_DISPATCHER_H_
#define SRC_UI_SCENIC_LIB_SCENIC_COMMAND_DISPATCHER_H_

#include <fuchsia/ui/scenic/cpp/fidl.h>

#include <functional>

#include "src/lib/fxl/macros.h"
#include "src/ui/scenic/lib/scheduling/id.h"

namespace scenic_impl {

class CommandDispatcher {
 public:
  CommandDispatcher() = default;
  virtual ~CommandDispatcher() = default;

  virtual void SetDebugName(const std::string& debug_name) = 0;
  virtual void DispatchCommand(fuchsia::ui::scenic::Command command, scheduling::PresentId) = 0;

  // Only implemented by gfx::Session. |on_view_created| should be called with the ViewRef koid
  // when the View is created.
  // Does nothing by default.
  virtual void SetOnViewCreated(fit::function<void(zx_koid_t)> on_view_created) {}

 private:
  FXL_DISALLOW_COPY_AND_ASSIGN(CommandDispatcher);
};

using CommandDispatcherUniquePtr =
    std::unique_ptr<CommandDispatcher, std::function<void(CommandDispatcher*)>>;

}  // namespace scenic_impl

#endif  // SRC_UI_SCENIC_LIB_SCENIC_COMMAND_DISPATCHER_H_
