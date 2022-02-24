// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_PLAYBACK_MEDIAPLAYER_RENDER_RENDERER_H_
#define SRC_MEDIA_PLAYBACK_MEDIAPLAYER_RENDER_RENDERER_H_

#include <fuchsia/media/cpp/fidl.h>

#include <limits>

#include "lib/media/cpp/timeline_function.h"
#include "src/media/playback/mediaplayer/graph/nodes/node.h"
#include "src/media/playback/mediaplayer/graph/types/stream_type.h"

namespace media_player {

// Abstract base class for sinks that render packets.
class Renderer : public Node {
 public:
  Renderer();

  ~Renderer() override;

  // Provides an dispatcher object and update callback to the renderer. The
  // callback should be called to notify of changes in the value returned by
  // end_of_stream(). Subclasses of Renderer may use this callback to signal
  // additional changes.
  void Provision(async_dispatcher_t* dispatcher, fit::closure update_callback);

  // Revokes the task runner and update callback provided in a previous call to
  // |Provision|.
  void Deprovision();

  // Node implementation.
  void Dump(std::ostream& os) const override;

  void ConfigureConnectors() override;

  // Returns the types of the streams the renderer is able
  // to consume.
  virtual const std::vector<std::unique_ptr<StreamTypeSet>>& GetSupportedStreamTypes() = 0;

  // Sets the type of stream the renderer will consume.
  virtual void SetStreamType(const StreamType& stream_type) = 0;

  // Prepares renderer for playback by satisfying initial demand.
  virtual void Prime(fit::closure callback) = 0;

  // Sets the timeline function.
  virtual void SetTimelineFunction(media::TimelineFunction timeline_function,
                                   fit::closure callback);

  // Sets a program range for this renderer.
  virtual void SetProgramRange(uint64_t program, int64_t min_pts, int64_t max_pts);

  // Determines whether end-of-stream has been reached.
  bool end_of_stream() const;

 protected:
  async_dispatcher_t* dispatcher() const {
    FX_DCHECK(dispatcher_) << "dispatcher() called on unprovisioned renderer.";
    return dispatcher_;
  }

  // Notifies of state updates (calls the update callback).
  void NotifyUpdate();

  // Called when the value returned by |Progressing| transitions from false to
  // true. The default implementation does nothing.
  virtual void OnProgressStarted() {}

  // Determines if presentation time is progressing or a pending change will
  // cause it to progress.
  bool Progressing();

  // Sets the PTS at which end of stream will occur. Passing kUnspecifiedTime
  // indicates that end-of-stream PTS isn't known.
  void SetEndOfStreamPts(int64_t end_of_stream_pts);

  // Updates the PTS of the last content known to be rendered. This value is
  // used to determine whether end-of-stream has been reached.
  void UpdateLastRenderedPts(int64_t pts);

  // Posts a task to check for timeline transitions or end-of-stream at the
  // specified reference time.
  void UpdateTimelineAt(int64_t reference_time);

  // Called when the timeline function changes. The default implementation
  // does nothing.
  virtual void OnTimelineTransition();

  // Gets the current timeline function.
  const media::TimelineFunction& current_timeline_function() const {
    return current_timeline_function_;
  }

  // Indicates whether the end of stream packet has been encountered.
  bool end_of_stream_pending() const { return end_of_stream_pts_ != Packet::kNoPts; }

  // PTS at which end-of-stream is to occur or |kUnspecifiedTime| if an end-
  // of-stream packet has not yet been encountered.
  int64_t end_of_stream_pts() const { return end_of_stream_pts_; }

  // Returns the minimum PTS for the specified program.
  int64_t min_pts(uint64_t program) {
    FX_DCHECK(program == 0);
    return program_0_min_pts_;
  }

  // Returns the maximum PTS for the specified program.
  int64_t max_pts(uint64_t program) {
    FX_DCHECK(program == 0);
    return program_0_max_pts_;
  }

 private:
  // Applies pending_timeline_function_ if it's time to do so based on the
  // given reference time.
  void ApplyPendingChanges(int64_t reference_time);

  // Clears the pending timeline function and calls its associated callback.
  void ClearPendingTimelineFunction();

  // Determines if an unrealized timeline function is currently pending.
  bool TimelineFunctionPending() {
    return pending_timeline_function_.reference_time() != Packet::kNoPts;
  }

  async_dispatcher_t* dispatcher_;
  fit::closure update_callback_;
  media::TimelineFunction current_timeline_function_;
  media::TimelineFunction pending_timeline_function_;
  int64_t last_rendered_pts_ = Packet::kNoPts;
  int64_t end_of_stream_pts_ = Packet::kNoPts;
  bool end_of_stream_published_ = false;
  fit::closure set_timeline_function_callback_;
  int64_t program_0_min_pts_ = std::numeric_limits<int64_t>::min();
  int64_t program_0_max_pts_ = std::numeric_limits<int64_t>::max();
};

}  // namespace media_player

#endif  // SRC_MEDIA_PLAYBACK_MEDIAPLAYER_RENDER_RENDERER_H_
