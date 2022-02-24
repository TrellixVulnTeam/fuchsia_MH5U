// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_FLATLAND_FLATLAND_PRESENTER_H_
#define SRC_UI_SCENIC_LIB_FLATLAND_FLATLAND_PRESENTER_H_

#include <unordered_map>

#include "src/ui/scenic/lib/scheduling/frame_scheduler.h"
#include "src/ui/scenic/lib/scheduling/id.h"

namespace flatland {

// Interface for Flatland instances to register user Present calls. Primarily intended to provide
// a thread-safe abstraction around a FrameScheduler.
class FlatlandPresenter {
 public:
  virtual ~FlatlandPresenter() {}

  // From scheduling::FrameScheduler::RegisterPresent():
  //
  // Registers per-present information with the frame scheduler and returns an incrementing
  // PresentId unique to that session.
  //
  // This function should be called from Flatland instance worker threads.
  virtual scheduling::PresentId RegisterPresent(scheduling::SessionId session_id,
                                                std::vector<zx::event> release_fences) = 0;

  // From scheduling::FrameScheduler::ScheduleUpdateForSession():
  //
  // Tells the frame scheduler to schedule a frame. This is also used for updates triggered by
  // something other than a Session update i.e. an ImagePipe with a new Image to present.
  //
  // Flatland should not call this function until it has reached the acquire fences and queued an
  // UberStruct for the associated |id_pair|.
  //
  // This function should be called from Flatland instance worker threads.
  virtual void ScheduleUpdateForSession(zx::time requested_presentation_time,
                                        scheduling::SchedulingIdPair id_pair, bool squashable) = 0;

  // From scheduling::FrameScheduler::GetFuturePresentationInfos():
  // Gets the predicted latch points and presentation times for the frames at or before the next
  // |requested_prediction_span| time span. Uses the FramePredictor to do so.
  //
  // The callback is guaranteed to run on the calling thread.
  virtual void GetFuturePresentationInfos(
      scheduling::FrameScheduler::GetFuturePresentationInfosCallback callback) = 0;

  // From scheduling::FrameScheduler::RemoveSession():
  //
  // Removes all references to |session_id|.
  //
  // This function should be called from the main render thread.
  virtual void RemoveSession(scheduling::SessionId session_id) = 0;
};

}  // namespace flatland

#endif  // SRC_UI_SCENIC_LIB_FLATLAND_FLATLAND_PRESENTER_H_
