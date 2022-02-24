// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_UI_SCENIC_LIB_INPUT_A11Y_LEGACY_CONTENDER_H_
#define SRC_UI_SCENIC_LIB_INPUT_A11Y_LEGACY_CONTENDER_H_

#include <fuchsia/ui/input/accessibility/cpp/fidl.h>
#include <lib/fit/function.h>

#include <deque>
#include <unordered_map>
#include <unordered_set>

#include "src/ui/scenic/lib/input/gesture_contender.h"
#include "src/ui/scenic/lib/input/gesture_contender_inspector.h"
#include "src/ui/scenic/lib/input/internal_pointer_event.h"

namespace scenic_impl::input {

// Class for incorporating a11y legacy clients with the gesture disambiguation protocol.
class A11yLegacyContender final : public GestureContender {
 public:
  A11yLegacyContender(fit::function<void(StreamId, GestureResponse)> respond,
                      fit::function<void(const InternalTouchEvent& event)> deliver_to_client,
                      GestureContenderInspector& inspector);
  ~A11yLegacyContender();

  void UpdateStream(StreamId stream_id, const InternalTouchEvent& event, bool is_end_of_stream,
                    view_tree::BoundingBox unused) override;

  void EndContest(StreamId stream_id, bool awarded_win) override;

  // Implementation of |fuchsia::ui::input::accessibility::PointerEventListener::OnStreamHandled|.
  void OnStreamHandled(uint32_t pointer_id,
                       fuchsia::ui::input::accessibility::EventHandling handled);

 private:
  struct Stream {
    bool consumed = false;
    bool has_ended = false;
    bool awarded_win = false;
    uint32_t pointer_id = 0;
    uint64_t num_received_events = 0;
  };

  void AddStream(StreamId stream_id, uint32_t pointer_id);
  void RemoveStream(StreamId stream_id);

  // Multiple streams with the same pointer id can start before A11y has time to respond to the
  // previous one. Handle them in order, since A11y responses should arrive in order.
  std::unordered_map</*pointer_id*/ uint32_t, std::deque<StreamId>> pointer_id_to_stream_id_map_;

  // Tracks all streams that have had at least one event passed into UpdateStream(), and that
  // haven't either "been won and has ended", or "haven't been lost".
  std::unordered_map<StreamId, Stream> ongoing_streams_;

  // Streams can be declared as won before the first UpdateStream() call concerning the stream,
  // this set tracks those streams. This set should never contain a stream that also exists in
  // |ongoing_streams_|.
  std::unordered_set<StreamId> won_streams_awaiting_first_message_;

  const fit::function<void(StreamId, GestureResponse)> respond_;
  const fit::function<void(const InternalTouchEvent& event)> deliver_to_client_;

  // Saved by reference since |inspector_| is guaranteed to outlive the contender.
  GestureContenderInspector& inspector_;
};

}  // namespace scenic_impl::input

#endif  // SRC_UI_SCENIC_LIB_INPUT_A11Y_LEGACY_CONTENDER_H_
