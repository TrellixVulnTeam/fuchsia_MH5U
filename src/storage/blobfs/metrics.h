// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains information for gathering Blobfs metrics.

#ifndef SRC_STORAGE_BLOBFS_METRICS_H_
#define SRC_STORAGE_BLOBFS_METRICS_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/time.h>

#include <mutex>

#include <cobalt-client/cpp/collector.h>

#include "src/lib/storage/vfs/cpp/journal/journal.h"
#include "src/lib/storage/vfs/cpp/metrics/cobalt_metrics.h"
#include "src/lib/storage/vfs/cpp/metrics/composite_latency_event.h"
#include "src/lib/storage/vfs/cpp/metrics/events.h"
#include "src/lib/storage/vfs/cpp/metrics/histograms.h"
#include "src/lib/storage/vfs/cpp/ticker.h"
#include "src/lib/storage/vfs/cpp/vnode.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/mount.h"
#include "src/storage/blobfs/read_metrics.h"
#include "src/storage/blobfs/verification_metrics.h"

namespace blobfs {

// Alias for the LatencyEvent used in blobfs.
using LatencyEvent = fs_metrics::CompositeLatencyEvent;

// This struct holds the inspect node for a blob and a map from block index to page-in frequency.
struct BlobPageInFrequencies {
  inspect::Node blob_root_node;
  std::map<uint32_t, inspect::UintProperty> offset_map;
};

// This class is not thread-safe except for the read_metrics() and verification_metrics()
// accessors, as well as calls to IncrementPageIn(). Everything else is only accessed from the main
// serving thread which is currently single-threaded.
//
// TODO(fxbug.dev/80285): Make this properly thread-safe.
class BlobfsMetrics : public fs::MetricsTrait {
 public:
  explicit BlobfsMetrics(
      bool should_record_page_in, inspect::Inspector inspector = {},
      const std::function<std::unique_ptr<cobalt_client::Collector>()>& collector_factory = {},
      zx::duration cobalt_flush_time = kMetricsFlushTime);
  ~BlobfsMetrics() override;

  // Print information about metrics to stdout.
  //
  // TODO(fxbug.dev/31862): This is a stop-gap solution; long-term, this information should be
  // extracted from devices.
  void Dump();

  // Begin collecting blobfs metrics. Metrics collection is not implicitly enabled with the creation
  // of a "BlobfsMetrics" object.
  void Collect();
  bool Collecting() const { return cobalt_metrics_.IsEnabled(); }

  // Updates aggregate information about the total number of created blobs since mounting.
  void UpdateAllocation(uint64_t size_data, const fs::Duration& duration);

  // Updates aggregate information about the number of blobs opened since mounting.
  void UpdateLookup(uint64_t size);

  // Updates aggregates information about blobs being written back to blobfs since mounting.
  void UpdateClientWrite(uint64_t data_size, uint64_t merkle_size,
                         const fs::Duration& enqueue_duration,
                         const fs::Duration& generate_duration);

  // Returns a new Latency event for the given event. This requires the event to be backed up by
  // an histogram in both cobalt metrics and Inspect.
  fs_metrics::CompositeLatencyEvent NewLatencyEvent(fs_metrics::Event event) final {
    return LatencyEvent(event, &histograms_, cobalt_metrics_.mutable_fs_common_metrics());
  }

  inspect::Node* GetInspectRoot() override { return &journal_stats_; }

  // Increments Cobalt metrics tracking compression formats. Extracts the compression format from
  // the |inode| header, and increments the counter for that format with the inode's |blob_size|.
  void IncrementCompressionFormatMetric(const Inode& inode);

  // Increments a read of Merkle Tree data from disk.
  // This method must only be called from the blobfs main thread.
  void IncrementMerkleDiskRead(uint64_t read_size, fs::Duration read_duration);

  // Increments the frequency count for blocks in the range [|offset|, |offset| + |length|). This
  // method must only be called from the pager thread.
  // NOTE: This method is a NOP unless |BLOBFS_ENABLE_PAGE_IN_METRICS| compiler flag
  // has been set in the BUILD.gn
  void IncrementPageIn(const fbl::String& merkle_hash, uint64_t offset, uint64_t length);

  // Accessors for ReadMetrics. The metrics objects returned are NOT thread-safe. The metrics
  // objects are to be used by exactly one thread (main or pager). Used to increment relevant
  // metrics from the blobfs main thread and the user pager thread.
  ReadMetrics& paged_read_metrics() { return paged_read_metrics_; }
  ReadMetrics& unpaged_read_metrics() { return unpaged_read_metrics_; }

  // Accessor for VerificationMetrics. This metrics object is thread-safe. Used to increment
  // relevant metrics from the blobfs main thread and the user pager thread.
  // The |BlobfsMetrics| class is not thread-safe except for this accessor.
  VerificationMetrics& verification_metrics() { return verification_metrics_; }

  // Accessor for BlobFS Inspector. This Inspector serves the BlobFS inspect tree.
  inspect::Inspector* inspector() { return &inspector_; }

  fs_metrics::Metrics& cobalt_metrics() { return cobalt_metrics_; }

 private:
  // Flushes the metrics to the cobalt client and schedules itself to flush again.
  void ScheduleMetricFlush();

  // Inspect instrumentation data.
  inspect::Inspector inspector_;
  inspect::Node& root_ = inspector_.GetRoot();

  // ALLOCATION STATS
  // Created with external-facing "Create".
  uint64_t blobs_created_ = 0;
  // Measured by space allocated with "Truncate".
  uint64_t blobs_created_total_size_ = 0;
  zx::ticks total_allocation_time_ticks_ = {};

  // WRITEBACK STATS
  // Measurements, from the client's perspective, of writing and enqueing
  // data that will later be written to disk.
  uint64_t data_bytes_written_ = 0;
  uint64_t merkle_bytes_written_ = 0;
  zx::ticks total_write_enqueue_time_ticks_ = {};
  zx::ticks total_merkle_generation_time_ticks_ = {};

  // LOOKUP STATS
  // Opened via "LookupBlob".
  uint64_t blobs_opened_ = 0;
  uint64_t blobs_opened_total_size_ = 0;

  // INSPECT NODES AND PROPERTIES
  inspect::Node allocation_stats_ = root_.CreateChild("allocation_stats");
  inspect::Node writeback_stats_ = root_.CreateChild("writeback_stats");
  inspect::Node lookup_stats_ = root_.CreateChild("lookup_stats");
  inspect::Node paged_read_stats_ = root_.CreateChild("paged_read_stats");
  inspect::Node unpaged_read_stats_ = root_.CreateChild("unpaged_read_stats");
  inspect::Node page_in_frequency_stats_ = root_.CreateChild("page_in_frequency_stats");
  inspect::Node journal_stats_ = root_.CreateChild("journal_stats");

  // Allocation properties
  inspect::UintProperty blobs_created_property_ =
      allocation_stats_.CreateUint("blobs_created", blobs_created_);
  inspect::UintProperty blobs_created_total_size_property_ =
      allocation_stats_.CreateUint("blobs_created_total_size", blobs_created_total_size_);
  inspect::IntProperty total_allocation_time_ticks_property_ = allocation_stats_.CreateInt(
      "total_allocation_time_ticks", total_allocation_time_ticks_.get());

  // Writeback properties
  inspect::UintProperty data_bytes_written_property_ =
      writeback_stats_.CreateUint("data_bytes_written", data_bytes_written_);
  inspect::UintProperty merkle_bytes_written_property_ =
      writeback_stats_.CreateUint("merkle_bytes_written", merkle_bytes_written_);
  inspect::IntProperty total_write_enqueue_time_ticks_property_ = writeback_stats_.CreateInt(
      "total_write_enqueue_time_ticks", total_write_enqueue_time_ticks_.get());
  inspect::IntProperty total_merkle_generation_time_ticks_property_ = writeback_stats_.CreateInt(
      "total_merkle_generation_time_ticks", total_merkle_generation_time_ticks_.get());

  // Lookup properties
  inspect::UintProperty blobs_opened_property_ =
      lookup_stats_.CreateUint("blobs_opened", blobs_opened_);
  inspect::UintProperty blobs_opened_total_size_property_ =
      lookup_stats_.CreateUint("blobs_opened_total_size", blobs_opened_total_size_);

  // READ STATS
  ReadMetrics paged_read_metrics_{&paged_read_stats_};
  ReadMetrics unpaged_read_metrics_{&unpaged_read_stats_};
  zx::ticks total_read_merkle_time_ticks_ = {};
  uint64_t bytes_merkle_read_from_disk_ = 0;

  // PAGE-IN FREQUENCY STATS
  bool should_record_page_in_ = false;
  std::mutex frequencies_lock_;
  std::map<fbl::String, BlobPageInFrequencies> all_page_in_frequencies_
      __TA_GUARDED(frequencies_lock_);

  // VERIFICATION STATS
  VerificationMetrics verification_metrics_;

  // FVM STATS
  // TODO(smklein)

  fs_metrics::Histograms histograms_ = fs_metrics::Histograms(&root_);

  // Cobalt metrics.
  fs_metrics::Metrics cobalt_metrics_;

  // Loop for flushing the collector periodically.
  async::Loop flush_loop_ = async::Loop(&kAsyncLoopConfigNoAttachToCurrentThread);

  // Time between each Cobalt flush.
  zx::duration cobalt_flush_time_ = kMetricsFlushTime;
};

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_METRICS_H_
