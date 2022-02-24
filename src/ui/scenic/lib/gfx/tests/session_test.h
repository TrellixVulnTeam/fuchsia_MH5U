// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_GFX_TESTS_SESSION_TEST_H_
#define SRC_UI_SCENIC_LIB_GFX_TESTS_SESSION_TEST_H_

#include <lib/fit/function.h>

#include "lib/gtest/test_loop_fixture.h"
#include "src/ui/scenic/lib/gfx/engine/image_pipe_updater.h"
#include "src/ui/scenic/lib/gfx/engine/session.h"
#include "src/ui/scenic/lib/gfx/engine/view_tree_updater.h"
#include "src/ui/scenic/lib/gfx/sysmem.h"
#include "src/ui/scenic/lib/gfx/tests/error_reporting_test.h"
#include "src/ui/scenic/lib/gfx/tests/mocks/mocks.h"
#include "src/ui/scenic/lib/scenic/event_reporter.h"
#include "src/ui/scenic/lib/scenic/tests/scenic_test.h"
#include "src/ui/scenic/lib/scheduling/default_frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/vsync_timing.h"

namespace scenic_impl::gfx::test {

class SessionTest : public ErrorReportingTest {
 protected:
  // Subclasses that override SetUp() and TearDown() should be sure to call
  // their parent class's implementations.

  // | ::testing::Test |
  void SetUp() override;
  // | ::testing::Test |
  void TearDown() override;

  std::unique_ptr<Session> CreateSession();

  // Apply the specified Command.  Return true if it was applied successfully,
  // and false if an error occurred.
  bool Apply(fuchsia::ui::gfx::Command command);

  template <class ResourceT>
  fxl::RefPtr<ResourceT> FindResource(ResourceId id) {
    return session_->resources()->FindResource<ResourceT>(id);
  }

  Session* session() { return session_.get(); }

 protected:
  // Creates a SessionContext with only a SessionManager and a
  // FrameScheduler. Subclasses should override to customize Session creation.
  virtual SessionContext CreateSessionContext();
  // Creates an empty CommandContext for Apply(). Subclasses should override to
  // customize command application.
  virtual CommandContext CreateCommandContext();

  ViewTreeUpdater view_tree_updater_;

 private:
  SessionContext session_context_;

  std::shared_ptr<scheduling::DefaultFrameScheduler> frame_scheduler_;
  std::shared_ptr<ImagePipeUpdater> image_pipe_updater_;
  std::unique_ptr<Session> session_;
};

}  // namespace scenic_impl::gfx::test

#endif  // SRC_UI_SCENIC_LIB_GFX_TESTS_SESSION_TEST_H_
