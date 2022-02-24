// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/vmm/device/guest_view.h"

#include <lib/images/cpp/images.h>
#include <lib/syslog/cpp/macros.h>

GuestView::GuestView(scenic::ViewContext view_context, GpuScanout* scanout,
                     fuchsia::virtualization::hardware::KeyboardListenerPtr keyboard_listener,
                     fuchsia::virtualization::hardware::PointerListenerPtr pointer_listener)
    : BaseView(std::move(view_context), "Guest"),
      background_(session()),
      material_(session()),
      scanout_(*scanout),
      keyboard_listener_(std::move(keyboard_listener)),
      pointer_listener_(std::move(pointer_listener)) {
  background_.SetMaterial(material_);
  root_node().AddChild(background_);

  // Request hard key events be delivered to the view.
  fuchsia::ui::input::Command command;
  command.set_set_hard_keyboard_delivery({.delivery_request = true});
  session()->Enqueue(std::move(command));

  scanout_.SetFlushHandler([this](virtio_gpu_rect_t rect) { InvalidateScene(); });
  scanout_.SetUpdateSourceHandler([this](uint32_t width, uint32_t height) {
    scanout_source_width_ = width;
    scanout_source_height_ = height;
    InvalidateScene();
  });
}

void GuestView::OnSceneInvalidated(fuchsia::images::PresentationInfo presentation_info) {
  if (!has_logical_size() || !has_physical_size()) {
    return;
  }
  if (static_cast<uint32_t>(physical_size().x) != image_info_.width ||
      static_cast<uint32_t>(physical_size().y) != image_info_.height) {
    image_info_.width = static_cast<uint32_t>(physical_size().x);
    image_info_.height = static_cast<uint32_t>(physical_size().y);
    image_info_.stride = image_info_.width * 4;
    image_info_.pixel_format = fuchsia::images::PixelFormat::BGRA_8;

    // Allocate a framebuffer and attach it as a GPU scanout.
    zx::vmo scanout_vmo;
    auto vmo_size = images::ImageSize(image_info_);
    zx_status_t status = zx::vmo::create(vmo_size, 0, &scanout_vmo);
    FX_CHECK(status == ZX_OK) << "Scanout target VMO creation failed " << status;
    zx::vmo scenic_vmo;
    status = scanout_vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &scenic_vmo);
    FX_CHECK(status == ZX_OK) << "Scanout target VMO duplication failed " << status;
    memory_ = std::make_unique<scenic::Memory>(session(), std::move(scenic_vmo), vmo_size,
                                               fuchsia::images::MemoryType::HOST_MEMORY);

    status = scanout_.SetFlushTarget(std::move(scanout_vmo), vmo_size, image_info_.width,
                                     image_info_.height, image_info_.stride);
    FX_CHECK(status == ZX_OK) << "Scanout target VMO flush failed " << status;
  }

  const float width = logical_size().x;
  const float height = logical_size().y;
  scenic::Rectangle shape(session(), width, height);
  background_.SetShape(shape);

  const float center_x = width * .5f;
  const float center_y = height * .5f;
  background_.SetAnchor(-center_x, -center_y, 0.0f);
  background_.SetTranslation(center_x, center_y, 0.0f);

  if (scanout_source_height_ > 0 && scanout_source_width_ > 0) {
    // Scale the background node such that the scanout resource sub-region
    // matches the image size. Ideally, this would just be a scale transform of
    // the material itself.
    // TODO(fxbug.dev/24174): Materials should support transforms
    const auto scale_x =
        static_cast<float>(image_info_.width) / static_cast<float>(scanout_source_width_);
    const auto scale_y =
        static_cast<float>(image_info_.height) / static_cast<float>(scanout_source_height_);
    background_.SetScale(scale_x, scale_y, 1.0f);

    scenic::Image image(*memory_, 0u, image_info_);
    material_.SetTexture(image);
  } else {
    // If Virtio GPU disables the scanout, the |scanout_source_height_| and
    // |scanout_source_width_| will be set to 0. In that case we should display
    // a plain color background instead.
    background_.SetScale(1.0f, 1.0f, 1.0f);
    material_.SetColor(0, 0, 0, 255);
  }
}

void GuestView::OnPropertiesChanged(fuchsia::ui::gfx::ViewProperties old_properties) {
  pointer_listener_->OnSizeChanged(logical_size());
}

void GuestView::OnInputEvent(fuchsia::ui::input::InputEvent event) {
  switch (event.Which()) {
    case fuchsia::ui::input::InputEvent::Tag::kKeyboard:
      keyboard_listener_->OnKeyboardEvent(event.keyboard());
      break;
    case fuchsia::ui::input::InputEvent::Tag::kPointer:
      if (logical_size().x > 0 && logical_size().y > 0) {
        pointer_listener_->OnPointerEvent(event.pointer());
      }
      break;
    default:
      break;
  }
}

void GuestView::OnScenicError(std::string error) {
  FX_LOGS(ERROR) << "Scenic session failed " << error;
}
