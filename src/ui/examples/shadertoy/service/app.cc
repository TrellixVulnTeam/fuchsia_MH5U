// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/examples/shadertoy/service/app.h"

#include <lib/sys/cpp/component_context.h>

#include "src/ui/lib/escher/vk/vulkan_device_queues.h"
#include "src/ui/lib/escher/vk/vulkan_instance.h"

namespace shadertoy {

App::App(async::Loop* loop, sys::ComponentContext* app_context, escher::EscherWeakPtr weak_escher)
    : escher_(std::move(weak_escher)),
      renderer_(escher_, kDefaultImageFormat),
      compiler_(loop, escher_, renderer_.render_pass(), renderer_.descriptor_set_layout()) {
  app_context->outgoing()->AddPublicService(factory_bindings_.GetHandler(this));
}

App::~App() = default;

void App::NewImagePipeShadertoy(
    fidl::InterfaceRequest<fuchsia::examples::shadertoy::Shadertoy> toy_request,
    fidl::InterfaceHandle<fuchsia::images::ImagePipe2> image_pipe) {
  shadertoy_bindings_.AddBinding(
      std::make_unique<ShadertoyImpl>(ShadertoyState::NewForImagePipe(this, std::move(image_pipe))),
      std::move(toy_request));
}

void App::NewViewShadertoy(
    fidl::InterfaceRequest<fuchsia::examples::shadertoy::Shadertoy> toy_request,
    zx::eventpair view_token, bool handle_input_events) {
  shadertoy_bindings_.AddBinding(std::make_unique<ShadertoyImpl>(ShadertoyState::NewForView(
                                     this, std::move(view_token), handle_input_events)),
                                 std::move(toy_request));
}

void App::CloseShadertoy(ShadertoyState* shadertoy) {
  for (auto& binding : shadertoy_bindings_.bindings()) {
    if (binding && shadertoy == binding->impl()->state()) {
      binding->Unbind();
      return;
    }
  }
}

}  // namespace shadertoy
