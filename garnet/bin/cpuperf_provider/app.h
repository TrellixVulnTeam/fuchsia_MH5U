// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef GARNET_BIN_CPUPERF_PROVIDER_APP_H_
#define GARNET_BIN_CPUPERF_PROVIDER_APP_H_

#include <lib/sys/cpp/component_context.h>
#include <lib/trace/observer.h>

#include <memory>

#include "garnet/bin/cpuperf_provider/categories.h"
#include "garnet/lib/perfmon/controller.h"
#include "src/lib/fxl/command_line.h"

namespace cpuperf_provider {

class App {
 public:
  explicit App(const fxl::CommandLine& command_line);
  ~App();

 private:
  void UpdateState();

  void StartTracing(std::unique_ptr<TraceConfig> trace_config);
  void StopTracing();
  bool IsTracing() const { return context_ != nullptr; }

  void PrintHelp();

  // This is per-cpu.
  static constexpr uint32_t kDefaultBufferSizeInMb = 16;
  static constexpr uint32_t kDefaultBufferSizeInPages =
      kDefaultBufferSizeInMb * 1024 * 1024 / perfmon::Controller::kPageSize;

  std::unique_ptr<sys::ComponentContext> startup_context_;
  trace::TraceObserver trace_observer_;
  std::unique_ptr<perfmon::ModelEventManager> model_event_manager_;
  std::unique_ptr<TraceConfig> trace_config_;
  // This context keeps the trace context alive until we've written our trace
  // records, which doesn't happen until after tracing has stopped.
  trace_prolonged_context_t* context_ = nullptr;
  std::unique_ptr<perfmon::Controller> controller_;

  trace_ticks_t start_time_ = 0;
  trace_ticks_t stop_time_ = 0;

  uint32_t buffer_size_in_pages_ = kDefaultBufferSizeInPages;

  App(const App&) = delete;
  App(App&&) = delete;
  App& operator=(const App&) = delete;
  App& operator=(App&&) = delete;
};

}  // namespace cpuperf_provider

#endif  // GARNET_BIN_CPUPERF_PROVIDER_APP_H_
