// Copyright 2021 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include "phys/zbitl-allocation.h"

#include <lib/memalloc/range.h>

#include <fbl/alloc_checker.h>
#include <ktl/move.h>

// Yet another allocation interface.
fitx::result<ktl::string_view, Allocation> ZbitlScratchAllocator(size_t size) {
  fbl::AllocChecker ac;
  auto result = Allocation::New(ac, memalloc::Type::kPhysScratch, size);
  if (ac.check()) {
    return fitx::ok(ktl::move(result));
  }
  return fitx::error{"cannot allocate scratch memory"sv};
}
