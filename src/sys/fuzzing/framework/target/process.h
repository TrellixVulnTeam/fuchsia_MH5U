// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_SYS_FUZZING_FRAMEWORK_TARGET_PROCESS_H_
#define SRC_SYS_FUZZING_FRAMEWORK_TARGET_PROCESS_H_

#include <fuchsia/fuzzer/cpp/fidl.h>
#include <lib/zx/time.h>
#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <limits>
#include <mutex>
#include <vector>

#include "src/lib/fxl/macros.h"
#include "src/lib/fxl/synchronization/thread_annotations.h"
#include "src/sys/fuzzing/common/sancov.h"
#include "src/sys/fuzzing/common/signal-coordinator.h"
#include "src/sys/fuzzing/common/sync-wait.h"
#include "src/sys/fuzzing/framework/target/module.h"
#include "src/sys/fuzzing/framework/target/process.h"

namespace fuzzing {

using ::fuchsia::fuzzer::InstrumentationSyncPtr;
using ::fuchsia::fuzzer::Options;

// Reserved target IDs. |kTimeoutTargetId| is a pseudo-ID used to signify a timeout across all
// target processes rather than an error in a specific one.
constexpr uint64_t kInvalidTargetId = std::numeric_limits<uint64_t>::min();
constexpr uint64_t kTimeoutTargetId = std::numeric_limits<uint64_t>::max();

// This class represents a target process being fuzzed. It is a singleton in each process, and its
// methods are typically invoked through various callbacks.
class Process {
 public:
  virtual ~Process();

  // Adds defaults to unspecified options.
  static void AddDefaults(Options* options);

  // Adds the counters and PCs associated with modules for this process. Invoked via the
  // |__sanitizer_cov_*_init| functions.
  void AddModules() FXL_LOCKS_EXCLUDED(mutex_);

  // |malloc| and |free| hooks, called from a static context via the
  // |__sanitizer_install_malloc_and_free_hooks| function.
  void OnMalloc(const volatile void* ptr, size_t size);
  void OnFree(const volatile void* ptr);

  // Exit hooks, called from a static context via the |__sanitizer_set_death_callback| function an
  // |std::atexit|.
  void OnDeath();
  void OnExit();

 protected:
  Process();

  // Accessors for unit testing.
  const Options& options() const { return options_; }
  size_t malloc_limit() const { return malloc_limit_; }
  zx::time next_purge() const { return next_purge_; }

  // Installs the hook functions above in the process' overall global, static context. The methods
  // used, e.g. |__sanitizer_set_death_callback|, do not have corresponding methods to unset the
  // hooks, so there is no corresponding "UninstallHooks". As a result, this method can only be
  // called once per process; subsequent calls will return immediately.
  static void InstallHooks();

  // Connects to the |Coverage| component. This should happen before main, typically as part of the
  // singleton's constructor. This method can only be called once per object; subsequent calls will
  // return immediately.
  void Connect(InstrumentationSyncPtr&& instrumentation) FXL_LOCKS_EXCLUDED(mutex_);

 private:
  // SignalCoordinator callback.
  bool OnSignal(zx_signals_t observed);

  // Blocks until the engine signals it has added a process or module proxy for this object.
  void AwaitSync();

  // Sends complete pending modules to the engine.
  void AddModulesLocked() FXL_REQUIRE(mutex_);

  // Update module counters.
  void UpdateModules();

  // Clear module counters.
  void ClearModules();

  // Configures the target for leak detection, if available. See |DetectLeak| below for details.
  void ConfigureLeakDetection();

  // Performs a leak check.
  //
  // Full leak detection is expensive, so the framework imitates libFuzzer's
  // approach to leak detection and uses a heuristic to try and limit the number of false positives:
  // For each input, it tracks the number of mallocs and frees, and reports whether these numbers
  // match when the run finishes. Upon mismatch, the framework will try the same input again using a
  // |kStartLeakCheck| signal. This is to distinguish between leaks and memory being accumulated in
  // some global state without being leaked. For this second pass, LSan is *disabled* to avoid
  // reporting the same leak twice. If the input still causes more mallocs than frees, the full leak
  // check is performed. If it is a true leak, LSan will report details of the leak from the first
  // run.
  //
  // Returns true if more mallocs were observed than frees. Returns false if the number of mallocs
  // and frees were the same. Exits and does NOT return if a full leak check was performed and a
  // leak was detected.
  //
  // See also libFuzzer's |Fuzzer::TryDetectingAMemoryLeak|.
  bool DetectLeak();

  // First call returns true if a sanitizer is present; all other calls return false.
  static bool AcquireCrashState();

  InstrumentationSyncPtr instrumentation_;
  SignalCoordinator coordinator_;
  SyncWait sync_;

  // Options provided by the engine.
  Options options_;
  bool can_detect_leaks_ = false;  // Is LSan available and is options.deteck_leaks == true?
  size_t malloc_limit_ = 0;

  // Module feedback.
  std::mutex mutex_;
  std::vector<Module> modules_ FXL_GUARDED_BY(mutex_);

  // Memory tracking.
  bool detecting_leaks_ = false;  // Was the current iteration started with |kStartLeakCheck|?
  std::atomic<uint64_t> num_mallocs_ = 0;
  std::atomic<uint64_t> num_frees_ = 0;
  zx::time next_purge_;

  FXL_DISALLOW_COPY_ASSIGN_AND_MOVE(Process);
};

}  // namespace fuzzing

#endif  // SRC_SYS_FUZZING_FRAMEWORK_TARGET_PROCESS_H_
