// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_RETPOLINE_INCLUDE_ARCH_X86_RETPOLINE_SELECTION_H_
#define ZIRCON_KERNEL_ARCH_X86_RETPOLINE_INCLUDE_ARCH_X86_RETPOLINE_SELECTION_H_

#include <lib/arch/x86/bug.h>
#include <lib/arch/x86/cpuid.h>
#include <lib/arch/x86/speculation.h>
#include <lib/boot-options/boot-options.h>

#include <string_view>

// Returns the appropriate code patching alternative of
// `__x86_indirect_thunk_r11()`.
template <typename CpuidIoProvider, typename MsrIoProvider>
inline std::string_view SelectX86RetpolineAlternative(CpuidIoProvider&& cpuid, MsrIoProvider&& msr,
                                                      const BootOptions& options) {
  // If the preferred Spectre v2 mitigation strategy is IBRS alone, then we
  // do not need retpolines.
  const bool mitigated_by_ibrs =
      arch::GetPreferredSpectreV2Mitigation(cpuid, msr) == arch::SpectreV2Mitigation::kIbrs;
  if (options.x86_disable_spec_mitigations || mitigated_by_ibrs) {
    return "__x86_indirect_thunk_unsafe_r11";
  }
  if (arch::GetVendor(cpuid) == arch::Vendor::kAmd) {
    return "__x86_indirect_thunk_amd_r11";
  }
  return "__x86_indirect_thunk_basic_r11";
}

#endif  // ZIRCON_KERNEL_ARCH_X86_RETPOLINE_INCLUDE_ARCH_X86_RETPOLINE_SELECTION_H_
