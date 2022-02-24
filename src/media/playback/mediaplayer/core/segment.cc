// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/core/segment.h"

#include <lib/async/dispatcher.h>

namespace media_player {

Segment::Segment() {}

Segment::~Segment() {}

void Segment::Provision(Graph* graph, async_dispatcher_t* dispatcher,
                        fit::closure update_callback) {
  FX_DCHECK(graph);
  FX_DCHECK(dispatcher);

  graph_ = graph;
  dispatcher_ = dispatcher;
  update_callback_ = std::move(update_callback);
  DidProvision();
}

void Segment::Deprovision() {
  WillDeprovision();
  graph_ = nullptr;
  dispatcher_ = nullptr;
  update_callback_ = nullptr;
}

void Segment::SetUpdateCallback(fit::closure update_callback) {
  update_callback_ = std::move(update_callback);
}

void Segment::NotifyUpdate() const {
  if (update_callback_) {
    update_callback_();
  }
}

void Segment::ReportProblem(const std::string& type, const std::string& details) {
  if (problem_ && problem_->type == type && problem_->details.value_or("") == details) {
    // No change.
    return;
  }

  problem_ = std::make_unique<fuchsia::media::playback::Problem>();
  problem_->type = type;
  problem_->details = details;
  NotifyUpdate();
}

void Segment::ReportNoProblem() {
  if (!problem_) {
    // No change.
    return;
  }

  problem_ = nullptr;
  NotifyUpdate();
}

}  // namespace media_player
