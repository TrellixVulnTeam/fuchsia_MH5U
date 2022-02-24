// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/lib/escher/renderer/render_funcs.h"

#include "src/ui/lib/escher/escher.h"
#include "src/ui/lib/escher/impl/vulkan_utils.h"
#include "src/ui/lib/escher/util/trace_macros.h"
#include "src/ui/lib/escher/vk/render_pass.h"
#include "src/ui/lib/escher/vk/texture.h"

#include <vulkan/vulkan.hpp>

namespace escher {

// Helper for NewVertexAttributeBindings().
static RenderFuncs::VertexAttributeBinding* FillVertexAttributeBindings(
    const MeshAttributeBindingLocations& attribute_binding_locations,
    RenderFuncs::VertexAttributeBinding* binding, uint32_t binding_index,
    MeshAttributes attributes) {
  using VertexAttributeBinding = RenderFuncs::VertexAttributeBinding;

  if (attributes & MeshAttribute::kPosition2D) {
    *binding++ = VertexAttributeBinding{
        .binding_index = binding_index,
        .attribute_index = attribute_binding_locations.position_2d,
        .format = vk::Format::eR32G32Sfloat,
        .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kPosition2D)};
  }
  if (attributes & MeshAttribute::kPosition3D) {
    *binding++ = VertexAttributeBinding{
        .binding_index = binding_index,
        .attribute_index = attribute_binding_locations.position_3d,
        .format = vk::Format::eR32G32B32Sfloat,
        .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kPosition3D)};
  }
  if (attributes & MeshAttribute::kPositionOffset) {
    *binding++ = VertexAttributeBinding{
        .binding_index = binding_index,
        .attribute_index = attribute_binding_locations.position_offset,
        .format = vk::Format::eR32G32Sfloat,
        .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kPositionOffset)};
  }
  if (attributes & MeshAttribute::kUV) {
    *binding++ =
        VertexAttributeBinding{.binding_index = binding_index,
                               .attribute_index = attribute_binding_locations.uv,
                               .format = vk::Format::eR32G32Sfloat,
                               .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kUV)};
  }
  if (attributes & MeshAttribute::kPerimeterPos) {
    *binding++ = VertexAttributeBinding{
        .binding_index = binding_index,
        .attribute_index = attribute_binding_locations.perimeter_pos,
        .format = vk::Format::eR32G32Sfloat,
        .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kPerimeterPos)};
  }
  if (attributes & MeshAttribute::kBlendWeight1) {
    *binding++ = VertexAttributeBinding{
        .binding_index = binding_index,
        .attribute_index = attribute_binding_locations.blend_weight1,
        .format = vk::Format::eR32Sfloat,
        .offset = GetMeshAttributeOffset(attributes, MeshAttribute::kBlendWeight1)};
  }
  return binding;
}

RenderFuncs::VertexAttributeBinding* RenderFuncs::NewVertexAttributeBindings(
    const MeshAttributeBindingLocations& attribute_binding_locations, BlockAllocator* allocator,
    const MeshSpec& mesh_spec, uint32_t total_attribute_count) {
  FX_DCHECK(total_attribute_count == mesh_spec.total_attribute_count());

  auto bindings = allocator->AllocateMany<VertexAttributeBinding>(total_attribute_count);
  {
    VertexAttributeBinding* current = bindings;
    for (uint32_t i = 0; i < VulkanLimits::kNumVertexBuffers; ++i) {
      if (mesh_spec.attribute_count(i) > 0) {
        current = FillVertexAttributeBindings(attribute_binding_locations, current, i,
                                              mesh_spec.attributes[i]);
      }
    }

    // Sanity check that we filled in the correct number of attributes.
    FX_DCHECK(current == (bindings + total_attribute_count));
  }
  return bindings;
}

void RenderFuncs::ObtainDepthAndMsaaTextures(
    Escher* escher, const FramePtr& frame, uint32_t width, uint32_t height, uint32_t sample_count,
    bool use_transient_attachment, vk::Format depth_stencil_format, vk::Format msaa_format,
    TexturePtr& depth_texture_inout, TexturePtr& msaa_texture_inout) {
  const bool realloc_textures =
      !depth_texture_inout ||
      (depth_texture_inout->image()->use_protected_memory() != frame->use_protected_memory()) ||
      (depth_texture_inout->image()->is_transient() != use_transient_attachment) ||
      width != depth_texture_inout->width() || height != depth_texture_inout->height() ||
      sample_count != depth_texture_inout->image()->info().sample_count;
  if (!realloc_textures) {
    return;
  }
  vk::ImageUsageFlags image_usage = use_transient_attachment
                                        ? vk::ImageUsageFlagBits::eTransientAttachment
                                        : vk::ImageUsageFlags();
  vk::MemoryPropertyFlags memory_properties = vk::MemoryPropertyFlagBits::eDeviceLocal;
  if (use_transient_attachment) {
    memory_properties |= vk::MemoryPropertyFlagBits::eLazilyAllocated;
  }
  if (frame->use_protected_memory()) {
    memory_properties |= vk::MemoryPropertyFlagBits::eProtected;
  }

  // Most times, we shouldn't need to reallocate.  If this becomes logspam, it is indicative of
  // other problems.
  FX_LOGS(INFO) << "RenderFuncs::ObtainDepthAndMsaaTextures() width=" << width
                << " height=" << height << " sample_count=" << sample_count
                << " mem_props=" << vk::to_string(memory_properties)
                << " depth_stencil_format=" << vk::to_string(depth_stencil_format)
                << " msaa_format=" << vk::to_string(msaa_format);

  // Need to generate a new depth buffer.
  {
    TRACE_DURATION("gfx", "RenderFuncs::ObtainDepthAndMsaaTextures (new depth)");
    depth_texture_inout = escher->NewAttachmentTexture(
        depth_stencil_format, width, height, sample_count, vk::Filter::eLinear, image_usage,
        /*use_unnormalized_coordinates=*/false, memory_properties);
    if (!depth_texture_inout) {
      FX_LOGS(ERROR) << "Failed to allocate depth texture.";
    }
  }
  if (sample_count == 1) {
    msaa_texture_inout = nullptr;
  } else {
    TRACE_DURATION("gfx", "RenderFuncs::ObtainDepthAndMsaaTextures (new msaa)");
    msaa_texture_inout = escher->NewAttachmentTexture(
        msaa_format, width, height, sample_count, vk::Filter::eLinear, image_usage,
        /*use_unnormalized_coordinates=*/false, memory_properties);
    if (!msaa_texture_inout) {
      FX_LOGS(ERROR) << "Failed to allocate msaa texture.";
    }

    // Don't transition layout for transient attachment images.
    if (!use_transient_attachment) {
      frame->cmds()->ImageBarrier(msaa_texture_inout->image(), vk::ImageLayout::eUndefined,
                                  vk::ImageLayout::eColorAttachmentOptimal,
                                  vk::PipelineStageFlagBits::eAllGraphics, vk::AccessFlags(),
                                  vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                  vk::AccessFlagBits::eColorAttachmentWrite);
    }
  }
}

void RenderFuncs::ObtainDepthTexture(Escher* escher, const bool use_protected_memory,
                                     const ImageInfo& info, vk::Format depth_stencil_format,
                                     TexturePtr& depth_texture) {
  // Support for other sample_counts should fairly easy to add, if necessary.
  FX_DCHECK(info.sample_count == 1);

  const bool realloc_textures =
      !depth_texture || (depth_texture->image()->use_protected_memory() != use_protected_memory) ||
      info.width != depth_texture->width() || info.height != depth_texture->height();

  // If the depth buffer does not exist, or if the depth buffer has a different
  // size than the output buffer, recreate it.
  if (realloc_textures) {
    // Need to generate a new depth buffer.
    {
      TRACE_DURATION("gfx", "RenderFuncs::ObtainDepthAndMsaaTextures (new depth)");
      depth_texture = escher->NewAttachmentTexture(
          depth_stencil_format, info.width, info.height, 1, vk::Filter::eLinear,
          vk::ImageUsageFlags(), /*use_unnormalized_coordinates=*/false,
          use_protected_memory ? vk::MemoryPropertyFlagBits::eProtected
                               : vk::MemoryPropertyFlags());
    }
  }
}
}  // namespace escher
