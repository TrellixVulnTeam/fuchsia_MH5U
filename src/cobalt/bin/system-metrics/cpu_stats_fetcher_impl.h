// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_COBALT_BIN_SYSTEM_METRICS_CPU_STATS_FETCHER_IMPL_H_
#define SRC_COBALT_BIN_SYSTEM_METRICS_CPU_STATS_FETCHER_IMPL_H_

#include <fidl/fuchsia.kernel/cpp/wire.h>
#include <lib/zx/resource.h>

#include <chrono>
#include <vector>

#include "src/cobalt/bin/system-metrics/cpu_stats_fetcher.h"

using cobalt::CpuStatsFetcher;

namespace cobalt {

class CpuStatsFetcherImpl : public CpuStatsFetcher {
 public:
  CpuStatsFetcherImpl();
  FetchCpuResult FetchCpuPercentage(double* cpu_percentage) override;

 private:
  bool FetchCpuCoreCount();
  bool FetchCpuStats();
  bool CalculateCpuPercentage(double* cpu_percentage);
  void InitializeKernelStats();

  size_t num_cpu_cores_ = 0;
  std::chrono::time_point<std::chrono::high_resolution_clock> cpu_fetch_time_;
  std::chrono::time_point<std::chrono::high_resolution_clock> last_cpu_fetch_time_;
  fidl::WireSyncClient<fuchsia_kernel::Stats> stats_service_;
  std::unique_ptr<fidl::SyncClientBuffer<fuchsia_kernel::Stats::GetCpuStats>> cpu_stats_buffer_;
  fuchsia_kernel::wire::CpuStats* cpu_stats_ = nullptr;
  std::unique_ptr<fidl::SyncClientBuffer<fuchsia_kernel::Stats::GetCpuStats>>
      last_cpu_stats_buffer_;
  fuchsia_kernel::wire::CpuStats* last_cpu_stats_ = nullptr;
};

}  // namespace cobalt

#endif  // SRC_COBALT_BIN_SYSTEM_METRICS_CPU_STATS_FETCHER_IMPL_H_
