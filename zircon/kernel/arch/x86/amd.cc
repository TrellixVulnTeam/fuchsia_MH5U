// Copyright 2019 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "amd.h"

#include <arch/x86.h>
#include <arch/x86/cpuid.h>
#include <arch/x86/feature.h>
#include <arch/x86/platform_access.h>
#include <kernel/mp.h>

uint32_t x86_amd_get_patch_level(void) {
  uint32_t patch_level = 0;
  if (!x86_feature_test(X86_FEATURE_HYPERVISOR)) {
    patch_level = static_cast<uint32_t>(read_msr(X86_MSR_IA32_BIOS_SIGN_ID));
  }
  return patch_level;
}

void x86_amd_set_lfence_serializing(const cpu_id::CpuId* cpuid, MsrAccess* msr) {
  // "Software Techniques for Managing Speculation on AMD Processors"
  // Mitigation G-2: Set MSR so that LFENCE is a dispatch-serializing instruction.
  //
  // To mitigate certain speculative execution infoleaks (Spectre) efficiently, configure the
  // CPU to treat LFENCE as a dispatch serializing instruction. This allows code to use LFENCE
  // in contexts to restrict speculative execution.
  if (cpuid->ReadProcessorId().family() >= 0x10) {
    uint64_t de_cfg = msr->read_msr(X86_MSR_AMD_F10_DE_CFG);
    if (!(de_cfg & X86_MSR_AMD_F10_DE_CFG_LFENCE_SERIALIZE)) {
      msr->write_msr(X86_MSR_AMD_F10_DE_CFG, de_cfg | X86_MSR_AMD_F10_DE_CFG_LFENCE_SERIALIZE);
    }
  }
}

void x86_amd_init_percpu(void) {
  cpu_id::CpuId cpuid;
  MsrAccess msr;
  x86_amd_set_lfence_serializing(&cpuid, &msr);
}
