// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/vk/image.h"

#include "src/ui/lib/escher/util/image_utils.h"

namespace escher {

const ResourceTypeInfo Image::kTypeInfo("Image", ResourceType::kResource, ResourceType::kImage);

ImagePtr Image::WrapVkImage(ResourceManager* image_owner, ImageInfo info, vk::Image vk_image,
                            vk::ImageLayout initial_layout) {
  // Wrapping transient image is disallowed because this class doesn't have access to the
  // vk::DeviceMemory required to implement GetDeviceMemoryCommitment().
  FX_CHECK(!info.is_transient()) << "Cannot wrap a transient image.";
  return fxl::AdoptRef(new Image(image_owner, info, vk_image, 0, nullptr, initial_layout));
}

Image::Image(ResourceManager* image_owner, ImageInfo info, vk::Image image, vk::DeviceSize size,
             uint8_t* host_ptr, vk::ImageLayout initial_layout)
    : Resource(image_owner),
      info_(info),
      image_(image),
      has_depth_(image_utils::IsDepthFormat(info.format)),
      has_stencil_(image_utils::IsStencilFormat(info.format)),
      size_(size),
      host_ptr_(host_ptr),
      layout_(initial_layout) {}

vk::DeviceSize Image::GetDeviceMemoryCommitment() {
  // See WrapVkImage().  Since WrapVkImage() is the only way to directly instantiate an Image,
  // and since WrapVkImage() disallows wrapping of transient images, this implies that this is an
  // instance of a subclass of Image, which must override this implementation of
  // GetDeviceMemoryCommitment().
  FX_CHECK(!is_transient()) << "Subclass must implement GetDeviceMemoryCommitment()";
  return size();
}

}  // namespace escher
