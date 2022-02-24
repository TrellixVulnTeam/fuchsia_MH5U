// Copyright 2020 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#include <stdio.h>
#include <zircon/assert.h>

#include <phys/frame-pointer.h>
#include <phys/main.h>
#include <phys/stack.h>
#include <phys/symbolize.h>

// This is what ZX_ASSERT calls.
PHYS_SINGLETHREAD void __zx_panic(const char* format, ...) {
  // Print the message.
  va_list args;
  va_start(args, format);
  vprintf(format, args);
  va_end(args);

  // Now print the backtrace and stack dump.

  FramePointer frame_pointer_backtrace = FramePointer::BackTrace();

  uintptr_t scsp = GetShadowCallStackPointer();
  ShadowCallStackBacktrace shadow_call_stack_backtrace = boot_shadow_call_stack.BackTrace(scsp);
  if (shadow_call_stack_backtrace.empty()) {
    shadow_call_stack_backtrace = phys_exception_shadow_call_stack.BackTrace(scsp);
  }

  Symbolize& symbolize = *Symbolize::GetInstance();
  symbolize.PrintBacktraces(frame_pointer_backtrace, shadow_call_stack_backtrace);

  uintptr_t sp = reinterpret_cast<uintptr_t>(__builtin_frame_address(0));
  symbolize.PrintStack(sp);

  // Now crash.
  ArchPanicReset();
}
