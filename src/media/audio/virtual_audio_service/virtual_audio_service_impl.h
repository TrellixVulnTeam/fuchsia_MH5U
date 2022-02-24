// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MEDIA_AUDIO_VIRTUAL_AUDIO_SERVICE_VIRTUAL_AUDIO_SERVICE_IMPL_H_
#define SRC_MEDIA_AUDIO_VIRTUAL_AUDIO_SERVICE_VIRTUAL_AUDIO_SERVICE_IMPL_H_

#include <fuchsia/virtualaudio/cpp/fidl.h>
#include <lib/sys/cpp/component_context.h>

#include "src/lib/fxl/macros.h"

namespace virtual_audio {

class VirtualAudioServiceImpl {
 public:
  VirtualAudioServiceImpl(std::unique_ptr<sys::ComponentContext> component_context);
  ~VirtualAudioServiceImpl();

  zx_status_t Init();

 private:
  zx_status_t ForwardControlRequest(fidl::InterfaceRequest<fuchsia::virtualaudio::Control> request);
  zx_status_t ForwardInputRequest(fidl::InterfaceRequest<fuchsia::virtualaudio::Input> request);
  zx_status_t ForwardOutputRequest(fidl::InterfaceRequest<fuchsia::virtualaudio::Output> request);

  bool OpenControlDriver();

  std::unique_ptr<sys::ComponentContext> component_context_;
  bool driver_open_ = false;
  zx_handle_t channel_handle_ = ZX_KOID_INVALID;

  FXL_DISALLOW_COPY_AND_ASSIGN(VirtualAudioServiceImpl);
};

}  // namespace virtual_audio

#endif  // SRC_MEDIA_AUDIO_VIRTUAL_AUDIO_SERVICE_VIRTUAL_AUDIO_SERVICE_IMPL_H_
