// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_ARCH_X86_PHYS_ADDRESS_SPACE_H_
#define ZIRCON_KERNEL_ARCH_X86_PHYS_ADDRESS_SPACE_H_

#include <lib/page-table/types.h>

// Build page tables for identity-mapping all of physical memory and install
// them in the CPU (%cr3 register).
void InstallIdentityMapPageTables(page_table::MemoryManager& allocator);

#endif  // ZIRCON_KERNEL_ARCH_X86_PHYS_ADDRESS_SPACE_H_
