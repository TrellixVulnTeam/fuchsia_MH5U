// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FSHOST_FSHOST_BOOT_ARGS_H_
#define SRC_STORAGE_FSHOST_FSHOST_BOOT_ARGS_H_

#include <fidl/fuchsia.boot/cpp/wire.h>
#include <lib/zx/channel.h>
#include <lib/zx/status.h>
#include <zircon/status.h>
#include <zircon/types.h>

#include <map>
#include <optional>

#include <fbl/unique_fd.h>

namespace fshost {

class FshostBootArgs {
 public:
  // Create an FshostBootArgs object by attempting to connect to fuchsia.boot.Arguments through the
  // namespace. If the service connection fails, this creates an object that returns default values.
  // TODO: This probably shouldn't automatically fall back to defaults just to accomodate test
  // environments. The test environment should provide the services fshost needs, faking if needed.
  static std::shared_ptr<FshostBootArgs> Create();

  // Constructor for FshostBootArgs that allows injecting a different BootArgs member. Intended for
  // use in unit tests; use Create for non-test code.
  explicit FshostBootArgs(fidl::WireSyncClient<fuchsia_boot::Arguments> boot_args);

  bool netboot() const { return netsvc_netboot_ || zircon_system_disable_automount_; }
  bool check_filesystems() const { return zircon_system_filesystem_check_; }
  bool wait_for_data() const { return zircon_system_wait_for_data_; }

  zx::status<std::string> pkgfs_cmd();
  zx::status<std::string> pkgfs_file_with_path(std::string path);

  // Returns the write compression algorithm to pass to blobfs (via the --compression flag).
  std::optional<std::string> blobfs_write_compression_algorithm() const {
    return blobfs_write_compression_algorithm_;
  }

  // The seal of the factory partition, required for opening the block device for verified read.
  zx::status<std::string> block_verity_seal();

  // Returns the eviction policy to pass to blobfs (via the --eviction_policy flag).
  std::optional<std::string> blobfs_eviction_policy() const { return blobfs_eviction_policy_; }

 protected:
 private:
  zx::status<std::string> GetStringArgument(std::string key);

  fidl::WireSyncClient<fuchsia_boot::Arguments> boot_args_;
  bool netsvc_netboot_ = false;
  bool zircon_system_disable_automount_ = false;
  bool zircon_system_filesystem_check_ = false;
  bool zircon_system_wait_for_data_ = true;
  std::optional<std::string> blobfs_write_compression_algorithm_;
  std::optional<std::string> blobfs_eviction_policy_;
};

}  // namespace fshost

#endif  // SRC_STORAGE_FSHOST_FSHOST_BOOT_ARGS_H_
