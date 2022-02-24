// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/vmm/linux.h"

#include <endian.h>
#include <fcntl.h>
#include <lib/fdio/fd.h>
#include <lib/zircon-internal/e820.h>
#include <sys/stat.h>

#include <fbl/unique_fd.h>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/virtualization/bin/vmm/bits.h"
#include "src/virtualization/bin/vmm/guest.h"

__BEGIN_CDECLS
#include <libfdt.h>
__END_CDECLS

#if __aarch64__
// This address works for direct-mapping of host memory. This address is chosen
// to ensure that we do not collide with the mapping of the host kernel.
static constexpr uintptr_t kKernelOffset = 0x2080000;
#elif __x86_64__
static constexpr uintptr_t kKernelOffset = 0x200000;
#include "src/virtualization/bin/vmm/arch/x64/acpi.h"
#include "src/virtualization/bin/vmm/arch/x64/e820.h"
#else
#error Unknown architecture.
#endif

static constexpr uint8_t kLoaderTypeUnspecified = 0xff;  // Unknown bootloader
static constexpr uint16_t kMinBootProtocol = 0x200;      // bzImage boot protocol
static constexpr uint16_t kBootFlagMagic = 0xaa55;
static constexpr uint32_t kHeaderMagic = 0x53726448;
static constexpr uintptr_t kEntryOffset = 0x200;
__UNUSED static constexpr uintptr_t kE820MapOffset = 0x02d0;
__UNUSED static constexpr size_t kMaxE820Entries = 128;
__UNUSED static constexpr size_t kSectorSize = 512;

static constexpr uint32_t kArm64ImageMagic = 0x644d5241;  // ARM\x64

struct SetupData {
  enum Type : uint32_t {
    Dtb = 2,
  };

  uint64_t next;
  Type type;
  uint32_t len;
  uint8_t data[0];
} __PACKED;

static constexpr char kDtbPath[] = "/pkg/data/board.dtb";
static constexpr uintptr_t kRamdiskOffset = 0x4000000;
static constexpr uintptr_t kDtbOffset = kRamdiskOffset - (PAGE_SIZE * 2);
static constexpr uintptr_t kDtbOverlayOffset = kDtbOffset - (PAGE_SIZE * 2);
static constexpr uintptr_t kDtbBootParamsOffset = kDtbOffset + sizeof(SetupData);

// clang-format off

// For the Linux x86 boot protocol, see:
// https://www.kernel.org/doc/Documentation/x86/boot.txt
// https://www.kernel.org/doc/Documentation/x86/zero-page.txt

enum Bp8 : uintptr_t {
  VIDEO_MODE                = 0x0006,   // Original video mode
  VIDEO_COLS                = 0x0007,   // Original video cols
  VIDEO_LINES               = 0x000e,   // Original video lines
  E820_COUNT                = 0x01e8,   // Number of entries in e820 map
  SETUP_SECTS               = 0x01f1,   // Size of real mode kernel in sectors
  LOADER_TYPE               = 0x0210,   // Type of bootloader
  LOADFLAGS                 = 0x0211,   // Boot protocol flags
  RELOCATABLE               = 0x0234,   // Whether the kernel relocatable
};

enum Bp16 : uintptr_t {
  BOOTFLAG                  = 0x01fe,   // Bootflag, should match BOOT_FLAG_MAGIC
  VERSION                   = 0x0206,   // Boot protocol version
  XLOADFLAGS                = 0x0236,   // Extended boot protocol flags
};

enum Bp32 : uintptr_t {
  SYSSIZE                   = 0x01f4,   // Size of protected-mode code in units of 16 bytes
  HEADER                    = 0x0202,   // Header, should match HEADER_MAGIC
  RAMDISK_IMAGE             = 0x0218,   // RAM disk image address
  RAMDISK_SIZE              = 0x021c,   // RAM disk image size
  COMMAND_LINE              = 0x0228,   // Pointer to command line args string
  KERNEL_ALIGN              = 0x0230,   // Kernel alignment
};

enum Bp64 : uintptr_t {
  SETUP_DATA                = 0x0250,   // Physical address to linked list of SetupData
};

enum Lf : uint8_t {
  LOAD_HIGH                 = 1u << 0,  // Protected mode code loads at 0x100000
};

enum Xlf : uint16_t {
  KERNEL_64                 = 1u << 0,  // Has legacy 64-bit entry point at 0x200
  CAN_BE_LOADED_ABOVE_4G    = 1u << 1,  // Kernel/boot_params/cmdline/ramdisk can be above 4G
};

// clang-format on

static uint8_t read_bp(const PhysMem& phys_mem, Bp8 off) {
  return phys_mem.read<uint8_t>(kKernelOffset + off);
}

static uint16_t read_bp(const PhysMem& phys_mem, Bp16 off) {
  return phys_mem.read<uint16_t>(kKernelOffset + off);
}

static uint32_t read_bp(const PhysMem& phys_mem, Bp32 off) {
  return phys_mem.read<uint32_t>(kKernelOffset + off);
}

[[maybe_unused]] static uint64_t read_bp(const PhysMem& phys_mem, Bp64 off) {
  return phys_mem.read<uint64_t>(kKernelOffset + off);
}

static void write_bp(const PhysMem& phys_mem, Bp8 off, uint8_t data) {
  phys_mem.write<uint8_t>(kKernelOffset + off, data);
}

[[maybe_unused]] static void write_bp(const PhysMem& phys_mem, Bp16 off, uint16_t data) {
  phys_mem.write<uint16_t>(kKernelOffset + off, data);
}

static void write_bp(const PhysMem& phys_mem, Bp32 off, uint32_t data) {
  phys_mem.write<uint32_t>(kKernelOffset + off, data);
}

static void write_bp(const PhysMem& phys_mem, Bp64 off, uint64_t data) {
  phys_mem.write<uint64_t>(kKernelOffset + off, data);
}

static bool is_boot_params(const PhysMem& phys_mem) {
  return read_bp(phys_mem, BOOTFLAG) == kBootFlagMagic && read_bp(phys_mem, HEADER) == kHeaderMagic;
}

// Header used to boot ARM64 kernels.
//
// See: https://www.kernel.org/doc/Documentation/arm64/booting.txt.
struct Arm64ImageHeader {
  uint32_t code0;
  uint32_t code1;
  uint64_t kernel_off;
  uint64_t kernel_len;
  uint64_t flags;
  uint64_t reserved0;
  uint64_t reserved1;
  uint64_t reserved2;
  uint32_t magic;
  uint32_t pe_off;
} __PACKED;
static_assert(sizeof(Arm64ImageHeader) == 64, "");

static bool is_arm64_image(const Arm64ImageHeader* header) {
  return header->kernel_len > sizeof(Arm64ImageHeader) && header->magic == kArm64ImageMagic;
}

static inline bool is_within(uintptr_t x, uintptr_t addr, uintptr_t size) {
  return x >= addr && x < addr + size;
}

static zx_status_t read_fd(const int fd, const PhysMem& phys_mem, const uintptr_t off,
                           size_t* file_size) {
  // Get the image file size.
  struct stat stat;
  ssize_t ret = fstat(fd, &stat);
  if (ret < 0) {
    FX_LOGS(ERROR) << "Failed to stat file:" << strerror(errno);
    return ZX_ERR_IO;
  }

  // Ensure it will fit in guest memory.
  if (static_cast<size_t>(stat.st_size) > phys_mem.size() - off) {
    FX_LOGS(ERROR) << "File too large for guest memory. File size: " << stat.st_size
                   << " byte(s), guest physical memory size: " << phys_mem.size()
                   << " byte(s), load offset: " << off;
    return ZX_ERR_NO_RESOURCES;
  }

  // Read into guest memory.
  ret = read(fd, phys_mem.ptr(off, stat.st_size), stat.st_size);
  if (ret != stat.st_size) {
    FX_LOGS(ERROR) << "Failed to read file:" << strerror(errno);
    return ZX_ERR_IO;
  }
  *file_size = stat.st_size;
  return ZX_OK;
}

zx_status_t load_kernel(fbl::unique_fd kernel_fd, const PhysMem& phys_mem,
                        const uintptr_t kernel_off) {
  size_t kernel_size;
  zx_status_t status = read_fd(kernel_fd.get(), phys_mem, kernel_off, &kernel_size);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to read kernel image";
    return status;
  }
  if (is_within(kDtbOffset, kernel_off, kernel_size)) {
    FX_LOGS(ERROR) << "Kernel location overlaps DTB location";
    return ZX_ERR_OUT_OF_RANGE;
  }
  return ZX_OK;
}

static zx_status_t read_device_tree(const int fd, const PhysMem& phys_mem, const uintptr_t off,
                                    const uintptr_t limit, void** dtb, size_t* dtb_size) {
  zx_status_t status = read_fd(fd, phys_mem, off, dtb_size);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to read device tree";
    return status;
  }
  if (off + *dtb_size > limit) {
    FX_LOGS(ERROR) << "Device tree is too large";
    return ZX_ERR_OUT_OF_RANGE;
  }
  *dtb = phys_mem.ptr(off, *dtb_size);
  int ret = fdt_check_header(*dtb);
  if (ret != 0) {
    FX_LOGS(ERROR) << "Invalid device tree " << ret;
    return ZX_ERR_IO;
  }
  return ZX_OK;
}

static zx_status_t read_boot_params(const PhysMem& phys_mem, uintptr_t* guest_ip) {
  // Validate kernel configuration.
  uint16_t xloadflags = read_bp(phys_mem, XLOADFLAGS);
  if (~xloadflags & (KERNEL_64 | CAN_BE_LOADED_ABOVE_4G)) {
    FX_LOGS(ERROR) << "Unsupported Linux kernel";
    return ZX_ERR_NOT_SUPPORTED;
  }
  uint16_t protocol = read_bp(phys_mem, VERSION);
  uint8_t loadflags = read_bp(phys_mem, LOADFLAGS);
  if (protocol < kMinBootProtocol || !(loadflags & LOAD_HIGH)) {
    FX_LOGS(ERROR) << "Linux kernel is not a bzImage";
    return ZX_ERR_NOT_SUPPORTED;
  }
  if (read_bp(phys_mem, RELOCATABLE) == 0) {
    FX_LOGS(ERROR) << "Linux kernel is not relocatable";
    return ZX_ERR_NOT_SUPPORTED;
  }
  uint64_t kernel_align = read_bp(phys_mem, KERNEL_ALIGN);
  if (kKernelOffset % kernel_align != 0) {
    FX_LOGS(ERROR) << "Linux kernel has unsupported alignment";
    return ZX_ERR_NOT_SUPPORTED;
  }

  // Calculate the offset to the protected mode kernel.
  uint8_t setup_sects = read_bp(phys_mem, SETUP_SECTS);
  if (setup_sects == 0) {
    // 0 here actually means 4, see boot.txt.
    setup_sects = 4;
  }
  uintptr_t setup_off = (setup_sects + 1) * kSectorSize;
  *guest_ip = kKernelOffset + kEntryOffset + setup_off;
  return ZX_OK;
}

static zx_status_t write_boot_params(const PhysMem& phys_mem, const DevMem& dev_mem,
                                     const std::string& cmdline, fbl::unique_fd dtb_overlay_fd,
                                     const size_t ramdisk_size) {
  // Set type of bootloader.
  write_bp(phys_mem, LOADER_TYPE, kLoaderTypeUnspecified);

  // Zero video, columns and lines to skip early video init.
  write_bp(phys_mem, VIDEO_MODE, 0);
  write_bp(phys_mem, VIDEO_COLS, 0);
  write_bp(phys_mem, VIDEO_LINES, 0);

  // Set the address and size of the initial RAM disk.
  if (ramdisk_size > 0) {
    write_bp(phys_mem, RAMDISK_IMAGE, kRamdiskOffset);
    write_bp(phys_mem, RAMDISK_SIZE, static_cast<uint32_t>(ramdisk_size));
  }

  // Copy the command line string.
  size_t cmdline_len = cmdline.size() + 1;
  if (phys_mem.size() < PAGE_SIZE || cmdline_len > PAGE_SIZE) {
    FX_LOGS(ERROR) << "Command line is too long";
    return ZX_ERR_OUT_OF_RANGE;
  }
  auto cmdline_off = static_cast<uint32_t>(phys_mem.size() - PAGE_SIZE);
  memcpy(phys_mem.ptr(cmdline_off, cmdline_len), cmdline.data(), cmdline_len);
  write_bp(phys_mem, COMMAND_LINE, cmdline_off);

  // If specified, load a device tree overlay.
  if (dtb_overlay_fd) {
    void* dtb;
    size_t dtb_size;
    zx_status_t status = read_device_tree(dtb_overlay_fd.get(), phys_mem, kDtbBootParamsOffset,
                                          kRamdiskOffset, &dtb, &dtb_size);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to read device tree";
      return status;
    }
    auto setup_data = phys_mem.read<SetupData>(kDtbOffset);
    setup_data.type = SetupData::Dtb;
    setup_data.len = static_cast<uint32_t>(dtb_size);
    phys_mem.write<SetupData>(kDtbOffset, setup_data);
    write_bp(phys_mem, SETUP_DATA, kDtbOffset);
  }

#if __x86_64__
  // Setup e820 memory map.
  E820Map e820_map(phys_mem.size(), dev_mem);
  for (const auto& range : dev_mem) {
    e820_map.AddReservedRegion(range.addr, range.size);
  }
  size_t e820_entries = e820_map.size();
  if (e820_entries > kMaxE820Entries) {
    FX_LOGS(ERROR) << "Not enough space for e820 memory map";
    return ZX_ERR_BAD_STATE;
  }
  write_bp(phys_mem, E820_COUNT, static_cast<uint8_t>(e820_entries));
  const size_t e820_size = e820_entries * sizeof(E820Entry);
  static_assert(((kKernelOffset + kE820MapOffset) % alignof(E820Entry)) == 0);
  E820Entry* e820_addr = phys_mem.aligned_as<E820Entry>(kKernelOffset + kE820MapOffset, e820_size);
  e820_map.copy(e820_addr);
#endif
  return ZX_OK;
}

static zx_status_t read_image_header(const PhysMem& phys_mem, uintptr_t* guest_ip) {
  Arm64ImageHeader image_header = phys_mem.read<Arm64ImageHeader>(kKernelOffset);
  if (!is_arm64_image(&image_header)) {
    FX_LOGS(ERROR) << "Kernel does not have a valid header";
    return ZX_ERR_NOT_SUPPORTED;
  }

  *guest_ip = kKernelOffset;
  return ZX_OK;
}

static void device_tree_error_msg(const char* property_name) {
  FX_LOGS(ERROR) << "Failed to add \"" << property_name << "\" to device "
                 << "tree, space must be reserved in the device tree";
}

static zx_status_t add_memory_entry(void* dtb, int memory_off, zx_gpaddr_t addr, size_t size) {
  uint64_t entry[2];
  entry[0] = htobe64(addr);
  entry[1] = htobe64(size);
  int ret = fdt_appendprop(dtb, memory_off, "reg", entry, sizeof(entry));
  if (ret < 0) {
    device_tree_error_msg("reg");
    return ZX_ERR_BAD_STATE;
  }
  return ZX_OK;
}

static zx_status_t load_device_tree(fbl::unique_fd dtb_fd,
                                    const fuchsia::virtualization::GuestConfig& cfg,
                                    const PhysMem& phys_mem, const DevMem& dev_mem,
                                    const std::vector<PlatformDevice*>& devices,
                                    const std::string& cmdline, fbl::unique_fd dtb_overlay_fd,
                                    const size_t ramdisk_size) {
  void* dtb;
  size_t dtb_size;
  zx_status_t status =
      read_device_tree(dtb_fd.get(), phys_mem, kDtbOffset, kRamdiskOffset, &dtb, &dtb_size);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to read device tree";
    return status;
  }

  // If specified, load a device tree overlay.
  if (dtb_overlay_fd) {
    void* dtb_overlay;
    status = read_device_tree(dtb_overlay_fd.get(), phys_mem, kDtbOverlayOffset, kDtbOffset,
                              &dtb_overlay, &dtb_size);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to read device tree overlay";
      return status;
    }
    int ret = fdt_overlay_apply(dtb, dtb_overlay);
    if (ret != 0) {
      FX_LOGS(ERROR) << "Failed to apply device tree overlay " << ret;
      return ZX_ERR_BAD_STATE;
    }
  }

  int off = fdt_path_offset(dtb, "/chosen");
  if (off < 0) {
    FX_LOGS(ERROR) << "Failed to find \"/chosen\" in device tree";
    return ZX_ERR_BAD_STATE;
  }

  // Add command line to device tree.
  int ret = fdt_setprop(dtb, off, "bootargs", cmdline.data(), static_cast<int>(cmdline.size() + 1));
  if (ret != 0) {
    device_tree_error_msg("bootargs");
    return ZX_ERR_BAD_STATE;
  }

  if (ramdisk_size > 0) {
    // Add the memory range of the initial RAM disk.
    ret = fdt_setprop_u64(dtb, off, "linux,initrd-start", kRamdiskOffset);
    if (ret != 0) {
      device_tree_error_msg("linux,initrd-start");
      return ZX_ERR_BAD_STATE;
    }
    ret = fdt_setprop_u64(dtb, off, "linux,initrd-end", kRamdiskOffset + ramdisk_size);
    if (ret != 0) {
      device_tree_error_msg("linux,initrd-end");
      return ZX_ERR_BAD_STATE;
    }
  }

  // Add CPUs to device tree.
  int cpus_off = fdt_path_offset(dtb, "/cpus");
  if (cpus_off < 0) {
    FX_LOGS(ERROR) << "Failed to find \"/cpus\" in device tree";
    return ZX_ERR_BAD_STATE;
  }
  for (int cpu = cfg.cpus() - 1; cpu >= 0; --cpu) {
    std::string name = fxl::StringPrintf("cpu@%d", cpu);
    int cpu_off = fdt_add_subnode(dtb, cpus_off, name.data());
    if (cpu_off < 0) {
      device_tree_error_msg("cpu");
      return ZX_ERR_BAD_STATE;
    }
    ret = fdt_setprop_string(dtb, cpu_off, "device_type", "cpu");
    if (ret != 0) {
      device_tree_error_msg("device_type");
      return ZX_ERR_BAD_STATE;
    }
    ret = fdt_setprop_string(dtb, cpu_off, "compatible", "arm,armv8");
    if (ret != 0) {
      device_tree_error_msg("compatible");
      return ZX_ERR_BAD_STATE;
    }
    ret = fdt_setprop_u32(dtb, cpu_off, "reg", static_cast<uint32_t>(cpu));
    if (ret != 0) {
      device_tree_error_msg("reg");
      return ZX_ERR_BAD_STATE;
    }
    ret = fdt_setprop_string(dtb, cpu_off, "enable-method", "psci");
    if (ret != 0) {
      device_tree_error_msg("enable-method");
      return ZX_ERR_BAD_STATE;
    }
  }

  // Add memory to device tree.
  int root_off = fdt_path_offset(dtb, "/");
  if (root_off < 0) {
    FX_LOGS(ERROR) << "Failed to find root node in device tree";
    return ZX_ERR_BAD_STATE;
  }
  auto yield = [&status, dtb, root_off](zx_gpaddr_t addr, size_t size) {
    if (status != ZX_OK) {
      return;
    }
    std::string name = fxl::StringPrintf("memory@%lx", addr);
    int memory_off = fdt_add_subnode(dtb, root_off, name.data());
    if (memory_off < 0) {
      status = ZX_ERR_BAD_STATE;
      return;
    }
    int ret = fdt_setprop_string(dtb, memory_off, "device_type", "memory");
    if (ret != 0) {
      status = ZX_ERR_BAD_STATE;
      return;
    }
    status = add_memory_entry(dtb, memory_off, addr, size);
  };
  for (const fuchsia::virtualization::MemorySpec& spec : cfg.memory()) {
    // Do not use device memory when yielding normal memory.
    if (spec.policy != fuchsia::virtualization::MemoryPolicy::HOST_DEVICE) {
      dev_mem.YieldInverseRange(spec.base, spec.size, yield);
      if (status != ZX_OK) {
        return status;
      }
    }
  }

  // Add all platform devices to device tree.
  for (auto device : devices) {
    status = device->ConfigureDtb(dtb);
    if (status != ZX_OK) {
      return status;
    }
  }

  return ZX_OK;
}

static std::string linux_cmdline(std::string cmdline) {
#if __x86_64__
  return fxl::StringPrintf("acpi_rsdp=%#lx %s", kAcpiOffset, cmdline.data());
#else
  return cmdline;
#endif
}

zx_status_t setup_linux(fuchsia::virtualization::GuestConfig* cfg, const PhysMem& phys_mem,
                        const DevMem& dev_mem, const std::vector<PlatformDevice*>& devices,
                        uintptr_t* guest_ip, uintptr_t* boot_ptr) {
  fbl::unique_fd kernel_fd;
  zx_status_t status = fdio_fd_create(cfg->mutable_kernel()->TakeChannel().release(),
                                      kernel_fd.reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to open kernel image";
    return status;
  }
  status = load_kernel(std::move(kernel_fd), phys_mem, kKernelOffset);
  if (status != ZX_OK) {
    return status;
  }

  size_t ramdisk_size = 0;
  if (cfg->has_ramdisk()) {
    fbl::unique_fd ramdisk_fd;
    status = fdio_fd_create(cfg->mutable_ramdisk()->TakeChannel().release(),
                            ramdisk_fd.reset_and_get_address());
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to open initial RAM disk";
      return ZX_ERR_IO;
    }

    status = read_fd(ramdisk_fd.get(), phys_mem, kRamdiskOffset, &ramdisk_size);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to read initial RAM disk";
      return status;
    }
  }

  fbl::unique_fd dtb_overlay_fd;
  if (cfg->has_dtb_overlay()) {
    status = fdio_fd_create(cfg->mutable_dtb_overlay()->TakeChannel().release(),
                            dtb_overlay_fd.reset_and_get_address());
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to open device tree overlay";
      return ZX_ERR_IO;
    }
  }

  const std::string cmdline = linux_cmdline(cfg->cmdline());
  if (is_boot_params(phys_mem)) {
    status = read_boot_params(phys_mem, guest_ip);
    if (status != ZX_OK) {
      return status;
    }
    status = write_boot_params(phys_mem, dev_mem, cmdline, std::move(dtb_overlay_fd), ramdisk_size);
    if (status != ZX_OK) {
      return status;
    }
    *boot_ptr = kKernelOffset;
  } else {
    status = read_image_header(phys_mem, guest_ip);
    if (status != ZX_OK) {
      return status;
    }
    fbl::unique_fd dtb_fd(open(kDtbPath, O_RDONLY));
    if (!dtb_fd) {
      FX_LOGS(ERROR) << "Failed to open device tree " << kDtbPath;
      return ZX_ERR_IO;
    }
    status = load_device_tree(std::move(dtb_fd), *cfg, phys_mem, dev_mem, devices, cmdline,
                              std::move(dtb_overlay_fd), ramdisk_size);
    if (status != ZX_OK) {
      return status;
    }
    *boot_ptr = kDtbOffset;
  }

  return ZX_OK;
}
