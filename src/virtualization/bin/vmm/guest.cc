// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/virtualization/bin/vmm/guest.h"

#include <fcntl.h>
#include <lib/syslog/cpp/macros.h>
#include <zircon/status.h>
#include <zircon/syscalls/hypervisor.h>
#include <zircon/threads.h>

#include "src/lib/fxl/strings/string_printf.h"
#include "src/virtualization/bin/vmm/sysinfo.h"

#if __aarch64__
static constexpr uint8_t kSpiBase = 32;
#endif

static constexpr uint32_t trap_kind(TrapType type) {
  switch (type) {
    case TrapType::MMIO_SYNC:
      return ZX_GUEST_TRAP_MEM;
    case TrapType::MMIO_BELL:
      return ZX_GUEST_TRAP_BELL;
    case TrapType::PIO_SYNC:
      return ZX_GUEST_TRAP_IO;
    default:
      ZX_PANIC("Unhandled TrapType %d\n", static_cast<int>(type));
      return 0;
  }
}

static constexpr uint32_t cache_policy(fuchsia::virtualization::MemoryPolicy policy) {
  switch (policy) {
    case fuchsia::virtualization::MemoryPolicy::HOST_DEVICE:
      return ZX_CACHE_POLICY_UNCACHED_DEVICE;
    default:
      return ZX_CACHE_POLICY_CACHED;
  }
}

zx_status_t Guest::Init(const std::vector<fuchsia::virtualization::MemorySpec>& memory) {
  zx::resource hypervisor_resource;
  zx_status_t status = get_hypervisor_resource(&hypervisor_resource);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to get hypervisor resource " << zx_status_get_string(status);
    return status;
  }
  status = zx::guest::create(hypervisor_resource, 0, &guest_, &vmar_);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to create guest " << zx_status_get_string(status);
    return status;
  }

  zx::resource mmio_resource;
  zx::resource vmex_resource;
  for (const fuchsia::virtualization::MemorySpec& spec : memory) {
    zx::vmo vmo;
    switch (spec.policy) {
      case fuchsia::virtualization::MemoryPolicy::GUEST_CACHED:
        status = zx::vmo::create(spec.size, 0, &vmo);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to create VMO " << zx_status_get_string(status);
          return status;
        }
        break;
      case fuchsia::virtualization::MemoryPolicy::HOST_CACHED:
      case fuchsia::virtualization::MemoryPolicy::HOST_DEVICE:
        if (!mmio_resource) {
          status = get_mmio_resource(&mmio_resource);
          if (status != ZX_OK) {
            FX_LOGS(ERROR) << "Failed to get mmio resource " << zx_status_get_string(status);
            return status;
          }
        }
        status = zx::vmo::create_physical(mmio_resource, spec.base, spec.size, &vmo);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to create physical VMO " << zx_status_get_string(status);
          return status;
        }
        status = vmo.set_cache_policy(cache_policy(spec.policy));
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to set cache policy on VMO " << zx_status_get_string(status);
          return status;
        }
        break;
      default:
        FX_LOGS(ERROR) << "Unknown memory policy " << static_cast<uint32_t>(spec.policy);
        return ZX_ERR_INVALID_ARGS;
    }

    if (!vmex_resource) {
      status = get_vmex_resource(&vmex_resource);
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to get VMEX resource " << zx_status_get_string(status);
        return status;
      }
    }
    status = vmo.replace_as_executable(vmex_resource, &vmo);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to make VMO executable " << zx_status_get_string(status);
      return status;
    }

    zx_gpaddr_t addr;
    status = vmar_.map(ZX_VM_PERM_READ | ZX_VM_PERM_WRITE | ZX_VM_PERM_EXECUTE | ZX_VM_SPECIFIC |
                           ZX_VM_REQUIRE_NON_RESIZABLE,
                       spec.base, vmo, 0, spec.size, &addr);
    if (status != ZX_OK) {
      FX_LOGS(ERROR) << "Failed to map guest physical memory " << zx_status_get_string(status);
      return status;
    }
    if (!phys_mem_.vmo() && spec.policy == fuchsia::virtualization::MemoryPolicy::GUEST_CACHED) {
      status = phys_mem_.Init(std::move(vmo));
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to initialize guest physical memory "
                       << zx_status_get_string(status);
        return status;
      }
    }
  }

  return ZX_OK;
}

zx_status_t Guest::CreateMapping(TrapType type, uint64_t addr, size_t size, uint64_t offset,
                                 IoHandler* handler, async_dispatcher_t* dispatcher) {
  uint32_t kind = trap_kind(type);
  mappings_.emplace_front(kind, addr, size, offset, handler);
  zx_status_t status = mappings_.front().SetTrap(this, dispatcher);
  if (status != ZX_OK) {
    mappings_.pop_front();
    return status;
  }
  return ZX_OK;
}

zx_status_t Guest::CreateSubVmar(uint64_t addr, size_t size, zx::vmar* vmar) {
  uintptr_t guest_addr;
  return vmar_.allocate(ZX_VM_CAN_MAP_READ | ZX_VM_CAN_MAP_WRITE | ZX_VM_SPECIFIC, addr, size, vmar,
                        &guest_addr);
}

zx_status_t Guest::StartVcpu(uint64_t id, zx_gpaddr_t entry, zx_gpaddr_t boot_ptr,
                             async::Loop* loop) {
  if (id >= kMaxVcpus) {
    FX_LOGS(ERROR) << "Failed to start VCPU-" << id << ", up to " << kMaxVcpus
                   << " VCPUs are supported";
    return ZX_ERR_OUT_OF_RANGE;
  }

  std::lock_guard<std::shared_mutex> lock(mutex_);
  if (!vcpus_[0].has_value() && id != 0) {
    FX_LOGS(ERROR) << "VCPU-0 must be started before other VCPUs";
    return ZX_ERR_BAD_STATE;
  }
  if (vcpus_[id].has_value()) {
    // The guest might make multiple requests to start a particular VCPU. On
    // x86, the guest should send two START_UP IPIs but we initialize the VCPU
    // on the first. So, we ignore subsequent requests.
    return ZX_OK;
  }
  vcpus_[id].emplace(id, this, entry, boot_ptr, loop);
  return vcpus_[id]->Start();
}

zx_status_t Guest::Interrupt(uint64_t mask, uint32_t vector) {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  for (size_t id = 0; id != kMaxVcpus; ++id) {
    if (!(mask & (1ul << id)) || !vcpus_[id]) {
      continue;
    }
    zx_status_t status = vcpus_[id]->Interrupt(vector);
    if (status != ZX_OK) {
      return status;
    }
#if __aarch64__
    if (vector >= kSpiBase) {
      break;
    }
#endif
  }
  return ZX_OK;
}
