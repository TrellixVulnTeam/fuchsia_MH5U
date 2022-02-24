// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_FILESYSTEM_MOUNTER_H_
#define SRC_STORAGE_FSHOST_FILESYSTEM_MOUNTER_H_

#include <fidl/fuchsia.fs.startup/cpp/wire.h>
#include <fidl/fuchsia.fxfs/cpp/wire.h>
#include <lib/zx/channel.h>
#include <lib/zx/status.h>
#include <zircon/types.h>

#include <memory>

#include <fbl/unique_fd.h>

#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/storage/fshost/config.h"
#include "src/storage/fshost/fs-manager.h"
#include "src/storage/fshost/fshost-boot-args.h"
#include "src/storage/fshost/inspect-manager.h"
#include "src/storage/fshost/metrics.h"

namespace fshost {

// FilesystemMounter is a utility class which wraps the FsManager
// and helps clients mount filesystems within the fshost namespace.
class FilesystemMounter {
 public:
  FilesystemMounter(FsManager& fshost, const Config* config) : fshost_(fshost), config_(*config) {}

  virtual ~FilesystemMounter() = default;

  void FuchsiaStart() const { fshost_.FuchsiaStart(); }

  // Installs the filesystem rooted at |root_directory| at |point|.
  //
  // |export_root| should be a channel connected to the export root of the filesystem. Passing an
  // invalid handle should be avoided if possible, but if it isn't, then the filesystem will not get
  // shut down.
  //
  // |root_directory| can be an arbitrary Directory connection (although the fact that the) peer is
  // a directory is not verified).
  zx::status<> InstallFs(FsManager::MountPoint point, std::string_view device_path,
                         zx::channel export_root, zx::channel root_directory) {
    return fshost_.InstallFs(point, device_path, std::move(export_root), std::move(root_directory));
  }

  bool Netbooting() const { return config_.netboot(); }
  bool ShouldCheckFilesystems() const { return config_.check_filesystems(); }

  // Attempts to mount a block device to "/data".
  // Fails if already mounted.
  zx_status_t MountData(zx::channel block_device_client, const fs_management::MountOptions& options,
                        fs_management::DiskFormat format);

  // Attempts to mount a block device to "/durable".
  // Fails if already mounted.
  zx_status_t MountDurable(zx::channel block_device_client,
                           const fs_management::MountOptions& options);

  // Attempts to mount a block device to "/install".
  // Fails if already mounted.
  zx_status_t MountInstall(zx::channel block_device_client,
                           const fs_management::MountOptions& options);

  // Attempts to mount a block device to "/blob".
  // Fails if already mounted.
  zx_status_t MountBlob(zx::channel block_device_client,
                        fuchsia_fs_startup::wire::StartOptions options);

  // Attempts to mount a block device to "/factory".
  // Fails if already mounted.
  zx_status_t MountFactoryFs(zx::channel block_device_client,
                             const fs_management::MountOptions& options);

  // Attempts to mount pkgfs if all preconditions have been met:
  // - Pkgfs has not previously been mounted
  // - Blobfs has been mounted
  // - The data partition has been mounted
  void TryMountPkgfs();

  std::shared_ptr<FshostBootArgs> boot_args() { return fshost_.boot_args(); }
  void ReportMinfsCorruption();

  bool BlobMounted() const { return blob_mounted_; }
  bool DataMounted() const { return data_mounted_; }
  bool PkgfsMounted() const { return pkgfs_mounted_; }
  bool FactoryMounted() const { return factory_mounted_; }
  bool DurableMounted() const { return durable_mounted_; }

  // Returns a crypt client for a filesystem if configured. If configuration indicates the data
  // filesystem does not require it, zx::ok({}) is returned.
  zx::status<fidl::ClientEnd<fuchsia_fxfs::Crypt>> GetCryptClient();

  FsManager& manager() { return fshost_; }
  InspectManager& inspect_manager() { return fshost_.inspect_manager(); }

 private:
  // Performs the mechanical action of mounting a filesystem, without
  // validating the type of filesystem being mounted.
  zx::status<> MountFilesystem(FsManager::MountPoint point, const char* binary,
                               const fs_management::MountOptions& options,
                               zx::channel block_device_client, uint32_t fs_flags,
                               fidl::ClientEnd<fuchsia_fxfs::Crypt> crypt_client = {});

  bool WaitForData() const { return config_.wait_for_data(); }

  // Actually launches the filesystem process.
  //
  // Virtualized to enable testing.
  virtual zx_status_t LaunchFs(int argc, const char** argv, zx_handle_t* hnd, uint32_t* ids,
                               size_t len, uint32_t fs_flags);

  // Actually launches the filesystem component.
  //
  // TODO(fxbug.dev/91577): All filesystems should be launched as components. Once they are, remove
  // LaunchFs.
  //
  // Virtualized to enable testing.
  virtual zx::status<> LaunchFsComponent(zx::channel block_device,
                                         fuchsia_fs_startup::wire::StartOptions options,
                                         const std::string& fs_name);

  FsManager& fshost_;
  const Config& config_;
  bool data_mounted_ = false;
  bool durable_mounted_ = false;
  bool install_mounted_ = false;
  bool blob_mounted_ = false;
  bool pkgfs_mounted_ = false;
  bool factory_mounted_ = false;
  fidl::ClientEnd<fuchsia_io::Directory> crypt_outgoing_directory_;
};

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_FILESYSTEM_MOUNTER_H_
