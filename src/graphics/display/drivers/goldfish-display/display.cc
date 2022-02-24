// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "display.h"

#include <fidl/fuchsia.hardware.goldfish/cpp/wire.h>
#include <fuchsia/sysmem/c/fidl.h>
#include <lib/async/cpp/task.h>
#include <lib/async/cpp/time.h>
#include <lib/async/cpp/wait.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/trace/event.h>
#include <lib/zircon-internal/align.h>
#include <zircon/pixelformat.h>
#include <zircon/threads.h>

#include <memory>
#include <sstream>
#include <vector>

#include <fbl/auto_lock.h>

#include "src/graphics/display/drivers/goldfish-display/goldfish-display-bind.h"

namespace goldfish {
namespace {

const char* kTag = "goldfish-display";

const char* kPipeName = "pipe:opengles";

constexpr uint64_t kPrimaryDisplayId = 1;

constexpr uint32_t kClientFlags = 0;

constexpr zx_pixel_format_t kPixelFormats[] = {
    ZX_PIXEL_FORMAT_RGB_x888,
    ZX_PIXEL_FORMAT_ARGB_8888,
    ZX_PIXEL_FORMAT_BGR_888x,
    ZX_PIXEL_FORMAT_ABGR_8888,
};

constexpr uint32_t FB_WIDTH = 1;
constexpr uint32_t FB_HEIGHT = 2;
constexpr uint32_t FB_FPS = 5;

constexpr uint32_t GL_RGBA = 0x1908;
constexpr uint32_t GL_BGRA_EXT = 0x80E1;
constexpr uint32_t GL_UNSIGNED_BYTE = 0x1401;

struct GetFbParamCmd {
  uint32_t op;
  uint32_t size;
  uint32_t param;
};
constexpr uint32_t kOP_rcGetFbParam = 10007;
constexpr uint32_t kSize_rcGetFbParam = 12;

struct CreateColorBufferCmd {
  uint32_t op;
  uint32_t size;
  uint32_t width;
  uint32_t height;
  uint32_t internalformat;
};
constexpr uint32_t kOP_rcCreateColorBuffer = 10012;
constexpr uint32_t kSize_rcCreateColorBuffer = 20;

struct OpenColorBufferCmd {
  uint32_t op;
  uint32_t size;
  uint32_t id;
};
constexpr uint32_t kOP_rcOpenColorBuffer = 10013;
constexpr uint32_t kSize_rcOpenColorBuffer = 12;

struct CloseColorBufferCmd {
  uint32_t op;
  uint32_t size;
  uint32_t id;
};
constexpr uint32_t kOP_rcCloseColorBuffer = 10014;
constexpr uint32_t kSize_rcCloseColorBuffer = 12;

struct SetColorBufferVulkanModeCmd {
  uint32_t op;
  uint32_t size;
  uint32_t id;
  uint32_t mode;
};
constexpr uint32_t kOP_rcSetColorBufferVulkanMode = 10045;
constexpr uint32_t kSize_rcSetColorBufferVulkanMode = 16;

struct UpdateColorBufferCmd {
  uint32_t op;
  uint32_t size;
  uint32_t id;
  uint32_t x;
  uint32_t y;
  uint32_t width;
  uint32_t height;
  uint32_t format;
  uint32_t type;
  uint32_t size_pixels;
};
constexpr uint32_t kOP_rcUpdateColorBuffer = 10024;
constexpr uint32_t kSize_rcUpdateColorBuffer = 40;

struct FbPostCmd {
  uint32_t op;
  uint32_t size;
  uint32_t id;
};
constexpr uint32_t kOP_rcFbPost = 10018;
constexpr uint32_t kSize_rcFbPost = 12;

struct CreateDisplayCmd {
  uint32_t op;
  uint32_t size;
  uint32_t size_display_id;
};
constexpr uint32_t kOP_rcCreateDisplay = 10038;
constexpr uint32_t kSize_rcCreateDisplay = 12;

struct DestroyDisplayCmd {
  uint32_t op;
  uint32_t size;
  uint32_t display_id;
};
constexpr uint32_t kOP_rcDestroyDisplay = 10039;
constexpr uint32_t kSize_rcDestroyDisplay = 12;

struct SetDisplayColorBufferCmd {
  uint32_t op;
  uint32_t size;
  uint32_t display_id;
  uint32_t id;
};
constexpr uint32_t kOP_rcSetDisplayColorBuffer = 10040;
constexpr uint32_t kSize_rcSetDisplayColorBuffer = 16;

struct SetDisplayPoseCmd {
  uint32_t op;
  uint32_t size;
  uint32_t display_id;
  int32_t x;
  int32_t y;
  uint32_t w;
  uint32_t h;
};
constexpr uint32_t kOP_rcSetDisplayPose = 10044;
constexpr uint32_t kSize_rcSetDisplayPose = 28;

}  // namespace

// static
zx_status_t Display::Create(void* ctx, zx_device_t* device) {
  auto display = std::make_unique<Display>(device);

  zx_status_t status = display->Bind();
  if (status == ZX_OK) {
    // devmgr now owns device.
    __UNUSED auto* dev = display.release();
  }
  return status;
}

Display::Display(zx_device_t* parent)
    : DisplayType(parent), loop_(&kAsyncLoopConfigNeverAttachToThread) {
  if (parent) {
    control_ = parent;
    pipe_ = parent;
  }
}

Display::~Display() {
  loop_.Shutdown();

  for (auto& it : devices_) {
    TeardownDisplay(it.first);
  }

  if (id_) {
    fbl::AutoLock lock(&lock_);
    if (cmd_buffer_.is_valid()) {
      auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
      buffer->id = id_;
      buffer->cmd = PIPE_CMD_CODE_CLOSE;
      buffer->status = PIPE_ERROR_INVAL;

      pipe_.Exec(id_);
      ZX_DEBUG_ASSERT(!buffer->status);
    }
    pipe_.Destroy(id_);
  }
}

zx_status_t Display::Bind() {
  fbl::AutoLock lock(&lock_);

  if (!control_.is_valid()) {
    zxlogf(ERROR, "%s: no control protocol", kTag);
    return ZX_ERR_NOT_SUPPORTED;
  }

  if (!pipe_.is_valid()) {
    zxlogf(ERROR, "%s: no pipe protocol", kTag);
    return ZX_ERR_NOT_SUPPORTED;
  }

  zx_status_t status = pipe_.GetBti(&bti_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: GetBti failed: %d", kTag, status);
    return status;
  }

  status = io_buffer_.Init(bti_.get(), PAGE_SIZE, IO_BUFFER_RW | IO_BUFFER_CONTIG);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: io_buffer_init failed: %d", kTag, status);
    return status;
  }

  status = zx::event::create(0u, &pipe_event_);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_event_create failed: %d", kTag, status);
    return status;
  }

  zx::event pipe_event_dup;
  status = pipe_event_.duplicate(ZX_RIGHT_SAME_RIGHTS, &pipe_event_dup);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_handle_duplicate failed: %d", kTag, status);
    return status;
  }

  zx::vmo vmo;
  status = pipe_.Create(&id_, &vmo);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Create failed: %d", kTag, status);
    return status;
  }
  status = pipe_.SetEvent(id_, std::move(pipe_event_dup));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: SetEvent failed: %d", kTag, status);
    return status;
  }

  status = cmd_buffer_.InitVmo(bti_.get(), vmo.get(), 0, IO_BUFFER_RW);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: io_buffer_init_vmo failed: %d", kTag, status);
    return status;
  }

  auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
  buffer->id = id_;
  buffer->cmd = PIPE_CMD_CODE_OPEN;
  buffer->status = PIPE_ERROR_INVAL;

  pipe_.Open(id_);
  if (buffer->status) {
    zxlogf(ERROR, "%s: Open failed: %d", kTag, buffer->status);
    cmd_buffer_.release();
    return ZX_ERR_INTERNAL;
  }

  size_t length = strlen(kPipeName) + 1;
  memcpy(io_buffer_.virt(), kPipeName, length);
  status = WriteLocked(static_cast<uint32_t>(length));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Pipe name write failed: %d", kTag, status);
    return status;
  }

  memcpy(io_buffer_.virt(), &kClientFlags, sizeof(kClientFlags));
  status = WriteLocked(sizeof(kClientFlags));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: Client flags write failed: %d", kTag, status);
    return status;
  }

  uint64_t next_display_id = kPrimaryDisplayId;

  // Parse optional display params. This is comma seperated list of
  // display devices. The format is:
  //
  // widthxheight[-xpos+ypos][@refresh][%scale]
  char* flag = getenv("driver.goldfish.displays");
  if (flag) {
    std::stringstream devices_stream(flag);
    std::string device_string;
    while (std::getline(devices_stream, device_string, ',')) {
      Device device;
      char delim = 0;
      std::stringstream device_stream(device_string);
      do {
        switch (delim) {
          case 0:
            device_stream >> device.width;
            break;
          case 'x':
            device_stream >> device.height;
            break;
          case '-':
            device_stream >> device.x;
            break;
          case '+':
            device_stream >> device.y;
            break;
          case '@':
            device_stream >> device.refresh_rate_hz;
            break;
          case '%':
            device_stream >> device.scale;
            break;
        }
      } while (device_stream >> delim);

      if (!device.width || !device.height) {
        zxlogf(ERROR, "%s: skip device=%s, missing size", kTag, device_string.c_str());
        continue;
      }
      if (!device.refresh_rate_hz) {
        zxlogf(ERROR, "%s: skip device=%s, refresh rate is zero", kTag, device_string.c_str());
        continue;
      }
      if (device.scale < 0.1f || device.scale > 100.f) {
        zxlogf(ERROR, "%s: skip device=%s, scale is not in range 0.1-100", kTag,
               device_string.c_str());
        continue;
      }

      devices_[next_display_id++] = device;
    }
  }

  // Create primary device if needed.
  if (devices_.empty()) {
    Device device;
    device.width = static_cast<uint32_t>(GetFbParamLocked(FB_WIDTH, 1024));
    device.height = static_cast<uint32_t>(GetFbParamLocked(FB_HEIGHT, 768));
    device.refresh_rate_hz = static_cast<uint32_t>(GetFbParamLocked(FB_FPS, 60));
    devices_[kPrimaryDisplayId] = device;
  }

  // Set up display and set up flush task for each device.
  for (auto& it : devices_) {
    zx_status_t status = SetupDisplayLocked(it.first);
    ZX_DEBUG_ASSERT(status == ZX_OK);

    async::PostTask(loop_.dispatcher(), [this, display_id = it.first] {
      FlushDisplay(loop_.dispatcher(), display_id);
    });
  }

  // Start async event thread.
  loop_.StartThread("goldfish_display_event_thread");

  return DdkAdd("goldfish-display");
}

void Display::DdkRelease() { delete this; }

void Display::DisplayControllerImplSetDisplayControllerInterface(
    const display_controller_interface_protocol_t* interface) {
  std::vector<added_display_args_t> args;
  for (auto& it : devices_) {
    added_display_args_t display = {
        .display_id = it.first,
        .edid_present = false,
        .panel =
            {
                .params =
                    {
                        .width = it.second.width,
                        .height = it.second.height,
                        .refresh_rate_e2 = it.second.refresh_rate_hz * 100,
                    },
            },
        .pixel_format_list = kPixelFormats,
        .pixel_format_count = sizeof(kPixelFormats) / sizeof(kPixelFormats[0]),
        .cursor_info_list = nullptr,
        .cursor_info_count = 0,
    };
    args.push_back(display);
  }

  {
    fbl::AutoLock lock(&flush_lock_);
    dc_intf_ = ddk::DisplayControllerInterfaceProtocolClient(interface);
    dc_intf_.OnDisplaysChanged(args.data(), args.size(), nullptr, 0, nullptr, 0, nullptr);
  }
}

zx_status_t Display::ImportVmoImage(image_t* image, zx::vmo vmo, size_t offset) {
  auto color_buffer = std::make_unique<ColorBuffer>();

  // Linear images must be pinned.
  unsigned pixel_size = ZX_PIXEL_FORMAT_BYTES(image->pixel_format);
  color_buffer->size = ZX_ROUNDUP(image->width * image->height * pixel_size, PAGE_SIZE);
  zx_status_t status = bti_.pin(ZX_BTI_PERM_READ | ZX_BTI_CONTIGUOUS, vmo, offset,
                                color_buffer->size, &color_buffer->paddr, 1, &color_buffer->pmt);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to pin VMO: %d", kTag, status);
    return status;
  }

  uint32_t format = (image->pixel_format == ZX_PIXEL_FORMAT_RGB_x888 ||
                     image->pixel_format == ZX_PIXEL_FORMAT_ARGB_8888)
                        ? GL_BGRA_EXT
                        : GL_RGBA;

  color_buffer->vmo = std::move(vmo);
  color_buffer->width = image->width;
  color_buffer->height = image->height;
  color_buffer->format = format;

  {
    fbl::AutoLock lock(&lock_);
    status = CreateColorBufferLocked(image->width, image->height, format, &color_buffer->id);
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: failed to create color buffer", kTag);
      return status;
    }
  }

  image->handle = reinterpret_cast<uint64_t>(color_buffer.release());
  return ZX_OK;
}

zx_status_t Display::DisplayControllerImplImportImage(image_t* image, zx_unowned_handle_t handle,
                                                      uint32_t index) {
  zx_status_t status, status2;
  fuchsia_sysmem_BufferCollectionInfo_2 collection_info;
  status =
      fuchsia_sysmem_BufferCollectionWaitForBuffersAllocated(handle, &status2, &collection_info);
  if (status != ZX_OK) {
    return status;
  }
  if (status2 != ZX_OK) {
    return status2;
  }

  zx::vmo vmo;
  if (index < collection_info.buffer_count) {
    vmo = zx::vmo(collection_info.buffers[index].vmo);
    collection_info.buffers[index].vmo = ZX_HANDLE_INVALID;
  }
  for (uint32_t i = 0; i < collection_info.buffer_count; ++i) {
    zx_handle_close(collection_info.buffers[i].vmo);
  }

  if (!vmo.is_valid()) {
    zxlogf(ERROR, "%s: invalid index", kTag);
    return ZX_ERR_OUT_OF_RANGE;
  }

  uint64_t offset = collection_info.buffers[index].vmo_usable_start;

  if (collection_info.settings.buffer_settings.heap !=
      fuchsia_sysmem_HeapType_GOLDFISH_DEVICE_LOCAL) {
    return ImportVmoImage(image, std::move(vmo), offset);
  }

  if (!collection_info.settings.has_image_format_constraints || offset) {
    zxlogf(ERROR, "%s: invalid image format or offset", kTag);
    return ZX_ERR_OUT_OF_RANGE;
  }

  auto color_buffer = std::make_unique<ColorBuffer>();
  color_buffer->vmo = std::move(vmo);
  image->handle = reinterpret_cast<uint64_t>(color_buffer.release());
  return ZX_OK;
}

void Display::DisplayControllerImplReleaseImage(image_t* image) {
  auto color_buffer = reinterpret_cast<ColorBuffer*>(image->handle);

  // Color buffer is owned by image in the linear case.
  if (image->type == IMAGE_TYPE_SIMPLE) {
    fbl::AutoLock lock(&lock_);
    CloseColorBufferLocked(color_buffer->id);
  }

  async::PostTask(loop_.dispatcher(), [this, color_buffer] {
    const auto color_buffer_maps = {&current_config_, &pending_config_};
    for (const auto map : color_buffer_maps) {
      for (const auto& kv : *map) {
        if (kv.second.color_buffer == color_buffer) {
          map->at(kv.first).color_buffer = nullptr;
          map->at(kv.first).config_stamp = {.value = INVALID_CONFIG_STAMP_VALUE};
        }
      }
    }
    delete color_buffer;
  });
}

uint32_t Display::DisplayControllerImplCheckConfiguration(const display_config_t** display_configs,
                                                          size_t display_count,
                                                          uint32_t** layer_cfg_results,
                                                          size_t* layer_cfg_result_count) {
  if (display_count == 0) {
    return CONFIG_DISPLAY_OK;
  }
  for (unsigned i = 0; i < display_count; i++) {
    const size_t layer_count = display_configs[i]->layer_count;
    if (layer_count > 0) {
      ZX_DEBUG_ASSERT(devices_.find(display_configs[i]->display_id) != devices_.end());
      const Device& device = devices_[display_configs[i]->display_id];

      if (display_configs[i]->cc_flags != 0) {
        // Color Correction is not supported, but we will pretend we do.
        // TODO(fxbug.dev/36184): Returning error will cause blank screen if scenic requests
        // color correction. For now, lets pretend we support it, until a proper
        // fix is done (either from scenic or from core display)
        zxlogf(WARNING, "%s: Color Correction not support. No error reported", __func__);
      }

      if (display_configs[i]->layer_list[0]->type != LAYER_TYPE_PRIMARY) {
        // We only support PRIMARY layer. Notify client to convert layer to
        // primary type.
        layer_cfg_results[i][0] |= CLIENT_USE_PRIMARY;
        layer_cfg_result_count[i] = 1;
      } else {
        primary_layer_t* layer = &display_configs[i]->layer_list[0]->cfg.primary;
        // Scaling is allowed if destination frame match display and
        // source frame match image.
        frame_t dest_frame = {
            .x_pos = 0,
            .y_pos = 0,
            .width = device.width,
            .height = device.height,
        };
        frame_t src_frame = {
            .x_pos = 0,
            .y_pos = 0,
            .width = layer->image.width,
            .height = layer->image.height,
        };
        if (memcmp(&layer->dest_frame, &dest_frame, sizeof(frame_t)) != 0) {
          // TODO(fxbug.dev/36222): Need to provide proper flag to indicate driver only
          // accepts full screen dest frame.
          layer_cfg_results[i][0] |= CLIENT_FRAME_SCALE;
        }
        if (memcmp(&layer->src_frame, &src_frame, sizeof(frame_t)) != 0) {
          layer_cfg_results[i][0] |= CLIENT_SRC_FRAME;
        }

        if (layer->alpha_mode != ALPHA_DISABLE) {
          // Alpha is not supported.
          layer_cfg_results[i][0] |= CLIENT_ALPHA;
        }

        if (layer->transform_mode != FRAME_TRANSFORM_IDENTITY) {
          // Transformation is not supported.
          layer_cfg_results[i][0] |= CLIENT_TRANSFORM;
        }

        // Check if any changes to the base layer were required.
        if (layer_cfg_results[i][0] != 0) {
          layer_cfg_result_count[i] = 1;
        }
      }
      // If there is more than one layer, the rest need to be merged into the base layer.
      if (layer_count > 1) {
        layer_cfg_results[i][0] |= CLIENT_MERGE_BASE;
        for (unsigned j = 1; j < layer_count; j++) {
          layer_cfg_results[i][j] |= CLIENT_MERGE_SRC;
        }
        layer_cfg_result_count[i] = layer_count;
      }
    }
  }
  return CONFIG_DISPLAY_OK;
}

zx_status_t Display::PresentColorBuffer(uint32_t display_id, const DisplayConfig& display_config) {
  auto* color_buffer = display_config.color_buffer;
  if (!color_buffer) {
    return ZX_HANDLE_INVALID;
  }
  if (color_buffer->sync_event.is_valid()) {
    return ZX_ERR_SHOULD_WAIT;
  }

  zx::eventpair event_display, event_sync_device;
  zx_status_t status = zx::eventpair::create(0u, &event_display, &event_sync_device);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: zx_eventpair_create failed: %d", kTag, status);
    return status;
  }
  color_buffer->sync_event = std::move(event_display);

  // Set up async wait.
  color_buffer->async_wait =
      std::make_unique<async::WaitOnce>(color_buffer->sync_event.get(), ZX_EVENTPAIR_SIGNALED, 0u);
  color_buffer->async_wait->Begin(
      loop_.dispatcher(), [this, display_config, color_buffer, display_id](
                              async_dispatcher_t* dispatcher, async::WaitOnce* wait,
                              zx_status_t status, const zx_packet_signal_t* signal) {
        TRACE_DURATION("gfx", "Display::SyncEventHandler", "color_buffer", color_buffer->id);
        if (status == ZX_ERR_CANCELED) {
          zxlogf(INFO, "Wait cancelled.\n");
          return;
        }
        ZX_DEBUG_ASSERT_MSG(status == ZX_OK, "Invalid wait status: %d\n", status);

        color_buffer->async_wait.reset();
        color_buffer->sync_event.reset();
        current_config_[display_id] = display_config;
      });

  // Update host-writeable display buffers before presenting.
  if (color_buffer->paddr) {
    fbl::AutoLock lock(&lock_);
    uint32_t result;
    zx_status_t status = UpdateColorBufferLocked(color_buffer->id, color_buffer->paddr,
                                                 color_buffer->width, color_buffer->height,
                                                 color_buffer->format, color_buffer->size, &result);
    if (status != ZX_OK || result) {
      zxlogf(ERROR, "%s : color buffer update failed: %d:%u", kTag, status, result);
      return status;
    }
  }

  // Present the buffer.
  {
    fbl::AutoLock lock(&lock_);

    uint32_t host_display_id = devices_[display_id].host_display_id;
    if (host_display_id) {
      // Set color buffer for secondary displays.
      uint32_t result;
      status = SetDisplayColorBufferLocked(host_display_id, color_buffer->id, &result);
      if (status != ZX_OK || result) {
        zxlogf(ERROR, "%s: failed to set display color buffer", kTag);
        return status;
      }
    } else {
      status = FbPostLocked(color_buffer->id);
      if (status != ZX_OK) {
        zxlogf(ERROR, "%s: FbPost failed: %d", kTag, status);
        return status;
      }
    }

    status = control_.CreateSyncFence(std::move(event_sync_device));
    if (status != ZX_OK) {
      zxlogf(ERROR, "%s: CreateSyncFence failed: %d", kTag, status);
      return status;
    }
  }

  return ZX_OK;
}

void Display::DisplayControllerImplApplyConfiguration(const display_config_t** display_configs,
                                                      size_t display_count,
                                                      const config_stamp_t* config_stamp) {
  for (auto it : devices_) {
    uint64_t handle = 0;
    for (unsigned i = 0; i < display_count; i++) {
      if (display_configs[i]->display_id == it.first) {
        if (display_configs[i]->layer_count) {
          handle = display_configs[i]->layer_list[0]->cfg.primary.image.handle;
        }
        break;
      }
    }

    if (handle == 0u) {
      // The display doesn't have any active layers right now. For layers that
      // previously existed, we should cancel waiting events on the pending
      // color buffer and remove references to both pending and current color
      // buffers.
      async::PostTask(loop_.dispatcher(),
                      [this, display_id = it.first, config_stamp = *config_stamp] {
                        if (pending_config_[display_id].color_buffer &&
                            pending_config_[display_id].color_buffer->async_wait) {
                          pending_config_[display_id].color_buffer->async_wait->Cancel();
                        }
                        pending_config_.erase(display_id);
                        current_config_.erase(display_id);
                        latest_config_stamp_ = config_stamp;
                      });
      return;
    }

    auto color_buffer = reinterpret_cast<ColorBuffer*>(handle);
    if (color_buffer && !color_buffer->id) {
      zx::vmo vmo;

      zx_status_t status = color_buffer->vmo.duplicate(ZX_RIGHT_SAME_RIGHTS, &vmo);
      if (status != ZX_OK) {
        zxlogf(ERROR, "%s: failed to duplicate vmo: %d", kTag, status);
      } else {
        fbl::AutoLock lock(&lock_);

        status = control_.GetColorBuffer(std::move(vmo), &color_buffer->id);
        if (status != ZX_OK) {
          zxlogf(ERROR, "%s: failed to get color buffer: %d", kTag, status);
        }

        // Color buffers are in vulkan-only mode by default as that avoids
        // unnecessary copies on the host in some cases. The color buffer
        // needs to be moved out of vulkan-only mode before being used for
        // presentation.
        if (color_buffer->id) {
          uint32_t result = 0;
          status = SetColorBufferVulkanModeLocked(color_buffer->id, 0, &result);
          if (status != ZX_OK || result) {
            zxlogf(ERROR, "%s: failed to set vulkan mode: %d %d", kTag, status, result);
          }
        }
      }
    }

    if (color_buffer) {
      async::PostTask(loop_.dispatcher(),
                      [this, config_stamp = *config_stamp, color_buffer, display_id = it.first] {
                        pending_config_[display_id] = {
                            .color_buffer = color_buffer,
                            .config_stamp = config_stamp,
                        };
                      });
    }
  }
}

zx_status_t Display::DisplayControllerImplGetSysmemConnection(zx::channel connection) {
  fbl::AutoLock lock(&lock_);

  zx_status_t status = pipe_.ConnectSysmem(std::move(connection));
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to connect to sysmem: %d", kTag, status);
    return status;
  }
  return ZX_OK;
}

zx_status_t Display::DisplayControllerImplSetBufferCollectionConstraints(const image_t* config,
                                                                         uint32_t collection) {
  fuchsia_sysmem_BufferCollectionConstraints constraints = {};
  constraints.usage.display = fuchsia_sysmem_displayUsageLayer;
  constraints.has_buffer_memory_constraints = true;
  fuchsia_sysmem_BufferMemoryConstraints& buffer_constraints =
      constraints.buffer_memory_constraints;
  buffer_constraints.min_size_bytes = 0;
  buffer_constraints.max_size_bytes = 0xffffffff;
  buffer_constraints.physically_contiguous_required = true;
  buffer_constraints.secure_required = false;
  buffer_constraints.ram_domain_supported = true;
  buffer_constraints.cpu_domain_supported = true;
  buffer_constraints.inaccessible_domain_supported = true;
  buffer_constraints.heap_permitted_count = 2;
  buffer_constraints.heap_permitted[0] = fuchsia_sysmem_HeapType_SYSTEM_RAM;
  buffer_constraints.heap_permitted[1] = fuchsia_sysmem_HeapType_GOLDFISH_DEVICE_LOCAL;
  constraints.image_format_constraints_count = 4;
  for (uint32_t i = 0; i < constraints.image_format_constraints_count; i++) {
    fuchsia_sysmem_ImageFormatConstraints& image_constraints =
        constraints.image_format_constraints[i];
    image_constraints.pixel_format.type =
        i & 0b01 ? fuchsia_sysmem_PixelFormatType_R8G8B8A8 : fuchsia_sysmem_PixelFormatType_BGRA32;
    image_constraints.pixel_format.has_format_modifier = true;
    image_constraints.pixel_format.format_modifier.value =
        i & 0b10 ? fuchsia_sysmem_FORMAT_MODIFIER_LINEAR
                 : fuchsia_sysmem_FORMAT_MODIFIER_GOOGLE_GOLDFISH_OPTIMAL;
    image_constraints.color_spaces_count = 1;
    image_constraints.color_space[0].type = fuchsia_sysmem_ColorSpaceType_SRGB;
    image_constraints.min_coded_width = 0;
    image_constraints.max_coded_width = 0xffffffff;
    image_constraints.min_coded_height = 0;
    image_constraints.max_coded_height = 0xffffffff;
    image_constraints.min_bytes_per_row = 0;
    image_constraints.max_bytes_per_row = 0xffffffff;
    image_constraints.max_coded_width_times_coded_height = 0xffffffff;
    image_constraints.layers = 1;
    image_constraints.coded_width_divisor = 1;
    image_constraints.coded_height_divisor = 1;
    image_constraints.bytes_per_row_divisor = 1;
    image_constraints.start_offset_divisor = 1;
    image_constraints.display_width_divisor = 1;
    image_constraints.display_height_divisor = 1;
  }

  zx_status_t status =
      fuchsia_sysmem_BufferCollectionSetConstraints(collection, true, &constraints);
  if (status != ZX_OK) {
    zxlogf(ERROR, "%s: failed to set constraints", kTag);
    return status;
  }
  return ZX_OK;
}

zx_status_t Display::WriteLocked(uint32_t cmd_size) {
  TRACE_DURATION("gfx", "Display::Write", "cmd_size", cmd_size);

  auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
  uint32_t remaining = cmd_size;
  while (remaining) {
    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_WRITE;
    buffer->status = PIPE_ERROR_INVAL;
    buffer->rw_params.ptrs[0] = io_buffer_.phys() + cmd_size - remaining;
    buffer->rw_params.sizes[0] = remaining;
    buffer->rw_params.buffers_count = 1;
    buffer->rw_params.consumed_size = 0;
    pipe_.Exec(id_);

    if (buffer->rw_params.consumed_size) {
      remaining -= buffer->rw_params.consumed_size;
      continue;
    }

    // Early out if error is not because of back-pressure.
    if (buffer->status != PIPE_ERROR_AGAIN) {
      zxlogf(ERROR, "%s: write to pipe buffer failed: %d", kTag, buffer->status);
      return ZX_ERR_INTERNAL;
    }

    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_WAKE_ON_WRITE;
    buffer->status = PIPE_ERROR_INVAL;
    pipe_.Exec(id_);

    // Wait for pipe to become writable.
    zx_status_t status = pipe_event_.wait_one(fuchsia_hardware_goldfish::wire::kSignalHangup |
                                                  fuchsia_hardware_goldfish::wire::kSignalWritable,
                                              zx::time::infinite(), nullptr);
    if (status != ZX_OK) {
      if (status != ZX_ERR_CANCELED) {
        zxlogf(ERROR, "%s: zx_object_wait_one failed: %d", kTag, status);
      }
      return status;
    }
  }
  return ZX_OK;
}

zx_status_t Display::ReadResultLocked(uint32_t* result, uint32_t count) {
  TRACE_DURATION("gfx", "Display::ReadResult");

  size_t length = sizeof(*result) * count;
  size_t remaining = length;
  while (remaining) {
    auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_READ;
    buffer->status = PIPE_ERROR_INVAL;
    buffer->rw_params.ptrs[0] = io_buffer_.phys();
    buffer->rw_params.sizes[0] = static_cast<uint32_t>(remaining);
    buffer->rw_params.buffers_count = 1;
    buffer->rw_params.consumed_size = 0;
    pipe_.Exec(id_);

    // Positive consumed size always indicate a successful transfer.
    if (buffer->rw_params.consumed_size) {
      memcpy(reinterpret_cast<char*>(result) + (length - remaining), io_buffer_.virt(),
             buffer->rw_params.consumed_size);
      remaining -= buffer->rw_params.consumed_size;
      continue;
    }

    // Early out if error is not because of back-pressure.
    if (buffer->status != PIPE_ERROR_AGAIN) {
      zxlogf(ERROR, "%s: reading result failed: %d", kTag, buffer->status);
      return ZX_ERR_INTERNAL;
    }

    buffer->id = id_;
    buffer->cmd = PIPE_CMD_CODE_WAKE_ON_READ;
    buffer->status = PIPE_ERROR_INVAL;
    pipe_.Exec(id_);
    ZX_DEBUG_ASSERT(!buffer->status);

    // Wait for pipe to become readable.
    zx_status_t status = pipe_event_.wait_one(fuchsia_hardware_goldfish::wire::kSignalHangup |
                                                  fuchsia_hardware_goldfish::wire::kSignalReadable,
                                              zx::time::infinite(), nullptr);
    if (status != ZX_OK) {
      if (status != ZX_ERR_CANCELED) {
        zxlogf(ERROR, "%s: zx_object_wait_one failed: %d", kTag, status);
      }
      return status;
    }
  }

  return ZX_OK;
}

zx_status_t Display::ExecuteCommandLocked(uint32_t cmd_size, uint32_t* result) {
  TRACE_DURATION("gfx", "Display::ExecuteCommand", "cmd_size", cmd_size);

  zx_status_t status = WriteLocked(cmd_size);
  if (status != ZX_OK) {
    return status;
  }
  return ReadResultLocked(result, 1);
}

int32_t Display::GetFbParamLocked(uint32_t param, int32_t default_value) {
  TRACE_DURATION("gfx", "Display::GetFbParam", "param", param);

  auto cmd = static_cast<GetFbParamCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcGetFbParam;
  cmd->size = kSize_rcGetFbParam;
  cmd->param = param;

  uint32_t result;
  zx_status_t status = ExecuteCommandLocked(kSize_rcGetFbParam, &result);
  return status == ZX_OK ? result : default_value;
}

zx_status_t Display::CreateColorBufferLocked(uint32_t width, uint32_t height, uint32_t format,
                                             uint32_t* id) {
  TRACE_DURATION("gfx", "Display::CreateColorBuffer", "width", width, "height", height, "format",
                 format);

  auto cmd = static_cast<CreateColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCreateColorBuffer;
  cmd->size = kSize_rcCreateColorBuffer;
  cmd->width = width;
  cmd->height = height;
  cmd->internalformat = format;

  return ExecuteCommandLocked(kSize_rcCreateColorBuffer, id);
}

zx_status_t Display::OpenColorBufferLocked(uint32_t id) {
  TRACE_DURATION("gfx", "Display::OpenColorBuffer", "id", id);

  auto cmd = static_cast<OpenColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcOpenColorBuffer;
  cmd->size = kSize_rcOpenColorBuffer;
  cmd->id = id;

  return WriteLocked(kSize_rcOpenColorBuffer);
}

zx_status_t Display::CloseColorBufferLocked(uint32_t id) {
  TRACE_DURATION("gfx", "Display::CloseColorBuffer", "id", id);

  auto cmd = static_cast<CloseColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCloseColorBuffer;
  cmd->size = kSize_rcCloseColorBuffer;
  cmd->id = id;

  return WriteLocked(kSize_rcCloseColorBuffer);
}

zx_status_t Display::SetColorBufferVulkanModeLocked(uint32_t id, uint32_t mode, uint32_t* result) {
  TRACE_DURATION("gfx", "Display::SetColorBufferVulkanMode", "id", id, "mode", mode);

  auto cmd = static_cast<SetColorBufferVulkanModeCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcSetColorBufferVulkanMode;
  cmd->size = kSize_rcSetColorBufferVulkanMode;
  cmd->id = id;
  cmd->mode = mode;

  return ExecuteCommandLocked(kSize_rcSetColorBufferVulkanMode, result);
}

zx_status_t Display::UpdateColorBufferLocked(uint32_t id, zx_paddr_t paddr, uint32_t width,
                                             uint32_t height, uint32_t format, size_t size,
                                             uint32_t* result) {
  TRACE_DURATION("gfx", "Display::UpdateColorBuffer", "size", size);

  auto cmd = static_cast<UpdateColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcUpdateColorBuffer;
  cmd->size = kSize_rcUpdateColorBuffer + static_cast<uint32_t>(size);
  cmd->id = id;
  cmd->x = 0;
  cmd->y = 0;
  cmd->width = width;
  cmd->height = height;
  cmd->format = format;
  cmd->type = GL_UNSIGNED_BYTE;
  cmd->size_pixels = static_cast<uint32_t>(size);

  auto buffer = static_cast<pipe_cmd_buffer_t*>(cmd_buffer_.virt());
  buffer->id = id_;
  buffer->cmd = PIPE_CMD_CODE_WRITE;
  buffer->status = PIPE_ERROR_INVAL;
  buffer->rw_params.ptrs[0] = io_buffer_.phys();
  buffer->rw_params.ptrs[1] = paddr;
  buffer->rw_params.sizes[0] = kSize_rcUpdateColorBuffer;
  buffer->rw_params.sizes[1] = static_cast<uint32_t>(size);
  buffer->rw_params.buffers_count = 2;
  buffer->rw_params.consumed_size = 0;

  pipe_.Exec(id_);
  ZX_DEBUG_ASSERT(buffer->rw_params.consumed_size ==
                  static_cast<int32_t>(kSize_rcUpdateColorBuffer + size));

  return ReadResultLocked(result, 1);
}

zx_status_t Display::FbPostLocked(uint32_t id) {
  TRACE_DURATION("gfx", "Display::FbPost", "id", id);

  auto cmd = static_cast<FbPostCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcFbPost;
  cmd->size = kSize_rcFbPost;
  cmd->id = id;

  return WriteLocked(kSize_rcFbPost);
}

zx_status_t Display::CreateDisplayLocked(uint32_t* result) {
  TRACE_DURATION("gfx", "Display::CreateDisplay");

  auto cmd = static_cast<CreateDisplayCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcCreateDisplay;
  cmd->size = kSize_rcCreateDisplay;
  cmd->size_display_id = sizeof(uint32_t);

  zx_status_t status = WriteLocked(kSize_rcCreateDisplay);
  if (status != ZX_OK) {
    return status;
  }
  return ReadResultLocked(result, 2);
}

zx_status_t Display::DestroyDisplayLocked(uint32_t display_id, uint32_t* result) {
  TRACE_DURATION("gfx", "Display::DestroyDisplay", "display_id", display_id);

  auto cmd = static_cast<DestroyDisplayCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcDestroyDisplay;
  cmd->size = kSize_rcDestroyDisplay;
  cmd->display_id = display_id;

  return ExecuteCommandLocked(kSize_rcDestroyDisplay, result);
}

zx_status_t Display::SetDisplayColorBufferLocked(uint32_t display_id, uint32_t id,
                                                 uint32_t* result) {
  TRACE_DURATION("gfx", "Display::SetDisplayColorBuffer", "display_id", display_id, "id", id);

  auto cmd = static_cast<SetDisplayColorBufferCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcSetDisplayColorBuffer;
  cmd->size = kSize_rcSetDisplayColorBuffer;
  cmd->display_id = display_id;
  cmd->id = id;

  return ExecuteCommandLocked(kSize_rcSetDisplayColorBuffer, result);
}

zx_status_t Display::SetDisplayPoseLocked(uint32_t display_id, int32_t x, int32_t y, uint32_t w,
                                          uint32_t h, uint32_t* result) {
  TRACE_DURATION("gfx", "Display::SetDisplayPose", "display_id", display_id);

  auto cmd = static_cast<SetDisplayPoseCmd*>(io_buffer_.virt());
  cmd->op = kOP_rcSetDisplayPose;
  cmd->size = kSize_rcSetDisplayPose;
  cmd->display_id = display_id;
  cmd->x = x;
  cmd->y = y;
  cmd->w = w;
  cmd->h = h;

  return ExecuteCommandLocked(kSize_rcSetDisplayPose, result);
}

zx_status_t Display::SetupDisplayLocked(uint64_t display_id) {
  Device& device = devices_[display_id];

  // Create secondary displays.
  if (display_id != kPrimaryDisplayId) {
    uint32_t result[2] = {0, 1};
    zx_status_t status = CreateDisplayLocked(result);
    if (status != ZX_OK || result[1]) {
      zxlogf(ERROR, "%s: failed to create display: %d %d", kTag, status, result[1]);
      return status != ZX_OK ? status : ZX_ERR_INTERNAL;
    }
    device.host_display_id = result[0];
  }
  uint32_t width = static_cast<uint32_t>(static_cast<float>(device.width) * device.scale);
  uint32_t height = static_cast<uint32_t>(static_cast<float>(device.height) * device.scale);
  uint32_t result = 1;
  zx_status_t status =
      SetDisplayPoseLocked(device.host_display_id, device.x, device.y, width, height, &result);
  if (status != ZX_OK || result) {
    zxlogf(ERROR, "%s: failed to set display pose: %d %d", kTag, status, result);
    return status != ZX_OK ? status : ZX_ERR_INTERNAL;
  }
  device.expected_next_flush = async::Now(loop_.dispatcher());

  return ZX_OK;
}

void Display::TeardownDisplay(uint64_t display_id) {
  Device& device = devices_[display_id];

  if (device.host_display_id) {
    fbl::AutoLock lock(&lock_);
    uint32_t result;
    zx_status_t status = DestroyDisplayLocked(device.host_display_id, &result);
    ZX_DEBUG_ASSERT(status == ZX_OK);
    ZX_DEBUG_ASSERT(!result);
  }
}

void Display::FlushDisplay(async_dispatcher_t* dispatcher, uint64_t display_id) {
  Device& device = devices_[display_id];

  zx::duration period = zx::sec(1) / device.refresh_rate_hz;
  zx::time expected_next_flush = device.expected_next_flush + period;

  DisplayConfig pending_config = {};
  if (pending_config_.find(display_id) != pending_config_.end()) {
    pending_config = pending_config_[display_id];
  }

  if (pending_config.color_buffer) {
    zx_status_t status = PresentColorBuffer(display_id, pending_config);
    ZX_DEBUG_ASSERT(status == ZX_OK || status == ZX_ERR_SHOULD_WAIT);
  }

  {
    fbl::AutoLock lock(&flush_lock_);

    if (dc_intf_.is_valid()) {
      DisplayConfig current_cb = {};
      if (current_config_.find(display_id) != current_config_.end()) {
        current_cb = current_config_[display_id];
        latest_config_stamp_ = current_cb.config_stamp;
      }

      zx::time now = async::Now(dispatcher);
      dc_intf_.OnDisplayVsync(display_id, now.get(), &latest_config_stamp_);
    }
  }

  // If we've already passed the |expected_next_flush| deadline, skip the
  // Vsync and adjust the deadline to the earliest next available frame.
  zx::time now = async::Now(dispatcher);
  if (now > expected_next_flush) {
    expected_next_flush +=
        period * (((now - expected_next_flush + period).get() - 1L) / period.get());
  }

  device.expected_next_flush = expected_next_flush;
  async::PostTaskForTime(
      dispatcher, [this, dispatcher, display_id] { FlushDisplay(dispatcher, display_id); },
      expected_next_flush);
}

}  // namespace goldfish

static constexpr zx_driver_ops_t goldfish_display_driver_ops = []() -> zx_driver_ops_t {
  zx_driver_ops_t ops = {};
  ops.version = DRIVER_OPS_VERSION;
  ops.bind = goldfish::Display::Create;
  return ops;
}();

ZIRCON_DRIVER(goldfish_display, goldfish_display_driver_ops, "zircon", "0.1");
