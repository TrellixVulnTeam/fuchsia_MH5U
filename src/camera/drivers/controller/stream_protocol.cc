// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/drivers/controller/stream_protocol.h"

#include <lib/fit/defer.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/trace/event.h>
#include <zircon/errors.h>

#include <utility>

#include "src/camera/drivers/controller/graph_utils.h"
#include "src/camera/drivers/controller/processing_node.h"

namespace camera {

constexpr auto kTag = "camera_controller";

StreamImpl::StreamImpl(ProcessNode* output_node) : binding_(this), output_node_(*output_node) {}

zx_status_t StreamImpl::Attach(zx::channel channel, fit::function<void(void)> disconnect_handler) {
  FX_DCHECK(!binding_.is_bound());
  disconnect_handler_ = std::move(disconnect_handler);
  binding_.set_error_handler([this](zx_status_t status) {
    Shutdown(status);
    disconnect_handler_();
  });

  zx_status_t status = binding_.Bind(std::move(channel));
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status);
    return status;
  }
  return ZX_OK;
}

void StreamImpl::FrameReady(const frame_available_info_t* info) {
  TRACE_DURATION("camera", "StreamImpl::FrameReady");
  TRACE_FLOW_BEGIN("camera", "camera_stream_on_frame_available", info->metadata.timestamp);
  ZX_ASSERT(thread_checker_.is_thread_valid());
  fuchsia::camera2::FrameAvailableInfo frame_info;
  frame_info.frame_status = static_cast<fuchsia::camera2::FrameStatus>(info->frame_status);
  frame_info.buffer_id = info->buffer_id;
  frame_info.metadata.set_image_format_index(info->metadata.image_format_index);
  frame_info.metadata.set_timestamp(info->metadata.timestamp);
  frame_info.metadata.set_capture_timestamp(info->metadata.capture_timestamp);
  ZX_ASSERT(binding_.is_bound());
  binding_.events().OnFrameAvailable(std::move(frame_info));
}

void StreamImpl::Shutdown(zx_status_t status) {
  // Close the connection if it's open.
  if (binding_.is_bound()) {
    binding_.Close(status);
  }

  // Stop streaming if its started
  if (started_) {
    Stop();
  }
}

void StreamImpl::Stop() {
  output_node_.OnStopStreaming();
  started_ = false;
}

void StreamImpl::Start() {
  output_node_.OnStartStreaming();
  started_ = true;
}

void StreamImpl::ReleaseFrame(uint32_t buffer_id) { output_node_.OnReleaseFrame(buffer_id); }

void StreamImpl::AcknowledgeFrameError() {
  FX_LOGST(ERROR, kTag) << __PRETTY_FUNCTION__ << " not implemented";
  Shutdown(ZX_ERR_UNAVAILABLE);
}

void StreamImpl::SetRegionOfInterest(float x_min, float y_min, float x_max, float y_max,
                                     SetRegionOfInterestCallback callback) {
  zx_status_t status = ZX_OK;
  auto cleanup = fit::defer([&]() { callback(status); });

  auto stream_type = output_node_.configured_streams().at(0);
  auto* parent_node = output_node_.parent_node();
  while (parent_node) {
    if (parent_node->is_crop_region_supported(stream_type)) {
      status = parent_node->OnSetCropRect(x_min, y_min, x_max, y_max);
      break;
    }
    parent_node = parent_node->parent_node();
  }
  if (parent_node == nullptr) {
    status = ZX_ERR_NOT_SUPPORTED;
  }
}

void StreamImpl::SetImageFormat(uint32_t image_format_index, SetImageFormatCallback callback) {
  zx_status_t status = ZX_OK;
  auto cleanup = fit::defer([&]() {
    if (status == ZX_OK) {
      output_node_.set_current_image_format_index(image_format_index);
    }
    callback(status);
  });

  auto& available_image_formats = output_node_.output_image_formats();
  if (image_format_index >= available_image_formats.size()) {
    status = ZX_ERR_INVALID_ARGS;
    return;
  }

  auto stream_type = output_node_.configured_streams().at(0);

  auto* parent_node = output_node_.parent_node();
  if (output_node_.current_image_format_index() != image_format_index) {
    while (parent_node) {
      if (parent_node->is_dynamic_resolution_supported(stream_type)) {
        parent_node->OnResolutionChangeRequest(image_format_index);
        break;
      }
      parent_node = parent_node->parent_node();
    }
    if (parent_node == nullptr) {
      status = ZX_ERR_INVALID_ARGS;
    }
  }
}

void StreamImpl::GetImageFormats(GetImageFormatsCallback callback) {
  auto& available_image_formats = output_node_.output_image_formats();
  callback({available_image_formats.begin(), available_image_formats.end()});
}

void StreamImpl::GetBuffers(GetBuffersCallback callback) {
  auto parent_node = output_node_.parent_node();

  // in_place nodes may not have bound buffer collection ptr, walk up to find the real collection.
  while (parent_node && !parent_node->output_buffer_collection()) {
    parent_node = parent_node->parent_node();
  }

  auto& input_buffer_collection = parent_node->output_buffer_collection();
  ZX_ASSERT(input_buffer_collection);

  fuchsia::sysmem::BufferCollectionTokenHandle token;
  input_buffer_collection->AttachToken(ZX_RIGHT_SAME_RIGHTS, token.NewRequest());
  callback(std::move(token));
}

}  // namespace camera
