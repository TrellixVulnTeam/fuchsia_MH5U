// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
#define SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_

#include <stdint.h>

#include <set>

#include "src/developer/debug/debug_agent/arch.h"
#include "src/developer/debug/debug_agent/process_breakpoint.h"
#include "src/developer/debug/debug_agent/watchpoint_info.h"

namespace debug_agent {

class Watchpoint : public ProcessBreakpoint {
 public:
  // |type| must be kRead, kReadWrite or kWrite.
  explicit Watchpoint(debug_ipc::BreakpointType type, Breakpoint* breakpoint,
                      DebuggedProcess* process, const debug::AddressRange& range);
  ~Watchpoint();

  debug_ipc::BreakpointType Type() const override { return type_; }
  bool Installed(zx_koid_t thread_koid) const override;

  bool MatchesException(zx_koid_t thread_koid, uint64_t watchpoint_address, int slot);

  debug::Status Update() override;

  // Public ProcessBreakpoint overrides. See ProcessBreakpoint for more details.

  void EndStepOver(DebuggedThread* thread) override;
  void ExecuteStepOver(DebuggedThread* thread) override;
  void StepOverCleanup(DebuggedThread* thread) override {}

  // Getters.

  const std::map<zx_koid_t, WatchpointInfo>& installed_threads() const {
    return installed_threads_;
  }

  const debug::AddressRange& range() const { return range_; }

 private:
  bool Install(DebuggedThread* thread);

  debug::Status Uninstall(DebuggedThread* thread) override;
  debug::Status Uninstall() override;

  debug_ipc::BreakpointType type_ = debug_ipc::BreakpointType::kLast;

  debug::AddressRange range_;

  std::map<zx_koid_t, WatchpointInfo> installed_threads_;
  std::set<zx_koid_t> current_stepping_over_threads_;
};

}  // namespace debug_agent

#endif  // SRC_DEVELOPER_DEBUG_DEBUG_AGENT_WATCHPOINT_H_
