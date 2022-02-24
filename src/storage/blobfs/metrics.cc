// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains information for gathering Blobfs metrics.

#include "src/storage/blobfs/metrics.h"

#include <lib/async/cpp/task.h>
#include <lib/fzl/time.h>
#include <lib/inspect/cpp/inspector.h>
#include <lib/inspect/cpp/vmo/types.h>
#include <lib/inspect/service/cpp/service.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/time.h>
#include <zircon/assert.h>

#include <string>

#include <cobalt-client/cpp/collector.h>
#include <fbl/algorithm.h>

#include "src/lib/storage/vfs/cpp/metrics/cobalt_metrics.h"
#include "src/lib/storage/vfs/cpp/metrics/events.h"
#include "src/lib/storage/vfs/cpp/service.h"
#include "src/lib/storage/vfs/cpp/vnode.h"

namespace blobfs {
namespace {

size_t TicksToMs(const zx::ticks& ticks) { return fzl::TicksToNs(ticks) / zx::msec(1); }

fs_metrics::CompressionFormat FormatForInode(const Inode& inode) {
  if (inode.IsCompressed()) {
    auto compression = inode.header.flags & kBlobFlagMaskAnyCompression;
    switch (compression) {
      case kBlobFlagChunkCompressed:
        return fs_metrics::CompressionFormat::kCompressedZSTDChunked;
      default:
        return fs_metrics::CompressionFormat::kUnknown;
    }
  } else {
    return fs_metrics::CompressionFormat::kUncompressed;
  }
}

}  // namespace

BlobfsMetrics::BlobfsMetrics(
    bool should_record_page_in, inspect::Inspector inspector,
    const std::function<std::unique_ptr<cobalt_client::Collector>()>& collector_factory,
    zx::duration cobalt_flush_time)
    : inspector_{std::move(inspector)},
      should_record_page_in_(should_record_page_in),
      cobalt_metrics_(collector_factory ? collector_factory()
                                        : std::make_unique<cobalt_client::Collector>(
                                              fs_metrics::kCobaltProjectId),
                      fs_metrics::Source::kBlobfs, fs_metrics::CompressionSource::kBlobfs),
      cobalt_flush_time_(cobalt_flush_time) {
  // Add a node that allows querying the size of the Inspect VMO at runtime.
  // TODO(fxbug.dev/85419): Replace the following lazy node with the one now part of the Inspector
  // class itself (i.e. call `inspector_.CreateStatsNode()` instead).
  root_.CreateLazyNode(
      "inspect_vmo_stats",
      [this] {
        inspect::InspectStats stats = inspector_.GetStats();
        inspect::Inspector insp;
        insp.GetRoot().CreateUint("current_size", stats.size, &insp);
        insp.GetRoot().CreateUint("maximum_size", stats.maximum_size, &insp);
        return fpromise::make_result_promise(fpromise::ok(std::move(insp)));
      },
      &inspector_);
}

BlobfsMetrics::~BlobfsMetrics() { Dump(); }

void PrintReadMetrics(ReadMetrics& metrics) {
  constexpr uint64_t mb = 1 << 20;
  auto snapshot = metrics.GetSnapshot(CompressionAlgorithm::kUncompressed);
  FX_LOGS(INFO) << "    Uncompressed: Read " << snapshot.read_bytes / mb << " MB (spent "
                << TicksToMs(zx::ticks(snapshot.read_ticks)) << " ms)";

  snapshot = metrics.GetSnapshot(CompressionAlgorithm::kChunked);
  FX_LOGS(INFO) << "    Chunked: Read " << snapshot.read_bytes / mb << " MB (spent "
                << TicksToMs(zx::ticks(snapshot.read_ticks)) << " ms) | Decompressed "
                << snapshot.decompress_bytes / mb << " MB (spent "
                << TicksToMs(zx::ticks(snapshot.decompress_ticks)) << " ms)";
  FX_LOGS(INFO) << "    Remote decompressions: " << metrics.GetRemoteDecompressions();
}

void BlobfsMetrics::Dump() {
  constexpr uint64_t mb = 1 << 20;

  // Timings are only recorded when Cobalt metrics are enabled.

  FX_LOGS(INFO) << "Allocation Info:";
  FX_LOGS(INFO) << "  Allocated " << blobs_created_ << " blobs (" << blobs_created_total_size_ / mb
                << " MB)";
  if (Collecting()) {
    FX_LOGS(INFO) << "  Total allocation time is " << TicksToMs(total_allocation_time_ticks_)
                  << " ms";
  }

  FX_LOGS(INFO) << "Write Info:";
  FX_LOGS(INFO) << "  Wrote " << data_bytes_written_ / mb << " MB of data and "
                << merkle_bytes_written_ / mb << " MB of merkle trees";
  if (Collecting()) {
    FX_LOGS(INFO) << "  Enqueued to journal in " << TicksToMs(total_write_enqueue_time_ticks_)
                  << " ms, made merkle tree in " << TicksToMs(total_merkle_generation_time_ticks_)
                  << " ms";
  }

  FX_LOGS(INFO) << "Read Info:";
  FX_LOGS(INFO) << "  Paged:";
  PrintReadMetrics(paged_read_metrics_);
  FX_LOGS(INFO) << "  Unpaged:";
  PrintReadMetrics(unpaged_read_metrics_);

  FX_LOGS(INFO) << "  Merkle data read: " << bytes_merkle_read_from_disk_ / mb << " MB (spent "
                << TicksToMs(zx::ticks(total_read_merkle_time_ticks_)) << " ms)";

  FX_LOGS(INFO) << "  Opened " << blobs_opened_ << " blobs (" << blobs_opened_total_size_ / mb
                << " MB)";
  auto verify_snapshot = verification_metrics_.Get();
  FX_LOGS(INFO) << "  Verified " << verify_snapshot.blobs_verified << " blobs ("
                << verify_snapshot.data_size / mb << " MB data, "
                << verify_snapshot.merkle_size / mb << " MB merkle)";
  if (Collecting()) {
    FX_LOGS(INFO) << "  Spent " << TicksToMs(zx::ticks(verify_snapshot.verification_time))
                  << " ms verifying";
  }

  FX_LOGS(INFO) << "Inspect VMO:";
  FX_LOGS(INFO) << "  Maximum Size (bytes) = " << inspector_.GetStats().maximum_size;
  FX_LOGS(INFO) << "  Current Size (bytes) = " << inspector_.GetStats().size;
  FX_LOGS(INFO) << "Page-in Metrics Recording Enabled = "
                << (should_record_page_in_ ? "true" : "false");
}

void BlobfsMetrics::ScheduleMetricFlush() {
  async::PostDelayedTask(
      flush_loop_.dispatcher(),
      [this]() {
        cobalt_metrics_.Flush();
        ScheduleMetricFlush();
      },
      cobalt_flush_time_);
}

void BlobfsMetrics::Collect() {
  cobalt_metrics_.EnableMetrics(true);
  // TODO(gevalentino): Once we have async llcpp bindings, instead pass a dispatcher for
  // handling collector IPCs.
  flush_loop_.StartThread("blobfs-metric-flusher");
  ScheduleMetricFlush();
}

void BlobfsMetrics::UpdateAllocation(uint64_t size_data, const fs::Duration& duration) {
  blobs_created_++;
  blobs_created_total_size_ += size_data;
  total_allocation_time_ticks_ += duration;
  blobs_created_property_.Add(1);
  blobs_created_total_size_property_.Add(size_data);
  total_allocation_time_ticks_property_.Add(duration.get());
}

void BlobfsMetrics::UpdateLookup(uint64_t size) {
  blobs_opened_++;
  blobs_opened_total_size_ += size;
  blobs_opened_property_.Add(1);
  blobs_opened_total_size_property_.Add(size);
}

void BlobfsMetrics::UpdateClientWrite(uint64_t data_size, uint64_t merkle_size,
                                      const fs::Duration& enqueue_duration,
                                      const fs::Duration& generate_duration) {
  data_bytes_written_ += data_size;
  merkle_bytes_written_ += merkle_size;
  total_write_enqueue_time_ticks_ += enqueue_duration;
  total_merkle_generation_time_ticks_ += generate_duration;
  data_bytes_written_property_.Add(data_size);
  merkle_bytes_written_property_.Add(merkle_size);
  total_write_enqueue_time_ticks_property_.Add(enqueue_duration.get());
  total_merkle_generation_time_ticks_property_.Add(generate_duration.get());
}

void BlobfsMetrics::IncrementCompressionFormatMetric(const Inode& inode) {
  if (!Collecting()) {
    return;
  }
  fs_metrics::CompressionFormat format = FormatForInode(inode);
  cobalt_metrics_.mutable_compression_format_metrics()->IncrementCounter(format, inode.blob_size);
}

void BlobfsMetrics::IncrementMerkleDiskRead(uint64_t read_size, fs::Duration read_duration) {
  total_read_merkle_time_ticks_ += read_duration;
  bytes_merkle_read_from_disk_ += read_size;
}

void BlobfsMetrics::IncrementPageIn(const fbl::String& merkle_hash, uint64_t offset,
                                    uint64_t length) {
  // Page-in metrics are a developer feature that is not intended to be used in production. Enabling
  // this feature also requires increasing the size of the Inspect VMO considerably (>512KB).
  if (!should_record_page_in_) {
    return;
  }

  inspect::InspectStats stats = inspector_.GetStats();
  if (stats.maximum_size <= stats.size) {
    FX_LOGS(ERROR) << "Blobfs has run out of space in the Inspect VMO.";
    FX_LOGS(ERROR) << "To record page-in metrics accurately, increase the VMO size.";
    FX_LOGS(ERROR) << "    Maximum size  : " << stats.maximum_size;
    FX_LOGS(ERROR) << "    Current size  : " << stats.size;
    should_record_page_in_ = false;
    return;
  }

  std::lock_guard lock(frequencies_lock_);
  if (all_page_in_frequencies_.find(merkle_hash) == all_page_in_frequencies_.end()) {
    // We have no page in metrics on this blob yet. Create a new child node.
    all_page_in_frequencies_[merkle_hash].blob_root_node =
        page_in_frequency_stats_.CreateChild(merkle_hash.c_str());
  }

  BlobPageInFrequencies& blob_frequencies = all_page_in_frequencies_[merkle_hash];

  // Calculate the start+end frame indexes to increment
  uint32_t cur =
      static_cast<uint32_t>(fbl::round_down(offset, kBlobfsBlockSize) / kBlobfsBlockSize);
  uint32_t end =
      static_cast<uint32_t>(fbl::round_up(offset + length, kBlobfsBlockSize) / kBlobfsBlockSize);

  for (; cur < end; cur += 1) {
    if (blob_frequencies.offset_map.find(cur) == blob_frequencies.offset_map.end()) {
      // We have no frequencies recorded at this frame index. Create a new property.
      blob_frequencies.offset_map[cur] =
          blob_frequencies.blob_root_node.CreateUint(std::to_string(cur), 1);
    } else {
      blob_frequencies.offset_map[cur].Add(1);
    }
  }
}

}  // namespace blobfs
