// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "system_load_heart_model.h"

#include <fcntl.h>
#include <fuchsia/boot/c/fidl.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/zx/clock.h>
#include <sys/types.h>
#include <unistd.h>
#include <zircon/status.h>
#include <zircon/syscalls/object.h>

#include <algorithm>
#include <cstdio>
#include <iostream>
#include <limits>

namespace bt_le_heart_rate {
namespace {

zx::handle GetRootResource() {
  zx::channel local, remote;
  zx_status_t status = zx::channel::create(0, &local, &remote);
  if (status != ZX_OK)
    return {};

  status = fdio_service_connect("/svc/fuchsia.boot.RootResource", remote.release());
  if (status != ZX_OK)
    return {};

  zx_handle_t root_resource;
  zx_status_t fidl_status = fuchsia_boot_RootResourceGet(local.get(), &root_resource);
  if (fidl_status != ZX_OK)
    return {};

  return zx::handle(root_resource);
}

size_t ReadCpuCount(const zx::handle& root_resource) {
  size_t actual, available;

  zx_status_t err = root_resource.get_info(ZX_INFO_CPU_STATS, nullptr, 0, &actual, &available);

  if (err != ZX_OK)
    return 0;

  return available;
}

}  // namespace

SystemLoadHeartModel::SystemLoadHeartModel()
    : root_resource_(GetRootResource()),
      cpu_stats_(ReadCpuCount(root_resource_)),
      last_cpu_stats_(cpu_stats_.size()),
      last_read_time_(zx::clock::get_monotonic()) {
  FX_DCHECK(root_resource_);
  FX_DCHECK(!cpu_stats_.empty());

  ReadCpuStats();
  last_cpu_stats_.swap(cpu_stats_);
}

bool SystemLoadHeartModel::ReadMeasurement(Measurement* measurement) {
  if (!ReadCpuStats())
    return false;

  const zx::time read_time = zx::clock::get_monotonic();
  zx::duration idle_sum;
  for (size_t i = 0; i < cpu_stats_.size(); i++) {
    idle_sum += zx::duration(cpu_stats_[i].idle_time - last_cpu_stats_[i].idle_time);
    energy_counter_ += cpu_stats_[i].context_switches - last_cpu_stats_[i].context_switches;
  }

  const zx::duration elapsed = read_time - last_read_time_;

  const auto idle_percent = (idle_sum.get() * 100) / (elapsed.get() * cpu_stats_.size());

  measurement->contact = true;
  measurement->rate = static_cast<int>(100 - idle_percent);
  measurement->energy_expended = static_cast<int>(
      std::min<decltype(energy_counter_)>(energy_counter_, std::numeric_limits<int>::max()));

  last_read_time_ = read_time;
  last_cpu_stats_.swap(cpu_stats_);

  return true;
}

bool SystemLoadHeartModel::ReadCpuStats() {
  size_t actual, available;

  zx_status_t err =
      root_resource_.get_info(ZX_INFO_CPU_STATS, cpu_stats_.data(),
                              cpu_stats_.size() * sizeof(zx_info_cpu_stats), &actual, &available);

  return (err == ZX_OK && actual == available);
}

}  // namespace bt_le_heart_rate
