// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_VIRTUALIZATION_BIN_VMM_ARCH_X64_E820_H_
#define SRC_VIRTUALIZATION_BIN_VMM_ARCH_X64_E820_H_

#include <lib/zircon-internal/e820.h>

#include <vector>

#include "src/virtualization/bin/vmm/dev_mem.h"

/**
 * Used to construct an E820 memory map
 *
 * It is not the responsibility of this class to detect or prevent region
 * overlap of either same or differently typed regions.
 */
class E820Map {
 public:
  /*
   * Create a new E820 map
   *
   * @param pmem_size Size of physical memory. The E820 map will contain as much
   *  RAM regions as can fit in the defined physical memory that do not
   *  collide with the provided dev_mem regions.
   */
  E820Map(size_t mem_size, const DevMem &dev_mem);

  void AddReservedRegion(zx_gpaddr_t addr, size_t size) {
    entries_.emplace_back(E820Entry{addr, size, E820Type::kReserved});
  }

  size_t size() const { return entries_.size(); }

  void copy(E820Entry *dest) { std::copy(entries_.begin(), entries_.end(), dest); }

 private:
  std::vector<E820Entry> entries_;
};

#endif  // SRC_VIRTUALIZATION_BIN_VMM_ARCH_X64_E820_H_
