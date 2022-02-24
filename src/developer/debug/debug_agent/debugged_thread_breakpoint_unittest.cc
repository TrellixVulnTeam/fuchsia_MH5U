// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <gtest/gtest.h>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/mock_debug_agent_harness.h"
#include "src/developer/debug/debug_agent/mock_exception_handle.h"
#include "src/developer/debug/debug_agent/mock_process.h"
#include "src/developer/debug/debug_agent/mock_thread.h"
#include "src/developer/debug/debug_agent/mock_thread_handle.h"

namespace debug_agent {

TEST(DebuggedThreadBreakpoint, NormalException) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kProcKoid = 12;
  MockProcess* process = harness.AddProcess(kProcKoid);
  constexpr zx_koid_t kThreadKoid = 23;
  MockThread* thread = process->AddThread(kThreadKoid);

  // Trigger the exception
  constexpr uint64_t kAddress = 0xdeadbeef;
  thread->SendException(kAddress, debug_ipc::ExceptionType::kPageFault);

  // We should've received an exception notification.
  ASSERT_EQ(harness.stream_backend()->exceptions().size(), 1u);
  EXPECT_EQ(harness.stream_backend()->exceptions()[0].type, debug_ipc::ExceptionType::kPageFault);
  EXPECT_EQ(harness.stream_backend()->exceptions()[0].hit_breakpoints.size(), 0u);

  auto& thread_record = harness.stream_backend()->exceptions()[0].thread;
  EXPECT_EQ(thread_record.id.process, kProcKoid);
  EXPECT_EQ(thread_record.id.thread, kThreadKoid);
  EXPECT_EQ(thread_record.state, debug_ipc::ThreadRecord::State::kBlocked);
  EXPECT_EQ(thread_record.blocked_reason, debug_ipc::ThreadRecord::BlockedReason::kException);
  EXPECT_EQ(thread_record.stack_amount, debug_ipc::ThreadRecord::StackAmount::kMinimal);
}

TEST(DebuggedThreadBreakpoint, SoftwareBreakpoint) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kProcKoid = 12;
  MockProcess* process = harness.AddProcess(kProcKoid);
  constexpr zx_koid_t kThread1Koid = 23;
  constexpr zx_koid_t kThread2Koid = 24;
  MockThread* thread1 = process->AddThread(kThread1Koid);
  MockThread* thread2 = process->AddThread(kThread2Koid);

  // Set an exception for a software breakpoint instruction. Since no breakpoint has been installed,
  // this will look like a hardcoded breakpoint instruction.
  constexpr uint64_t kBreakpointAddress = 0xdeadbeef;
  const uint64_t kExceptionAddress =
      kBreakpointAddress + arch::kExceptionOffsetForSoftwareBreakpoint;
  thread1->SendException(kExceptionAddress, debug_ipc::ExceptionType::kSoftwareBreakpoint);

  // Validate the exception notification.
  ASSERT_EQ(harness.stream_backend()->exceptions().size(), 1u);
  auto exception = harness.stream_backend()->exceptions()[0];
  EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kSoftwareBreakpoint);
  EXPECT_EQ(exception.hit_breakpoints.size(), 0u);
  EXPECT_EQ(exception.other_affected_threads.size(), 0u);  // No other threads should be stopped.

  // Resume the thread to clear the exception.
  harness.Resume();

  // Provide backing memory for the breakpoint. This is needed for the software breakpoint to be
  // installed. It doesn't matter what the contents is, only that a read will succeed.
  process->mock_process_handle().mock_memory().AddMemory(kBreakpointAddress, {0, 0, 0, 0});

  // Add a breakpoint on that address and throw the same exception as above.
  constexpr uint32_t kBreakpointId = 1;
  harness.AddOrChangeBreakpoint(kBreakpointId, kProcKoid, kBreakpointAddress);
  thread1->SendException(kExceptionAddress, debug_ipc::ExceptionType::kSoftwareBreakpoint);

  // Now the exception notification should reference the hit breakpoint.
  ASSERT_EQ(harness.stream_backend()->exceptions().size(), 2u);
  exception = harness.stream_backend()->exceptions()[1];

  EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kSoftwareBreakpoint);
  ASSERT_EQ(exception.hit_breakpoints.size(), 1u);
  EXPECT_EQ(exception.hit_breakpoints[0].id, kBreakpointId);

  // The other thread should be stopped because the default breakpoint stop mode is "all".
  // Note that the test doesn't update the ThreadRecord so the
  // other_affected_threads[0].state won't be correct. But we do check whether the thread thinks
  // it has been client suspended which is a more detailed check.
  EXPECT_TRUE(thread2->mock_thread_handle().is_suspended());
  ASSERT_EQ(exception.other_affected_threads.size(), 1u);
  EXPECT_EQ(exception.other_affected_threads[0].id.process, kProcKoid);
  EXPECT_EQ(exception.other_affected_threads[0].id.thread, kThread2Koid);

  // The breakpoint stats should be up-to-date.
  Breakpoint* breakpoint = harness.debug_agent()->GetBreakpoint(kBreakpointId);
  ASSERT_TRUE(breakpoint);
  EXPECT_EQ(1u, breakpoint->stats().hit_count);
}

TEST(DebuggedThreadBreakpoint, HardwareBreakpoint) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kProcKoid = 12;
  MockProcess* process = harness.AddProcess(kProcKoid);
  constexpr zx_koid_t kThreadKoid = 23;
  MockThread* thread = process->AddThread(kThreadKoid);

  // Set the exception information the arch provider is going to return.
  constexpr uint64_t kAddress = 0xdeadbeef;

  // Add a breakpoint on that address.
  constexpr uint32_t kBreakpointId = 1;
  harness.AddOrChangeBreakpoint(kBreakpointId, kProcKoid, kAddress,
                                debug_ipc::BreakpointType::kHardware);

  // Trigger an exception.
  thread->SendException(kAddress, debug_ipc::ExceptionType::kHardwareBreakpoint);

  // Validate the exception notification.
  ASSERT_EQ(harness.stream_backend()->exceptions().size(), 1u);
  auto exception = harness.stream_backend()->exceptions()[0];
  EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kHardwareBreakpoint);
  EXPECT_EQ(exception.hit_breakpoints.size(), 1u);
  EXPECT_EQ(exception.hit_breakpoints[0].id, kBreakpointId);

  // The breakpoint stats should be up-to-date.
  Breakpoint* breakpoint = harness.debug_agent()->GetBreakpoint(kBreakpointId);
  ASSERT_TRUE(breakpoint);
  EXPECT_EQ(1u, breakpoint->stats().hit_count);
}

TEST(DebuggedThreadBreakpoint, Watchpoint) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kProcKoid = 12;
  MockProcess* process = harness.AddProcess(kProcKoid);
  constexpr zx_koid_t kThreadKoid = 23;
  MockThread* thread = process->AddThread(kThreadKoid);

  // Add a watchpoint.
  const debug::AddressRange kRange = {0x1000, 0x1008};
  constexpr uint32_t kBreakpointId = 99;
  ASSERT_TRUE(harness
                  .AddOrChangeBreakpoint(kBreakpointId, kProcKoid, kThreadKoid, kRange,
                                         debug_ipc::BreakpointType::kWrite)
                  .ok());

  // Set the exception information in the debug registers to return. This should indicate the
  // watchpoint that was set up and triggered.
  const uint64_t kAddress = kRange.begin();
  DebugRegisters debug_regs;
  auto set_result = debug_regs.SetWatchpoint(debug_ipc::BreakpointType::kWrite, kRange, 4);
  ASSERT_TRUE(set_result);
  debug_regs.SetForHitWatchpoint(set_result->slot);
  thread->mock_thread_handle().SetDebugRegisters(debug_regs);

  // Trigger an exception.
  thread->SendException(kAddress, debug_ipc::ExceptionType::kWatchpoint);

  // Validate the expection information.
  auto exception = harness.stream_backend()->exceptions()[0];
  EXPECT_EQ(exception.type, debug_ipc::ExceptionType::kWatchpoint);
  ASSERT_EQ(exception.hit_breakpoints.size(), 1u);
  EXPECT_EQ(exception.hit_breakpoints[0].id, kBreakpointId);

  // The breakpoint stats should be up-to-date.
  Breakpoint* breakpoint = harness.debug_agent()->GetBreakpoint(kBreakpointId);
  ASSERT_TRUE(breakpoint);
  EXPECT_EQ(1u, breakpoint->stats().hit_count);
}

TEST(DebuggedThreadBreakpoint, BreakpointStepSuspendResume) {
  MockDebugAgentHarness harness;

  constexpr zx_koid_t kProcKoid = 1234;
  MockProcess* process = harness.AddProcess(kProcKoid);
  constexpr zx_koid_t kThreadKoid = 1235;
  MockThread* thread = process->AddThread(kThreadKoid);

  // Provide backing memory for the breakpoint. This is needed for the software breakpoint to be
  // installed. It doesn't matter what the contents is, only that a read will succeed.
  uint64_t kBreakpointAddress = 0x5000;
  process->mock_process_handle().mock_memory().AddMemory(kBreakpointAddress, {0, 0, 0, 0});

  // Create the breakpoint we'll hit.
  EXPECT_TRUE(harness.AddOrChangeBreakpoint(1, kProcKoid, kBreakpointAddress).ok());

  // Set up a hit of the breakpoint.
  const uint64_t kBreakpointExceptionAddr =
      kBreakpointAddress + arch::kExceptionOffsetForSoftwareBreakpoint;
  thread->SendException(kBreakpointExceptionAddr, debug_ipc::ExceptionType::kSoftwareBreakpoint);

  // Resume from the breakpoint which should clear the exception and try to single-step. But before
  // that does anything, pause the thread.
  harness.Resume();
  EXPECT_TRUE(thread->mock_thread_handle().single_step());
  EXPECT_FALSE(thread->in_exception());
  harness.Pause();
  EXPECT_EQ(1, thread->mock_thread_handle().suspend_count());

  // Now resume from the pause. This should resume from the exception and leave the thread in
  // single-step mode. This is tricky because the resume should not have cleared the single-step
  // flag even though the resume requested "continue".
  harness.Resume();
  EXPECT_TRUE(thread->mock_thread_handle().single_step());
}

}  // namespace debug_agent
