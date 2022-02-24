// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_FRAMEWORK_TESTING_PROCESS_PROXY_H_
#define SRC_SYS_FUZZING_FRAMEWORK_TESTING_PROCESS_PROXY_H_

#include <fuchsia/fuzzer/cpp/fidl.h>
#include <lib/async/dispatcher.h>
#include <lib/zx/process.h>
#include <stddef.h>
#include <stdint.h>
#include <zircon/types.h>

#include <memory>
#include <unordered_map>
#include <vector>

#include "src/lib/fxl/macros.h"
#include "src/sys/fuzzing/common/binding.h"
#include "src/sys/fuzzing/common/options.h"
#include "src/sys/fuzzing/common/sync-wait.h"
#include "src/sys/fuzzing/common/testing/signal-coordinator.h"
#include "src/sys/fuzzing/framework/engine/module-pool.h"
#include "src/sys/fuzzing/framework/testing/module.h"

namespace fuzzing {

using ::fuchsia::fuzzer::Instrumentation;
using ::fuchsia::fuzzer::InstrumentationSyncPtr;
using ::fuchsia::fuzzer::InstrumentedProcess;
using ::fuchsia::fuzzer::LlvmModule;

// This class combines a simple implementation of |Instrumentation| with the signal coordination of
// |ProcessProxy| to create a test fixture for processes that bypasses the coverage component.
class FakeProcessProxy : public Instrumentation {
 public:
  FakeProcessProxy(const std::shared_ptr<ModulePool>& pool);
  ~FakeProcessProxy() override = default;

  zx_koid_t process_koid() const { return process_koid_; }
  size_t num_modules() const { return ids_.size(); }
  bool has_module(FakeFrameworkModule* module) const;

  void Configure(const std::shared_ptr<Options>& options);

  // FIDL methods.
  InstrumentationSyncPtr Bind(bool disable_warnings);
  void Initialize(InstrumentedProcess instrumented, InitializeCallback callback) override;
  void AddLlvmModule(LlvmModule llvm_module, AddLlvmModuleCallback callback) override;

  // FakeSignalCoordinator methods.
  bool SignalPeer(Signal signal) { return coordinator_.SignalPeer(signal); }
  zx_signals_t AwaitSignal() { return coordinator_.AwaitSignal(); }

 private:
  Binding<Instrumentation> binding_;
  std::shared_ptr<ModulePool> pool_;
  std::shared_ptr<Options> options_;
  zx_koid_t process_koid_ = 0;
  std::unordered_map<uint64_t, uint64_t> ids_;
  std::vector<SharedMemory> counters_;
  FakeSignalCoordinator coordinator_;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(FakeProcessProxy);
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_FRAMEWORK_TESTING_PROCESS_PROXY_H_
