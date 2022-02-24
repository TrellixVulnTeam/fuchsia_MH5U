// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fcntl.h>
#include <getopt.h>
#include <lib/fdio/fd.h>
#include <lib/syslog/cpp/macros.h>
#include <lib/zx/channel.h>

#include <fbl/unique_fd.h>

#include "src/lib/digest/digest.h"
#include "src/lib/storage/block_client/cpp/remote_block_device.h"
#include "src/lib/storage/vfs/cpp/vfs.h"
#include "src/storage/tools/blobfs-corrupt/corrupt_blob.h"
#include "src/storage/tools/blobfs-corrupt/fs_block_client.h"

using block_client::BlockDevice;
using block_client::RemoteBlockDevice;

namespace {

constexpr char kUsage[] = R"(
Usage: blobfs-corrupt [ <options>* ]

options: (-d|--device) DEVICE    The path to the block device
         (-m|--merkle) MERKLE    The blob identity to corrupt

Given the path to a blobfs block device and a merkle root, this tool corrupts the data contents
of the blob so that it cannot be read when blobfs is mounted.

)";

zx_status_t Usage() {
  fprintf(stderr, kUsage);
  return ZX_ERR_INVALID_ARGS;
}

zx_status_t ProcessArgs(int argc, char** argv, zx::channel* block_channel,
                        BlobCorruptOptions* options) {
  char* arg_block_path = nullptr;
  char* arg_merkle = nullptr;

  while (1) {
    static struct option opts[] = {
        {"device", required_argument, nullptr, 'd'},
        {"merkle", required_argument, nullptr, 'm'},
    };

    int opt_index;
    int c = getopt_long(argc, argv, "d:m:", opts, &opt_index);

    if (c < 0) {
      break;
    }

    switch (c) {
      case 'd':
        arg_block_path = optarg;
        break;
      case 'm':
        arg_merkle = optarg;
        break;
      default:
        return Usage();
    }
  }

  if (arg_block_path == nullptr) {
    FX_LOGS(ERROR) << "'-d <device_path>' is required";
    return Usage();
  }

  if (arg_merkle == nullptr) {
    FX_LOGS(ERROR) << "'-m <merkle>' is required";
    return Usage();
  }

  zx_status_t status = options->merkle.Parse(arg_merkle);

  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "invalid merkle root: '" << arg_merkle << "'";
    return Usage();
  }

  fbl::unique_fd block_fd(open(arg_block_path, O_RDWR));

  if (!block_fd) {
    FX_LOGS(ERROR) << "unable to open block device: '" << arg_block_path << "' " << errno;
    return Usage();
  }

  status = fdio_fd_transfer(block_fd.release(), block_channel->reset_and_get_address());
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "unable to open block device: " << status;
    return Usage();
  }

  return ZX_OK;
}

}  // namespace

int main(int argc, char** argv) {
  BlobCorruptOptions options;
  zx::channel block_connection;
  zx_status_t status = ProcessArgs(argc, argv, &block_connection, &options);
  if (status != ZX_OK) {
    return -1;
  }

  std::unique_ptr<RemoteBlockDevice> device;
  status = RemoteBlockDevice::Create(std::move(block_connection), &device);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not initialize block device";
    return -1;
  }

  status = CorruptBlob(std::move(device), &options);
  if (status != ZX_OK) {
    FX_LOGS(ERROR) << "Could not corrupt the requested blob. Failed with error " << status;
    return -1;
  }
  return 0;
}
