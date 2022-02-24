// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "dsi-host.h"

#include <lib/ddk/debug.h>
#include <lib/device-protocol/display-panel.h>

#include <fbl/alloc_checker.h>

namespace amlogic_display {

#define READ32_MIPI_DSI_REG(a) mipi_dsi_mmio_->Read32(a)
#define WRITE32_MIPI_DSI_REG(a, v) mipi_dsi_mmio_->Write32(v, a)

#define READ32_HHI_REG(a) hhi_mmio_->Read32(a)
#define WRITE32_HHI_REG(a, v) hhi_mmio_->Write32(v, a)

constexpr uint32_t kFitiDisplayId = 0x00936504;

void DsiHost::FixupPanelType() {
  if (panel_type_ != PANEL_TV070WSM_FT_9365 || display_id_ != 0) {
    // This fixup is either unnecessary or has been done before.
    return;
  }
  if (Lcd::GetDisplayId(dsiimpl_, &display_id_) != ZX_OK) {
    DISP_ERROR("Failed to read display ID, assuming the board driver panel type is correct");
    display_id_ = 0;
    return;
  }
  if (display_id_ != kFitiDisplayId) {
    DISP_INFO("Display ID is not 0x%x, rather 0x%x\nAssuming Sitronix\n", kFitiDisplayId,
              display_id_);
    panel_type_ = PANEL_TV070WSM_ST7703I;
  }
}

DsiHost::DsiHost(zx_device_t* parent, uint32_t panel_type)
    : pdev_(ddk::PDev::FromFragment(parent)),
      dsiimpl_(parent, "dsi"),
      lcd_gpio_(parent, "gpio"),
      panel_type_(panel_type) {}

// static
zx::status<std::unique_ptr<DsiHost>> DsiHost::Create(zx_device_t* parent, uint32_t panel_type) {
  fbl::AllocChecker ac;
  std::unique_ptr<DsiHost> self = fbl::make_unique_checked<DsiHost>(&ac, DsiHost(parent, panel_type));
  if (!ac.check()) {
    DISP_ERROR("No memory to allocate a DSI host\n");
    return zx::error(ZX_ERR_NO_MEMORY);
  }
  if (!self->pdev_.is_valid()) {
    DISP_ERROR("DsiHost: Could not get ZX_PROTOCOL_PDEV protocol\n");
    return zx::error(ZX_ERR_INVALID_ARGS);
  }

  // Map MIPI DSI and HHI registers
  zx_status_t status = self->pdev_.MapMmio(MMIO_MPI_DSI, &(self->mipi_dsi_mmio_));
  if (status != ZX_OK) {
    DISP_ERROR("Could not map MIPI DSI mmio %s\n", zx_status_get_string(status));
    return zx::error(status);
  }

  status = self->pdev_.MapMmio(MMIO_HHI, &(self->hhi_mmio_));
  if (status != ZX_OK) {
    DISP_ERROR("Could not map HHI mmio %s\n", zx_status_get_string(status));
    return zx::error(status);
  }

  return zx::ok(std::move(self));
}

zx_status_t DsiHost::HostModeInit(const display_setting_t& disp_setting) {
  // Setup relevant TOP_CNTL register -- Undocumented --
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_CNTL, SUPPORTED_DPI_FORMAT, TOP_CNTL_DPI_CLR_MODE_START,
            TOP_CNTL_DPI_CLR_MODE_BITS);
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_CNTL, SUPPORTED_VENC_DATA_WIDTH, TOP_CNTL_IN_CLR_MODE_START,
            TOP_CNTL_IN_CLR_MODE_BITS);
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_CNTL, 0, TOP_CNTL_CHROMA_SUBSAMPLE_START,
            TOP_CNTL_CHROMA_SUBSAMPLE_BITS);

  // setup dsi config
  dsi_config_t dsi_cfg;
  dsi_cfg.display_setting = disp_setting;
  dsi_cfg.video_mode_type = VIDEO_MODE_BURST;
  dsi_cfg.color_coding = COLOR_CODE_PACKED_24BIT_888;

  designware_config_t dw_cfg;
  dw_cfg.lp_escape_time = phy_->GetLowPowerEscaseTime();
  dw_cfg.lp_cmd_pkt_size = LPCMD_PKT_SIZE;
  dw_cfg.phy_timer_clkhs_to_lp = PHY_TMR_LPCLK_CLKHS_TO_LP;
  dw_cfg.phy_timer_clklp_to_hs = PHY_TMR_LPCLK_CLKLP_TO_HS;
  dw_cfg.phy_timer_hs_to_lp = PHY_TMR_HS_TO_LP;
  dw_cfg.phy_timer_lp_to_hs = PHY_TMR_LP_TO_HS;
  dw_cfg.auto_clklane = 1;
  dsi_cfg.vendor_config_buffer = reinterpret_cast<uint8_t*>(&dw_cfg);

  dsiimpl_.Config(&dsi_cfg);

  return ZX_OK;
}

void DsiHost::PhyEnable() {
  WRITE32_REG(HHI, HHI_MIPI_CNTL0,
              MIPI_CNTL0_CMN_REF_GEN_CTRL(0x29) | MIPI_CNTL0_VREF_SEL(VREF_SEL_VR) |
                  MIPI_CNTL0_LREF_SEL(LREF_SEL_L_ROUT) | MIPI_CNTL0_LBG_EN |
                  MIPI_CNTL0_VR_TRIM_CNTL(0x7) | MIPI_CNTL0_VR_GEN_FROM_LGB_EN);
  WRITE32_REG(HHI, HHI_MIPI_CNTL1, MIPI_CNTL1_DSI_VBG_EN | MIPI_CNTL1_CTL);
  WRITE32_REG(HHI, HHI_MIPI_CNTL2, MIPI_CNTL2_DEFAULT_VAL);  // 4 lane
}

void DsiHost::PhyDisable() {
  WRITE32_REG(HHI, HHI_MIPI_CNTL0, 0);
  WRITE32_REG(HHI, HHI_MIPI_CNTL1, 0);
  WRITE32_REG(HHI, HHI_MIPI_CNTL2, 0);
}

void DsiHost::Disable(const display_setting_t& disp_setting) {
  // turn host off only if it's been fully turned on
  if (!enabled_) {
    return;
  }

  // Place dsi in command mode first
  dsiimpl_.SetMode(DSI_MODE_COMMAND);

  // Turn off LCD
  lcd_->Disable();

  // disable PHY
  PhyDisable();

  // finally shutdown host
  phy_->Shutdown();

  enabled_ = false;
}

zx_status_t DsiHost::Enable(const display_setting_t& disp_setting, uint32_t bitrate) {
  if (enabled_) {
    return ZX_OK;
  }

  // Enable MIPI PHY
  PhyEnable();

  // Create MIPI PHY object
  fbl::AllocChecker ac;
  phy_ = fbl::make_unique_checked<amlogic_display::MipiPhy>(&ac);
  if (!ac.check()) {
    DISP_ERROR("Could not create MipiPhy object\n");
    return ZX_ERR_NO_MEMORY;
  }
  zx_status_t status = phy_->Init(pdev_, dsiimpl_, disp_setting.lane_num);
  if (status != ZX_OK) {
    DISP_ERROR("MIPI PHY Init failed!\n");
    return status;
  }

  // Load Phy configuration
  status = phy_->PhyCfgLoad(bitrate);
  if (status != ZX_OK) {
    DISP_ERROR("Error during phy config calculations! %d\n", status);
    return status;
  }

  // Enable dwc mipi_dsi_host's clock
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_CNTL, 0x3, 4, 2);
  // mipi_dsi_host's reset
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_SW_RESET, 0xf, 0, 4);
  // Release mipi_dsi_host's reset
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_SW_RESET, 0x0, 0, 4);
  // Enable dwc mipi_dsi_host's clock
  SET_BIT32(MIPI_DSI, MIPI_DSI_TOP_CLK_CNTL, 0x3, 0, 2);

  WRITE32_REG(MIPI_DSI, MIPI_DSI_TOP_MEM_PD, 0);
  zx_nanosleep(zx_deadline_after(ZX_MSEC(10)));

  // Initialize host in command mode first
  dsiimpl_.SetMode(DSI_MODE_COMMAND);
  if ((status = HostModeInit(disp_setting)) != ZX_OK) {
    DISP_ERROR("Error during dsi host init! %d\n", status);
    return status;
  }

  // Initialize mipi dsi D-phy
  if ((status = phy_->Startup()) != ZX_OK) {
    DISP_ERROR("Error during MIPI D-PHY Initialization! %d\n", status);
    return status;
  }

  // TODO(fxbug.dev/12345): remove this hack when we have a solution for the
  // bootloader to inform the board driver of the correct panel type.
  FixupPanelType();

  // Load LCD Init values while in command mode
  lcd_ = fbl::make_unique_checked<amlogic_display::Lcd>(&ac, panel_type_);
  if (!ac.check()) {
    DISP_ERROR("Failed to create LCD object\n");
    return ZX_ERR_NO_MEMORY;
  }

  status = lcd_->Init(dsiimpl_, lcd_gpio_);
  if (status != ZX_OK) {
    DISP_ERROR("Error during LCD Initialization! %d\n", status);
    return status;
  }

  status = lcd_->Enable();
  if (status != ZX_OK) {
    DISP_ERROR("Could not enable LCD! %d\n", status);
    return status;
  }

  // switch to video mode
  dsiimpl_.SetMode(DSI_MODE_VIDEO);

  // Host is On and Active at this point
  enabled_ = true;
  return ZX_OK;
}

void DsiHost::Dump() {
  DISP_INFO("MIPI_DSI_TOP_SW_RESET = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_SW_RESET));
  DISP_INFO("MIPI_DSI_TOP_CLK_CNTL = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_CLK_CNTL));
  DISP_INFO("MIPI_DSI_TOP_CNTL = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_CNTL));
  DISP_INFO("MIPI_DSI_TOP_SUSPEND_CNTL = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_SUSPEND_CNTL));
  DISP_INFO("MIPI_DSI_TOP_SUSPEND_LINE = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_SUSPEND_LINE));
  DISP_INFO("MIPI_DSI_TOP_SUSPEND_PIX = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_SUSPEND_PIX));
  DISP_INFO("MIPI_DSI_TOP_MEAS_CNTL = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEAS_CNTL));
  DISP_INFO("MIPI_DSI_TOP_STAT = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_STAT));
  DISP_INFO("MIPI_DSI_TOP_MEAS_STAT_TE0 = 0x%x\n",
            READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEAS_STAT_TE0));
  DISP_INFO("MIPI_DSI_TOP_MEAS_STAT_TE1 = 0x%x\n",
            READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEAS_STAT_TE1));
  DISP_INFO("MIPI_DSI_TOP_MEAS_STAT_VS0 = 0x%x\n",
            READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEAS_STAT_VS0));
  DISP_INFO("MIPI_DSI_TOP_MEAS_STAT_VS1 = 0x%x\n",
            READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEAS_STAT_VS1));
  DISP_INFO("MIPI_DSI_TOP_INTR_CNTL_STAT = 0x%x\n",
            READ32_REG(MIPI_DSI, MIPI_DSI_TOP_INTR_CNTL_STAT));
  DISP_INFO("MIPI_DSI_TOP_MEM_PD = 0x%x\n", READ32_REG(MIPI_DSI, MIPI_DSI_TOP_MEM_PD));
}

}  // namespace amlogic_display
