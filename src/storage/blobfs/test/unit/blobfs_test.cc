// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/blobfs/blobfs.h"

#include <lib/sync/completion.h>
#include <lib/zx/time.h>
#include <zircon/errors.h>

#include <chrono>
#include <mutex>
#include <sstream>

#include <cobalt-client/cpp/in_memory_logger.h>
#include <gtest/gtest.h>
#include <storage/buffer/vmo_buffer.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/lib/storage/block_client/cpp/reader.h"
#include "src/lib/storage/vfs/cpp/metrics/events.h"
#include "src/storage/blobfs/blob.h"
#include "src/storage/blobfs/directory.h"
#include "src/storage/blobfs/format.h"
#include "src/storage/blobfs/fsck.h"
#include "src/storage/blobfs/mkfs.h"
#include "src/storage/blobfs/test/blob_utils.h"
#include "src/storage/blobfs/test/blobfs_test_setup.h"
#include "src/storage/blobfs/test/test_scoped_vnode_open.h"
#include "src/storage/blobfs/transaction.h"

namespace blobfs {
namespace {

using ::block_client::FakeBlockDevice;

constexpr uint32_t kBlockSize = 512;
constexpr uint32_t kNumBlocks = 400 * kBlobfsBlockSize / kBlockSize;
constexpr uint32_t kNumNodes = 128;

class MockBlockDevice : public FakeBlockDevice {
 public:
  MockBlockDevice(uint64_t block_count, uint32_t block_size)
      : FakeBlockDevice(block_count, block_size) {}

  static std::unique_ptr<MockBlockDevice> CreateAndFormat(const FilesystemOptions& options,
                                                          uint64_t num_blocks) {
    auto device = std::make_unique<MockBlockDevice>(num_blocks, kBlockSize);
    EXPECT_EQ(FormatFilesystem(device.get(), options), ZX_OK);
    return device;
  }

  bool saw_trim() const { return saw_trim_; }

  zx_status_t FifoTransaction(block_fifo_request_t* requests, size_t count) final;
  zx_status_t BlockGetInfo(fuchsia_hardware_block_BlockInfo* info) const final;

 private:
  bool saw_trim_ = false;
};

zx_status_t MockBlockDevice::FifoTransaction(block_fifo_request_t* requests, size_t count) {
  for (size_t i = 0; i < count; i++) {
    if (requests[i].opcode == BLOCKIO_TRIM) {
      saw_trim_ = true;
      return ZX_OK;
    }
  }
  return FakeBlockDevice::FifoTransaction(requests, count);
}

zx_status_t MockBlockDevice::BlockGetInfo(fuchsia_hardware_block_BlockInfo* info) const {
  zx_status_t status = FakeBlockDevice::BlockGetInfo(info);
  if (status == ZX_OK) {
    info->flags |= fuchsia_hardware_block_FLAG_TRIM_SUPPORT;
  }
  return status;
}

template <uint64_t oldest_minor_version, uint64_t num_blocks = kNumBlocks,
          typename Device = MockBlockDevice>
class BlobfsTestAtRevision : public BlobfsTestSetup, public testing::Test {
 public:
  void SetUp() final {
    FilesystemOptions fs_options{.blob_layout_format = BlobLayoutFormat::kCompactMerkleTreeAtEnd,
                                 .oldest_minor_version = oldest_minor_version};
    auto device = Device::CreateAndFormat(fs_options, num_blocks);
    ASSERT_TRUE(device);
    device_ = device.get();

    ASSERT_EQ(ZX_OK, Mount(std::move(device), GetMountOptions()));

    srand(testing::UnitTest::GetInstance()->random_seed());
  }

  void TearDown() final {
    // Process any pending notifications before tearing down blobfs (necessary for paged vmos).
    loop().RunUntilIdle();
  }

 protected:
  virtual MountOptions GetMountOptions() const { return MountOptions(); }

  Device* device_ = nullptr;
};

using BlobfsTest = BlobfsTestAtRevision<blobfs::kBlobfsCurrentMinorVersion>;

TEST_F(BlobfsTest, GetDevice) { ASSERT_EQ(device_, blobfs()->GetDevice()); }

TEST_F(BlobfsTest, BlockNumberToDevice) {
  ASSERT_EQ(42 * kBlobfsBlockSize / kBlockSize, blobfs()->BlockNumberToDevice(42));
}

TEST_F(BlobfsTest, CleanFlag) {
  // Scope all operations while the filesystem is alive to ensure they
  // don't have dangling references once it is destroyed.
  {
    storage::VmoBuffer buffer;
    ASSERT_EQ(buffer.Initialize(blobfs(), 1, kBlobfsBlockSize, "source"), ZX_OK);

    // Write the superblock with the clean flag unset on Blobfs::Create in Setup.
    storage::Operation operation = {};
    memcpy(buffer.Data(0), &blobfs()->Info(), sizeof(Superblock));
    operation.type = storage::OperationType::kWrite;
    operation.dev_offset = 0;
    operation.length = 1;

    ASSERT_EQ(blobfs()->RunOperation(operation, &buffer), ZX_OK);

    // Read the superblock with the clean flag unset.
    operation.type = storage::OperationType::kRead;
    ASSERT_EQ(blobfs()->RunOperation(operation, &buffer), ZX_OK);
    Superblock* info = reinterpret_cast<Superblock*>(buffer.Data(0));
    EXPECT_EQ(0u, (info->flags & kBlobFlagClean));
  }

  // Destroy the blobfs instance to force writing of the clean bit.
  auto device = Unmount();

  // Read the superblock, verify the clean flag is set.
  uint8_t block[kBlobfsBlockSize] = {};
  static_assert(sizeof(block) >= sizeof(Superblock));
  block_client::Reader reader(*device);
  ASSERT_EQ(reader.Read(0, kBlobfsBlockSize, &block), ZX_OK);
  Superblock* info = reinterpret_cast<Superblock*>(block);
  EXPECT_EQ(kBlobFlagClean, (info->flags & kBlobFlagClean));
}

// Tests reading a well known location.
TEST_F(BlobfsTest, RunOperationExpectedRead) {
  storage::VmoBuffer buffer;
  ASSERT_EQ(buffer.Initialize(blobfs(), 1, kBlobfsBlockSize, "source"), ZX_OK);

  // Read the first block.
  storage::Operation operation = {};
  operation.type = storage::OperationType::kRead;
  operation.length = 1;
  ASSERT_EQ(blobfs()->RunOperation(operation, &buffer), ZX_OK);

  uint64_t* data = reinterpret_cast<uint64_t*>(buffer.Data(0));
  EXPECT_EQ(kBlobfsMagic0, data[0]);
  EXPECT_EQ(kBlobfsMagic1, data[1]);
}

// Tests that we can read back what we write.
TEST_F(BlobfsTest, RunOperationReadWrite) {
  char data[kBlobfsBlockSize] = "something to test";

  storage::VmoBuffer buffer;
  ASSERT_EQ(buffer.Initialize(blobfs(), 1, kBlobfsBlockSize, "source"), ZX_OK);
  memcpy(buffer.Data(0), data, kBlobfsBlockSize);

  storage::Operation operation = {};
  operation.type = storage::OperationType::kWrite;
  operation.dev_offset = 1;
  operation.length = 1;

  ASSERT_EQ(blobfs()->RunOperation(operation, &buffer), ZX_OK);

  memset(buffer.Data(0), 'a', kBlobfsBlockSize);
  operation.type = storage::OperationType::kRead;
  ASSERT_EQ(blobfs()->RunOperation(operation, &buffer), ZX_OK);

  ASSERT_EQ(memcmp(data, buffer.Data(0), kBlobfsBlockSize), 0);
}

TEST_F(BlobfsTest, TrimsData) {
  fbl::RefPtr<fs::Vnode> root;
  ASSERT_EQ(blobfs()->OpenRootNode(&root), ZX_OK);
  fs::Vnode* root_node = root.get();

  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", 1024);
  memmove(info->path, info->path + 1, strlen(info->path));  // Remove leading slash.

  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root_node->Create(info->path, 0, &file), ZX_OK);

  size_t actual;
  EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);
  EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &actual), ZX_OK);
  EXPECT_EQ(file->Close(), ZX_OK);

  EXPECT_FALSE(device_->saw_trim());
  ASSERT_EQ(root_node->Unlink(info->path, false), ZX_OK);

  sync_completion_t completion;
  blobfs()->Sync([&completion](zx_status_t status) { sync_completion_signal(&completion); });
  EXPECT_EQ(sync_completion_wait(&completion, zx::duration::infinite().get()), ZX_OK);

  ASSERT_TRUE(device_->saw_trim());
}

TEST_F(BlobfsTest, GetNodeWithAnInvalidNodeIndexIsAnError) {
  uint32_t invalid_node_index = kMaxNodeId - 1;
  auto node = blobfs()->GetNode(invalid_node_index);
  EXPECT_EQ(node.status_value(), ZX_ERR_INVALID_ARGS);
}

TEST_F(BlobfsTest, FreeInodeWithAnInvalidNodeIndexIsAnError) {
  BlobTransaction transaction;
  uint32_t invalid_node_index = kMaxNodeId - 1;
  EXPECT_EQ(blobfs()->FreeInode(invalid_node_index, transaction), ZX_ERR_INVALID_ARGS);
}

TEST_F(BlobfsTest, BlockIteratorByNodeIndexWithAnInvalidNodeIndexIsAnError) {
  uint32_t invalid_node_index = kMaxNodeId - 1;
  auto block_iterator = blobfs()->BlockIteratorByNodeIndex(invalid_node_index);
  EXPECT_EQ(block_iterator.status_value(), ZX_ERR_INVALID_ARGS);
}

using BlobfsTestWithLargeDevice =
    BlobfsTestAtRevision<blobfs::kBlobfsCurrentMinorVersion,
                         /*num_blocks=*/2560 * kBlobfsBlockSize / kBlockSize>;

TEST_F(BlobfsTestWithLargeDevice, WritingBlobLargerThanWritebackCapacitySucceeds) {
  fbl::RefPtr<fs::Vnode> root;
  ASSERT_EQ(blobfs()->OpenRootNode(&root), ZX_OK);
  fs::Vnode* root_node = root.get();

  std::unique_ptr<BlobInfo> info =
      GenerateRealisticBlob("", (blobfs()->WriteBufferBlockCount() + 1) * kBlobfsBlockSize);
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root_node->Create(info->path + 1, 0, &file), ZX_OK);
  auto blob = fbl::RefPtr<Blob>::Downcast(std::move(file));
  // Force no compression so that we have finer control over the size.
  EXPECT_EQ(blob->PrepareWrite(info->size_data, /*compress=*/false), ZX_OK);
  size_t actual;
  // If this starts to fail with an ERR_NO_SPACE error it could be because WriteBufferBlockCount()
  // has changed and is now returning something too big for the device we're using in this test.
  EXPECT_EQ(blob->Write(info->data.get(), info->size_data, 0, &actual), ZX_OK);

  sync_completion_t sync;
  blob->Sync([&](zx_status_t status) {
    EXPECT_EQ(status, ZX_OK);
    sync_completion_signal(&sync);
  });
  sync_completion_wait(&sync, ZX_TIME_INFINITE);
  EXPECT_EQ(blob->Close(), ZX_OK);
  blob.reset();

  ASSERT_EQ(root_node->Lookup(info->path + 1, &file), ZX_OK);
  TestScopedVnodeOpen open(file);  // File must be open to read from it.

  auto buffer = std::make_unique<uint8_t[]>(info->size_data);
  EXPECT_EQ(file->Read(buffer.get(), info->size_data, 0, &actual), ZX_OK);
  EXPECT_EQ(memcmp(buffer.get(), info->data.get(), info->size_data), 0);
}

#ifndef NDEBUG

class FsckAtEndOfEveryTransactionTest : public BlobfsTest {
 protected:
  MountOptions GetMountOptions() const override {
    MountOptions options = BlobfsTest::GetMountOptions();
    options.fsck_at_end_of_every_transaction = true;
    return options;
  }
};

TEST_F(FsckAtEndOfEveryTransactionTest, FsckAtEndOfEveryTransaction) {
  fbl::RefPtr<fs::Vnode> root;
  ASSERT_EQ(blobfs()->OpenRootNode(&root), ZX_OK);
  fs::Vnode* root_node = root.get();

  std::unique_ptr<BlobInfo> info = GenerateRealisticBlob("", 500123);
  {
    fbl::RefPtr<fs::Vnode> file;
    ASSERT_EQ(root_node->Create(info->path + 1, 0, &file), ZX_OK);
    EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);
    size_t actual;
    EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &actual), ZX_OK);
    EXPECT_EQ(actual, info->size_data);
    EXPECT_EQ(file->Close(), ZX_OK);
  }
  EXPECT_EQ(root_node->Unlink(info->path + 1, false), ZX_OK);

  blobfs()->Sync([loop = &loop()](zx_status_t) { loop->Quit(); });
  loop().Run();
}

#endif  // !defined(NDEBUG)

/*
void VnodeSync(fs::Vnode* vnode) {
  // It's difficult to get a precise hook into the period between when data has been written and
  // when it has been flushed to disk.  The journal will delay flushing metadata, so the following
  // should test sync being called before metadata has been flushed, and then again afterwards.
  for (int i = 0; i < 2; ++i) {
    sync_completion_t sync;
    vnode->Sync([&](zx_status_t status) {
      EXPECT_EQ(ZX_OK, status);
      sync_completion_signal(&sync);
    });
    sync_completion_wait(&sync, ZX_TIME_INFINITE);
  }
}
*/

std::unique_ptr<BlobInfo> CreateBlob(const fbl::RefPtr<fs::Vnode>& root, size_t size) {
  std::unique_ptr<BlobInfo> info = GenerateRandomBlob("", size);
  memmove(info->path, info->path + 1, strlen(info->path));  // Remove leading slash.

  fbl::RefPtr<fs::Vnode> file;
  EXPECT_EQ(root->Create(info->path, 0, &file), ZX_OK);

  size_t out_actual = 0;
  EXPECT_EQ(file->Truncate(info->size_data), ZX_OK);

  EXPECT_EQ(file->Write(info->data.get(), info->size_data, 0, &out_actual), ZX_OK);
  EXPECT_EQ(info->size_data, out_actual);

  file->Close();
  return info;
}

// In this test we try to simulate fragmentation and test fragmentation metrics. We create
// fragmentation by first creating few blobs, deleting a subset of those blobs and then finally
// creating a huge blob that occupies all the blocks freed by blob deletion. We measure/verify
// metrics at each stage.
// This test has an understanding about block allocation policy.
TEST(BlobfsFragmentationTest, FragmentationMetrics) {
  struct Stats {
    int64_t total_nodes = 0;
    int64_t blobs_in_use = 0;
    int64_t extent_containers_in_use = 0;
    std::map<size_t, uint64_t> extents_per_blob;
    std::map<size_t, uint64_t> free_fragments;
    std::map<size_t, uint64_t> in_use_fragments;

    bool operator==(const Stats& other) const {
      return total_nodes == other.total_nodes && blobs_in_use == other.blobs_in_use &&
             extent_containers_in_use == other.extent_containers_in_use &&
             extents_per_blob == other.extents_per_blob && free_fragments == other.free_fragments &&
             in_use_fragments == other.in_use_fragments;
    }

    void ClearMaps() {
      extents_per_blob.clear();
      free_fragments.clear();
      in_use_fragments.clear();
    }
  };

  // We have to do things this way because InMemoryLogger is not thread-safe.
  class Logger : public cobalt_client::InMemoryLogger {
   public:
    bool WaitUntilStatsEq(const Stats& expected) {
      const auto timeout = std::chrono::seconds(10);
      const auto start = std::chrono::steady_clock::now();
      const auto end = start + timeout;
      for (auto now = start; now < end; now = std::chrono::steady_clock::now()) {
        auto sync_timeout = std::chrono::duration_cast<std::chrono::microseconds>(end - now);
        if (sync_completion_wait(&sync_, zx::usec(sync_timeout.count()).get()) != ZX_OK) {
          break;
        }
        sync_completion_reset(&sync_);
        std::lock_guard<std::mutex> lock(mtx_);
        if (found_ == expected) {
          found_.ClearMaps();
          return true;
        }
      }
      std::lock_guard<std::mutex> lock(mtx_);
      found_.ClearMaps();
      return false;
    }

    void MaybeSignal() FXL_REQUIRE(mtx_) {
      // Wake up if all of the relevant metrics have been logged more than past the last wakeup
      // watermark.
      constexpr const std::array<fs_metrics::Event, 6> kRelevantEvents{
          fs_metrics::Event::kFragmentationTotalNodes,
          fs_metrics::Event::kFragmentationInUseFragments,
          fs_metrics::Event::kFragmentationFreeFragments,
          fs_metrics::Event::kFragmentationInodesInUse,
          fs_metrics::Event::kFragmentationExtentContainersInUse,
          fs_metrics::Event::kFragmentationExtentsPerFile,
      };
      uint64_t min_value = 0;
      for (auto event : kRelevantEvents) {
        if (!min_value || log_counts_[event] < min_value) {
          min_value = log_counts_[event];
        }
      }
      if (min_value > last_signal_watermark_) {
        last_signal_watermark_ = min_value;
        sync_completion_signal(&sync_);
      }
    }

    bool LogInteger(const cobalt_client::MetricOptions& metric_info, int64_t value) override {
      if (!InMemoryLogger::LogInteger(metric_info, value)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(mtx_);

      auto id = static_cast<fs_metrics::Event>(metric_info.metric_id);
      log_counts_[id]++;
      switch (id) {
        case fs_metrics::Event::kFragmentationTotalNodes: {
          if (value != 0)
            found_.total_nodes = value;
          break;
        }
        case fs_metrics::Event::kFragmentationInodesInUse: {
          if (value != 0)
            found_.blobs_in_use = value;
          break;
        }
        case fs_metrics::Event::kFragmentationExtentContainersInUse: {
          if (value != 0)
            found_.extent_containers_in_use = value;
          break;
        }
        default:
          break;
      }
      MaybeSignal();
      return true;
    }

    bool Log(const cobalt_client::MetricOptions& metric_info, const HistogramBucket* buckets,
             size_t num_buckets) override {
      if (!InMemoryLogger::Log(metric_info, buckets, num_buckets)) {
        return false;
      }
      std::lock_guard<std::mutex> lock(mtx_);
      if (num_buckets == 0) {
        MaybeSignal();
        return true;
      }

      auto id = static_cast<fs_metrics::Event>(metric_info.metric_id);
      log_counts_[id]++;
      std::map<size_t, uint64_t>* map = nullptr;
      switch (id) {
        case fs_metrics::Event::kFragmentationExtentsPerFile: {
          map = &found_.extents_per_blob;
          break;
        }
        case fs_metrics::Event::kFragmentationInUseFragments: {
          map = &found_.in_use_fragments;
          break;
        }
        case fs_metrics::Event::kFragmentationFreeFragments: {
          map = &found_.free_fragments;
          break;
        }
        default:
          break;
      }

      if (map == nullptr) {
        return true;
      }
      for (size_t i = 0; i < num_buckets; i++) {
        if (buckets[i].count > 0)
          (*map)[i] += buckets[i].count;
      }
      MaybeSignal();
      return true;
    }

   private:
    // The metric flushing thread in Blobfs calls Log and LogInteger while the test is looping in
    // WaitUntilStatsEq. This mutex guards the members that are used by both threads.
    std::mutex mtx_;
    Stats found_ FXL_GUARDED_BY(mtx_);
    sync_completion_t sync_;

    std::map<fs_metrics::Event, uint64_t> log_counts_ FXL_GUARDED_BY(mtx_);
    // The last signal was delivered when the min of the relevant entries in log_counts_ was this
    // value.
    uint64_t last_signal_watermark_ FXL_GUARDED_BY(mtx_) = 0;
  };

  std::unique_ptr<Logger> logger = std::make_unique<Logger>();
  auto* logger_ptr = logger.get();

  auto device = MockBlockDevice::CreateAndFormat(
      {
          .blob_layout_format = BlobLayoutFormat::kCompactMerkleTreeAtEnd,
          .oldest_minor_version = kBlobfsCurrentMinorVersion,
          .num_inodes = kNumNodes,
      },
      kNumBlocks);
  ASSERT_TRUE(device);

  MountOptions mount_options{
      .metrics = true,
      .collector_factory =
          [&logger] { return std::make_unique<cobalt_client::Collector>(std::move(logger)); },
      .metrics_flush_time = zx::msec(100)};
  BlobfsTestSetup setup;
  ASSERT_EQ(ZX_OK, setup.Mount(std::move(device), mount_options));

  srand(testing::UnitTest::GetInstance()->random_seed());

  {
    Stats expected;
    expected.total_nodes = static_cast<int64_t>(setup.blobfs()->Info().inode_count);
    expected.free_fragments[6] = 2;
    setup.blobfs()->UpdateFragmentationMetrics();
    ASSERT_TRUE(logger_ptr->WaitUntilStatsEq(expected));
  }

  fbl::RefPtr<fs::Vnode> root;
  ASSERT_EQ(setup.blobfs()->OpenRootNode(&root), ZX_OK);
  std::vector<std::unique_ptr<BlobInfo>> infos;
  constexpr int kSmallBlobCount = 10;
  infos.reserve(kSmallBlobCount);
  // We create 10 blobs that occupy 1 block each. After these creation, data block bitmap should
  // look like (first 10 bits set and all other bits unset.)
  // 111111111100000000....
  for (int i = 0; i < kSmallBlobCount; i++) {
    infos.push_back(CreateBlob(root, 64));
  }

  {
    Stats expected;
    expected.total_nodes = static_cast<int64_t>(setup.blobfs()->Info().inode_count);
    expected.blobs_in_use = kSmallBlobCount;
    expected.extents_per_blob[1] = kSmallBlobCount;
    expected.in_use_fragments[1] = kSmallBlobCount;
    expected.free_fragments[6] = 1;
    setup.blobfs()->UpdateFragmentationMetrics();
    ASSERT_TRUE(logger_ptr->WaitUntilStatsEq(expected));
  }

  // Delete few blobs. Notice the pattern we delete. With these deletions free(0) and used(1)
  // block bitmap will look as follows 1010100111000000... This creates 4 free fragments. 6 used
  // fragments.
  constexpr uint64_t kBlobsDeleted = 4;
  ASSERT_EQ(root->Unlink(infos[1]->path, false), ZX_OK);
  ASSERT_EQ(root->Unlink(infos[3]->path, false), ZX_OK);
  ASSERT_EQ(root->Unlink(infos[5]->path, false), ZX_OK);
  ASSERT_EQ(root->Unlink(infos[6]->path, false), ZX_OK);

  {
    Stats expected;
    expected.total_nodes = static_cast<int64_t>(setup.blobfs()->Info().inode_count);
    expected.blobs_in_use = kSmallBlobCount - kBlobsDeleted;
    expected.free_fragments[1] = 3;
    expected.free_fragments[6] = 1;
    expected.extents_per_blob[1] = kSmallBlobCount - kBlobsDeleted;
    expected.in_use_fragments[1] = kSmallBlobCount - kBlobsDeleted;
    setup.blobfs()->UpdateFragmentationMetrics();
    ASSERT_TRUE(logger_ptr->WaitUntilStatsEq(expected));
  }

  // Create a huge(10 blocks) blob that potentially fills atleast three free fragments that we
  // created above.
  auto info = CreateBlob(root, 20 * 8192);
  fbl::RefPtr<fs::Vnode> file;
  ASSERT_EQ(root->Lookup(info->path, &file), ZX_OK);
  fs::VnodeAttributes attributes;
  file->GetAttributes(&attributes);
  uint64_t blocks = attributes.storage_size / 8192;

  // For some reason, if it turns out that the random data is highly compressible then our math
  // belows blows up. Assert that is not the case.
  ASSERT_GT(blocks, kBlobsDeleted);

  {
    Stats expected;
    expected.total_nodes = static_cast<int64_t>(setup.blobfs()->Info().inode_count);
    expected.blobs_in_use = kSmallBlobCount - kBlobsDeleted + 1;
    expected.extent_containers_in_use = 1;
    expected.free_fragments[1] = 1;
    expected.free_fragments[5] = 1;
    expected.extents_per_blob[1] = kSmallBlobCount - kBlobsDeleted + 1;
    expected.in_use_fragments[1] = kSmallBlobCount - kBlobsDeleted + 2;
    expected.in_use_fragments[2] = 1;
    setup.blobfs()->UpdateFragmentationMetrics();
    ASSERT_TRUE(logger_ptr->WaitUntilStatsEq(expected));
  }
}

}  // namespace
}  // namespace blobfs
