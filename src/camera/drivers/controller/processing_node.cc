// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#include "processing_node.h"

#include <lib/ddk/trace/event.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "stream_protocol.h"

namespace camera {

void ProcessNode::UpdateFrameCounterForAllChildren() {
  // Update the current frame counter.
  for (auto& node : child_nodes_) {
    node->AddToCurrentFrameCount(node->output_fps());
  }
}

bool ProcessNode::NeedToDropFrame() {
  return !enabled_ || AllChildNodesDisabled() ||
         std::none_of(child_nodes_.begin(), child_nodes_.end(),
                      [this](auto& node) { return node->current_frame_count() >= output_fps(); });
}

void ProcessNode::OnFrameAvailable(const frame_available_info_t* info) {
  ZX_ASSERT(thread_checker_.is_thread_valid());
  ZX_ASSERT_MSG(type_ != NodeType::kOutputStream, "Invalid for OuputNode");
  TRACE_DURATION("camera", "ProcessNode::OnFrameAvailable");

  for (auto& node : child_nodes_) {
    // Check if this frame needs to be passed on to the next node.
    if (node->enabled() && node->current_frame_count() >= output_fps()) {
      node->SubtractFromCurrentFrameCount(output_fps());
      {
        std::lock_guard al(in_use_buffer_lock_);
        ZX_ASSERT(info->buffer_id < in_use_buffer_count_.size());
        in_use_buffer_count_[info->buffer_id]++;
      }
      node->OnReadyToProcess(info);
    }
  }
}

void ProcessNode::OnStartStreaming() {
  if (!shutdown_requested_) {
    enabled_ = true;
    parent_node_->OnStartStreaming();
  }
}

bool ProcessNode::AllChildNodesDisabled() {
  return std::none_of(child_nodes_.begin(), child_nodes_.end(),
                      [](auto& node) { return node->enabled(); });
}

void ProcessNode::OnStopStreaming() {
  if (!shutdown_requested_) {
    if (AllChildNodesDisabled()) {
      enabled_ = false;
      parent_node_->OnStopStreaming();
    }
  }
}

void ProcessNode::OnResolutionChanged(const frame_available_info* info) {
  TRACE_DURATION("camera", "ProcessNode::OnResolutionChanged", "index",
                 info->metadata.image_format_index);
  for (auto& node : child_nodes_) {
    if (node->enabled()) {
      node->OnResolutionChangeRequest(info->metadata.image_format_index);
    }
  }
}

}  // namespace camera
