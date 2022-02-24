// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/scheduling/tests/frame_scheduler_test.h"

#include <lib/gtest/test_loop_fixture.h>

#include "src/ui/scenic/lib/scheduling/default_frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/windowed_frame_predictor.h"

namespace scheduling {
namespace test {

void FrameSchedulerTest::SetUp() {
  vsync_timing_ = std::make_shared<VsyncTiming>();
  mock_updater_ = std::make_shared<MockSessionUpdater>();
  mock_renderer_ = std::make_shared<MockFrameRenderer>();
  SetupDefaultVsyncValues();
}

void FrameSchedulerTest::TearDown() {
  vsync_timing_.reset();
  mock_updater_.reset();
  mock_renderer_.reset();
}

std::unique_ptr<DefaultFrameScheduler> FrameSchedulerTest::CreateDefaultFrameScheduler() {
  auto scheduler = std::make_unique<DefaultFrameScheduler>(
      vsync_timing_,
      std::make_unique<WindowedFramePredictor>(DefaultFrameScheduler::kMinPredictedFrameDuration,
                                               DefaultFrameScheduler::kInitialRenderDuration,
                                               DefaultFrameScheduler::kInitialUpdateDuration));
  scheduler->Initialize(mock_renderer_, {mock_updater_});

  return scheduler;
}

void FrameSchedulerTest::SetupDefaultVsyncValues() {
  // Needs to be big enough so that FrameScheduler can always fit a latch point
  // in the frame.
  const auto vsync_interval = zx::msec(100);
  vsync_timing_->set_vsync_interval(vsync_interval);
  vsync_timing_->set_last_vsync_time(zx::time(0));
}

}  // namespace test
}  // namespace scheduling
