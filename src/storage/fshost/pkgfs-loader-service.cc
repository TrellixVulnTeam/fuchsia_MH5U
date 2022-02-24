// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/fshost/pkgfs-loader-service.h"

#include <fidl/fuchsia.io/cpp/wire.h>
#include <lib/async-loop/default.h>
#include <lib/fdio/directory.h>
#include <lib/fdio/io.h>
#include <lib/syslog/cpp/macros.h>

#include "src/lib/files/path.h"

namespace fio = fuchsia_io;

namespace fshost {

// static
std::shared_ptr<PkgfsLoaderService> PkgfsLoaderService::Create(
    fbl::unique_fd blob_dir, std::shared_ptr<FshostBootArgs> boot_args) {
  auto loop = std::make_unique<async::Loop>(&kAsyncLoopConfigNoAttachToCurrentThread);
  loop->StartThread("pkgfs_loader");

  // Can't use make_shared because constructor is private
  return std::shared_ptr<PkgfsLoaderService>(
      new PkgfsLoaderService(std::move(loop), std::move(blob_dir), std::move(boot_args)));
}

zx::status<zx::vmo> PkgfsLoaderService::LoadObjectImpl(std::string path) {
  return LoadPkgfsFile(files::JoinPath("lib", path));
}

zx::status<zx::vmo> PkgfsLoaderService::LoadPkgfsFile(std::string path) {
  auto merkleroot = boot_args_->pkgfs_file_with_path(path);
  if (merkleroot.is_error()) {
    FX_LOGS(ERROR) << "failed to find pkgfs file merkleroot in boot arguments \"" << path << "\"";
    return merkleroot.take_error();
  }

  auto vmo = LoadBlob(merkleroot.value());
  if (vmo.is_error()) {
    FX_LOGS(ERROR) << "failed to load pkgfs file \"" << path << "\": " << vmo.status_string();
    return vmo.take_error();
  }

  auto status = zx::make_status(vmo->set_property(ZX_PROP_NAME, path.c_str(), path.length()));
  if (status.is_error()) {
    FX_LOGS(ERROR) << "failed to set vmo name to " << path << ": " << status.status_string();
    return status.take_error();
  }
  return vmo;
}

zx::status<zx::vmo> PkgfsLoaderService::LoadBlob(std::string merkleroot) {
  const uint32_t kFlags = fio::wire::kOpenFlagNotDirectory | fio::wire::kOpenRightReadable |
                          fio::wire::kOpenRightExecutable;

  fbl::unique_fd fd;
  zx_status_t status =
      fdio_open_fd_at(blob_dir_.get(), merkleroot.data(), kFlags, fd.reset_and_get_address());
  if (status != ZX_OK) {
    return zx::error(status);
  }

  zx::vmo vmo;
  status = fdio_get_vmo_exec(fd.get(), vmo.reset_and_get_address());
  if (status != ZX_OK) {
    return zx::error(status);
  }
  return zx::ok(std::move(vmo));
}

}  // namespace fshost
