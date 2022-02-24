// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <cmdline.h>
#include <inttypes.h>
#include <lib/zbi/zbi.h>
#include <stdio.h>
#include <string.h>
#include <xefi.h>
#include <zircon/boot/image.h>
#include <zircon/limits.h>
#include <zircon/pixelformat.h>

#include <efi/protocol/graphics-output.h>
#include <efi/runtime-services.h>

#include "osboot.h"

static efi_guid zircon_guid = ZIRCON_VENDOR_GUID;
static char16_t crashlog_name[] = ZIRCON_CRASHLOG_EFIVAR;

static int add_staged_zbi_files(zbi_header_t* zbi, size_t capacity);
static size_t get_last_crashlog(efi_system_table* sys, void* ptr, size_t max) {
  efi_runtime_services* rs = sys->RuntimeServices;

  uint32_t attr = ZIRCON_CRASHLOG_EFIATTR;
  size_t sz = max;
  efi_status r = rs->GetVariable(crashlog_name, &zircon_guid, &attr, &sz, ptr);
  if (r == EFI_SUCCESS) {
    // Erase it
    rs->SetVariable(crashlog_name, &zircon_guid, ZIRCON_CRASHLOG_EFIATTR, 0, NULL);
  } else {
    sz = 0;
  }
  return sz;
}

// Converts an EFI memory type to a zbi_mem_range_t type.
uint32_t to_mem_range_type(uint32_t efi_mem_type) {
  switch (efi_mem_type) {
    case EfiLoaderCode:
    case EfiLoaderData:
    case EfiBootServicesCode:
    case EfiBootServicesData:
    case EfiConventionalMemory:
      return ZBI_MEM_RANGE_RAM;
  }
  return ZBI_MEM_RANGE_RESERVED;
}

static unsigned char scratch[32768];

static void start_zircon(uint64_t entry, void* bootdata) {
#if __x86_64__
  // ebx = 0, ebp = 0, edi = 0, esi = bootdata
  __asm__ __volatile__(
      "movl $0, %%ebp \n"
      "cli \n"
      "jmp *%[entry] \n" ::[entry] "a"(entry),
      [bootdata] "S"(bootdata), "b"(0), "D"(0));
#elif defined(__aarch64__)
  __asm__(
      "mov x0, %[zbi]\n"  // Argument register.
      "mov x29, xzr\n"    // Clear FP.
      "mov x30, xzr\n"    // Clear LR.
      "br %[entry]\n" ::[entry] "r"(entry),
      [zbi] "r"(bootdata)
      : "x0", "x29", "x30");
#else
#error "add code for other arches here"
#endif
  __builtin_unreachable();
}

size_t image_getsize(void* image, size_t sz) {
  if (sz < sizeof(zircon_kernel_t)) {
    return 0;
  }
  zircon_kernel_t* kernel = image;
  if ((kernel->hdr_file.type != ZBI_TYPE_CONTAINER) || (kernel->hdr_file.magic != ZBI_ITEM_MAGIC) ||
      (kernel->hdr_kernel.type != ZBI_TYPE_KERNEL_X64) ||
      (kernel->hdr_kernel.magic != ZBI_ITEM_MAGIC)) {
    return 0;
  }
  return ZBI_ALIGN(kernel->hdr_file.length) + sizeof(zbi_header_t);
}

static int header_check(void* image, size_t sz, uint64_t* _entry, size_t* _flen, size_t* _klen) {
  zbi_header_t* bd = image;
  size_t flen, klen;
  uint64_t entry;

  if (!(bd->flags & ZBI_FLAG_VERSION)) {
    printf("boot: v1 bootdata kernel no longer supported\n");
    return -1;
  }
  zircon_kernel_t* kernel = image;
  if ((sz < sizeof(zircon_kernel_t)) || (kernel->hdr_kernel.type != ZBI_TYPE_KERNEL_X64) ||
      ((kernel->hdr_kernel.flags & ZBI_FLAG_VERSION) == 0)) {
    printf("boot: invalid zircon kernel header\n");
    return -1;
  }
  flen = ZBI_ALIGN(kernel->hdr_file.length);
  klen = ZBI_ALIGN(kernel->hdr_kernel.length);
  entry = kernel->data_kernel.entry;
  if (flen > (sz - sizeof(zbi_header_t))) {
    printf("boot: invalid zircon kernel header (bad flen)\n");
    return -1;
  }

  if (klen > (sz - (sizeof(zbi_header_t) * 2))) {
    printf("boot: invalid zircon kernel header (bad klen)\n");
    return -1;
  }
  // TODO(fxbug.dev/32255): Eventually the fixed-position case can be removed.

  const uint64_t kFixedLoadAddress = 0x100000;
  const uint64_t image_len = (2 * sizeof(zbi_header_t)) + klen;
  if (entry > kFixedLoadAddress && entry - kFixedLoadAddress < image_len) {
    printf("detected fixed-position kernel: entry address %#" PRIx64 "\n", entry);
  } else if (entry < kFixedLoadAddress && entry < image_len) {
    printf("detected position-independent kernel: entry offset %#" PRIx64 "\n", entry);
    entry += kernel_zone_base;
  } else {
    printf("boot: invalid entry address %#" PRIx64 "\n", entry);
    return -1;
  }
  if (_entry) {
    *_entry = entry;
  }
  if (_flen) {
    *_flen = flen;
    *_klen = klen;
  }

  return 0;
}

// TODO: verify crc32 when present
static int item_check(zbi_header_t* bd, size_t sz) {
  if (sz > 0x7FFFFFFF) {
    // disallow 2GB+ items to avoid wrap on align issues
    return -1;
  }
  if ((bd->magic != ZBI_ITEM_MAGIC) || ((bd->flags & ZBI_FLAG_VERSION) == 0) ||
      (ZBI_ALIGN(bd->length) > sz)) {
    return -1;
  } else {
    return 0;
  }
}

bool image_is_valid(void* image, size_t sz) {
  if (sz < sizeof(zbi_header_t)) {
    printf("image is too small\n");
    return false;
  }

  zbi_header_t* bd = image;
  sz -= sizeof(zbi_header_t);
  if ((bd->type != ZBI_TYPE_CONTAINER) || item_check(bd, sz)) {
    printf("image has invalid header\n");
    return false;
  }
  image += sizeof(zbi_header_t);

  enum {
    kKernelAbsent,
    kKernelFirst,
    kKernelLater,
  } kernel = kKernelAbsent;
  bool bootfs = false;
  bool empty = true;

  while (sz > sizeof(zbi_header_t)) {
    bd = image;
    sz -= sizeof(zbi_header_t);
    if (item_check(image, sz)) {
      printf("image has invalid bootitem\n");
      return false;
    }
    if (ZBI_IS_KERNEL_BOOTITEM(bd->type)) {
      kernel = (empty && kernel == kKernelAbsent) ? kKernelFirst : kKernelLater;
    } else if (bd->type == ZBI_TYPE_STORAGE_BOOTFS) {
      bootfs = true;
    }
    empty = false;
    image += ZBI_ALIGN(bd->length) + sizeof(zbi_header_t);
    sz -= ZBI_ALIGN(bd->length);
  }

  if (empty) {
    printf("empty ZBI\n");
  }
  switch (kernel) {
    case kKernelAbsent:
      printf("no kernel item found\n");
      break;
    case kKernelLater:
      printf("kernel item out of order: must be first\n");
      break;
    case kKernelFirst:
      if (bootfs) {  // It's complete.
        return true;
      }
      printf("missing BOOTFS\n");
      break;
  }

  return false;
}

int boot_zircon(efi_handle img, efi_system_table* sys, void* image, size_t isz, void* ramdisk,
                size_t rsz, void* cmdline, size_t csz) {
  efi_boot_services* bs = sys->BootServices;
  uint64_t entry;

  if (header_check(image, isz, &entry, NULL, NULL)) {
    return -1;
  }
  if ((ramdisk == NULL) || (rsz < sizeof(zbi_header_t))) {
    printf("boot: ramdisk missing or too small\n");
    return -1;
  }
  if (isz > kernel_zone_size) {
    printf("boot: kernel image too large\n");
    return -1;
  }

  zbi_header_t* hdr0 = ramdisk;
  if ((hdr0->type != ZBI_TYPE_CONTAINER) || (hdr0->extra != ZBI_CONTAINER_MAGIC) ||
      !(hdr0->flags & ZBI_FLAG_VERSION)) {
    printf("boot: ramdisk has invalid bootdata header\n");
    return -1;
  }

  if ((hdr0->length > (rsz - sizeof(zbi_header_t)))) {
    printf("boot: ramdisk has invalid bootdata length\n");
    return -1;
  }

  // pass kernel commandline
  zbi_result_t result =
      zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_CMDLINE, 0, 0, cmdline, csz);
  if (result != ZBI_RESULT_OK) {
    return -1;
  }

  // pass ACPI root pointer
  uint64_t rsdp = find_acpi_root(img, sys);
  if (rsdp != 0) {
    result =
        zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_ACPI_RSDP, 0, 0, &rsdp, sizeof(rsdp));
    if (result != ZBI_RESULT_OK) {
      return -1;
    }
  }

  // pass SMBIOS entry point pointer
  uint64_t smbios = find_smbios(img, sys);
  if (smbios != 0) {
    result =
        zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_SMBIOS, 0, 0, &smbios, sizeof(smbios));
    if (result != ZBI_RESULT_OK) {
      return -1;
    }
  }

  // pass EFI system table
  uint64_t addr = (uintptr_t)sys;
  result = zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_EFI_SYSTEM_TABLE, 0, 0, &addr,
                                         sizeof(addr));
  if (result != ZBI_RESULT_OK) {
    return -1;
  }

  // pass framebuffer data
  efi_graphics_output_protocol* gop = NULL;
  bs->LocateProtocol(&GraphicsOutputProtocol, NULL, (void**)&gop);
  if (gop) {
    zbi_swfb_t fb = {
        .base = gop->Mode->FrameBufferBase,
        .width = gop->Mode->Info->HorizontalResolution,
        .height = gop->Mode->Info->VerticalResolution,
        .stride = gop->Mode->Info->PixelsPerScanLine,
        .format = get_zx_pixel_format(gop),
    };
    result =
        zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_FRAMEBUFFER, 0, 0, &fb, sizeof(fb));
    if (result != ZBI_RESULT_OK) {
      return -1;
    }
  }

  add_staged_zbi_files(ramdisk, rsz);

  memcpy((void*)kernel_zone_base, image, isz);

  // Obtain the system memory map
  size_t msize, dsize;
  for (int attempts = 0;; attempts++) {
    uint32_t dversion = 0;
    size_t mkey = 0;
    msize = sizeof(scratch);
    dsize = 0;
    efi_status r = sys->BootServices->GetMemoryMap(&msize, (efi_memory_descriptor*)scratch, &mkey,
                                                   &dsize, &dversion);
    if (r != EFI_SUCCESS) {
      printf("boot: cannot GetMemoryMap()\n");
      goto fail;
    }

    r = sys->BootServices->ExitBootServices(img, mkey);
    if (r == EFI_SUCCESS) {
      break;
    }
    if (r == EFI_INVALID_PARAMETER) {
      if (attempts > 0) {
        printf("boot: cannot ExitBootServices(): %s\n", xefi_strerror(r));
        goto fail;
      }
      // Attempting to exit may cause us to have to re-grab the
      // memory map, but if it happens more than once something's
      // broken.
      continue;
    }
    printf("boot: cannot ExitBootServices(): %s\n", xefi_strerror(r));
    goto fail;
  }

  // Past this block, we can assume that sizeof(zbi_mem_range_t) <= dsize.
  if (dsize < sizeof(efi_memory_descriptor)) {
    printf("boot: bad descriptor size: %zu\n", dsize);
    goto fail;
  }
  _Static_assert(sizeof(zbi_mem_range_t) <= sizeof(efi_memory_descriptor),
                 "Cannot assume that sizeof(zbi_mem_range_t) <= dsize");

  // Convert the memory map in place to a range of zbi_mem_range_t, the
  // preferred ZBI memory format. In-place conversion can safely be done
  // one-by-one, given that zbi_mem_range_t is smaller than a descriptor.
  const size_t num_ranges = msize / dsize;
  zbi_mem_range_t* ranges = (zbi_mem_range_t*)scratch;
  for (size_t i = 0; i < num_ranges; ++i) {
    const efi_memory_descriptor* desc = (const efi_memory_descriptor*)&scratch[i * dsize];
    const zbi_mem_range_t range = {
        .paddr = desc->PhysicalStart,
        .length = desc->NumberOfPages * ZX_PAGE_SIZE,
        .type = to_mem_range_type(desc->Type),
    };
    memcpy(&ranges[i], &range, sizeof(range));
  }

  result = zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_MEM_CONFIG, 0, 0, ranges,
                                         num_ranges * sizeof(zbi_mem_range_t));
  if (result != ZBI_RESULT_OK) {
    goto fail;
  }

  // obtain the last crashlog if we can
  size_t sz = get_last_crashlog(sys, scratch, 4096);
  if (sz > 0) {
    zbi_create_entry_with_payload(ramdisk, rsz, ZBI_TYPE_CRASHLOG, 0, 0, scratch, sz);
  }

  // jump to the kernel
  start_zircon(entry, ramdisk);

fail:
  return -1;
}

static char cmdline[CMDLINE_MAX];

int zbi_boot(efi_handle img, efi_system_table* sys, void* image, size_t sz) {
  size_t flen, klen;
  if (header_check(image, sz, NULL, &flen, &klen)) {
    return -1;
  }

  // ramdisk portion is file - headers - kernel len
  uint32_t rlen = flen - sizeof(zbi_header_t) - klen;
  uint32_t roff = (sizeof(zbi_header_t) * 2) + klen;
  if (rlen == 0) {
    printf("zedboot: no ramdisk?!\n");
    return -1;
  }

  // allocate space for the ramdisk
  efi_boot_services* bs = sys->BootServices;
  size_t rsz = rlen + sizeof(zbi_header_t) + EXTRA_ZBI_ITEM_SPACE;
  size_t pages = BYTES_TO_PAGES(rsz);
  void* ramdisk = NULL;
  efi_status r =
      bs->AllocatePages(AllocateAnyPages, EfiLoaderData, pages, (efi_physical_addr*)&ramdisk);
  if (r) {
    printf("zedboot: cannot allocate ramdisk buffer\n");
    return -1;
  }

  // Set up the header.
  *(zbi_header_t*)ramdisk = (zbi_header_t)ZBI_CONTAINER_HEADER(rlen);
  // Copy in place the existing ramdisk and boot items.
  memcpy(ramdisk + sizeof(zbi_header_t), image + roff, rlen);
  rlen += sizeof(zbi_header_t);

  printf("ramdisk @ %p\n", ramdisk);
  zbi_result_t r2 = zbi_check(ramdisk, NULL);
  printf("check result %d\n", r2);

  size_t csz = cmdline_to_string(cmdline, sizeof(cmdline));

  // shrink original image header to include only the kernel
  zircon_kernel_t* kernel = image;
  kernel->hdr_file.length = sizeof(zbi_header_t) + klen;

  return boot_zircon(img, sys, image, roff, ramdisk, rsz, cmdline, csz);
}

// Buffer to keep staged ZBI files.
// We store them in their own ZBI container, so we lose a little bit of extra space, but makes
// copying to the final ZBI trivial.
//
// We have enough space for 3 SSH keys.
static uint8_t zbi_files[4096] __attribute__((aligned(ZBI_ALIGNMENT)));
static bool zbi_files_initialized = false;

int zircon_stage_zbi_file(const char* name, const uint8_t* data, size_t data_len) {
  size_t name_len = strlen(name);
  if (name_len > UINT8_MAX) {
    printf("ZBI filename too long");
    return -1;
  }
  // Payload = (name_length_byte + name + data), size must fit in a uint32_t.
  size_t payload_length = 1 + name_len + data_len;
  if (payload_length > UINT32_MAX || payload_length < data_len) {
    printf("ZBI file data too large");
    return -1;
  }
  if (!zbi_files_initialized) {
    zbi_result_t result = zbi_init(zbi_files, sizeof(zbi_files));
    if (result != ZBI_RESULT_OK) {
      printf("Failed to initialize zbi_files: %d\n", result);
      return -1;
    }
    zbi_files_initialized = true;
  }
  void* payload_as_void = NULL;
  zbi_result_t result = zbi_create_entry(zbi_files, sizeof(zbi_files), ZBI_TYPE_BOOTLOADER_FILE, 0,
                                         0, payload_length, &payload_as_void);
  if (result != ZBI_RESULT_OK) {
    printf("Failed to create ZBI file entry: %d\n", result);
    return -1;
  }
  uint8_t* payload = payload_as_void;
  payload[0] = name_len;
  memcpy(&payload[1], name, name_len);
  memcpy(&payload[1 + name_len], data, data_len);
  return 0;
}

static int add_staged_zbi_files(zbi_header_t* zbi, size_t capacity) {
  if (!zbi_files_initialized) {
    return 0;
  }
  zbi_result_t result = zbi_extend(zbi, capacity, zbi_files);
  if (result != ZBI_RESULT_OK) {
    printf("Failed to add staged ZBI files: %d\n", result);
    return -1;
  }
  printf("Added staged ZBI files with total ZBI size %u\n", ((zbi_header_t*)zbi_files)->length);
  return 0;
}
