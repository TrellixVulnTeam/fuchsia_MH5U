// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// This file contains functionality for checking the consistency of Blobfs.

#ifndef SRC_STORAGE_BLOBFS_FSCK_H_
#define SRC_STORAGE_BLOBFS_FSCK_H_

#include "src/lib/storage/block_client/cpp/block_device.h"
#include "src/storage/blobfs/mount.h"

namespace blobfs {

zx_status_t Fsck(std::unique_ptr<block_client::BlockDevice> device, const MountOptions& options);

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_FSCK_H_
