// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_BLOCK_DEVICE_H_
#define SRC_STORAGE_FSHOST_BLOCK_DEVICE_H_

#include <fidl/fuchsia.fs.startup/cpp/wire.h>
#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/zx/status.h>
#include <zircon/types.h>

#include <memory>
#include <optional>
#include <string_view>

#include <fbl/algorithm.h>
#include <fbl/string_buffer.h>

#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/storage/fshost/block-device-interface.h"
#include "src/storage/fshost/config.h"
#include "src/storage/fshost/filesystem-mounter.h"

namespace fshost {

// Get the topological path of the device backing |fd|.
std::string GetTopologicalPath(int fd);

// Collect and synthesize the blobfs startup options.
fuchsia_fs_startup::wire::StartOptions GetBlobfsStartOptions(
    const fshost::Config* config, std::shared_ptr<FshostBootArgs> boot_args);

// A concrete implementation of the block device interface.
//
// Used by fshost to attach either drivers or filesystems to incoming block devices.
class BlockDevice : public BlockDeviceInterface {
 public:
  BlockDevice(FilesystemMounter* mounter, fbl::unique_fd fd, const Config* device_config);
  BlockDevice(const BlockDevice&) = delete;
  BlockDevice& operator=(const BlockDevice&) = delete;

  fs_management::DiskFormat GetFormat() override;
  void SetFormat(fs_management::DiskFormat format) override;
  zx_status_t GetInfo(fuchsia_hardware_block_BlockInfo* out_info) const override;
  const fuchsia_hardware_block_partition::wire::Guid& GetInstanceGuid() const override;
  const fuchsia_hardware_block_partition::wire::Guid& GetTypeGuid() const override;
  zx_status_t AttachDriver(const std::string_view& driver) override;
  zx_status_t UnsealZxcrypt() override;
  zx_status_t FormatZxcrypt() override;
  bool ShouldCheckFilesystems() override;
  zx_status_t CheckFilesystem() override;
  zx_status_t FormatFilesystem() override;
  zx_status_t MountFilesystem() override;
  zx::status<std::string> VeritySeal() override;
  zx_status_t OpenBlockVerityForVerifiedRead(std::string seal_hex) override;
  bool ShouldAllowAuthoringFactory() override;
  zx_status_t SetPartitionMaxSize(const std::string& fvm_path, uint64_t max_byte_size) override;
  bool IsNand() const override { return false; }
  zx_status_t SetPartitionName(const std::string& fvm_path, std::string_view name) override;

  fs_management::DiskFormat content_format() const override;
  const std::string& topological_path() const override { return topological_path_; }
  const std::string& partition_name() const override;
  zx::status<fidl::ClientEnd<fuchsia_io::Node>> GetDeviceEndPoint() const;
  zx_status_t CheckCustomFilesystem(const std::string& binary_path) const;
  zx_status_t FormatCustomFilesystem(const std::string& binary_path) const;

 private:
  zx_status_t MountData(fs_management::MountOptions* options, zx::channel block_device);

  // TODO(https://fxbug.dev/92302): Temporarily allow selection of filesystem format.
  zx_status_t MaybeChangeDataPartitionFormat() const;

  FilesystemMounter* mounter_ = nullptr;
  fbl::unique_fd fd_;
  const Config* device_config_;
  mutable std::optional<fuchsia_hardware_block_BlockInfo> info_;
  mutable fs_management::DiskFormat content_format_;
  fs_management::DiskFormat format_ = fs_management::kDiskFormatUnknown;
  std::string topological_path_;
  mutable std::string partition_name_;
  mutable std::optional<fuchsia_hardware_block_partition::wire::Guid> instance_guid_;
  mutable std::optional<fuchsia_hardware_block_partition::wire::Guid> type_guid_;
};

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_BLOCK_DEVICE_H_
