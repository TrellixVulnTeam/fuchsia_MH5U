// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fvm/driver/diagnostics.h"

#include <lib/ddk/debug.h>
#include <lib/inspect/cpp/inspect.h>

#include <string>

namespace fvm {

Diagnostics::Diagnostics() {
  root_ = inspector_.GetRoot().CreateChild("fvm");

  mount_time_ = root_.CreateChild("mount_time");
  mount_time_major_version_ = mount_time_.CreateUint("major_version", 0);
  mount_time_oldest_minor_version_ = mount_time_.CreateUint("oldest_minor_version", 0);
  mount_time_version_combo_ = mount_time_.CreateString("version_combo", "0/0");
  mount_time_slice_size_ = mount_time_.CreateUint("slice_size", 0);
  mount_time_num_slices_ = mount_time_.CreateUint("num_slices", 0);
  mount_time_partition_table_entries_ = mount_time_.CreateUint("partition_table_entries", 0);
  mount_time_partition_table_reserved_entries_ =
      mount_time_.CreateUint("partition_table_reserved_entries", 0);
  mount_time_allocation_table_entries_ = mount_time_.CreateUint("allocation_table_entries", 0);
  mount_time_allocation_table_reserved_entries_ =
      mount_time_.CreateUint("allocation_table_reserved_entries", 0);
  mount_time_num_partitions_ = mount_time_.CreateUint("num_partitions", 0);
  mount_time_num_reserved_slices_ = mount_time_.CreateUint("num_reserved_slices", 0);

  per_partition_node_ = root_.CreateChild("partitions");
}

void Diagnostics::OnMount(OnMountArgs args) {
  mount_time_major_version_.Set(args.major_version);
  mount_time_oldest_minor_version_.Set(args.oldest_minor_version);

  std::string version_combo =
      std::to_string(args.major_version) + "/" + std::to_string(args.oldest_minor_version);
  mount_time_version_combo_.Set(version_combo);

  mount_time_slice_size_.Set(args.slice_size);
  mount_time_num_slices_.Set(args.num_slices);
  mount_time_partition_table_entries_.Set(args.partition_table_entries);
  mount_time_partition_table_reserved_entries_.Set(args.partition_table_reserved_entries);
  mount_time_allocation_table_entries_.Set(args.allocation_table_entries);
  mount_time_allocation_table_reserved_entries_.Set(args.allocation_table_reserved_entries);
  mount_time_num_partitions_.Set(args.partitions.size());
  mount_time_num_reserved_slices_.Set(args.num_reserved_slices);
  for (auto& partition : args.partitions) {
    AddPerPartitionMetrics(std::move(partition.name), partition.num_slices);
  }
}

void Diagnostics::UpdatePartitionMetrics(const std::string& partition_name, size_t num_slices) {
  auto partition = per_partition_.find(partition_name);
  if (partition == per_partition_.end()) {
    AddPerPartitionMetrics(std::string(partition_name), 0u);
    partition = per_partition_.find(partition_name);
  }
  partition->second.total_slices_reserved.Set(num_slices);
}

void Diagnostics::UpdateMaxBytes(const std::string& partition_name, size_t max_bytes) {
  auto partition = per_partition_.find(partition_name);
  if (partition == per_partition_.end()) {
    AddPerPartitionMetrics(std::string(partition_name), 0u);
    partition = per_partition_.find(partition_name);
  }
  partition->second.max_bytes.Set(max_bytes);
}

void Diagnostics::AddPerPartitionMetrics(std::string name, uint64_t num_slices) {
  Diagnostics::PerPartitionMetrics metrics{.root = per_partition_node_.CreateChild(name)};
  metrics.total_slices_reserved = metrics.root.CreateUint("total_slices_reserved", num_slices);
  metrics.max_bytes = metrics.root.CreateUint("max_bytes", 0);
  per_partition_.insert(
      std::pair<std::string, PerPartitionMetrics>(std::move(name), std::move(metrics)));
}

}  // namespace fvm
