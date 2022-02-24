// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "osd.h"

#include <float.h>
#include <fuchsia/hardware/display/controller/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/clock.h>
#include <lib/zx/time.h>
#include <math.h>
#include <stdint.h>
#include <threads.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/pixelformat.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>
#include <zircon/utc.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <ddktl/device.h>
#include <fbl/algorithm.h>
#include <fbl/auto_lock.h>

#include "lib/zx/pmt.h"
#include "lib/zx/vmar.h"
#include "rdma-regs.h"
#include "src/graphics/display/drivers/amlogic-display/amlogic-display.h"
#include "src/graphics/display/drivers/amlogic-display/common.h"
#include "src/graphics/display/drivers/amlogic-display/hhi-regs.h"
#include "vpp-regs.h"
#include "vpu-regs.h"

namespace amlogic_display {

#define READ32_VPU_REG(a) vpu_mmio_->Read32(a)
#define WRITE32_VPU_REG(a, v) vpu_mmio_->Write32(v, a)

namespace {
constexpr uint32_t VpuViuOsd1BlkCfgOsdBlkMode32Bit = 5;
constexpr uint32_t VpuViuOsd1BlkCfgColorMatrixArgb = 1;
constexpr uint32_t kMaximumAlpha = 0xff;

// We use bicubic interpolation for scaling.
// TODO(payamm): Add support for other types of interpolation
unsigned int osd_filter_coefs_bicubic[] = {
    0x00800000, 0x007f0100, 0xff7f0200, 0xfe7f0300, 0xfd7e0500, 0xfc7e0600, 0xfb7d0800,
    0xfb7c0900, 0xfa7b0b00, 0xfa7a0dff, 0xf9790fff, 0xf97711ff, 0xf87613ff, 0xf87416fe,
    0xf87218fe, 0xf8701afe, 0xf76f1dfd, 0xf76d1ffd, 0xf76b21fd, 0xf76824fd, 0xf76627fc,
    0xf76429fc, 0xf7612cfc, 0xf75f2ffb, 0xf75d31fb, 0xf75a34fb, 0xf75837fa, 0xf7553afa,
    0xf8523cfa, 0xf8503ff9, 0xf84d42f9, 0xf84a45f9, 0xf84848f8};

constexpr uint32_t kFloatToFixed3_10ScaleFactor = 1024;
constexpr int32_t kMaxFloatToFixed3_10 = (4 * kFloatToFixed3_10ScaleFactor) - 1;
constexpr int32_t kMinFloatToFixed3_10 = -4 * kFloatToFixed3_10ScaleFactor;
constexpr uint32_t kFloatToFixed3_10Mask = 0x1FFF;

constexpr uint32_t kFloatToFixed2_10ScaleFactor = 1024;
constexpr int32_t kMaxFloatToFixed2_10 = (2 * kFloatToFixed2_10ScaleFactor) - 1;
constexpr int32_t kMinFloatToFixed2_10 = -2 * kFloatToFixed2_10ScaleFactor;
constexpr uint32_t kFloatToFixed2_10Mask = 0xFFF;

// AFBC related constants
constexpr uint32_t kAfbcb16x16Pixel = 0;
__UNUSED constexpr uint32_t kAfbc32x8Pixel = 1;
constexpr uint32_t kAfbcSplitOff = 0;
__UNUSED constexpr uint32_t kAfbcSplitOn = 1;
constexpr uint32_t kAfbcYuvTransferOff = 0;
__UNUSED constexpr uint32_t kAfbcYuvTransferOn = 1;
constexpr uint32_t kAfbcRGBA8888 = 5;
constexpr uint32_t kAfbcColorReorderR = 1;
constexpr uint32_t kAfbcColorReorderG = 2;
constexpr uint32_t kAfbcColorReorderB = 3;
constexpr uint32_t kAfbcColorReorderA = 4;

}  // namespace

void Osd::TryResolvePendingRdma() {
  ZX_DEBUG_ASSERT(rdma_active_);

  zx::time now = zx::clock::get_monotonic();
  auto rdma_status = RdmaStatusReg::Get().ReadFrom(&(*vpu_mmio_));
  if (!rdma_status.ChannelDone(kRdmaChannel)) {
    // The configs scheduled to apply on the previous vsync have not been processed by the RDMA
    // engine yet. Log some statistics on how often this situation occurs.
    rdma_pending_in_vsync_count_.Add(1);

    zx::duration interval = now - last_rdma_pending_in_vsync_timestamp_;
    last_rdma_pending_in_vsync_timestamp_ = now;
    last_rdma_pending_in_vsync_timestamp_ns_prop_.Set(last_rdma_pending_in_vsync_timestamp_.get());
    last_rdma_pending_in_vsync_interval_ns_.Set(interval.get());
  }

  // If RDMA for AFBC just completed, simply clear the interrupt. We keep RDMA enabled to
  // automatically get triggered on every vsync. FlipOnVsync is responsible for enabling/disabling
  // AFBC-related RDMA based on configs.
  if (rdma_status.ChannelDone(kAfbcRdmaChannel, &(*vpu_mmio_))) {
    RdmaCtrlReg::ClearInterrupt(kAfbcRdmaChannel, &(*vpu_mmio_));
  }

  if (rdma_status.ChannelDone(kRdmaChannel)) {
    RdmaCtrlReg::ClearInterrupt(kRdmaChannel, &(*vpu_mmio_));

    uint32_t regVal = vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO);
    regVal &= ~RDMA_ACCESS_AUTO_INT_EN(kRdmaChannel);  // Remove VSYNC interrupt source
    vpu_mmio_->Write32(regVal, VPU_RDMA_ACCESS_AUTO);

    // Read and store the last applied image handle and drive the RDMA state machine forward.
    ProcessRdmaUsageTable();
  }
}

config_stamp_t Osd::GetLastConfigStampApplied() {
  ZX_DEBUG_ASSERT(initialized_);
  fbl::AutoLock lock(&rdma_lock_);
  if (rdma_active_) {
    TryResolvePendingRdma();
  }
  return latest_applied_config_;
}

void Osd::ProcessRdmaUsageTable() {
  ZX_DEBUG_ASSERT(rdma_active_);

  // Find out how far did the RDMA write
  uint64_t val = (static_cast<uint64_t>(vpu_mmio_->Read32(VPP_DUMMY_DATA1)) << 32) |
                 (vpu_mmio_->Read32(VPP_OSD_SC_DUMMY_DATA));
  size_t last_table_index = -1;
  // FIXME: or search until end_index_used_. Either way, end_index_used_ will
  // always be less than kNumberOfTables. So no penalty
  for (size_t i = start_index_used_; i < kNumberOfTables && last_table_index == -1ul; i++) {
    if (val == rdma_usage_table_[i]) {
      // Found the last table that was written to
      last_table_index = i;
      latest_applied_config_ = {.value = rdma_usage_table_[i]};  // save this for vsync
    }
    rdma_usage_table_[i] = kRdmaTableUnavailable;  // mark as unavailable for now
  }
  if (last_table_index == -1ul) {
    DISP_ERROR("RDMA handler could not find last used table index");
    DumpRdmaState();

    // Pretend that all configs have been completed to recover. The next code block will initialize
    // the entire table as ready to consume new configs.
    last_table_index = end_index_used_;
  }

  rdma_active_ = false;

  // Only mark ready if we actually completed all the configs.
  if (last_table_index == end_index_used_) {
    // Clear them up
    for (size_t i = 0; i <= last_table_index; i++) {
      rdma_usage_table_[i] = kRdmaTableReady;
    }
  } else {
    // We have pending configs. Let's schedule it. First,
    // We want to schedule a new RDMA from <last_table_index + 1> to end_index_used
    // Write the start and end address of the table. End address is the last address that
    // the RDMA engine reads from.
    start_index_used_ = static_cast<uint8_t>(last_table_index + 1);
    vpu_mmio_->Write32(static_cast<uint32_t>(rdma_chnl_container_[start_index_used_].phys_offset),
                       VPU_RDMA_AHB_START_ADDR(kRdmaChannel));
    vpu_mmio_->Write32(
        static_cast<uint32_t>(rdma_chnl_container_[start_index_used_].phys_offset + kTableSize - 4),
        VPU_RDMA_AHB_END_ADDR(kRdmaChannel));
    uint32_t regVal = vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO);
    regVal |= RDMA_ACCESS_AUTO_INT_EN(kRdmaChannel);  // VSYNC interrupt source
    regVal |= RDMA_ACCESS_AUTO_WRITE(kRdmaChannel);   // Write
    vpu_mmio_->Write32(regVal, VPU_RDMA_ACCESS_AUTO);
    rdma_active_ = true;
    rdma_begin_count_.Add(1);
  }
}

int Osd::RdmaIrqThread() {
  zx_status_t status;
  while (true) {
    status = rdma_irq_.wait(nullptr);
    if (status != ZX_OK) {
      DISP_ERROR("RDMA Interrupt wait failed");
      break;
    }

    rdma_irq_count_.Add(1);
  }
  return status;
}

Osd::Osd(bool supports_afbc, uint32_t fb_width, uint32_t fb_height, uint32_t display_width,
         uint32_t display_height, inspect::Node* parent_node)
    : supports_afbc_(supports_afbc),
      fb_width_(fb_width),
      fb_height_(fb_height),
      display_width_(display_width),
      display_height_(display_height),
      inspect_node_(parent_node->CreateChild("osd")) {
  rdma_allocation_failures_ = inspect_node_.CreateUint("rdma_allocation_failures", 0);
  rdma_irq_count_ = inspect_node_.CreateUint("rdma_irq_count", 0);
  rdma_begin_count_ = inspect_node_.CreateUint("rdma_begin_count", 0);
  rdma_pending_in_vsync_count_ = inspect_node_.CreateUint("rdma_pending_in_vsync_count", 0);
  rdma_stall_count_ = inspect_node_.CreateUint("rdma_stalls", 0);
  last_rdma_stall_timestamp_ns_ = inspect_node_.CreateUint("last_rdma_stall_timestamp_ns", 0);
  last_rdma_pending_in_vsync_interval_ns_ =
      inspect_node_.CreateUint("last_rdma_pending_in_vsync_interval_ns", 0);
  last_rdma_pending_in_vsync_timestamp_ns_prop_ =
      inspect_node_.CreateUint("last_rdma_pending_in_vsync_timestamp_ns", 0);
}

zx_status_t Osd::Init(ddk::PDev& pdev) {
  if (initialized_) {
    return ZX_OK;
  }

  // Map vpu mmio used by the OSD object
  zx_status_t status = pdev.MapMmio(MMIO_VPU, &vpu_mmio_);
  if (status != ZX_OK) {
    DISP_ERROR("osd: Could not map VPU mmio");
    return status;
  }

  // Get BTI from parent
  status = pdev.GetBti(0, &bti_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not get BTI handle");
    return status;
  }

  // Map RDMA Done Interrupt
  status = pdev.GetInterrupt(IRQ_RDMA, 0, &rdma_irq_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not map RDMA interrupt");
    return status;
  }

  auto start_thread = [](void* arg) { return static_cast<Osd*>(arg)->RdmaIrqThread(); };
  status = thrd_create_with_name(&rdma_irq_thread_, start_thread, this, "rdma_irq_thread");
  if (status != ZX_OK) {
    DISP_ERROR("Could not create rdma_thread");
    return status;
  }

  // Setup RDMA
  status = SetupRdma();
  if (status != ZX_OK) {
    DISP_ERROR("Could not setup RDMA");
    return status;
  }

  // OSD object is ready to be used.
  initialized_ = true;
  return ZX_OK;
}

void Osd::Disable(void) {
  ZX_DEBUG_ASSERT(initialized_);
  StopRdma();
  Osd1CtrlStatReg::Get().ReadFrom(&(*vpu_mmio_)).set_blk_en(0).WriteTo(&(*vpu_mmio_));
  fbl::AutoLock lock(&rdma_lock_);
  latest_applied_config_ = {.value = INVALID_CONFIG_STAMP_VALUE};
}

void Osd::Enable(void) {
  ZX_DEBUG_ASSERT(initialized_);
  Osd1CtrlStatReg::Get().ReadFrom(&(*vpu_mmio_)).set_blk_en(1).WriteTo(&(*vpu_mmio_));
}

uint32_t Osd::FloatToFixed2_10(float f) {
  auto fixed_num = static_cast<int32_t>(round(f * kFloatToFixed2_10ScaleFactor));

  // Amlogic hardware accepts values [-2 2). Let's make sure the result is within this range.
  // If not, clamp it
  fixed_num = std::clamp(fixed_num, kMinFloatToFixed2_10, kMaxFloatToFixed2_10);
  return fixed_num & kFloatToFixed2_10Mask;
}

uint32_t Osd::FloatToFixed3_10(float f) {
  auto fixed_num = static_cast<int32_t>(round(f * kFloatToFixed3_10ScaleFactor));

  // Amlogic hardware accepts values [-4 4). Let's make sure the result is within this range.
  // If not, clamp it
  fixed_num = std::clamp(fixed_num, kMinFloatToFixed3_10, kMaxFloatToFixed3_10);
  return fixed_num & kFloatToFixed3_10Mask;
}

int Osd::GetNextAvailableRdmaTableIndex() {
  fbl::AutoLock lock(&rdma_lock_);
  for (uint32_t i = 0; i < kNumberOfTables; i++) {
    if (rdma_usage_table_[i] == kRdmaTableReady) {
      return i;
    }
  }
  return -1;
}

void Osd::SetColorCorrection(uint32_t rdma_table_idx, const display_config_t* config) {
  if (!config->cc_flags) {
    // Disable color conversion engine
    SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_EN_CTRL,
                      vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_EN_CTRL) & ~(1 << 0));
    return;
  }

  // Set enable bit
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_EN_CTRL,
                    vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_EN_CTRL) | (1 << 0));

  // Load PreOffset values (or 0 if none entered)
  auto offset0_1 = (config->cc_flags & COLOR_CONVERSION_PREOFFSET
                        ? (FloatToFixed2_10(config->cc_preoffsets[0]) << 16 |
                           FloatToFixed2_10(config->cc_preoffsets[1]) << 0)
                        : 0);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_PRE_OFFSET0_1, offset0_1);
  auto offset2 = (config->cc_flags & COLOR_CONVERSION_PREOFFSET
                      ? (FloatToFixed2_10(config->cc_preoffsets[2]) << 0)
                      : 0);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_PRE_OFFSET2, offset2);
  // TODO(b/182481217): remove when this bug is closed.
  DISP_TRACE("pre offset0_1=%u offset2=%u\n", offset0_1, offset2);

  // Load PostOffset values (or 0 if none entered)
  offset0_1 = (config->cc_flags & COLOR_CONVERSION_POSTOFFSET
                   ? (FloatToFixed2_10(config->cc_postoffsets[0]) << 16 |
                      FloatToFixed2_10(config->cc_postoffsets[1]) << 0)
                   : 0);
  offset2 = (config->cc_flags & COLOR_CONVERSION_PREOFFSET
                 ? (FloatToFixed2_10(config->cc_postoffsets[2]) << 0)
                 : 0);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_OFFSET0_1, offset0_1);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_OFFSET2, offset2);
  // TODO(b/182481217): remove when this bug is closed.
  DISP_TRACE("post offset0_1=%u offset2=%u\n", offset0_1, offset2);

  // clang-format off
  const float identity[3][3] = {
      {1, 0, 0,},
      {0, 1, 0,},
      {0, 0, 1,},
  };
  // clang-format on

  const auto* ccm =
      (config->cc_flags & COLOR_CONVERSION_COEFFICIENTS) ? config->cc_coefficients : identity;

  // Load up the coefficient matrix registers
  auto coef00_01 = FloatToFixed3_10(ccm[0][0]) << 16 | FloatToFixed3_10(ccm[0][1]) << 0;
  auto coef02_10 = FloatToFixed3_10(ccm[0][2]) << 16 | FloatToFixed3_10(ccm[1][0]) << 0;
  auto coef11_12 = FloatToFixed3_10(ccm[1][1]) << 16 | FloatToFixed3_10(ccm[1][2]) << 0;
  auto coef20_21 = FloatToFixed3_10(ccm[2][0]) << 16 | FloatToFixed3_10(ccm[2][1]) << 0;
  auto coef22 = FloatToFixed3_10(ccm[2][2]) << 0;
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_COEF00_01, coef00_01);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_COEF02_10, coef02_10);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_COEF11_12, coef11_12);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_COEF20_21, coef20_21);
  SetRdmaTableValue(rdma_table_idx, IDX_MATRIX_COEF22, coef22);
  // TODO(b/182481217): remove when this bug is closed.
  DISP_TRACE("color correction regs 00_01=%xu 02_12=%xu 11_12=%xu 20_21=%u 22=%xu\n", coef00_01,
             coef02_10, coef11_12, coef20_21, coef22);
}

void Osd::FlipOnVsync(uint8_t idx, const display_config_t* config,
                      const config_stamp_t* config_stamp) {
  auto info = reinterpret_cast<ImageInfo*>(config[0].layer_list[0]->cfg.primary.image.handle);
  const int next_table_idx = GetNextAvailableRdmaTableIndex();
  if (next_table_idx < 0) {
    DISP_ERROR("No table available!");
    rdma_allocation_failures_.Add(1);
    return;
  }

  DISP_TRACE("Table index %d used", next_table_idx);

  if ((config[0].mode.h_addressable != display_width_) ||
      (config[0].mode.v_addressable != display_height_)) {
    display_width_ = config[0].mode.h_addressable;
    display_height_ = config[0].mode.v_addressable;
    fb_width_ = config[0].mode.h_addressable;
    fb_height_ = config[0].mode.v_addressable;
    HwInit();
  }

  if (config->gamma_table_present) {
    if (config->apply_gamma_table) {
      // Gamma Table needs to be programmed manually. Cannot use RDMA
      SetGamma(GammaChannel::kRed, config->gamma_red_list);
      SetGamma(GammaChannel::kGreen, config->gamma_green_list);
      SetGamma(GammaChannel::kBlue, config->gamma_blue_list);
    }
    // Enable Gamma at vsync using RDMA
    SetRdmaTableValue(next_table_idx, IDX_GAMMA_EN, 1);
    // Set flag to indicate we have enabled gamma
    osd_enabled_gamma_ = true;
  } else {
    // Only disbale gamma if we enabled it.
    if (osd_enabled_gamma_) {
      // Disable Gamma at vsync using RDMA
      SetRdmaTableValue(next_table_idx, IDX_GAMMA_EN, 0);
    } else {
      SetRdmaTableValue(next_table_idx, IDX_GAMMA_EN,
                        VppGammaCntlPortReg::Get().ReadFrom(&(*vpu_mmio_)).en());
    }
  }
  auto cfg_w0 = Osd1Blk0CfgW0Reg::Get().FromValue(0);
  cfg_w0.set_blk_mode(VpuViuOsd1BlkCfgOsdBlkMode32Bit)
      .set_color_matrix(VpuViuOsd1BlkCfgColorMatrixArgb);
  if (supports_afbc_ && info->is_afbc) {
    // AFBC: Enable sourcing from mali + configure as big endian
    cfg_w0.set_mali_src_en(1).set_little_endian(0);
  } else {
    // Update CFG_W0 with correct Canvas Index
    cfg_w0.set_mali_src_en(0).set_little_endian(1).set_tbl_addr(idx);
  }
  SetRdmaTableValue(next_table_idx, IDX_BLK0_CFG_W0, cfg_w0.reg_value());

  auto primary_layer = config->layer_list[0]->cfg.primary;

  // Configure ctrl_stat and ctrl_stat2 registers
  auto osd_ctrl_stat_val = Osd1CtrlStatReg::Get().ReadFrom(&(*vpu_mmio_));
  auto osd_ctrl_stat2_val = Osd1CtrlStat2Reg::Get().ReadFrom(&(*vpu_mmio_));

  // enable OSD Block
  osd_ctrl_stat_val.set_blk_en(1);

  // Amlogic supports two types of alpha blending:
  // Global: This alpha value is applied to the entire plane (i.e. all pixels)
  // Per-Pixel: Each pixel will be multiplied by its corresponding alpha channel
  //
  // If alpha blending is disabled by the client or we are supporting a format that does
  // not have an alpha channel, we need to:
  // a) Set global alpha multiplier to 1 (i.e. 0xFF)
  // b) Enable "replaced_alpha" and set its value to 0xFF. This will effectively
  //    tell the hardware to replace the value found in alpha channel with the "replaced"
  //    value
  //
  // If alpha blending is enabled but alpha_layer_val is NaN:
  // - Set global alpha multiplier to 1 (i.e. 0xFF)
  // - Disable "replaced_alpha" which allows hardware to use per-pixel alpha channel.
  //
  // If alpha blending is enabled and alpha_layer_val has a value:
  // - Set global alpha multiplier to alpha_layer_val
  // - Disable "replaced_alpha" which allows hardware to use per-pixel alpha channel.

  // Load default values: Set global alpha to 1 and enable replaced_alpha.
  osd_ctrl_stat2_val.set_replaced_alpha_en(1).set_replaced_alpha(kMaximumAlpha);
  osd_ctrl_stat_val.set_global_alpha(kMaximumAlpha);

  if (primary_layer.alpha_mode != ALPHA_DISABLE) {
    // If a global alpha value is provided, apply it.
    if (!isnan(primary_layer.alpha_layer_val)) {
      auto num = static_cast<uint8_t>(round(primary_layer.alpha_layer_val * kMaximumAlpha));
      osd_ctrl_stat_val.set_global_alpha(num);
    }
    // If format includes alpha channel, disable "replaced_alpha"
    if (primary_layer.image.pixel_format != ZX_PIXEL_FORMAT_RGB_x888) {
      osd_ctrl_stat2_val.set_replaced_alpha_en(0);
    }
  }

  // Use linear address for AFBC, Canvas otherwise
  osd_ctrl_stat_val.set_osd_mem_mode((supports_afbc_ && info->is_afbc) ? 1 : 0);
  osd_ctrl_stat2_val.set_pending_status_cleanup(1);

  SetRdmaTableValue(next_table_idx, IDX_CTRL_STAT, osd_ctrl_stat_val.reg_value());
  SetRdmaTableValue(next_table_idx, IDX_CTRL_STAT2, osd_ctrl_stat2_val.reg_value());

  // Complain if doesn't support AFBC, but trying to display with AFBC
  ZX_DEBUG_ASSERT(!(!supports_afbc_ && info->is_afbc));
  if (supports_afbc_ && info->is_afbc) {
    // Line Stride calculation based on vendor code
    auto a = fbl::round_up(fbl::round_up(info->image_width * 4, 16u) / 16, 2u);
    auto r = Osd1Blk2CfgW4Reg::Get().FromValue(0).set_linear_stride(a).reg_value();
    SetRdmaTableValue(next_table_idx, IDX_BLK2_CFG_W4, r);

    // Set AFBC's Physical address since it does not use Canvas
    SetRdmaTableValue(next_table_idx, IDX_AFBC_HEAD_BUF_ADDR_LOW, (info->paddr & 0xFFFFFFFF));
    SetRdmaTableValue(next_table_idx, IDX_AFBC_HEAD_BUF_ADDR_HIGH, (info->paddr >> 32));

    // Set OSD to unpack Mali source
    auto upackreg = Osd1MaliUnpackCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).set_mali_unpack_en(1);
    SetRdmaTableValue(next_table_idx, IDX_MALI_UNPACK_CTRL, upackreg.reg_value());

    // Switch OSD to Mali Source
    auto miscctrl = OsdPathMiscCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).set_osd1_mali_sel(1);
    SetRdmaTableValue(next_table_idx, IDX_PATH_MISC_CTRL, miscctrl.reg_value());

    // S0 is our index of 0, which is programmed for OSD1
    SetRdmaTableValue(
        next_table_idx, IDX_AFBC_SURFACE_CFG,
        AfbcSurfaceCfgReg::Get().ReadFrom(&(*vpu_mmio_)).set_cont(0).set_s0_en(1).reg_value());
    // set command - This uses a separate RDMA Table
    SetAfbcRdmaTableValue(AfbcCommandReg::Get().FromValue(0).set_direct_swap(1).reg_value());
  } else {
    // Set OSD to unpack Normal source
    auto upackreg = Osd1MaliUnpackCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).set_mali_unpack_en(0);
    SetRdmaTableValue(next_table_idx, IDX_MALI_UNPACK_CTRL, upackreg.reg_value());

    // Switch OSD to DDR Source
    auto miscctrl = OsdPathMiscCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).set_osd1_mali_sel(0);
    SetRdmaTableValue(next_table_idx, IDX_PATH_MISC_CTRL, miscctrl.reg_value());

    // Disable afbc sourcing
    SetRdmaTableValue(next_table_idx, IDX_AFBC_SURFACE_CFG,
                      AfbcSurfaceCfgReg::Get().ReadFrom(&(*vpu_mmio_)).set_s0_en(0).reg_value());
    // clear command - This uses a separate RDMA Table
    SetAfbcRdmaTableValue(AfbcCommandReg::Get().FromValue(0).set_direct_swap(0).reg_value());
  }

  SetColorCorrection(next_table_idx, config);

  // update last element of table which will be used to indicate whether RDMA operation was
  // completed or not
  SetRdmaTableValue(next_table_idx, IDX_RDMA_CFG_STAMP_HIGH, (config_stamp->value >> 32));
  SetRdmaTableValue(next_table_idx, IDX_RDMA_CFG_STAMP_LOW, (config_stamp->value & 0xFFFFFFFF));

  FlushRdmaTable(next_table_idx);
  if (supports_afbc_ && info->is_afbc) {
    FlushAfbcRdmaTable();
    // Write the start and end address of the table.  End address is the last address that the
    // RDMA engine reads from.
    vpu_mmio_->Write32(static_cast<uint32_t>(afbc_rdma_chnl_container_.phys_offset),
                       VPU_RDMA_AHB_START_ADDR(kAfbcRdmaChannel));
    vpu_mmio_->Write32(
        static_cast<uint32_t>(afbc_rdma_chnl_container_.phys_offset + kAfbcTableSize - 4),
        VPU_RDMA_AHB_END_ADDR(kAfbcRdmaChannel));
  }

  fbl::AutoLock lock(&rdma_lock_);
  // Write the start and end address of the table. End address is the last address that the
  // RDMA engine reads from.
  // if rdma is already active, just update the end_addr
  if (rdma_active_) {
    end_index_used_ = static_cast<uint8_t>(next_table_idx);
    rdma_usage_table_[next_table_idx] = config_stamp->value;
    vpu_mmio_->Write32(
        static_cast<uint32_t>(rdma_chnl_container_[next_table_idx].phys_offset + kTableSize - 4),
        VPU_RDMA_AHB_END_ADDR(kRdmaChannel));
    return;
  }

  start_index_used_ = static_cast<uint8_t>(next_table_idx);
  end_index_used_ = start_index_used_;

  vpu_mmio_->Write32(static_cast<uint32_t>(rdma_chnl_container_[next_table_idx].phys_offset),
                     VPU_RDMA_AHB_START_ADDR(kRdmaChannel));
  vpu_mmio_->Write32(
      static_cast<uint32_t>(rdma_chnl_container_[next_table_idx].phys_offset + kTableSize - 4),
      VPU_RDMA_AHB_END_ADDR(kRdmaChannel));

  // Enable Auto mode: Non-Increment, VSync Interrupt Driven, Write
  uint32_t regVal = vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO);
  regVal |= RDMA_ACCESS_AUTO_INT_EN(kRdmaChannel);  // VSYNC interrupt source
  regVal |= RDMA_ACCESS_AUTO_WRITE(kRdmaChannel);   // Write
  vpu_mmio_->Write32(regVal, VPU_RDMA_ACCESS_AUTO);
  rdma_usage_table_[next_table_idx] = config_stamp->value;
  rdma_active_ = true;
  rdma_begin_count_.Add(1);
  if (supports_afbc_ && info->is_afbc) {
    // Enable Auto mode: Non-Increment, VSync Interrupt Driven, Write
    RdmaAccessAuto2Reg::Get().FromValue(0).set_chn7_auto_write(1).WriteTo(&(*vpu_mmio_));
    RdmaAccessAuto3Reg::Get().FromValue(0).set_chn7_intr(1).WriteTo(&(*vpu_mmio_));
  } else {
    // Remove interrupt source
    RdmaAccessAuto3Reg::Get().FromValue(0).set_chn7_intr(0).WriteTo(&(*vpu_mmio_));
  }
}

void Osd::DefaultSetup() {
  // osd blend ctrl
  WRITE32_REG(VPU, VIU_OSD_BLEND_CTRL,
              4 << 29 | 0 << 27 |  // blend2_premult_en
                  1 << 26 |        // blend_din0 input to blend0
                  0 << 25 |        // blend1_dout to blend2
                  0 << 24 |        // blend1_din3 input to blend1
                  1 << 20 |        // blend_din_en
                  0 << 16 |        // din_premult_en
                  1 << 0);         // din_reoder_sel = OSD1

  // vpp osd1 blend ctrl
  WRITE32_REG(VPU, OSD1_BLEND_SRC_CTRL,
              (0 & 0xf) << 0 | (0 & 0x1) << 4 | (3 & 0xf) << 8 |  // postbld_src3_sel
                  (0 & 0x1) << 16 |                               // postbld_osd1_premult
                  (1 & 0x1) << 20);
  // vpp osd2 blend ctrl
  WRITE32_REG(VPU, OSD2_BLEND_SRC_CTRL,
              (0 & 0xf) << 0 | (0 & 0x1) << 4 | (0 & 0xf) << 8 |  // postbld_src4_sel
                  (0 & 0x1) << 16 |                               // postbld_osd2_premult
                  (1 & 0x1) << 20);

  // used default dummy data
  WRITE32_REG(VPU, VIU_OSD_BLEND_DUMMY_DATA0, 0x0 << 16 | 0x0 << 8 | 0x0);
  // used default dummy alpha data
  WRITE32_REG(VPU, VIU_OSD_BLEND_DUMMY_ALPHA, 0x0 << 20 | 0x0 << 11 | 0x0);

  // osdx setting
  WRITE32_REG(VPU, VPU_VIU_OSD_BLEND_DIN0_SCOPE_H, (fb_width_ - 1) << 16);

  WRITE32_REG(VPU, VPU_VIU_OSD_BLEND_DIN0_SCOPE_V, (fb_height_ - 1) << 16);

  WRITE32_REG(VPU, VIU_OSD_BLEND_BLEND0_SIZE, fb_height_ << 16 | fb_width_);
  WRITE32_REG(VPU, VIU_OSD_BLEND_BLEND1_SIZE, fb_height_ << 16 | fb_width_);
  SET_BIT32(VPU, DOLBY_PATH_CTRL, 0x3, 2, 2);

  WRITE32_REG(VPU, VPP_OSD1_IN_SIZE, fb_height_ << 16 | fb_width_);

  // setting blend scope
  WRITE32_REG(VPU, VPP_OSD1_BLD_H_SCOPE, 0 << 16 | (fb_width_ - 1));
  WRITE32_REG(VPU, VPP_OSD1_BLD_V_SCOPE, 0 << 16 | (fb_height_ - 1));

  // Set geometry to normal mode
  uint32_t data32 = ((fb_width_ - 1) & 0xfff) << 16;
  WRITE32_REG(VPU, VPU_VIU_OSD1_BLK0_CFG_W3, data32);
  data32 = ((fb_height_ - 1) & 0xfff) << 16;
  WRITE32_REG(VPU, VPU_VIU_OSD1_BLK0_CFG_W4, data32);

  WRITE32_REG(VPU, VPU_VIU_OSD1_BLK0_CFG_W1, ((fb_width_ - 1) & 0x1fff) << 16);
  WRITE32_REG(VPU, VPU_VIU_OSD1_BLK0_CFG_W2, ((fb_height_ - 1) & 0x1fff) << 16);

  // enable osd blk0
  Osd1CtrlStatReg::Get()
      .ReadFrom(&(*vpu_mmio_))
      .set_rsv(0)
      .set_osd_mem_mode(0)
      .set_premult_en(0)
      .set_blk_en(1)
      .WriteTo(&(*vpu_mmio_));
}

void Osd::EnableScaling(bool enable) {
  int hf_phase_step, vf_phase_step;
  int src_w, src_h, dst_w, dst_h;
  int bot_ini_phase;
  int vsc_ini_rcv_num, vsc_ini_rpt_p0_num;
  int hsc_ini_rcv_num, hsc_ini_rpt_p0_num;
  int hf_bank_len = 4;
  int vf_bank_len = 0;
  uint32_t data32 = 0x0;

  vf_bank_len = 4;
  hsc_ini_rcv_num = hf_bank_len;
  vsc_ini_rcv_num = vf_bank_len;
  hsc_ini_rpt_p0_num = (hf_bank_len / 2 - 1) > 0 ? (hf_bank_len / 2 - 1) : 0;
  vsc_ini_rpt_p0_num = (vf_bank_len / 2 - 1) > 0 ? (vf_bank_len / 2 - 1) : 0;
  src_w = fb_width_;
  src_h = fb_height_;
  dst_w = display_width_;
  dst_h = display_height_;

  data32 = 0x0;
  if (enable) {
    /* enable osd scaler */
    data32 |= 1 << 2; /* enable osd scaler */
    data32 |= 1 << 3; /* enable osd scaler path */
    WRITE32_REG(VPU, VPU_VPP_OSD_SC_CTRL0, data32);
  } else {
    /* disable osd scaler path */
    WRITE32_REG(VPU, VPU_VPP_OSD_SC_CTRL0, 0);
  }
  hf_phase_step = (src_w << 18) / dst_w;
  hf_phase_step = (hf_phase_step << 6);
  vf_phase_step = (src_h << 20) / dst_h;
  bot_ini_phase = 0;
  vf_phase_step = (vf_phase_step << 4);

  /* config osd scaler in/out hv size */
  data32 = 0x0;
  if (enable) {
    data32 = (((src_h - 1) & 0x1fff) | ((src_w - 1) & 0x1fff) << 16);
    WRITE32_REG(VPU, VPU_VPP_OSD_SCI_WH_M1, data32);
    data32 = (((display_width_ - 1) & 0xfff));
    WRITE32_REG(VPU, VPU_VPP_OSD_SCO_H_START_END, data32);
    data32 = (((display_height_ - 1) & 0xfff));
    WRITE32_REG(VPU, VPU_VPP_OSD_SCO_V_START_END, data32);
  }
  data32 = 0x0;
  if (enable) {
    data32 |=
        (vf_bank_len & 0x7) | ((vsc_ini_rcv_num & 0xf) << 3) | ((vsc_ini_rpt_p0_num & 0x3) << 8);
    data32 |= 1 << 24;
  }
  WRITE32_REG(VPU, VPU_VPP_OSD_VSC_CTRL0, data32);
  data32 = 0x0;
  if (enable) {
    data32 |=
        (hf_bank_len & 0x7) | ((hsc_ini_rcv_num & 0xf) << 3) | ((hsc_ini_rpt_p0_num & 0x3) << 8);
    data32 |= 1 << 22;
  }
  WRITE32_REG(VPU, VPU_VPP_OSD_HSC_CTRL0, data32);
  data32 = 0x0;
  if (enable) {
    data32 |= (bot_ini_phase & 0xffff) << 16;
    SET_BIT32(VPU, VPU_VPP_OSD_HSC_PHASE_STEP, hf_phase_step, 0, 28);
    SET_BIT32(VPU, VPU_VPP_OSD_HSC_INI_PHASE, 0, 0, 16);
    SET_BIT32(VPU, VPU_VPP_OSD_VSC_PHASE_STEP, vf_phase_step, 0, 28);
    WRITE32_REG(VPU, VPU_VPP_OSD_VSC_INI_PHASE, data32);
  }
}

void Osd::ResetRdmaTable() {
  for (auto& i : rdma_chnl_container_) {
    auto* rdma_table = reinterpret_cast<RdmaTable*>(i.virt_offset);
    rdma_table[IDX_BLK0_CFG_W0].reg = (VPU_VIU_OSD1_BLK0_CFG_W0 >> 2);
    rdma_table[IDX_CTRL_STAT].reg = (VPU_VIU_OSD1_CTRL_STAT >> 2);
    rdma_table[IDX_CTRL_STAT2].reg = (VPU_VIU_OSD1_CTRL_STAT2 >> 2);
    rdma_table[IDX_MATRIX_EN_CTRL].reg = (VPU_VPP_POST_MATRIX_EN_CTRL >> 2);
    rdma_table[IDX_MATRIX_COEF00_01].reg = (VPU_VPP_POST_MATRIX_COEF00_01 >> 2);
    rdma_table[IDX_MATRIX_COEF02_10].reg = (VPU_VPP_POST_MATRIX_COEF02_10 >> 2);
    rdma_table[IDX_MATRIX_COEF11_12].reg = (VPU_VPP_POST_MATRIX_COEF11_12 >> 2);
    rdma_table[IDX_MATRIX_COEF20_21].reg = (VPU_VPP_POST_MATRIX_COEF20_21 >> 2);
    rdma_table[IDX_MATRIX_COEF22].reg = (VPU_VPP_POST_MATRIX_COEF22 >> 2);
    rdma_table[IDX_MATRIX_OFFSET0_1].reg = (VPU_VPP_POST_MATRIX_OFFSET0_1 >> 2);
    rdma_table[IDX_MATRIX_OFFSET2].reg = (VPU_VPP_POST_MATRIX_OFFSET2 >> 2);
    rdma_table[IDX_MATRIX_PRE_OFFSET0_1].reg = (VPU_VPP_POST_MATRIX_PRE_OFFSET0_1 >> 2);
    rdma_table[IDX_MATRIX_PRE_OFFSET2].reg = (VPU_VPP_POST_MATRIX_PRE_OFFSET2 >> 2);
    rdma_table[IDX_GAMMA_EN].reg = (VPP_GAMMA_CNTL_PORT >> 2);
    rdma_table[IDX_BLK2_CFG_W4].reg = (VPU_VIU_OSD1_BLK2_CFG_W4 >> 2);
    rdma_table[IDX_MALI_UNPACK_CTRL].reg = (VIU_OSD1_MALI_UNPACK_CTRL >> 2);
    rdma_table[IDX_PATH_MISC_CTRL].reg = (VPU_OSD_PATH_MISC_CTRL >> 2);
    rdma_table[IDX_AFBC_HEAD_BUF_ADDR_LOW].reg = (VPU_MAFBC_HEADER_BUF_ADDR_LOW_S0 >> 2);
    rdma_table[IDX_AFBC_HEAD_BUF_ADDR_HIGH].reg = (VPU_MAFBC_HEADER_BUF_ADDR_HIGH_S0 >> 2);
    rdma_table[IDX_AFBC_SURFACE_CFG].reg = (VPU_MAFBC_SURFACE_CFG >> 2);
    rdma_table[IDX_RDMA_CFG_STAMP_HIGH].reg = (VPP_DUMMY_DATA1 >> 2);
    rdma_table[IDX_RDMA_CFG_STAMP_LOW].reg = (VPP_OSD_SC_DUMMY_DATA >> 2);
  }
  auto* afbc_rdma_table = reinterpret_cast<RdmaTable*>(afbc_rdma_chnl_container_.virt_offset);
  afbc_rdma_table->reg = (VPU_MAFBC_COMMAND >> 2);
}

void Osd::SetRdmaTableValue(uint32_t table_index, uint32_t idx, uint32_t val) {
  ZX_DEBUG_ASSERT(idx < IDX_MAX);
  ZX_DEBUG_ASSERT(table_index < kNumberOfTables);
  auto* rdma_table = reinterpret_cast<RdmaTable*>(rdma_chnl_container_[table_index].virt_offset);
  rdma_table[idx].val = val;
}

void Osd::FlushRdmaTable(uint32_t table_index) {
  zx_status_t status =
      zx_cache_flush(rdma_chnl_container_[table_index].virt_offset, IDX_MAX * sizeof(RdmaTable),
                     ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
  if (status != ZX_OK) {
    DISP_ERROR("Could not clean cache %d", status);
    return;
  }
}

zx_status_t Osd::SetupRdma() {
  zx_status_t status = ZX_OK;
  DISP_INFO("Setting up Display RDMA");

  // First, clean up any ongoing DMA that a previous incarnation of this driver
  // may have started, and tell the BTI to drop its quarantine list.
  StopRdma();
  bti_.release_quarantine();

  status = zx::vmo::create_contiguous(bti_, kRdmaRegionSize, 0, &rdma_vmo_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not create RDMA VMO (%d)", status);
    return status;
  }

  status = bti_.pin(ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE, rdma_vmo_, 0, kRdmaRegionSize,
                    &rdma_phys_, 1, &rdma_pmt_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not create RDMA VMO (%d)", status);
    return status;
  }

  status = zx::vmar::root_self()->map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, rdma_vmo_, 0,
                                      kRdmaRegionSize, reinterpret_cast<zx_vaddr_t*>(&rdma_vbuf_));

  // At this point, we have a table initialized.
  // Initialize each rdma channel container
  fbl::AutoLock l(&rdma_lock_);
  for (uint32_t i = 0; i < kNumberOfTables; i++) {
    rdma_chnl_container_[i].phys_offset = rdma_phys_ + (i * kTableSize);
    rdma_chnl_container_[i].virt_offset = rdma_vbuf_ + (i * kTableSize);
    rdma_usage_table_[i] = kRdmaTableReady;
  }

  // Allocate RDMA Table for AFBC engine
  status = zx_vmo_create_contiguous(bti_.get(), kAfbcRdmaRegionSize, 0,
                                    afbc_rdma_vmo_.reset_and_get_address());
  if (status != ZX_OK) {
    DISP_ERROR("Could not create afbc RDMA VMO (%d)", status);
    return status;
  }

  status = zx_bti_pin(bti_.get(), ZX_BTI_PERM_READ | ZX_BTI_PERM_WRITE, afbc_rdma_vmo_.get(), 0,
                      kAfbcRdmaRegionSize, &afbc_rdma_phys_, 1, &afbc_rdma_pmt_);
  if (status != ZX_OK) {
    DISP_ERROR("Could not pin afbc RDMA VMO (%d)", status);
    return status;
  }

  status =
      zx_vmar_map(zx_vmar_root_self(), ZX_VM_PERM_READ | ZX_VM_PERM_WRITE, 0, afbc_rdma_vmo_.get(),
                  0, kAfbcRdmaRegionSize, reinterpret_cast<zx_vaddr_t*>(&afbc_rdma_vbuf_));
  if (status != ZX_OK) {
    DISP_ERROR("Could not map afbc vmar (%d)", status);
    return status;
  }

  // Initialize AFBC rdma channel container
  afbc_rdma_chnl_container_.phys_offset = afbc_rdma_phys_;
  afbc_rdma_chnl_container_.virt_offset = afbc_rdma_vbuf_;

  // Setup RDMA_CTRL:
  // Default: no reset, no clock gating, burst size 4x16B for read and write
  // DDR Read/Write request urgent
  RdmaCtrlReg::Get().FromValue(0).set_write_urgent(1).set_read_urgent(1).WriteTo(&(*vpu_mmio_));

  ResetRdmaTable();

  return ZX_OK;
}

void Osd::SetAfbcRdmaTableValue(uint32_t val) const {
  auto* afbc_rdma_table = reinterpret_cast<RdmaTable*>(afbc_rdma_chnl_container_.virt_offset);
  afbc_rdma_table->val = val;
}

void Osd::FlushAfbcRdmaTable() const {
  zx_status_t status = zx_cache_flush(afbc_rdma_chnl_container_.virt_offset, sizeof(RdmaTable),
                                      ZX_CACHE_FLUSH_DATA | ZX_CACHE_FLUSH_INVALIDATE);
  if (status != ZX_OK) {
    DISP_ERROR("Could not clean cache %d", status);
    return;
  }
}

// TODO(fxbug.dev/57633): stop all channels for safer reloads.
void Osd::StopRdma() {
  DISP_INFO("Stopping RDMA");

  fbl::AutoLock l(&rdma_lock_);

  // Grab a copy of active DMA channels before clearing it
  const uint32_t aa = RdmaAccessAutoReg::Get().ReadFrom(&(*vpu_mmio_)).reg_value();
  const uint32_t aa3 = RdmaAccessAuto3Reg::Get().ReadFrom(&(*vpu_mmio_)).reg_value();

  // Disable triggering for channels 0-2.
  RdmaAccessAutoReg::Get()
      .ReadFrom(&(*vpu_mmio_))
      .set_chn1_intr(0)
      .set_chn2_intr(0)
      .set_chn3_intr(0)
      .WriteTo(&(*vpu_mmio_));
  // Also disable 7, the dedicated AFBC channel.
  RdmaAccessAuto3Reg::Get().FromValue(0).set_chn7_intr(0).WriteTo(&(*vpu_mmio_));

  // Wait for all active copies to complete
  constexpr size_t kMaxRdmaWaits = 5;
  uint32_t expected = RdmaStatusReg::DoneFromAccessAuto(aa, 0, aa3);
  for (size_t i = 0; i < kMaxRdmaWaits; i++) {
    if (RdmaStatusReg::Get().ReadFrom(&(*vpu_mmio_)).done() == expected) {
      break;
    }
    zx::nanosleep(zx::deadline_after(zx::usec(5)));
  }

  // Clear interrupt status
  RdmaCtrlReg::Get().ReadFrom(&(*vpu_mmio_)).set_clear_done(0xFF).WriteTo(&(*vpu_mmio_));
  rdma_active_ = false;
  for (auto& i : rdma_usage_table_) {
    i = kRdmaTableReady;
  }
}

void Osd::EnableGamma() {
  VppGammaCntlPortReg::Get().ReadFrom(&(*vpu_mmio_)).set_en(1).WriteTo(&(*vpu_mmio_));
}
void Osd::DisableGamma() {
  VppGammaCntlPortReg::Get().ReadFrom(&(*vpu_mmio_)).set_en(0).WriteTo(&(*vpu_mmio_));
}

zx_status_t Osd::WaitForGammaAddressReady() {
  // The following delay and retry count is from hardware vendor
  constexpr int32_t kGammaRetry = 100;
  constexpr zx::duration kGammaDelay = zx::usec(10);
  auto retry = kGammaRetry;
  while (!(VppGammaCntlPortReg::Get().ReadFrom(&(*vpu_mmio_)).adr_rdy()) && retry--) {
    zx::nanosleep(zx::deadline_after(kGammaDelay));
  }
  if (retry <= 0) {
    return ZX_ERR_TIMED_OUT;
  }
  return ZX_OK;
}

zx_status_t Osd::WaitForGammaWriteReady() {
  // The following delay and retry count is from hardware vendor
  constexpr int32_t kGammaRetry = 100;
  constexpr zx::duration kGammaDelay = zx::usec(10);
  auto retry = kGammaRetry;
  while (!(VppGammaCntlPortReg::Get().ReadFrom(&(*vpu_mmio_)).wr_rdy()) && retry--) {
    zx::nanosleep(zx::deadline_after(kGammaDelay));
  }
  if (retry <= 0) {
    return ZX_ERR_TIMED_OUT;
  }
  return ZX_OK;
}

zx_status_t Osd::SetGamma(GammaChannel channel, const float* data) {
  // Make sure Video Encoder is enabled
  // WRITE32_REG(VPU, ENCL_VIDEO_EN, 0);
  if (!(vpu_mmio_->Read32(ENCL_VIDEO_EN) & 0x1)) {
    return ZX_ERR_UNAVAILABLE;
  }

  // Wait for ADDR port to be ready
  zx_status_t status;
  if ((status = WaitForGammaAddressReady()) != ZX_OK) {
    return status;
  }

  // Select channel and enable auto-increment.
  // auto-increment: increments the gamma table address as we write into the
  // data register
  auto gamma_addrport_reg = VppGammaAddrPortReg::Get().FromValue(0);
  gamma_addrport_reg.set_auto_inc(1);
  gamma_addrport_reg.set_adr(0);
  switch (channel) {
    case GammaChannel::kRed:
      gamma_addrport_reg.set_sel_r(1);
      break;
    case GammaChannel::kGreen:
      gamma_addrport_reg.set_sel_g(1);
      break;
    case GammaChannel::kBlue:
      gamma_addrport_reg.set_sel_b(1);
      break;
    default:
      return ZX_ERR_INVALID_ARGS;
  }
  gamma_addrport_reg.WriteTo(&(*vpu_mmio_));

  // Write Gamma Table
  for (size_t i = 0; i < kGammaTableSize; i++) {
    // Only write if ready. The delay seems very excessive but this comes from vendor.
    status = WaitForGammaWriteReady();
    if (status != ZX_OK) {
      return status;
    }
    auto val = std::clamp(static_cast<uint16_t>(std::round(data[i] * 1023.0)),
                          static_cast<uint16_t>(0), static_cast<uint16_t>(1023));
    VppGammaDataPortReg::Get().FromValue(0).set_reg_value(val).WriteTo(&(*vpu_mmio_));
  }

  // Wait for ADDR port to be ready
  if ((status = WaitForGammaAddressReady()) != ZX_OK) {
    return status;
  }
  return ZX_OK;
}

void Osd::SetMinimumRgb(uint8_t minimum_rgb) {
  ZX_DEBUG_ASSERT(initialized_);
  // According to spec, minimum rgb should be set as follows:
  // Shift value by 2bits (8bit -> 10bit) and write new value for
  // each channel separately.
  VppClipMisc1Reg::Get()
      .FromValue(0)
      .set_r_clamp(minimum_rgb << 2)
      .set_g_clamp(minimum_rgb << 2)
      .set_b_clamp(minimum_rgb << 2)
      .WriteTo(&(*vpu_mmio_));
}

// These configuration could be done during initialization.
zx_status_t Osd::ConfigAfbc() {
  // Set AFBC to 16x16 Blocks, Split Mode OFF, YUV Transfer OFF, and RGBA8888 Format
  // Note RGBA8888 works for both RGBA and ABGR formats. The channels order will be set
  // by mali_unpack_ctrl register
  AfbcFormatSpecifierS0Reg::Get()
      .FromValue(0)
      .set_block_split(kAfbcSplitOff)
      .set_yuv_transform(kAfbcYuvTransferOff)
      .set_super_block_aspect(kAfbcb16x16Pixel)
      .set_pixel_format(kAfbcRGBA8888)
      .WriteTo(&(*vpu_mmio_));

  // Setup color RGBA channel order
  Osd1MaliUnpackCtrlReg::Get()
      .ReadFrom(&(*vpu_mmio_))
      .set_r(kAfbcColorReorderR)
      .set_g(kAfbcColorReorderG)
      .set_b(kAfbcColorReorderB)
      .set_a(kAfbcColorReorderA)
      .WriteTo(&(*vpu_mmio_));

  // Set afbc input buffer width/height in pixel
  AfbcBufferWidthS0Reg::Get().FromValue(0).set_buffer_width(fb_width_).WriteTo(&(*vpu_mmio_));
  AfbcBufferHeightS0Reg::Get().FromValue(0).set_buffer_height(fb_height_).WriteTo(&(*vpu_mmio_));

  // Set afbc input buffer
  AfbcBoundingBoxXStartS0Reg::Get().FromValue(0).set_buffer_x_start(0).WriteTo(&(*vpu_mmio_));
  AfbcBoundingBoxXEndS0Reg::Get()
      .FromValue(0)
      .set_buffer_x_end(fb_width_ - 1)  // vendor code has width - 1 - 1, which is technically
                                        // incorrect and gives the same result as this.
      .WriteTo(&(*vpu_mmio_));
  AfbcBoundingBoxYStartS0Reg::Get().FromValue(0).set_buffer_y_start(0).WriteTo(&(*vpu_mmio_));
  AfbcBoundingBoxYEndS0Reg::Get()
      .FromValue(0)
      .set_buffer_y_end(fb_height_ -
                        1)  // vendor code has height -1 -1, but that cuts off the bottom row.
      .WriteTo(&(*vpu_mmio_));

  // Set output buffer stride
  AfbcOutputBufStrideS0Reg::Get()
      .FromValue(0)
      .set_output_buffer_stride(fb_width_ * 4)
      .WriteTo(&(*vpu_mmio_));

  // Set afbc output buffer index
  // The way this is calculated based on vendor code is as follows:
  // Take OSD being used (1-based index): Therefore OSD1 -> index 1
  // out_addr = index << 24
  AfbcOutputBufAddrLowS0Reg::Get().FromValue(0).set_output_buffer_addr(1 << 24).WriteTo(
      &(*vpu_mmio_));
  AfbcOutputBufAddrHighS0Reg::Get().FromValue(0).set_output_buffer_addr(0).WriteTo(&(*vpu_mmio_));

  // Set linear address to the out_addr mentioned above
  Osd1Blk1CfgW4Reg::Get().FromValue(0).set_frame_addr(1 << 24).WriteTo(&(*vpu_mmio_));

  return ZX_OK;
}

void Osd::HwInit() {
  ZX_DEBUG_ASSERT(initialized_);
  // Setup VPP horizontal width
  WRITE32_REG(VPU, VPP_POSTBLEND_H_SIZE, display_width_);

  // init vpu fifo control register
  uint32_t regVal = READ32_REG(VPU, VPP_OFIFO_SIZE);
  regVal = 0xfff << 20;
  regVal |= (0xfff + 1);
  WRITE32_REG(VPU, VPP_OFIFO_SIZE, regVal);

  // init osd fifo control and set DDR request priority to be urgent
  regVal = 1;
  regVal |= 4 << 5;   // hold_fifo_lines
  regVal |= 1 << 10;  // burst_len_sel 3 = 64. This bit is split between 10 and 31
  regVal |= 2 << 22;
  regVal |= 2 << 24;
  regVal |= 1 << 31;
  regVal |= 32 << 12;  // fifo_depth_val: 32*8 = 256
  WRITE32_REG(VPU, VPU_VIU_OSD1_FIFO_CTRL_STAT, regVal);
  WRITE32_REG(VPU, VPU_VIU_OSD2_FIFO_CTRL_STAT, regVal);

  SET_MASK32(VPU, VPP_MISC, VPP_POSTBLEND_EN);
  CLEAR_MASK32(VPU, VPP_MISC, VPP_PREBLEND_EN);

  Osd1CtrlStatReg::Get()
      .FromValue(0)
      .set_blk_en(1)
      .set_global_alpha(kMaximumAlpha)
      .set_osd_en(1)
      .WriteTo(&(*vpu_mmio_));

  Osd2CtrlStatReg::Get()
      .FromValue(0)
      .set_blk_en(1)
      .set_global_alpha(kMaximumAlpha)
      .set_osd_en(1)
      .WriteTo(&(*vpu_mmio_));

  DefaultSetup();

  EnableScaling(false);

  // Apply scale coefficients
  SET_BIT32(VPU, VPU_VPP_OSD_SCALE_COEF_IDX, 0x0000, 0, 9);
  for (unsigned int i : osd_filter_coefs_bicubic) {
    WRITE32_REG(VPU, VPU_VPP_OSD_SCALE_COEF, i);
  }

  SET_BIT32(VPU, VPU_VPP_OSD_SCALE_COEF_IDX, 0x0100, 0, 9);
  for (unsigned int i : osd_filter_coefs_bicubic) {
    WRITE32_REG(VPU, VPU_VPP_OSD_SCALE_COEF, i);
  }

  // update blending
  WRITE32_REG(VPU, VPU_VPP_OSD1_BLD_H_SCOPE, display_width_ - 1);
  WRITE32_REG(VPU, VPU_VPP_OSD1_BLD_V_SCOPE, display_height_ - 1);
  WRITE32_REG(VPU, VPU_VPP_OUT_H_V_SIZE, display_width_ << 16 | display_height_);

  if (supports_afbc_) {
    // Configure AFBC Engine's one-time programmable fields, so it's ready
    ConfigAfbc();
  }
}

#define REG_OFFSET (0x20 << 2)
void Osd::Dump() {
  ZX_DEBUG_ASSERT(initialized_);
  DumpNonRdmaRegisters();
  DumpRdmaRegisters();
}

void Osd::DumpLocked() {
  DumpNonRdmaRegisters();
  DumpRdmaState();
}

void Osd::DumpRdmaRegisters() {
  DISP_INFO("Dumping all RDMA related Registers\n");
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_MAN = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_MAN));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_MAN = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_MAN));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_1 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_1));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_1 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_1));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_2 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_2));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_2 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_2));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_3 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_3));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_3 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_3));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_4 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_4));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_4 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_4));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_5 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_5));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_5 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_5));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_6 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_6));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_6 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_6));
  DISP_INFO("VPU_RDMA_AHB_START_ADDR_7 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_START_ADDR_7));
  DISP_INFO("VPU_RDMA_AHB_END_ADDR_7 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_AHB_END_ADDR_7));
  DISP_INFO("VPU_RDMA_ACCESS_AUTO = 0x%x", vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO));
  DISP_INFO("VPU_RDMA_ACCESS_AUTO2 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO2));
  DISP_INFO("VPU_RDMA_ACCESS_AUTO3 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_ACCESS_AUTO3));
  DISP_INFO("VPU_RDMA_ACCESS_MAN = 0x%x", vpu_mmio_->Read32(VPU_RDMA_ACCESS_MAN));
  DISP_INFO("VPU_RDMA_CTRL = 0x%x", vpu_mmio_->Read32(VPU_RDMA_CTRL));
  DISP_INFO("VPU_RDMA_STATUS = 0x%x", vpu_mmio_->Read32(VPU_RDMA_STATUS));
  DISP_INFO("VPU_RDMA_STATUS2 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_STATUS2));
  DISP_INFO("VPU_RDMA_STATUS3 = 0x%x", vpu_mmio_->Read32(VPU_RDMA_STATUS3));
  DISP_INFO("Scratch Reg High: 0x%x", vpu_mmio_->Read32(VPP_DUMMY_DATA1));
  DISP_INFO("Scratch Reg Low: 0x%x", vpu_mmio_->Read32(VPP_OSD_SC_DUMMY_DATA));
}

void Osd::DumpNonRdmaRegisters() {
  uint32_t reg = 0;
  uint32_t offset = 0;
  uint32_t index = 0;

  reg = VPU_VIU_VENC_MUX_CTRL;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_MISC;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OFIFO_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_HOLD_LINES;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));

  reg = VPU_OSD_PATH_MISC_CTRL;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_CTRL;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN0_SCOPE_H;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN0_SCOPE_V;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN1_SCOPE_H;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN1_SCOPE_V;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN2_SCOPE_H;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN2_SCOPE_V;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN3_SCOPE_H;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DIN3_SCOPE_V;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DUMMY_DATA0;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_DUMMY_ALPHA;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_BLEND0_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VIU_OSD_BLEND_BLEND1_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));

  reg = VPU_VPP_OSD1_IN_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD1_BLD_H_SCOPE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD1_BLD_V_SCOPE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD2_BLD_H_SCOPE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD2_BLD_V_SCOPE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = OSD1_BLEND_SRC_CTRL;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = OSD2_BLEND_SRC_CTRL;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_POSTBLEND_H_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OUT_H_V_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));

  reg = VPU_VPP_OSD_SC_CTRL0;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD_SCI_WH_M1;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD_SCO_H_START_END;
  DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_OSD_SCO_V_START_END;
  DISP_INFO("reg[0x%x]: 0x%08x\n", reg, READ32_REG(VPU, reg));
  reg = VPU_VPP_POSTBLEND_H_SIZE;
  DISP_INFO("reg[0x%x]: 0x%08x\n", reg, READ32_REG(VPU, reg));
  for (index = 0; index < 2; index++) {
    if (index == 1)
      offset = REG_OFFSET;
    reg = offset + VPU_VIU_OSD1_FIFO_CTRL_STAT;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_CTRL_STAT;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_CTRL_STAT2;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_BLK0_CFG_W0;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_BLK0_CFG_W1;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_BLK0_CFG_W2;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = offset + VPU_VIU_OSD1_BLK0_CFG_W3;
    DISP_INFO("reg[0x%x]: 0x%08x", reg, READ32_REG(VPU, reg));
    reg = VPU_VIU_OSD1_BLK0_CFG_W4;
    if (index == 1)
      reg = VPU_VIU_OSD2_BLK0_CFG_W4;
    DISP_INFO("reg[0x%x]: 0x%08x\n", reg, READ32_REG(VPU, reg));
  }

  DISP_INFO("Dumping all Color Correction Matrix related Registers\n");
  DISP_INFO("VPU_VPP_POST_MATRIX_COEF00_01 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_COEF00_01));
  DISP_INFO("VPU_VPP_POST_MATRIX_COEF02_10 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_COEF02_10));
  DISP_INFO("VPU_VPP_POST_MATRIX_COEF11_12 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_COEF11_12));
  DISP_INFO("VPU_VPP_POST_MATRIX_COEF20_21 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_COEF20_21));
  DISP_INFO("VPU_VPP_POST_MATRIX_COEF22 = 0x%x", vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_COEF22));
  DISP_INFO("VPU_VPP_POST_MATRIX_OFFSET0_1 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_OFFSET0_1));
  DISP_INFO("VPU_VPP_POST_MATRIX_OFFSET2 = 0x%x", vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_OFFSET2));
  DISP_INFO("VPU_VPP_POST_MATRIX_PRE_OFFSET0_1 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_PRE_OFFSET0_1));
  DISP_INFO("VPU_VPP_POST_MATRIX_PRE_OFFSET2 = 0x%x",
            vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_PRE_OFFSET2));
  DISP_INFO("VPU_VPP_POST_MATRIX_EN_CTRL = 0x%x", vpu_mmio_->Read32(VPU_VPP_POST_MATRIX_EN_CTRL));
}

void Osd::DumpRdmaState() {
  DISP_INFO("\n\n============ RDMA STATE DUMP ============\n\n");
  DISP_INFO("Dumping all RDMA related States\n");
  DISP_INFO("rdma is %s", rdma_active_ ? "Active" : "Not Active");

  DumpRdmaRegisters();

  DISP_INFO("RDMA Table Content:\n");
  for (auto& i : rdma_usage_table_) {
    DISP_INFO("[0x%lx]", i);
  }

  DISP_INFO("start_index = %ld, end_index = %ld", start_index_used_, end_index_used_);
  DISP_INFO("latest applied config stamp = 0x%lx", latest_applied_config_.value);
  DISP_INFO("\n\n=========================================\n\n");
}

void Osd::Release() {
  Disable();
  rdma_irq_.destroy();
  thrd_join(rdma_irq_thread_, nullptr);
  rdma_pmt_.unpin();
}

}  // namespace amlogic_display
