// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/bin/start-storage-benchmark/block-device.h"

#include <fcntl.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <lib/fdio/fd.h>
#include <lib/service/llcpp/service.h>

#include <fbl/unique_fd.h>
#include <gtest/gtest.h>

#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/testing/predicates/status.h"
#include "src/storage/testing/fvm.h"
#include "src/storage/testing/ram_disk.h"

namespace storage_benchmark {
namespace {

constexpr uint64_t kBlockSize = 8192;
constexpr uint64_t kBlockCount = 512;
constexpr uint64_t kFvmSliceSize = 32lu * 1024;
constexpr uint64_t kVolumeSize = 0;

TEST(BlockDeviceTest, ConnectToFvmReturnsAValidConnection) {
  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());

  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());

  auto fvm_info = fidl::WireCall(*fvm_client)->GetInfo();
  ASSERT_OK(fvm_info.status());
  ASSERT_OK(fvm_info->status);
  EXPECT_EQ(fvm_info->info->slice_size, kFvmSliceSize);
}

TEST(BlockDeviceTest, FvmVolumeCreateWorks) {
  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());
  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());

  auto fvm_colume = FvmVolume::Create(*fvm_client, kFvmSliceSize * 2);
  ASSERT_OK(fvm_colume.status_value());

  auto volume_client =
      service::Connect<fuchsia_hardware_block_volume::Volume>(fvm_colume->path().c_str());
  ASSERT_OK(fvm_colume.status_value());
  auto info = fidl::WireCall(*volume_client)->GetVolumeInfo();
  ASSERT_OK(info.status());
  ASSERT_OK(info->status);
  EXPECT_EQ(info->volume->partition_slice_count, 2lu);
}

TEST(BlockDeviceTest, CreateZxcryptVolumeWorks) {
  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());
  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());
  auto fvm_volume = FvmVolume::Create(*fvm_client, kVolumeSize);
  ASSERT_OK(fvm_volume.status_value());

  auto zxcrypt_path = CreateZxcryptVolume(fvm_volume->path());
  ASSERT_OK(zxcrypt_path.status_value());

  fbl::unique_fd volume_fd(open(fvm_volume->path().c_str(), O_RDWR));
  fs_management::DiskFormat format = fs_management::DetectDiskFormat(volume_fd.get());
  EXPECT_EQ(format, fs_management::kDiskFormatZxcrypt);
}

TEST(BlockDeviceTest, FormatBlockDeviceWorks) {
  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());
  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());
  auto fvm_volume = FvmVolume::Create(*fvm_client, kVolumeSize);
  ASSERT_OK(fvm_volume.status_value());

  auto status = FormatBlockDevice(fvm_volume->path(), fs_management::kDiskFormatMinfs);
  ASSERT_OK(status.status_value());

  fbl::unique_fd volume_fd(open(fvm_volume->path().c_str(), O_RDWR));
  fs_management::DiskFormat format = fs_management::DetectDiskFormat(volume_fd.get());
  EXPECT_EQ(format, fs_management::kDiskFormatMinfs);
}

TEST(BlockDeviceTest, FormatBlockDeviceWithZxcryptWorks) {
  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());
  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());
  auto fvm_volume = FvmVolume::Create(*fvm_client, kVolumeSize);
  ASSERT_OK(fvm_volume.status_value());
  auto zxcrypt_path = CreateZxcryptVolume(fvm_volume->path());
  ASSERT_OK(zxcrypt_path.status_value());

  auto status = FormatBlockDevice(*zxcrypt_path, fs_management::kDiskFormatMinfs);
  ASSERT_OK(status.status_value());

  fbl::unique_fd minfs_fd(open(zxcrypt_path->c_str(), O_RDWR));
  fs_management::DiskFormat format = fs_management::DetectDiskFormat(minfs_fd.get());
  EXPECT_EQ(format, fs_management::kDiskFormatMinfs);
}

TEST(BlockDeviceTest, StartBlockDeviceFilesystemWorks) {
  constexpr char kFileName[] = "file";
  constexpr std::string_view kFileContents = "file-contents";
  constexpr ssize_t kFileSize = kFileContents.size();

  auto ramdisk = storage::RamDisk::Create(kBlockSize, kBlockCount);
  ASSERT_OK(ramdisk.status_value());
  auto fvm_path = storage::CreateFvmInstance(ramdisk->path(), kFvmSliceSize);
  ASSERT_OK(fvm_path.status_value());
  auto fvm_client = ConnectToFvm(ramdisk->path());
  ASSERT_OK(fvm_client.status_value());
  auto fvm_volume = FvmVolume::Create(*fvm_client, kVolumeSize);
  ASSERT_OK(fvm_volume.status_value());
  auto status = FormatBlockDevice(fvm_volume->path(), fs_management::kDiskFormatMinfs);
  ASSERT_OK(status.status_value());
  std::string block_device_path = fvm_volume->path();

  auto fs = StartBlockDeviceFilesystem(block_device_path, fs_management::kDiskFormatMinfs,
                                       *std::move(fvm_volume));
  ASSERT_OK(fs.status_value());

  auto root = fs->GetFilesystemRoot();
  ASSERT_OK(root.status_value());
  fbl::unique_fd dir;
  ASSERT_OK(fdio_fd_create(root->TakeChannel().release(), dir.reset_and_get_address()));
  fbl::unique_fd file(openat(dir.get(), kFileName, O_CREAT | O_RDWR));
  ASSERT_EQ(pwrite(file.get(), kFileContents.data(), kFileSize, 0), kFileSize);
  std::string contents(kFileSize, 0);
  ASSERT_EQ(pread(file.get(), contents.data(), kFileSize, 0), kFileSize);
  EXPECT_EQ(contents, kFileContents);
}

}  // namespace
}  // namespace storage_benchmark
