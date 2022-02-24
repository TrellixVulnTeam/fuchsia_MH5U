// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "block-device.h"

#include <dirent.h>
#include <fcntl.h>
#include <fidl/fuchsia.device/cpp/wire.h>
#include <fidl/fuchsia.fs.startup/cpp/wire.h>
#include <fidl/fuchsia.fs/cpp/wire.h>
#include <fidl/fuchsia.hardware.block.volume/cpp/wire.h>
#include <fuchsia/device/c/fidl.h>
#include <fuchsia/hardware/block/c/fidl.h>
#include <inttypes.h>
#include <lib/fdio/cpp/caller.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/fd.h>
#include <lib/fdio/fdio.h>
#include <lib/fdio/unsafe.h>
#include <lib/fdio/watcher.h>
#include <lib/fzl/time.h>
#include <lib/service/llcpp/service.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>
#include <lib/zx/status.h>
#include <lib/zx/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zircon/device/block.h>
#include <zircon/errors.h>
#include <zircon/processargs.h>
#include <zircon/status.h>
#include <zircon/syscalls.h>
#include <zircon/types.h>

#include <utility>

#include <fbl/algorithm.h>
#include <fbl/string_buffer.h>
#include <fbl/unique_fd.h>
#include <gpt/gpt.h>

#include "block-watcher.h"
#include "constants.h"
#include "encrypted-volume.h"
#include "extract-metadata.h"
#include "pkgfs-launcher.h"
#include "src/devices/block/drivers/block-verity/verified-volume-client.h"
#include "src/lib/files/file.h"
#include "src/lib/storage/fs_management/cpp/format.h"
#include "src/lib/storage/fs_management/cpp/mount.h"
#include "src/lib/uuid/uuid.h"
#include "src/storage/fshost/block-device-interface.h"
#include "src/storage/fshost/copier.h"
#include "src/storage/fshost/fshost-fs-provider.h"
#include "src/storage/fvm/format.h"
#include "src/storage/minfs/fsck.h"
#include "src/storage/minfs/minfs.h"

namespace fshost {
namespace {

const char kAllowAuthoringFactoryConfigFile[] = "/boot/config/allow-authoring-factory";

// return value is ignored
int UnsealZxcryptThread(void* arg) {
  std::unique_ptr<int> fd_ptr(static_cast<int*>(arg));
  fbl::unique_fd fd(*fd_ptr);
  fbl::unique_fd devfs_root(open("/dev", O_RDONLY));
  EncryptedVolume volume(std::move(fd), std::move(devfs_root));
  volume.EnsureUnsealedAndFormatIfNeeded();
  return 0;
}

// Holds thread state for OpenVerityDeviceThread
struct VerityDeviceThreadState {
  fbl::unique_fd fd;
  digest::Digest seal;
};

// return value is ignored
int OpenVerityDeviceThread(void* arg) {
  std::unique_ptr<VerityDeviceThreadState> state(static_cast<VerityDeviceThreadState*>(arg));
  fbl::unique_fd devfs_root(open("/dev", O_RDONLY));

  std::unique_ptr<block_verity::VerifiedVolumeClient> vvc;
  zx_status_t status = block_verity::VerifiedVolumeClient::CreateFromBlockDevice(
      state->fd.get(), std::move(devfs_root),
      block_verity::VerifiedVolumeClient::Disposition::kDriverAlreadyBound, zx::sec(5), &vvc);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Couldn't create VerifiedVolumeClient: " << zx_status_get_string(status);
    return 1;
  }

  fbl::unique_fd inner_block_fd;
  status = vvc->OpenForVerifiedRead(std::move(state->seal), zx::sec(5), inner_block_fd);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "OpenForVerifiedRead failed: " << zx_status_get_string(status);
    return 1;
  }
  return 0;
}

// Runs the binary indicated in `argv`, which must always be terminated with nullptr.
// `device_channel`, containing a handle to the block device, is passed to the binary.  If
// `export_root` is specified, the binary is launched asynchronously.  Otherwise, this waits for the
// binary to terminate and returns the status.
zx_status_t RunBinary(const fbl::Vector<const char*>& argv,
                      fidl::ClientEnd<fuchsia_io::Node> device,
                      fidl::ServerEnd<fuchsia_io::Directory> export_root = {},
                      fidl::ClientEnd<fuchsia_fxfs::Crypt> crypt_client = {}) {
  FX_CHECK(argv[argv.size() - 1] == nullptr);
  FshostFsProvider fs_provider;
  DevmgrLauncher launcher(&fs_provider);
  zx::process proc;
  int handle_count = 1;
  zx_handle_t handles[3] = {device.TakeChannel().release()};
  uint32_t handle_ids[3] = {FS_HANDLE_BLOCK_DEVICE_ID};
  bool async = false;
  if (export_root) {
    handles[handle_count] = export_root.TakeChannel().release();
    handle_ids[handle_count] = PA_DIRECTORY_REQUEST;
    ++handle_count;
    async = true;
  }
  if (crypt_client) {
    handles[handle_count] = crypt_client.TakeChannel().release();
    handle_ids[handle_count] = PA_HND(PA_USER0, 2);
    ++handle_count;
  }
  if (zx_status_t status = launcher.Launch(
          *zx::job::default_job(), argv[0], argv.data(), nullptr, -1,
          /* TODO(fxbug.dev/32044) */ zx::resource(), handles, handle_ids, handle_count, &proc, 0);
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to launch binary: " << argv[0];
    return status;
  }

  if (async)
    return ZX_OK;

  if (zx_status_t status = proc.wait_one(ZX_PROCESS_TERMINATED, zx::time::infinite(), nullptr);
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Error waiting for process to terminate";
    return status;
  }

  zx_info_process_t info;
  if (zx_status_t status = proc.get_info(ZX_INFO_PROCESS, &info, sizeof(info), nullptr, nullptr);
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to get process info";
    return status;
  }

  if (!(info.flags & ZX_INFO_PROCESS_FLAG_EXITED) || info.return_code != 0) {
    FX_LOGS(ERROR) << "flags: " << info.flags << ", return_code: " << info.return_code;
    return ZX_ERR_BAD_STATE;
  }

  return ZX_OK;
}

// Tries to mount Minfs and reads all data found on the minfs partition.  Errors are ignored.
Copier TryReadingMinfs(fidl::ClientEnd<fuchsia_io::Node> device) {
  fbl::Vector<const char*> argv = {kMinfsPath, "mount", nullptr};
  auto export_root_or = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (export_root_or.is_error())
    return {};
  if (RunBinary(argv, std::move(device), std::move(export_root_or->server)) != ZX_OK)
    return {};

  auto root_dir_or = fs_management::FsRootHandle(export_root_or->client);
  if (root_dir_or.is_error())
    return {};

  fbl::unique_fd fd;
  if (zx_status_t status =
          fdio_fd_create(root_dir_or->TakeChannel().release(), fd.reset_and_get_address());
      status != ZX_OK) {
    FX_LOGS(ERROR) << "fdio_fd_create failed";
    return {};
  }

  // Clone the handle so that we can unmount.
  zx::channel root_dir_handle;
  if (zx_status_t status = fdio_fd_clone(fd.get(), root_dir_handle.reset_and_get_address());
      status != ZX_OK) {
    FX_LOGS(ERROR) << "fdio_fd_clone failed";
    return {};
  }

  fidl::ClientEnd<fuchsia_io::Directory> root_dir_client(std::move(root_dir_handle));
  auto unmount = fit::defer([&export_root_or] {
    [[maybe_unused]] auto ignore_failure = fs_management::Shutdown(export_root_or->client);
  });

  if (auto copier_or = Copier::Read(std::move(fd)); copier_or.is_error()) {
    FX_LOGS(ERROR) << "Copier::Read: " << copier_or.status_string();
    return {};
  } else {
    return std::move(copier_or).value();
  }
}

}  // namespace

std::string GetTopologicalPath(int fd) {
  fdio_cpp::UnownedFdioCaller disk_connection(fd);
  auto resp = fidl::WireCall<fuchsia_device::Controller>(
                  zx::unowned_channel(disk_connection.borrow_channel()))
                  ->GetTopologicalPath();
  if (resp.status() != ZX_OK) {
    FX_LOGS(WARNING) << "Unable to get topological path (fidl error): "
                     << zx_status_get_string(resp.status());
    return {};
  }
  if (resp->result.is_err()) {
    FX_LOGS(WARNING) << "Unable to get topological path: "
                     << zx_status_get_string(resp->result.err());
    return {};
  }
  const auto& path = resp->result.response().path;
  return {path.data(), path.size()};
}

fuchsia_fs_startup::wire::StartOptions GetBlobfsStartOptions(
    const fshost::Config* config, std::shared_ptr<FshostBootArgs> boot_args) {
  fuchsia_fs_startup::wire::StartOptions options;
  options.collect_metrics = true;
  options.write_compression_level = -1;
  if (config->is_set(Config::kSandboxDecompression)) {
    options.sandbox_decompression = true;
  }
  if (boot_args) {
    std::optional<std::string> algorithm = boot_args->blobfs_write_compression_algorithm();
    if (algorithm == "UNCOMPRESSED") {
      options.write_compression_algorithm =
          fuchsia_fs_startup::wire::CompressionAlgorithm::kUncompressed;
    } else if (algorithm == "ZSTD_CHUNKED") {
      options.write_compression_algorithm =
          fuchsia_fs_startup::wire::CompressionAlgorithm::kZstdChunked;
    } else if (algorithm.has_value()) {
      // An unrecognized compression algorithm was requested. Ignore it and continue.
      FX_LOGS(WARNING) << "Ignoring " << *algorithm << " algorithm";
    }
    std::optional<std::string> eviction_policy = boot_args->blobfs_eviction_policy();
    if (eviction_policy == "NEVER_EVICT") {
      options.cache_eviction_policy_override =
          fuchsia_fs_startup::wire::EvictionPolicyOverride::kNeverEvict;
    } else if (eviction_policy == "EVICT_IMMEDIATELY") {
      options.cache_eviction_policy_override =
          fuchsia_fs_startup::wire::EvictionPolicyOverride::kEvictImmediately;
    } else if (eviction_policy.has_value()) {
      // An unrecognized eviction policy override was requested. Ignore it and continue.
      FX_LOGS(WARNING) << "Ignoring " << *eviction_policy << " policy";
    }
  }
  return options;
}

BlockDevice::BlockDevice(FilesystemMounter* mounter, fbl::unique_fd fd, const Config* device_config)
    : mounter_(mounter),
      fd_(std::move(fd)),
      device_config_(device_config),
      content_format_(fs_management::kDiskFormatUnknown),
      topological_path_(GetTopologicalPath(fd_.get())) {}

fs_management::DiskFormat BlockDevice::content_format() const {
  if (content_format_ != fs_management::kDiskFormatUnknown) {
    return content_format_;
  }
  content_format_ = fs_management::DetectDiskFormat(fd_.get());
  return content_format_;
}

fs_management::DiskFormat BlockDevice::GetFormat() { return format_; }

void BlockDevice::SetFormat(fs_management::DiskFormat format) { format_ = format; }

const std::string& BlockDevice::partition_name() const {
  if (!partition_name_.empty()) {
    return partition_name_;
  }
  // The block device might not support the partition protocol in which case the connection will be
  // closed, so clone the channel in case that happens.
  fdio_cpp::UnownedFdioCaller connection(fd_.get());
  fidl::ClientEnd<fuchsia_hardware_block_partition::Partition> channel(
      zx::channel(fdio_service_clone(connection.borrow_channel())));
  auto resp = fidl::BindSyncClient(std::move(channel))->GetName();
  if (resp.status() != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partiton name (fidl error): "
                   << zx_status_get_string(resp.status());
    return partition_name_;
  }
  if (resp->status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partiton name: " << zx_status_get_string(resp->status);
    return partition_name_;
  }
  partition_name_ = std::string(resp->name.data(), resp->name.size());
  return partition_name_;
}

zx_status_t BlockDevice::GetInfo(fuchsia_hardware_block_BlockInfo* out_info) const {
  if (info_.has_value()) {
    memcpy(out_info, &*info_, sizeof(*out_info));
    return ZX_OK;
  }
  fdio_cpp::UnownedFdioCaller connection(fd_.get());
  zx_status_t io_status, call_status;
  io_status =
      fuchsia_hardware_block_BlockGetInfo(connection.borrow_channel(), &call_status, out_info);
  if (io_status != ZX_OK) {
    return io_status;
  }
  info_ = *out_info;
  return call_status;
}

const fuchsia_hardware_block_partition::wire::Guid& BlockDevice::GetInstanceGuid() const {
  if (instance_guid_) {
    return *instance_guid_;
  }
  instance_guid_.emplace();
  fdio_cpp::UnownedFdioCaller connection(fd_.get());
  // The block device might not support the partition protocol in which case the connection will be
  // closed, so clone the channel in case that happens.
  auto response = fidl::WireCall(fidl::ClientEnd<fuchsia_hardware_block_partition::Partition>(
                                     zx::channel(fdio_service_clone(connection.borrow_channel()))))
                      ->GetInstanceGuid();
  if (response.status() != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partition instance GUID (fidl error: "
                   << zx_status_get_string(response.status()) << ")";
  } else if (response->status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partition instance GUID: "
                   << zx_status_get_string(response->status);
  } else {
    *instance_guid_ = *response->guid;
  }
  return *instance_guid_;
}

const fuchsia_hardware_block_partition::wire::Guid& BlockDevice::GetTypeGuid() const {
  if (type_guid_) {
    return *type_guid_;
  }
  type_guid_.emplace();
  fdio_cpp::UnownedFdioCaller connection(fd_.get());
  // The block device might not support the partition protocol in which case the connection will be
  // closed, so clone the channel in case that happens.
  auto response = fidl::WireCall(fidl::ClientEnd<fuchsia_hardware_block_partition::Partition>(
                                     zx::channel(fdio_service_clone(connection.borrow_channel()))))
                      ->GetTypeGuid();
  if (response.status() != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partition type GUID (fidl error: "
                   << zx_status_get_string(response.status()) << ")";
  } else if (response->status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to get partition type GUID: "
                   << zx_status_get_string(response->status);
  } else {
    *type_guid_ = *response->guid;
  }
  return *type_guid_;
}

zx_status_t BlockDevice::AttachDriver(const std::string_view& driver) {
  FX_LOGS(INFO) << "Binding: " << driver;
  fdio_cpp::UnownedFdioCaller connection(fd_.get());
  zx_status_t call_status = ZX_OK;
  auto resp =
      fidl::WireCall<fuchsia_device::Controller>(zx::unowned_channel(connection.borrow_channel()))
          ->Bind(::fidl::StringView::FromExternal(driver));
  zx_status_t io_status = resp.status();
  if (io_status != ZX_OK) {
    return io_status;
  }
  if (resp->result.is_err()) {
    call_status = resp->result.err();
  }
  return call_status;
}

zx_status_t BlockDevice::UnsealZxcrypt() {
  FX_LOGS(INFO) << "unsealing zxcrypt with UUID "
                << uuid::Uuid(GetInstanceGuid().value.data()).ToString();
  // Bind and unseal the driver from a separate thread, since we
  // have to wait for a number of devices to do I/O and settle,
  // and we don't want to block block-watcher for any nontrivial
  // length of time.

  // We transfer fd to the spawned thread.  Since it's UB to cast
  // ints to pointers and back, we allocate the fd on the heap.
  int loose_fd = fd_.release();
  int* raw_fd_ptr = new int(loose_fd);
  thrd_t th;
  int err = thrd_create_with_name(&th, &UnsealZxcryptThread, raw_fd_ptr, "zxcrypt-unseal");
  if (err != thrd_success) {
    FX_LOGS(ERROR) << "failed to spawn zxcrypt worker thread";
    close(loose_fd);
    delete raw_fd_ptr;
    return ZX_ERR_INTERNAL;
  } else {
    thrd_detach(th);
  }
  return ZX_OK;
}

zx_status_t BlockDevice::OpenBlockVerityForVerifiedRead(std::string seal_hex) {
  FX_LOGS(INFO) << "preparing block-verity";

  std::unique_ptr<VerityDeviceThreadState> state = std::make_unique<VerityDeviceThreadState>();
  zx_status_t rc = state->seal.Parse(seal_hex.c_str());
  if (rc != ZX_OK) {
    FX_LOGS(ERROR) << "block-verity seal " << seal_hex
                   << " did not parse as SHA256 hex digest: " << zx_status_get_string(rc);
    return rc;
  }

  // Transfer FD to thread state.
  state->fd = std::move(fd_);

  thrd_t th;
  int err = thrd_create_with_name(&th, OpenVerityDeviceThread, state.get(), "block-verity-open");
  if (err != thrd_success) {
    FX_LOGS(ERROR) << "failed to spawn block-verity worker thread";
    return ZX_ERR_INTERNAL;
  } else {
    // Release our reference to the state now owned by the other thread.
    state.release();
    thrd_detach(th);
  }

  return ZX_OK;
}

zx_status_t BlockDevice::FormatZxcrypt() {
  fbl::unique_fd devfs_root_fd(open("/dev", O_RDONLY));
  if (!devfs_root_fd) {
    return ZX_ERR_NOT_FOUND;
  }
  EncryptedVolume volume(fd_.duplicate(), std::move(devfs_root_fd));
  return volume.Format();
}

zx::status<std::string> BlockDevice::VeritySeal() {
  return mounter_->boot_args()->block_verity_seal();
}

bool BlockDevice::ShouldAllowAuthoringFactory() {
  // Checks for presence of /boot/config/allow-authoring-factory
  fbl::unique_fd allow_authoring_factory_fd(open(kAllowAuthoringFactoryConfigFile, O_RDONLY));
  return allow_authoring_factory_fd.is_valid();
}

zx_status_t BlockDevice::SetPartitionMaxSize(const std::string& fvm_path, uint64_t max_byte_size) {
  // Get the partition GUID for talking to FVM.
  const fuchsia_hardware_block_partition::wire::Guid& instance_guid = GetInstanceGuid();
  if (std::all_of(std::begin(instance_guid.value), std::end(instance_guid.value),
                  [](auto val) { return val == 0; }))
    return ZX_ERR_NOT_SUPPORTED;  // Not a partition, nothing to do.

  fbl::unique_fd fvm_fd(open(fvm_path.c_str(), O_RDONLY));
  if (!fvm_fd)
    return ZX_ERR_NOT_SUPPORTED;  // Not in FVM, nothing to do.
  fdio_cpp::UnownedFdioCaller fvm_caller(fvm_fd.get());

  // Get the FVM slice size.
  auto info_response =
      fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                         fvm_caller.borrow_channel()))
          ->GetInfo();
  if (info_response.status() != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to request FVM Info: "
                   << zx_status_get_string(info_response.status());
    return info_response.status();
  }
  if (info_response->status != ZX_OK || !info_response->info) {
    FX_LOGS(ERROR) << "FVM info request failed: " << zx_status_get_string(info_response->status);
    return info_response->status;
  }
  uint64_t slice_size = info_response->info->slice_size;

  // Set the limit (convert to slice units, rounding down).
  uint64_t max_slice_count = max_byte_size / slice_size;
  auto response =
      fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                         fvm_caller.borrow_channel()))
          ->SetPartitionLimit(instance_guid, max_slice_count);
  if (response.status() != ZX_OK || response->status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to set partition limit for " << topological_path() << " to "
                   << max_byte_size << " bytes (" << max_slice_count << " slices).";
    if (response.status() != ZX_OK) {
      FX_LOGS(ERROR) << "  FIDL error: " << zx_status_get_string(response.status());
      return response.status();
    }
    FX_LOGS(ERROR) << " FVM error: " << zx_status_get_string(response->status);
    return response->status;
  }

  return ZX_OK;
}

zx_status_t BlockDevice::SetPartitionName(const std::string& fvm_path, std::string_view name) {
  // Get the partition GUID for talking to FVM.
  const fuchsia_hardware_block_partition::wire::Guid& instance_guid = GetInstanceGuid();
  if (std::all_of(std::begin(instance_guid.value), std::end(instance_guid.value),
                  [](auto val) { return val == 0; }))
    return ZX_ERR_NOT_SUPPORTED;  // Not a partition, nothing to do.

  fbl::unique_fd fvm_fd(open(fvm_path.c_str(), O_RDONLY));
  if (!fvm_fd)
    return ZX_ERR_NOT_SUPPORTED;  // Not in FVM, nothing to do.

  // Actually set the name.
  fdio_cpp::UnownedFdioCaller caller(fvm_fd.get());
  auto response =
      fidl::WireCall(fidl::UnownedClientEnd<fuchsia_hardware_block_volume::VolumeManager>(
                         caller.borrow_channel()))
          ->SetPartitionName(instance_guid, fidl::StringView::FromExternal(name));
  if (response.status() != ZX_OK || response->result.is_err()) {
    FX_LOGS(ERROR) << "Unable to set partition name for " << topological_path() << " to '" << name
                   << "'.";
    if (response.status() != ZX_OK) {
      FX_LOGS(ERROR) << "  FIDL error: " << zx_status_get_string(response.status());
      return response.status();
    }
    FX_LOGS(ERROR) << " FVM error: " << zx_status_get_string(response->result.err());
    return response->result.err();
  }

  return ZX_OK;
}

bool BlockDevice::ShouldCheckFilesystems() { return mounter_->ShouldCheckFilesystems(); }

zx_status_t BlockDevice::CheckFilesystem() {
  if (!ShouldCheckFilesystems()) {
    return ZX_OK;
  }

  zx_status_t status;
  fuchsia_hardware_block_BlockInfo info;
  if ((status = GetInfo(&info)) != ZX_OK) {
    return status;
  }

  switch (format_) {
    case fs_management::kDiskFormatBlobfs: {
      FX_LOGS(INFO) << "Skipping blobfs consistency checker.";
      return ZX_OK;
    }

    case fs_management::kDiskFormatFactoryfs: {
      FX_LOGS(INFO) << "Skipping factory consistency checker.";
      return ZX_OK;
    }

    case fs_management::kDiskFormatMinfs: {
      zx::ticks before = zx::ticks::now();
      auto timer = fit::defer([before]() {
        auto after = zx::ticks::now();
        auto duration = fzl::TicksToNs(after - before);
        FX_LOGS(INFO) << "fsck took " << duration.to_secs() << "." << duration.to_msecs() % 1000
                      << " seconds";
      });
      FX_LOGS(INFO) << "fsck of data partition started";

      if (device_config_->is_set(Config::kDataFilesystemBinaryPath)) {
        std::string binary_path =
            device_config_->ReadStringOptionValue(Config::kDataFilesystemBinaryPath);
        FX_LOGS(INFO) << "Using " << binary_path;
        status = CheckCustomFilesystem(std::move(binary_path));
      } else {
        uint64_t device_size = info.block_size * info.block_count / minfs::kMinfsBlockSize;
        auto device_or = minfs::FdToBlockDevice(fd_);
        if (device_or.is_error()) {
          FX_LOGS(ERROR) << "Cannot convert fd to block device: " << device_or.error_value();
          return device_or.error_value();
        }
        auto bc_or =
            minfs::Bcache::Create(std::move(device_or.value()), static_cast<uint32_t>(device_size));
        if (bc_or.is_error()) {
          FX_LOGS(ERROR) << "Could not initialize minfs bcache.";
          return bc_or.error_value();
        }
        status = minfs::Fsck(std::move(bc_or.value()), minfs::FsckOptions{.repair = true})
                     .status_value();
      }

      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "\n--------------------------------------------------------------\n"
                          "|\n"
                          "|   WARNING: fshost fsck failure!\n"
                          "|   Corrupt "
                       << fs_management::DiskFormatString(format_)
                       << " filesystem\n"
                          "|\n"
                          "|   If your system was shutdown cleanly (via 'dm poweroff'\n"
                          "|   or an OTA), report this device to the local-storage\n"
                          "|   team. Please file bugs with logs before and after reboot.\n"
                          "|\n"
                          "--------------------------------------------------------------";
        MaybeDumpMetadata(fd_.duplicate(), {.disk_format = fs_management::kDiskFormatMinfs});
        mounter_->ReportMinfsCorruption();
      } else {
        FX_LOGS(INFO) << "fsck of " << fs_management::DiskFormatString(format_) << " completed OK";
      }
      return status;
    }
    default:
      FX_LOGS(ERROR) << "Not checking unknown filesystem";
      return ZX_ERR_NOT_SUPPORTED;
  }
}

zx_status_t BlockDevice::FormatFilesystem() {
  zx_status_t status;
  fuchsia_hardware_block_BlockInfo info;
  if ((status = GetInfo(&info)) != ZX_OK) {
    return status;
  }

  switch (format_) {
    case fs_management::kDiskFormatBlobfs: {
      FX_LOGS(ERROR) << "Not formatting blobfs.";
      return ZX_ERR_NOT_SUPPORTED;
    }
    case fs_management::kDiskFormatFactoryfs: {
      FX_LOGS(ERROR) << "Not formatting factoryfs.";
      return ZX_ERR_NOT_SUPPORTED;
    }
    case fs_management::kDiskFormatMinfs: {
      const auto binary_path =
          device_config_->ReadStringOptionValue(Config::kDataFilesystemBinaryPath);
      if (!binary_path.empty()) {
        FX_LOGS(INFO) << "Formatting using " << binary_path;
        status = FormatCustomFilesystem(binary_path);
        if (status != ZX_OK) {
          FX_LOGS(ERROR) << "Failed to format: " << zx_status_get_string(status);
          return status;
        }
      } else {
        FX_LOGS(INFO) << "Formatting minfs.";
        uint64_t blocks = info.block_size * info.block_count / minfs::kMinfsBlockSize;
        auto device_or = minfs::FdToBlockDevice(fd_);
        if (device_or.is_error()) {
          FX_LOGS(ERROR) << "Cannot convert fd to block device: " << device_or.error_value();
          return status;
        }
        auto bc_or =
            minfs::Bcache::Create(std::move(device_or.value()), static_cast<uint32_t>(blocks));
        if (bc_or.is_error()) {
          FX_LOGS(ERROR) << "Could not initialize minfs bcache.";
          return bc_or.error_value();
        }
        minfs::MountOptions options = {};
        if (status = minfs::Mkfs(options, bc_or.value().get()).status_value(); status != ZX_OK) {
          FX_LOGS(ERROR) << "Could not format minfs filesystem.";
          return status;
        }
        FX_LOGS(INFO) << "Minfs filesystem re-formatted. Expect data loss.";
      }
      return ZX_OK;
    }
    default:
      FX_LOGS(ERROR) << "Not formatting unknown filesystem.";
      return ZX_ERR_NOT_SUPPORTED;
  }
}

zx_status_t BlockDevice::MountFilesystem() {
  if (!fd_) {
    return ZX_ERR_BAD_HANDLE;
  }
  zx::channel block_device;
  {
    fdio_cpp::UnownedFdioCaller disk_connection(fd_.get());
    zx::unowned_channel channel(disk_connection.borrow_channel());
    block_device.reset(fdio_service_clone(channel->get()));
  }
  switch (format_) {
    case fs_management::kDiskFormatFactoryfs: {
      FX_LOGS(INFO) << "BlockDevice::MountFilesystem(factoryfs)";
      fs_management::MountOptions options;
      options.collect_metrics = false;
      options.readonly = true;

      zx_status_t status = mounter_->MountFactoryFs(std::move(block_device), options);
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to mount factoryfs partition: " << zx_status_get_string(status)
                       << ".";
      }
      return status;
    }
    case fs_management::kDiskFormatBlobfs: {
      FX_LOGS(INFO) << "BlockDevice::MountFilesystem(blobfs)";
      zx_status_t status = mounter_->MountBlob(
          std::move(block_device), GetBlobfsStartOptions(device_config_, mounter_->boot_args()));
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to mount blobfs partition: " << zx_status_get_string(status)
                       << ".";
        return status;
      }
      mounter_->TryMountPkgfs();
      return ZX_OK;
    }
    case fs_management::kDiskFormatFxfs:
    case fs_management::kDiskFormatF2fs:
    case fs_management::kDiskFormatMinfs: {
      fs_management::MountOptions options;
      FX_LOGS(INFO) << "BlockDevice::MountFilesystem(data partition)";
      zx_status_t status = MountData(&options, std::move(block_device));
      if (status != ZX_OK) {
        FX_LOGS(ERROR) << "Failed to mount data partition: " << zx_status_get_string(status) << ".";
        MaybeDumpMetadata(fd_.duplicate(), {.disk_format = fs_management::kDiskFormatMinfs});
        return status;
      }
      mounter_->TryMountPkgfs();
      return ZX_OK;
    }
    default:
      FX_LOGS(ERROR) << "BlockDevice::MountFilesystem(unknown)";
      return ZX_ERR_NOT_SUPPORTED;
  }
}

zx_status_t BlockDevice::MaybeChangeDataPartitionFormat() const {
  auto endpoint_or = GetDeviceEndPoint();
  if (endpoint_or.is_error()) {
    FX_LOGS(ERROR) << "Failed to get device endpoint: " << endpoint_or.error_value();
    return ZX_ERR_BAD_STATE;
  }
  fbl::Vector<const char*> argv = {kMinfsPath, "mount", nullptr};
  auto export_root_or = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (export_root_or.is_error()) {
    FX_LOGS(ERROR) << "Failed to create endpoints.";
    return ZX_ERR_BAD_STATE;
  }
  if (zx_status_t status =
          RunBinary(argv, endpoint_or->TakeChannel(), std::move(export_root_or->server));
      status != ZX_OK) {
    // Device might not be minfs. That's ok.
    return ZX_ERR_BAD_STATE;
  }

  zx::status<fidl::ClientEnd<fuchsia_io::Directory>> root_dir_or =
      fs_management::FsRootHandle(export_root_or->client);
  if (root_dir_or.is_error()) {
    FX_LOGS(ERROR) << "Failed to get root handle: " << root_dir_or.error_value();
    return ZX_ERR_BAD_STATE;
  }

  fbl::unique_fd fd;
  if (zx_status_t status =
          fdio_fd_create(root_dir_or->TakeChannel().release(), fd.reset_and_get_address());
      status != ZX_OK) {
    FX_LOGS(ERROR) << "fdio_fd_create failed";
    return ZX_ERR_BAD_STATE;
  }

  std::string binary_path;
  std::string fmt_str;
  if (files::ReadFileToStringAt(fd.get(), "fs_switch", &fmt_str)) {
    if (fmt_str.back() == '\n') {
      fmt_str.pop_back();
    }
    if (fmt_str == "fxfs") {
      binary_path = kFxfsPath;
    } else if (fmt_str == "f2fs") {
      binary_path = kF2fsPath;
    } else if (fmt_str == "minfs") {
      binary_path = kMinfsPath;
    }
  }

  if (fs_management::Shutdown(export_root_or->client).is_error()) {
    return ZX_ERR_BAD_STATE;
  }
  if (!binary_path.empty()) {
    return FormatCustomFilesystem(binary_path);
  }
  return ZX_ERR_BAD_STATE;
}

// Attempt to mount the device at a known location.
//
// Returns ZX_ERR_ALREADY_BOUND if the device could be mounted, but something
// is already mounted at that location. Returns ZX_ERR_INVALID_ARGS if the
// GUID of the device does not match a known valid one. Returns
// ZX_ERR_NOT_SUPPORTED if the GUID is a system GUID. Returns ZX_OK if an
// attempt to mount is made, without checking mount success.
zx_status_t BlockDevice::MountData(fs_management::MountOptions* options, zx::channel block_device) {
  const uint8_t* guid = GetTypeGuid().value.data();

  if (gpt_is_sys_guid(guid, GPT_GUID_LEN)) {
    return ZX_ERR_NOT_SUPPORTED;
  } else if (gpt_is_data_guid(guid, GPT_GUID_LEN)) {
    if (device_config_->is_set(Config::kFsSwitch) &&
        content_format() == fs_management::kDiskFormatMinfs) {
      MaybeChangeDataPartitionFormat();
    }
    return mounter_->MountData(std::move(block_device), *options, content_format());
  } else if (gpt_is_install_guid(guid, GPT_GUID_LEN)) {
    options->readonly = true;
    return mounter_->MountInstall(std::move(block_device), *options);
  } else if (gpt_is_durable_guid(guid, GPT_GUID_LEN)) {
    return mounter_->MountDurable(std::move(block_device), *options);
  }
  FX_LOGS(ERROR) << "Unrecognized partition GUID for data partition; not mounting";
  return ZX_ERR_WRONG_TYPE;
}

zx_status_t BlockDeviceInterface::Add(bool format_on_corruption) {
  switch (GetFormat()) {
    case fs_management::kDiskFormatNandBroker: {
      return AttachDriver(kNandBrokerDriverPath);
    }
    case fs_management::kDiskFormatBootpart: {
      return AttachDriver(kBootpartDriverPath);
    }
    case fs_management::kDiskFormatGpt: {
      return AttachDriver(kGPTDriverPath);
    }
    case fs_management::kDiskFormatFvm: {
      return AttachDriver(kFVMDriverPath);
    }
    case fs_management::kDiskFormatMbr: {
      return AttachDriver(kMBRDriverPath);
    }
    case fs_management::kDiskFormatBlockVerity: {
      if (zx_status_t status = AttachDriver(kBlockVerityDriverPath); status != ZX_OK) {
        return status;
      }

      if (!ShouldAllowAuthoringFactory()) {
        zx::status<std::string> seal_text = VeritySeal();
        if (seal_text.is_error()) {
          FX_LOGS(ERROR) << "Couldn't get block-verity seal: " << seal_text.status_string();
          return seal_text.error_value();
        }

        return OpenBlockVerityForVerifiedRead(seal_text.value());
      }

      return ZX_OK;
    }
    case fs_management::kDiskFormatFactoryfs: {
      if (zx_status_t status = CheckFilesystem(); status != ZX_OK) {
        return status;
      }

      return MountFilesystem();
    }
    case fs_management::kDiskFormatZxcrypt: {
      return UnsealZxcrypt();
    }
    case fs_management::kDiskFormatBlobfs: {
      if (zx_status_t status = CheckFilesystem(); status != ZX_OK) {
        return status;
      }
      return MountFilesystem();
    }
    case fs_management::kDiskFormatMinfs: {
      FX_LOGS(INFO) << "mounting data partition: format on corruption is "
                    << (format_on_corruption ? "enabled" : "disabled");
      if (zx_status_t status = CheckFilesystem(); status != ZX_OK) {
        if (!format_on_corruption) {
          FX_LOGS(INFO) << "formatting data partition on this target is disabled";
          return status;
        }
        if (zx_status_t status = FormatFilesystem(); status != ZX_OK) {
          return status;
        }
      }
      if (zx_status_t status = MountFilesystem(); status != ZX_OK) {
        FX_LOGS(ERROR) << "failed to mount filesystem: " << zx_status_get_string(status);
        if (!format_on_corruption) {
          FX_LOGS(ERROR) << "formatting minfs on this target is disabled";
          return status;
        }
        if ((status = FormatFilesystem()) != ZX_OK) {
          return status;
        }
        return MountFilesystem();
      }
      return ZX_OK;
    }
    case fs_management::kDiskFormatFat:
    case fs_management::kDiskFormatVbmeta:
    case fs_management::kDiskFormatUnknown:
    case fs_management::kDiskFormatFxfs:
    case fs_management::kDiskFormatF2fs:
    case fs_management::kDiskFormatCount:
      return ZX_ERR_NOT_SUPPORTED;
  }
  return ZX_ERR_NOT_SUPPORTED;
}

// Clones the device handle.
zx::status<fidl::ClientEnd<fuchsia_io::Node>> BlockDevice::GetDeviceEndPoint() const {
  auto end_points_or = fidl::CreateEndpoints<fuchsia_io::Node>();
  if (end_points_or.is_error())
    return end_points_or.take_error();

  fdio_cpp::UnownedFdioCaller caller(fd_);
  if (zx_status_t status =
          fidl::WireCall<fuchsia_io::Node>(zx::unowned_channel(caller.borrow_channel()))
              ->Clone(fuchsia_io::wire::kCloneFlagSameRights, std::move(end_points_or->server))
              .status();
      status != ZX_OK) {
    return zx::error(status);
  }

  return zx::ok(std::move(end_points_or->client));
}

zx_status_t BlockDevice::CheckCustomFilesystem(const std::string& binary_path) const {
  fbl::Vector<const char*> argv;
  argv.push_back(binary_path.c_str());
  argv.push_back("fsck");
  argv.push_back(nullptr);
  auto device_or = GetDeviceEndPoint();
  if (device_or.is_error()) {
    return device_or.error_value();
  }
  auto crypt_client_or = mounter_->GetCryptClient();
  if (crypt_client_or.is_error())
    return crypt_client_or.error_value();
  return RunBinary(argv, std::move(device_or).value(), {}, *std::move(crypt_client_or));
}

// This is a destructive operation and isn't atomic (i.e. not resilient to power interruption).
zx_status_t BlockDevice::FormatCustomFilesystem(const std::string& binary_path) const {
  // Try mounting minfs and slurp all existing data off.
  zx_handle_t handle;
  if (zx_status_t status = fdio_fd_clone(fd_.get(), &handle); status != ZX_OK)
    return status;
  fbl::unique_fd fd;
  if (zx_status_t status = fdio_fd_create(handle, fd.reset_and_get_address()); status != ZX_OK)
    return status;

  Copier copier;
  {
    auto device_or = GetDeviceEndPoint();
    if (device_or.is_error())
      return device_or.error_value();
    copier = TryReadingMinfs(std::move(device_or).value());
  }

  fidl::ClientEnd<fuchsia_io::Node> device;
  if (auto device_or = GetDeviceEndPoint(); device_or.is_error()) {
    return device_or.error_value();
  } else {
    device = std::move(device_or).value();
  }

  fidl::UnownedClientEnd<fuchsia_hardware_block_volume::Volume> volume_client(
      device.channel().borrow());

  auto query_result = fidl::WireCall(volume_client)->GetVolumeInfo();
  if (query_result.status() != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to query FVM information: "
                   << zx_status_get_string(query_result.status());
    return query_result.status();
  }

  auto query_response = query_result.Unwrap();
  if (query_response->status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to query FVM information: "
                   << zx_status_get_string(query_response->status);
    return query_response->status;
  }

  const uint64_t slice_size = query_response->manager->slice_size;

  // Free all the existing slices.
  uint64_t slice = 1;
  // The -1 here is because of zxcrypt; zxcrypt will offset all slices by 1 to account for its
  // header.  zxcrypt isn't present in all cases, but that won't matter since minfs shouldn't be
  // using a slice so high.
  while (slice < fvm::kMaxVSlices - 1) {
    auto query_result = fidl::WireCall(volume_client)
                            ->QuerySlices(fidl::VectorView<uint64_t>::FromExternal(&slice, 1));
    if (query_result.status() != ZX_OK) {
      FX_LOGS(ERROR) << "Unable to query slices (slice: " << slice << ", max: " << fvm::kMaxVSlices
                     << "): " << zx_status_get_string(query_result.status());
      return query_result.status();
    }

    auto query_response = query_result.Unwrap();
    if (query_response->status != ZX_OK) {
      FX_LOGS(ERROR) << "Unable to query slices (slice: " << slice << ", max: " << fvm::kMaxVSlices
                     << "): " << zx_status_get_string(query_response->status);
      return query_response->status;
    }

    if (query_response->response_count == 0) {
      break;
    }

    for (uint64_t i = 0; i < query_response->response_count; ++i) {
      if (query_response->response[i].allocated) {
        auto shrink_result =
            fidl::WireCall(volume_client)->Shrink(slice, query_response->response[i].count);
        if (zx_status_t status = shrink_result.status() == ZX_OK ? shrink_result.Unwrap()->status
                                                                 : shrink_result.status();
            status != ZX_OK) {
          FX_LOGS(ERROR) << "Unable to shrink partition: " << zx_status_get_string(status);
          return status;
        }
      }
      slice += query_response->response[i].count;
    }
  }

  uint64_t slice_count =
      device_config_->ReadUint64OptionValue(Config::kMinfsMaxBytes, 0) / slice_size;

  if (slice_count == 0) {
    auto query_result = fidl::WireCall(volume_client)->GetVolumeInfo();
    if (query_result.status() != ZX_OK)
      return query_result.status();
    const auto* response = query_result.Unwrap();
    if (response->status != ZX_OK)
      return response->status;
    // If a size is not specified, limit the size of the data partition so as not to use up all
    // FVM's space (thus limiting blobfs growth).  10% or 24MiB (whichever is larger) should be
    // enough.
    // Due to reserved and over-provisoned area of f2fs, it needs volume size at least 100 MiB.
    const uint64_t slices_available =
        response->manager->slice_count - response->manager->assigned_slice_count;
    const uint64_t min_slices =
        binary_path == kF2fsPath ? fbl::round_up(kDefaultF2fsMinBytes, slice_size) / slice_size : 2;
    if (slices_available < min_slices) {
      FX_LOGS(ERROR) << "Not enough space for " << binary_path << " partition";
      return ZX_ERR_NO_SPACE;
    }
    uint64_t slice_target = kDefaultMinfsMaxBytes;
    if (binary_path == kF2fsPath) {
      slice_target = kDefaultF2fsMinBytes;
    }
    if (slices_available < slice_target) {
      FX_LOGS(WARNING) << "Only " << slices_available << " slices available for " << binary_path
                       << " partition; some functionality may be missing.";
    }
    slice_count = std::min(slices_available, std::max<uint64_t>(response->manager->slice_count / 10,
                                                                slice_target / slice_size));
  }
  // Account for the slice zxcrypt uses.
  --slice_count;
  FX_LOGS(INFO) << "Allocating " << slice_count << " slices (" << slice_count * slice_size
                << " bytes) for " << binary_path << " partition";

  auto extend_result =
      fidl::WireCall(volume_client)
          ->Extend(1, slice_count - 1);  // Another -1 here because we get the first slice for free.
  if (zx_status_t status =
          extend_result.status() == ZX_OK ? extend_result.Unwrap()->status : extend_result.status();
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to extend partition (slice_count: " << slice_count
                   << "): " << zx_status_get_string(status);
    return status;
  }

  fbl::Vector<const char*> argv = {binary_path.c_str(), "mkfs", nullptr};

  auto crypt_client_or = mounter_->GetCryptClient();
  crypt_client_or = mounter_->GetCryptClient();
  if (crypt_client_or.is_error())
    return crypt_client_or.error_value();
  if (zx_status_t status = RunBinary(argv, std::move(device), {}, *std::move(crypt_client_or));
      status != ZX_OK) {
    return status;
  }

  // Now mount and then copy all the data back.
  if (zx_status_t status = fdio_fd_clone(fd_.get(), &handle); status != ZX_OK) {
    FX_LOGS(ERROR) << "fdio_fd_clone failed";
    return status;
  }
  if (zx_status_t status = fdio_fd_create(handle, fd.reset_and_get_address()); status != ZX_OK)
    return status;

  auto export_root_or = fidl::CreateEndpoints<fuchsia_io::Directory>();
  if (export_root_or.is_error())
    return export_root_or.error_value();

  argv[1] = "mount";
  auto device_or = GetDeviceEndPoint();
  if (device_or.is_error()) {
    return device_or.error_value();
  }
  crypt_client_or = mounter_->GetCryptClient();
  if (crypt_client_or.is_error())
    return crypt_client_or.error_value();
  if (zx_status_t status =
          RunBinary(argv, std::move(device_or).value(), std::move(export_root_or->server),
                    *std::move(crypt_client_or));
      status != ZX_OK) {
    FX_LOGS(ERROR) << "Unable to mount after format";
    return status;
  }

  zx::channel root_client, root_server;
  if (zx_status_t status = zx::channel::create(0, &root_client, &root_server); status != ZX_OK)
    return status;

  if (auto resp = fidl::WireCall(export_root_or->client)
                      ->Open(fuchsia_io::wire::kOpenRightReadable |
                                 fuchsia_io::wire::kOpenFlagPosixWritable |
                                 fuchsia_io::wire::kOpenFlagPosixExecutable,
                             0, fidl::StringView("root"), std::move(root_server));
      !resp.ok()) {
    return resp.status();
  }

  if (zx_status_t status = fdio_fd_create(root_client.release(), fd.reset_and_get_address());
      status != ZX_OK) {
    FX_LOGS(ERROR) << "fdio_fd_create failed";
    return status;
  }

  if (zx_status_t status = copier.Write(std::move(fd)); status != ZX_OK) {
    FX_LOGS(ERROR) << "Failed to copy data";
    return status;
  }

  if (auto status = fs_management::Shutdown(export_root_or->client); status.is_error()) {
    // Ignore errors; there's nothing we can do.
    FX_LOGS(WARNING) << "Unmount failed: " << status.status_string();
  }

  content_format_ = fs_management::kDiskFormatUnknown;

  return ZX_OK;
}

}  // namespace fshost
