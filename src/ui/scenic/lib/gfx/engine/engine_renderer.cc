// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/ui/scenic/lib/gfx/engine/engine_renderer.h"

#include <lib/trace/event.h>

#include <src/lib/fostr/fidl/fuchsia/ui/gfx/formatting.h>

#include "src/ui/lib/escher/hmd/pose_buffer_latching_shader.h"
#include "src/ui/lib/escher/impl/image_cache.h"
#include "src/ui/lib/escher/impl/vulkan_utils.h"
#include "src/ui/lib/escher/paper/paper_renderer_config.h"
#include "src/ui/lib/escher/paper/paper_scene.h"
#include "src/ui/lib/escher/renderer/batch_gpu_uploader.h"
#include "src/ui/lib/escher/renderer/sampler_cache.h"
#include "src/ui/lib/escher/scene/model.h"
#include "src/ui/lib/escher/vk/color_space.h"
#include "src/ui/lib/escher/vk/image.h"
#include "src/ui/lib/escher/vk/image_layout_updater.h"
#include "src/ui/scenic/lib/gfx/engine/engine_renderer_visitor.h"
#include "src/ui/scenic/lib/gfx/resources/camera.h"
#include "src/ui/scenic/lib/gfx/resources/compositor/layer.h"
#include "src/ui/scenic/lib/gfx/resources/dump_visitor.h"
#include "src/ui/scenic/lib/gfx/resources/lights/ambient_light.h"
#include "src/ui/scenic/lib/gfx/resources/lights/directional_light.h"
#include "src/ui/scenic/lib/gfx/resources/lights/point_light.h"
#include "src/ui/scenic/lib/gfx/resources/renderers/renderer.h"
#include "src/ui/scenic/lib/gfx/resources/stereo_camera.h"

// TODO(fxbug.dev/24320): Move this someplace.  PoseBufferLatchingShader assumes this,
// but we can't put it there because it lives in a Zircon-ignorant part of
// Escher.
#include <type_traits>
static_assert(std::is_same<zx_time_t, int64_t>::value,
              "PoseBufferLatchingShader incorrectly assumes that zx_time_t is int64_t");

namespace {
// Format used for intermediate layers when we're rendering more than one layer.
constexpr vk::Format kIntermediateLayerFormat = vk::Format::eB8G8R8A8Srgb;
constexpr escher::ColorSpace kIntermediateLayerColorSpace = escher::ColorSpace::kSrgb;
// Color used to replace protected content.
static const escher::vec4 kReplacementMaterialColor = escher::vec4(0, 0, 0, 255);
}  // namespace

namespace scenic_impl {
namespace gfx {

EngineRenderer::EngineRenderer(escher::EscherWeakPtr weak_escher, vk::Format depth_stencil_format)
    : escher_(std::move(weak_escher)),
      // We use two depth buffers so that we can render multiple Layers without
      // introducing a GPU stall.
      paper_renderer_(escher::PaperRenderer::New(
          escher_,
          {.shadow_type = escher::PaperRendererShadowType::kNone, .num_depth_buffers = 2})),
      pose_buffer_latching_shader_(
          std::make_unique<escher::hmd::PoseBufferLatchingShader>(escher_)),
      depth_stencil_format_(depth_stencil_format) {}

EngineRenderer::~EngineRenderer() = default;

// Helper function for RenderLayer().
static escher::PaperRendererShadowType GetPaperRendererShadowType(
    fuchsia::ui::gfx::ShadowTechnique technique) {
  using escher::PaperRendererShadowType;
  using fuchsia::ui::gfx::ShadowTechnique;

  switch (technique) {
    case ShadowTechnique::UNSHADOWED:
      return PaperRendererShadowType::kNone;
    case ShadowTechnique::SCREEN_SPACE:
      return PaperRendererShadowType::kSsdo;
    case ShadowTechnique::SHADOW_MAP:
      return PaperRendererShadowType::kShadowMap;
    case ShadowTechnique::MOMENT_SHADOW_MAP:
      return PaperRendererShadowType::kMomentShadowMap;
    case ShadowTechnique::STENCIL_SHADOW_VOLUME:
      return PaperRendererShadowType::kShadowVolume;
  }
}

void EngineRenderer::RenderLayer(const escher::FramePtr& frame, zx::time target_presentation_time,
                                 const RenderTarget& render_target, const Layer& layer) {
  // NOTE: this name is important for benchmarking.  Do not remove or modify it
  // without also updating the "process_gfx_trace.go" script.
  TRACE_DURATION("gfx", "EngineRenderer::RenderLayers");

  // Check that we are working with a protected framebuffer.
  FX_DCHECK(render_target.output_image->use_protected_memory() == frame->use_protected_memory());

  FX_DCHECK(layer.IsDrawable());
  float stage_width = static_cast<float>(render_target.output_image->width());
  float stage_height = static_cast<float>(render_target.output_image->height());

  if (layer.size().x != stage_width || layer.size().y != stage_height) {
    // TODO(fxbug.dev/23494): Should be able to render into a viewport of the
    // output image, but we're not that fancy yet.
    FX_LOGS(ERROR)
        << "TODO(fxbug.dev/23494): scenic::gfx::EngineRenderer::DrawLayer(): layer size of "
        << layer.size().x << "x" << layer.size().y << " does not match output image size of "
        << stage_width << "x" << stage_height << "... not drawing.";
    return;
  }

  // TODO(fxbug.dev/24472): add pixel tests for various shadow modes (particularly
  // those implemented by PaperRenderer).
  escher::PaperRendererShadowType shadow_type =
      GetPaperRendererShadowType(layer.renderer()->shadow_technique());
  switch (shadow_type) {
    case escher::PaperRendererShadowType::kNone:
    case escher::PaperRendererShadowType::kShadowVolume:
      break;
    default:
      FX_LOGS(WARNING) << "EngineRenderer does not support " << layer.renderer()->shadow_technique()
                       << "; using UNSHADOWED.";
      shadow_type = escher::PaperRendererShadowType::kNone;
  }

  DrawLayerWithPaperRenderer(frame, target_presentation_time, layer, shadow_type, render_target);
}

std::vector<escher::Camera> EngineRenderer::GenerateEscherCamerasForPaperRenderer(
    const escher::FramePtr& frame, Camera* camera, escher::ViewingVolume viewing_volume,
    zx::time target_presentation_time) {
  if (camera->IsKindOf<StereoCamera>()) {
    auto stereo_camera = camera->As<StereoCamera>();
    escher::Camera left_camera = stereo_camera->GetEscherCamera(StereoCamera::Eye::LEFT);
    escher::Camera right_camera = stereo_camera->GetEscherCamera(StereoCamera::Eye::RIGHT);

    escher::BufferPtr latched_pose_buffer;
    if (escher::hmd::PoseBuffer pose_buffer = camera->GetEscherPoseBuffer()) {
      latched_pose_buffer = pose_buffer_latching_shader_->LatchStereoPose(
          frame, left_camera, right_camera, pose_buffer, target_presentation_time.get());
      left_camera.SetLatchedPoseBuffer(latched_pose_buffer, escher::CameraEye::kLeft);
      right_camera.SetLatchedPoseBuffer(latched_pose_buffer, escher::CameraEye::kRight);
    }

    return {left_camera, right_camera};
  } else {
    escher::Camera escher_camera = camera->GetEscherCamera(viewing_volume);

    escher::BufferPtr latched_pose_buffer;
    if (escher::hmd::PoseBuffer pose_buffer = camera->GetEscherPoseBuffer()) {
      latched_pose_buffer = pose_buffer_latching_shader_->LatchPose(
          frame, escher_camera, pose_buffer, target_presentation_time.get());
      escher_camera.SetLatchedPoseBuffer(latched_pose_buffer, escher::CameraEye::kLeft);
    }

    return {escher_camera};
  }
}

void EngineRenderer::DrawLayerWithPaperRenderer(const escher::FramePtr& frame,
                                                zx::time target_presentation_time,
                                                const Layer& layer,
                                                const escher::PaperRendererShadowType shadow_type,
                                                const RenderTarget& render_target) {
  TRACE_DURATION("gfx", "EngineRenderer::DrawLayerWithPaperRenderer");

  frame->cmds()->AddWaitSemaphore(render_target.output_image_acquire_semaphore,
                                  vk::PipelineStageFlagBits::eColorAttachmentOutput);

  auto& renderer = layer.renderer();
  auto camera = renderer->camera();
  auto& scene = camera->scene();

  FX_DCHECK(paper_renderer_);
  paper_renderer_->SetConfig(escher::PaperRendererConfig {
    .shadow_type = shadow_type, .debug = renderer->enable_debugging(),
#if SCENIC_DISPLAY_FRAME_NUMBER
    .debug_frame_number = true,
#endif
    .depth_stencil_format = depth_stencil_format_,
  });

  // Set up PaperScene from Scenic Scene resource.
  auto paper_scene = fxl::MakeRefCounted<escher::PaperScene>();
  paper_scene->bounding_box = layer.GetViewingVolume().bounding_box();

  // Set up ambient light.
  if (scene->ambient_lights().empty()) {
    FX_LOGS(WARNING) << "scenic_impl::gfx::EngineRenderer: scene has no ambient light.";
    paper_scene->ambient_light.color = escher::vec3(0, 0, 0);
  } else {
    paper_scene->ambient_light.color = scene->ambient_lights()[0]->color();
  }

  // Set up point lights.
  paper_scene->point_lights.reserve(scene->point_lights().size());
  for (auto& light : scene->point_lights()) {
    paper_scene->point_lights.push_back(escher::PaperPointLight{
        .position = light->position(),
        .color = light->color(),
        .falloff = light->falloff(),
    });
  }

  auto gpu_uploader = std::make_shared<escher::BatchGpuUploader>(escher_, frame->frame_number());
  auto layout_updater = std::make_unique<escher::ImageLayoutUpdater>(escher_);

  FX_CHECK(render_target.output_image->layout() == vk::ImageLayout::eColorAttachmentOptimal)
      << "Layout of output image is not initialized.";

  paper_renderer_->BeginFrame(
      frame, gpu_uploader, paper_scene,
      GenerateEscherCamerasForPaperRenderer(frame, camera, layer.GetViewingVolume(),
                                            target_presentation_time),
      render_target.output_image);

  // TODO(fxbug.dev/24457): scene-visitation should generate cameras, collect
  // lights, etc.
  // Using resources allocated with protected memory on non-protected CommandBuffers is not allowed.

  // In order to avoid breaking access rules, we should replace them with non-protected materials
  // when using a non-protected |frame|.
  const bool hide_protected_memory = !frame->use_protected_memory();
  EngineRendererVisitor visitor(
      paper_renderer_.get(), gpu_uploader.get(), layout_updater.get(), hide_protected_memory,
      hide_protected_memory ? GetReplacementMaterial(gpu_uploader.get()) : nullptr);
  visitor.Visit(camera->scene().get());

  paper_renderer_->FinalizeFrame();

  escher::SemaphorePtr escher_image_updater_semaphore = escher::SemaphorePtr();
  if (gpu_uploader->NeedsCommandBuffer() || layout_updater->NeedsCommandBuffer()) {
    auto updater_frame =
        escher_->NewFrame("EngineRenderer uploads and image layout updates", frame->frame_number(),
                          /* enable_gpu_logging */ false, escher::CommandBuffer::Type::kTransfer,
                          /* use_protected_memory */ false);
    escher_image_updater_semaphore = escher::Semaphore::New(escher_->vk_device());

    // Note that only host images (except for directly-mapped images) will be
    // uploaded to GPU by BatchGpuUploader; and only device images (and
    // directly-mapped host images) will be initialized by ImageLayoutUpdater;
    // so we can submit all the commands into one single command buffer without
    // causing any problem.
    gpu_uploader->GenerateCommands(updater_frame->cmds());
    layout_updater->GenerateCommands(updater_frame->cmds());
    updater_frame->EndFrame(escher_image_updater_semaphore, []() {});
  }
  paper_renderer_->EndFrame(std::move(escher_image_updater_semaphore));
}

void EngineRenderer::WarmPipelineCache(std::set<vk::Format> framebuffer_formats) const {
  TRACE_DURATION("gfx", "EngineRenderer::WarmPipelineCache");

  escher::PaperRendererConfig config;
  config.shadow_type = escher::PaperRendererShadowType::kNone;
  config.msaa_sample_count = 1;
  config.depth_stencil_format = depth_stencil_format_;

  std::vector<escher::SamplerPtr> immutable_samplers;
  if (escher_->allow_ycbcr()) {
    // Generate the list of immutable samples for all of the YUV types that we expect to see.
    const std::vector<vk::Format> immutable_sampler_formats{vk::Format::eG8B8G8R8422Unorm,
                                                            vk::Format::eG8B8R82Plane420Unorm,
                                                            vk::Format::eG8B8R83Plane420Unorm};
    const std::vector<escher::ColorSpace> color_spaces{
        escher::ColorSpace::kRec709,
        escher::ColorSpace::kRec601Ntsc,
    };
    const auto vk_physical_device = escher_->vk_physical_device();
    for (auto fmt : immutable_sampler_formats) {
      for (auto color_space : color_spaces) {
        if (escher::impl::IsYuvConversionSupported(vk_physical_device, fmt)) {
          vk::Filter filter = vk::Filter::eNearest;
          if (vk_physical_device.getFormatProperties(fmt).optimalTilingFeatures &
              vk::FormatFeatureFlagBits::eSampledImageFilterLinear) {
            filter = vk::Filter::eLinear;
          }
          immutable_samplers.push_back(
              escher_->sampler_cache()->ObtainYuvSampler(fmt, filter, color_space));
        }
      }
    }
  }

  framebuffer_formats.insert(kIntermediateLayerFormat);
  for (auto fmt : framebuffer_formats) {
    // Depending on the memory types provided by the Vulkan implementation, separate versions of the
    // render-passes (and therefore pipelines) may be required for protected/non-protected memory.
    // Or not; if not, then the second call will simply use the ones that are already cached.
    escher::PaperRenderer::WarmPipelineAndRenderPassCaches(
        escher_.get(), config, fmt, vk::ImageLayout::eColorAttachmentOptimal, immutable_samplers,
        /*use_protected_memory*/ false);
    escher::PaperRenderer::WarmPipelineAndRenderPassCaches(
        escher_.get(), config, fmt, vk::ImageLayout::eColorAttachmentOptimal, immutable_samplers,
        /*use_protected_memory*/ true);
  }
}

escher::ImagePtr EngineRenderer::GetLayerFramebufferImage(uint32_t width, uint32_t height,
                                                          bool use_protected_memory) {
  escher::ImageInfo info;
  info.format = kIntermediateLayerFormat;
  info.width = width;
  info.height = height;
  info.usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled;
  info.color_space = kIntermediateLayerColorSpace;
  if (use_protected_memory) {
    info.memory_flags = vk::MemoryPropertyFlagBits::eProtected;
  }
  return escher_->image_cache()->NewImage(info);
}

escher::MaterialPtr EngineRenderer::GetReplacementMaterial(escher::BatchGpuUploader* gpu_uploader) {
  if (!replacement_material_) {
    FX_DCHECK(escher_);
    replacement_material_ = escher::Material::New(kReplacementMaterialColor);
  }
  return replacement_material_;
}

}  // namespace gfx
}  // namespace scenic_impl
