// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_BLOBFS_READ_METRICS_H_
#define SRC_STORAGE_BLOBFS_READ_METRICS_H_

#include <lib/inspect/cpp/inspect.h>
#include <lib/zx/time.h>
#include <zircon/compiler.h>
#include <zircon/time.h>

#include <mutex>

#include "src/lib/storage/vfs/cpp/ticker.h"
#include "src/storage/blobfs/compression_settings.h"

namespace blobfs {

// The |ReadMetrics| class tracks blobfs metrics that are updated on the read path, i.e. metrics
// related to disk reads and decompression.
//
// This class is thread-safe.
class ReadMetrics {
 public:
  explicit ReadMetrics(inspect::Node* read_metrics_node);
  ReadMetrics() = delete;
  ReadMetrics(const ReadMetrics&) = delete;
  ReadMetrics& operator=(const ReadMetrics&) = delete;

  // Increments aggregate information about reading blobs from storage since mounting.
  void IncrementDiskRead(CompressionAlgorithm algorithm, uint64_t read_size,
                         fs::Duration read_duration);

  // Increments aggregate information about decompressing blobs from storage since mounting.
  void IncrementDecompression(CompressionAlgorithm algorithm, uint64_t decompressed_size,
                              fs::Duration decompress_duration, bool remote);

  struct PerCompressionSnapshot {
    // Metrics for reads from disk
    zx_ticks_t read_ticks;
    uint64_t read_bytes;

    // Metrics for decompression
    zx_ticks_t decompress_ticks;
    uint64_t decompress_bytes;
  };

  // Returns a snapshot of metrics recorded by this class.
  PerCompressionSnapshot GetSnapshot(CompressionAlgorithm algorithm);

  uint64_t GetRemoteDecompressions() const;

 private:
  struct PerCompressionMetrics {
    // Metrics for reads from disk
    zx::ticks read_ticks = {};
    uint64_t read_bytes = 0;

    // Metrics for decompression
    zx::ticks decompress_ticks = {};
    uint64_t decompress_bytes = 0;
  };

  struct PerCompressionInspect {
    explicit PerCompressionInspect(inspect::Node node);
    inspect::Node parent_node;
    inspect::IntProperty read_ticks_node;
    inspect::UintProperty read_bytes_node;
    inspect::IntProperty decompress_ticks_node;
    inspect::UintProperty decompress_bytes_node;
  };

  ReadMetrics::PerCompressionMetrics* GetMetrics(CompressionAlgorithm algorithm)
      __TA_REQUIRES(lock_);
  ReadMetrics::PerCompressionInspect* GetInspect(CompressionAlgorithm algorithm);

  // Guards all locally tracked metrics that get flushed to cobalt. The inspect metrics are all
  // thread-safe to increment and decrement.
  mutable std::mutex lock_;
  PerCompressionMetrics uncompressed_metrics_ __TA_GUARDED(lock_);
  PerCompressionMetrics chunked_metrics_ __TA_GUARDED(lock_);
  PerCompressionInspect uncompressed_inspect_;
  PerCompressionInspect chunked_inspect_;

  uint64_t remote_decompressions_ __TA_GUARDED(lock_) = 0;
  inspect::UintProperty remote_decompressions_node_;
};

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_READ_METRICS_H_
