// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuchsia/hardware/pci/c/banjo.h>
#include <lib/ddk/debug.h>
#include <lib/ddk/device.h>
#include <lib/ddk/driver.h>
#include <lib/ddk/mmio-buffer.h>
#include <lib/device-protocol/pci.h>
#include <zircon/errors.h>
#include <zircon/pixelformat.h>
#include <zircon/process.h>

#include "simple-display.h"
#include "src/graphics/display/drivers/simple/simple-bochs-bind.h"

#define DISPLAY_WIDTH 1024
#define DISPLAY_HEIGHT 768
#define DISPLAY_FORMAT ZX_PIXEL_FORMAT_RGB_x888

inline uint16_t bochs_vbe_dispi_read(MMIO_PTR void* base, uint32_t reg) {
  return MmioRead16(reinterpret_cast<MMIO_PTR uint16_t*>(reinterpret_cast<MMIO_PTR uint8_t*>(base) +
                                                         (0x500 + (reg << 1))));
}

inline void bochs_vbe_dispi_write(MMIO_PTR void* base, uint32_t reg, uint16_t val) {
  MmioWrite16(val, reinterpret_cast<MMIO_PTR uint16_t*>(reinterpret_cast<MMIO_PTR uint8_t*>(base) +
                                                        (0x500 + (reg << 1))));
}

#define BOCHS_VBE_DISPI_ID 0x0
#define BOCHS_VBE_DISPI_XRES 0x1
#define BOCHS_VBE_DISPI_YRES 0x2
#define BOCHS_VBE_DISPI_BPP 0x3
#define BOCHS_VBE_DISPI_ENABLE 0x4
#define BOCHS_VBE_DISPI_BANK 0x5
#define BOCHS_VBE_DISPI_VIRT_WIDTH 0x6
#define BOCHS_VBE_DISPI_VIRT_HEIGHT 0x7
#define BOCHS_VBE_DISPI_X_OFFSET 0x8
#define BOCHS_VBE_DISPI_Y_OFFSET 0x9
#define BOCHS_VBE_DISPI_VIDEO_MEMORY_64K 0xa

static int zx_display_format_to_bpp(zx_pixel_format_t format) {
  unsigned bpp = ZX_PIXEL_FORMAT_BYTES(format) * 8;
  if (bpp == 0) {
    // unknown
    return -1;
  } else {
    return bpp;
  }
}

static void set_hw_mode(MMIO_PTR void* regs, uint16_t width, uint16_t height,
                        zx_pixel_format_t format) {
  zxlogf(TRACE, "id: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_ID));

  int bpp = zx_display_format_to_bpp(format);
  assert(bpp >= 0);

  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_ENABLE, 0);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_BPP, bpp);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_XRES, width);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_YRES, height);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_BANK, 0);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_VIRT_WIDTH, width);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_VIRT_HEIGHT, height);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_X_OFFSET, 0);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_Y_OFFSET, 0);
  bochs_vbe_dispi_write(regs, BOCHS_VBE_DISPI_ENABLE, 0x41);

  zxlogf(TRACE, "bochs_vbe_set_hw_mode:");
  zxlogf(TRACE, "     ID: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_ID));
  zxlogf(TRACE, "   XRES: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_XRES));
  zxlogf(TRACE, "   YRES: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_YRES));
  zxlogf(TRACE, "    BPP: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_BPP));
  zxlogf(TRACE, " ENABLE: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_ENABLE));
  zxlogf(TRACE, "   BANK: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_BANK));
  zxlogf(TRACE, "VWIDTH: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_VIRT_WIDTH));
  zxlogf(TRACE, "VHEIGHT: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_VIRT_HEIGHT));
  zxlogf(TRACE, "   XOFF: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_X_OFFSET));
  zxlogf(TRACE, "   YOFF: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_Y_OFFSET));
  zxlogf(TRACE, "    64K: 0x%x", bochs_vbe_dispi_read(regs, BOCHS_VBE_DISPI_VIDEO_MEMORY_64K));
}

static zx_status_t bochs_vbe_bind(void* ctx, zx_device_t* dev) {
  pci_protocol_t pci;
  zx_status_t status = ZX_OK;

  if ((status = device_get_fragment_protocol(dev, "pci", ZX_PROTOCOL_PCI, &pci)) != ZX_OK) {
    return status;
  }

  mmio_buffer_t mmio;
  // map register window
  status = pci_map_bar_buffer(&pci, 2u, ZX_CACHE_POLICY_UNCACHED_DEVICE, &mmio);
  if (status != ZX_OK) {
    printf("bochs-vbe: failed to map pci config: %d\n", status);
    return status;
  }

  set_hw_mode(mmio.vaddr, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_FORMAT);

  mmio_buffer_release(&mmio);

  return bind_simple_pci_display(dev, "bochs_vbe", 0u, DISPLAY_WIDTH, DISPLAY_HEIGHT, DISPLAY_WIDTH,
                                 DISPLAY_FORMAT);
}

static zx_driver_ops_t bochs_vbe_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = bochs_vbe_bind,
};

ZIRCON_DRIVER(bochs_vbe, bochs_vbe_driver_ops, "zircon", "0.1");
