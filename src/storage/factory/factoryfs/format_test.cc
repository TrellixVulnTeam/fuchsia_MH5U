// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/factory/factoryfs/format.h"

#include <zxtest/zxtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/factory/factoryfs/factoryfs.h"
#include "src/storage/factory/factoryfs/mkfs.h"

using block_client::FakeBlockDevice;

namespace factoryfs {
namespace {

zx_status_t CheckMountability(std::unique_ptr<BlockDevice> device) {
  async::Loop loop(&kAsyncLoopConfigNoAttachToCurrentThread);

  MountOptions options = {};
  options.metrics = false;

  auto vfs = std::make_unique<fs::ManagedVfs>(loop.dispatcher());
  return Factoryfs::Create(nullptr, std::move(device), &options, vfs.get()).status_value();
}

// Formatting filesystems should fail on devices that cannot be written.
TEST(FormatFilesystemTest, CannotFormatReadOnlyDevice) {
  auto device = std::make_unique<FakeBlockDevice>(1 << 20, 512);
  device->SetInfoFlags(fuchsia_hardware_block_FLAG_READONLY);
  ASSERT_EQ(ZX_ERR_ACCESS_DENIED, FormatFilesystem(device.get()));
}

// Formatting filesystems should fail on devices that don't contain any blocks.
TEST(FormatFilesystemTest, CannotFormatEmptyDevice) {
  auto device = std::make_unique<FakeBlockDevice>(0, 0);
  ASSERT_EQ(ZX_ERR_NO_SPACE, FormatFilesystem(device.get()));
}

// Formatting filesystems should fail on devices which have a block size that
// does not cleanly divide the factoryfs block size.
TEST(FormatFilesystemTest, CannotFormatDeviceWithNonDivisorBlockSize) {
  const uint64_t kBlockCount = 1 << 20;
  uint64_t kBlockSize = 511;
  EXPECT_NE(kFactoryfsBlockSize % kBlockSize, 0, "Expected non-divisor block size");
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kBlockSize);
  ASSERT_EQ(ZX_ERR_IO_INVALID, FormatFilesystem(device.get()));
}

// Factoryfs can be formatted on devices that have "trailing device block(s)" that
// cannot be fully addressed by factoryfs blocks.
TEST(FormatFilesystemTest, FormatDeviceWithTrailingDiskBlock) {
  const uint64_t kBlockCount = (1 << 20) + 1;
  const uint32_t kBlockSize = 512;
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kBlockSize);
  ASSERT_OK(FormatFilesystem(device.get()));
  ASSERT_OK(CheckMountability(std::move(device)));
}

// Factoryfs can be formatted on devices that have block sizes up to and including
// the factoryfs block size itself.
TEST(FormatFilesystemTest, FormatDeviceWithLargestBlockSize) {
  const uint64_t kBlockCount = 1 << 20;
  const uint32_t kBlockSize = kFactoryfsBlockSize;
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kBlockSize);
  ASSERT_OK(FormatFilesystem(device.get()));
  ASSERT_OK(CheckMountability(std::move(device)));
}

// After formatting a filesystem with valid block size N, mounting on
// a device with an invalid block size should fail.
TEST(FormatFilesystemTest, CreateFactoryfsFailureOnUnalignedBlockSize) {
  const uint64_t kBlockCount = 1 << 20;
  const uint32_t kBlockSize = 512;
  auto device = std::make_unique<FakeBlockDevice>(kBlockCount, kBlockSize);
  ASSERT_OK(FormatFilesystem(device.get()));
  device->SetBlockSize(kBlockSize + 1);
  ASSERT_EQ(ZX_ERR_INVALID_ARGS, CheckMountability(std::move(device)));
}

}  // namespace
}  // namespace factoryfs
