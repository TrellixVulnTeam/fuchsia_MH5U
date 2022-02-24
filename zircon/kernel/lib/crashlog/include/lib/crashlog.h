// Copyright 2018 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#ifndef ZIRCON_KERNEL_LIB_CRASHLOG_INCLUDE_LIB_CRASHLOG_H_
#define ZIRCON_KERNEL_LIB_CRASHLOG_INCLUDE_LIB_CRASHLOG_H_

#include <lib/crashlog/panic_buffer.h>
#include <zircon/boot/crash-reason.h>

#include <kernel/persistent_ram.h>
#include <ktl/span.h>
#include <vm/vm_object.h>

#if defined(__aarch64__)

#include <arch/arm64.h>

#elif defined(__x86_64__)

#include <arch/x86.h>

#endif

#ifndef MIN_CRASHLOG_SIZE
#define MIN_CRASHLOG_SIZE 2048
#endif

static constexpr size_t kMinCrashlogSize = MIN_CRASHLOG_SIZE;
static_assert((kMinCrashlogSize % kPersistentRamAllocationGranularity) == 0,
              "Minimum reserved crashlog size must be a multiple of the persistent RAM allocation "
              "granularity");

typedef struct {
  uintptr_t base_address;
  iframe_t* iframe;
#if defined(__aarch64__)
  // On arm64, the ESR and FAR are important for diagnosing kernel crashes, but
  // are not included in the iframe_t.
  uint32_t esr;
  uint64_t far;
#endif
} crashlog_t;

extern crashlog_t g_crashlog;

// Serialize the crashlog to string into target. If `reason' is OOM, then a
// different preamble will be used, and the backtrace will not be included.
size_t crashlog_to_string(ktl::span<char> target, zircon_crash_reason_t reason);

// Stash the recovered crashlog for later retrieval with |crashlog_get_stashed|.
void crashlog_stash(fbl::RefPtr<VmObject> crashlog);

// Returns the previously stashed recovered crashlog, or nullptr.
fbl::RefPtr<VmObject> crashlog_get_stashed();

extern PanicBuffer panic_buffer;

// A FILE that writes to both |stdout| and the global |panic_buffer|.
extern FILE stdout_panic_buffer;

#endif  // ZIRCON_KERNEL_LIB_CRASHLOG_INCLUDE_LIB_CRASHLOG_H_
