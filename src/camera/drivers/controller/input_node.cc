// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/camera/drivers/controller/input_node.h"

#include <fuchsia/hardware/isp/c/banjo.h>
#include <lib/ddk/trace/event.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include "src/camera/drivers/controller/graph_utils.h"
#include "src/camera/lib/format_conversion/buffer_collection_helper.h"
#include "src/camera/lib/format_conversion/format_conversion.h"
#include "src/devices/lib/sysmem/sysmem.h"

namespace camera {

constexpr auto kTag = "camera_controller_input_node";

fpromise::result<std::unique_ptr<InputNode>, zx_status_t> InputNode::CreateInputNode(
    StreamCreationData* info, const ControllerMemoryAllocator& memory_allocator,
    async_dispatcher_t* dispatcher, const ddk::IspProtocolClient& isp) {
  uint8_t isp_stream_type;
  if (info->node.input_stream_type == fuchsia::camera2::CameraStreamType::FULL_RESOLUTION) {
    isp_stream_type = STREAM_TYPE_FULL_RESOLUTION;
  } else if (info->node.input_stream_type ==
             fuchsia::camera2::CameraStreamType::DOWNSCALED_RESOLUTION) {
    isp_stream_type = STREAM_TYPE_DOWNSCALED;
  } else {
    return fpromise::error(ZX_ERR_INVALID_ARGS);
  }

  auto result = camera::GetBuffers(memory_allocator, info->node, info, kTag);
  if (result.is_error()) {
    FX_PLOGST(ERROR, kTag, result.error()) << "Failed to get buffers";
    return fpromise::error(result.error());
  }
  auto buffers = std::move(result.value());

  // Use a BufferCollectionHelper to manage the conversion
  // between buffer collection representations.
  BufferCollectionHelper buffer_collection_helper(buffers.buffers);

  auto image_format = ConvertHlcppImageFormat2toCType(info->node.image_formats[0]);

  // Create Input Node
  auto processing_node =
      std::make_unique<camera::InputNode>(info, std::move(buffers), dispatcher, isp);
  if (!processing_node) {
    FX_LOGST(ERROR, kTag) << "Failed to create Input node";
    return fpromise::error(ZX_ERR_NO_MEMORY);
  }

  // Create stream with ISP
  auto isp_stream_protocol = std::make_unique<camera::IspStreamProtocol>();
  if (!isp_stream_protocol) {
    FX_LOGST(ERROR, kTag) << "Failed to create ISP stream protocol";
    return fpromise::error(ZX_ERR_INTERNAL);
  }

  buffer_collection_info_2 temp_buffer_collection;
  image_format_2_t temp_image_format;
  sysmem::buffer_collection_info_2_banjo_from_fidl(*buffer_collection_helper.GetC(),
                                                   temp_buffer_collection);
  sysmem::image_format_2_banjo_from_fidl(image_format, temp_image_format);
  auto status = isp.CreateOutputStream(
      &temp_buffer_collection, &temp_image_format,
      reinterpret_cast<const frame_rate_t*>(&info->node.output_frame_rate), isp_stream_type,
      processing_node->isp_frame_callback(), isp_stream_protocol->protocol());
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Failed to create output stream on ISP";
    return fpromise::error(status);
  }

  // Update the input node with the ISP stream protocol
  processing_node->set_isp_stream_protocol(std::move(isp_stream_protocol));
  return fpromise::ok(std::move(processing_node));
}

void InputNode::OnReadyToProcess(const frame_available_info_t* info) {
  // Use the timestamp of the input as the capture time.
  auto modified = *info;
  modified.metadata.capture_timestamp = info->metadata.timestamp;
  OnFrameAvailable(&modified);
}

void InputNode::OnFrameAvailable(const frame_available_info_t* info) {
  ZX_ASSERT(thread_checker_.is_thread_valid());
  TRACE_DURATION("camera", "InputNode::OnFrameAvailable", "buffer_index", info->buffer_id,
                 "isp_stream_type", (int)isp_stream_type_);
  if (shutdown_requested_ || info->frame_status != FRAME_STATUS_OK) {
    TRACE_INSTANT("camera", "bad_status", TRACE_SCOPE_THREAD, "frame_status",
                  static_cast<uint32_t>(info->frame_status));
    return;
  }

  UpdateFrameCounterForAllChildren();

  if (NeedToDropFrame()) {
    TRACE_INSTANT("camera", "drop_frame", TRACE_SCOPE_THREAD);
    isp_stream_protocol_->ReleaseFrame(info->buffer_id);
    return;
  }
  ProcessNode::OnFrameAvailable(info);
}

void InputNode::OnReleaseFrame(uint32_t buffer_index) {
  TRACE_DURATION("camera", "InputNode::OnReleaseFrame", "buffer_index", buffer_index);
  std::lock_guard al(in_use_buffer_lock_);
  ZX_ASSERT(buffer_index < in_use_buffer_count_.size());
  in_use_buffer_count_[buffer_index]--;
  if (in_use_buffer_count_[buffer_index] != 0) {
    return;
  }
  if (!shutdown_requested_) {
    isp_stream_protocol_->ReleaseFrame(buffer_index);
  }
}

void InputNode::OnStartStreaming() {
  enabled_ = true;
  isp_stream_protocol_->Start();
}

void InputNode::OnStopStreaming() {
  if (AllChildNodesDisabled()) {
    enabled_ = false;
    isp_stream_protocol_->Stop();
  }
}

void InputNode::OnShutdown(fit::function<void(void)> shutdown_callback) {
  shutdown_callback_ = std::move(shutdown_callback);

  // After a shutdown request has been made,
  // no other calls should be made to the ISP driver.
  shutdown_requested_ = true;

  isp_stream_shutdown_callback_t isp_stream_shutdown_cb = {
      .shutdown_complete =
          [](void* ctx, zx_status_t /*status*/) {
            auto* input_node = static_cast<decltype(this)>(ctx);
            input_node->node_callback_received_ = true;
            input_node->OnCallbackReceived();
          },
      .ctx = this,
  };

  zx_status_t status = isp_stream_protocol_->Shutdown(&isp_stream_shutdown_cb);
  if (status != ZX_OK) {
    FX_PLOGST(ERROR, kTag, status) << "Failure during stream shutdown";
    return;
  }

  auto child_shutdown_completion_callback = [this]() {
    child_node_callback_received_ = true;
    OnCallbackReceived();
  };

  ZX_ASSERT_MSG(configured_streams().size() == 1,
                "Cannot shutdown a stream which supports multiple streams");

  // Forward the shutdown request to child node.
  if (child_nodes().empty()) {
    // If an incomplete graph is shut down, invoke the completion directly.
    child_shutdown_completion_callback();
  } else {
    // Otherwise, propagate the shutdown command and completion to the next child.
    child_nodes().at(0)->OnShutdown(child_shutdown_completion_callback);
  }
}

}  // namespace camera
