// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FACTORY_FACTORYFS_UTILS_H_
#define SRC_STORAGE_FACTORY_FACTORYFS_UTILS_H_

#include <memory>
#include <optional>

#include <fbl/auto_lock.h>
#include <fbl/vector.h>
#include <zxtest/zxtest.h>

#include "src/lib/storage/block_client/cpp/block_device.h"
#include "src/lib/storage/vfs/cpp/transaction/transaction_handler.h"
#include "src/storage/factory/factoryfs/format.h"

namespace factoryfs {

constexpr uint32_t kBlockSize = kFactoryfsBlockSize;
constexpr uint32_t kDeviceBlockSize = 1024;
constexpr uint32_t kDiskBlockRatio = kBlockSize / kDeviceBlockSize;

// Callback for MockTransactionManager to invoke on calls to Transaction(). |request| is performed
// on the provided |vmo|.
using TransactionCallback =
    fit::function<zx_status_t(const block_fifo_request_t& request, const zx::vmo& vmo)>;

using block_client::BlockDevice;

class MockTransactionManager : public fs::TransactionHandler {
 public:
  MockTransactionManager() = default;
  ~MockTransactionManager() = default;

  // Sets the |callback| to be invoked for each request on calls to Transaction().
  void SetTransactionCallback(TransactionCallback callback) {
    fbl::AutoLock lock(&lock_);
    transaction_callback_ = std::move(callback);
  }

  uint64_t BlockNumberToDevice(uint64_t block_num) const final { return block_num; }

  zx_status_t RunOperation(const storage::Operation& operation,
                           storage::BlockBuffer* buffer) final {
    return ZX_OK;
  }

  zx_status_t RunRequests(const std::vector<storage::BufferedOperation>& operations) override {
    return ZX_ERR_NOT_SUPPORTED;
  }

  const Superblock& Info() const { return superblock_; }

  Superblock& MutableInfo() { return superblock_; }

  zx_status_t BlockAttachVmo(const zx::vmo& vmo, storage::Vmoid* out);

  zx_status_t BlockDetachVmo(storage::Vmoid vmoid);

 private:
  Superblock superblock_{};
  fbl::Vector<std::optional<zx::vmo>> attached_vmos_ __TA_GUARDED(lock_);
  TransactionCallback transaction_callback_ __TA_GUARDED(lock_);
  fbl::Mutex lock_;
};

// Reads |size| bytes from the |device| at byte offset |dev_offset| into |buf|.
// Expects |size| and |dev_offset| to be multiple of |device| block size.
void DeviceBlockRead(BlockDevice* device, void* buf, size_t size, uint64_t dev_offset);

// Writes |size| bytes from the |buf| to the |device| at offset |dev_offset|.
// Expects |size| and |dev_offset| to be multiple of |device| block size.
void DeviceBlockWrite(BlockDevice* device, const void* buf, size_t size, uint64_t dev_offset);

}  // namespace factoryfs

#endif  // SRC_STORAGE_FACTORY_FACTORYFS_UTILS_H_
