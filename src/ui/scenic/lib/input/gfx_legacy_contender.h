// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_INPUT_GFX_LEGACY_CONTENDER_H_
#define SRC_UI_SCENIC_LIB_INPUT_GFX_LEGACY_CONTENDER_H_

#include <lib/fit/function.h>

#include <vector>

#include "src/ui/scenic/lib/input/gesture_contender.h"
#include "src/ui/scenic/lib/input/gesture_contender_inspector.h"
#include "src/ui/scenic/lib/input/internal_pointer_event.h"

namespace scenic_impl::input {

// Class for incorporating gfx legacy clients with the gesture disambiguation protocol.
// Expect to create a new one for every each stream that has a legacy contender.
class GfxLegacyContender : public GestureContender {
 public:
  GfxLegacyContender(
      zx_koid_t view_ref_koid, fit::function<void(GestureResponse)> respond,
      fit::function<void(const std::vector<InternalTouchEvent>&)> deliver_events_to_client,
      fit::function<void()> self_destruct, GestureContenderInspector& inspector);
  ~GfxLegacyContender() = default;

  void UpdateStream(StreamId stream_id, const InternalTouchEvent& event, bool is_end_of_stream,
                    view_tree::BoundingBox unused) override;

  void EndContest(StreamId stream_id, bool awarded_win) override;

 private:
  bool awarded_win_ = false;
  bool is_end_of_stream_ = false;
  std::vector<InternalTouchEvent> undelivered_events_;

  const fit::function<void(GestureResponse)> respond_;
  const fit::function<void(const std::vector<InternalTouchEvent>&)> deliver_events_to_client_;
  const fit::function<void()> self_destruct_;

  // Saved by reference since |inspector_| is guaranteed to outlive the contender.
  GestureContenderInspector& inspector_;
};

}  // namespace scenic_impl::input

#endif  // SRC_UI_SCENIC_LIB_INPUT_GFX_LEGACY_CONTENDER_H_
