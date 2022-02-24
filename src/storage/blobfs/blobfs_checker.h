// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_BLOBFS_BLOBFS_CHECKER_H_
#define SRC_STORAGE_BLOBFS_BLOBFS_CHECKER_H_

#ifdef __Fuchsia__
#include "src/storage/blobfs/blobfs.h"
#else
#include "src/storage/blobfs/host.h"
#endif

#include <memory>

namespace blobfs {

class BlobfsChecker {
 public:
  struct Options {
    // If true, repair simple issues.
    bool repair = true;

    // If strict is true, perform checks on things like reserved fields which should be zeroed when
    // formatted, but might be non-zero if a future version of Blobfs has touched the filesystem in
    // a compatible way.
    bool strict = false;
  };

  // The Blobfs pointer must outlive this class.
  explicit BlobfsChecker(Blobfs* blobfs) : BlobfsChecker(blobfs, {}) {}
  explicit BlobfsChecker(Blobfs* blobfs, Options option);

  BlobfsChecker(const BlobfsChecker&) = delete;
  BlobfsChecker& operator=(const BlobfsChecker&) = delete;

  // Check validates the blobfs filesystem provided when the Checker was constructed. It walks each
  // of the inode and block allocation bitmaps only once.
  //
  // Returns true if the filesystem is valid.
  bool Check();

 private:
  Blobfs* blobfs_;  // Non-owning.
  uint32_t alloc_inodes_ = 0;
  uint32_t alloc_blocks_ = 0;
  uint32_t inode_blocks_ = 0;
  const Options options_;

  bool CheckBackupSuperblock();
  bool TraverseInodeBitmap();
  bool TraverseBlockBitmap();
  bool CheckAllocatedCounts() const;
};

#ifdef __Fuchsia__
// Validate that the contents of the superblock matches the results claimed in the underlying
// volume manager.
//
// If the results are inconsistent, update the FVM's allocation accordingly.
zx_status_t CheckFvmConsistency(const Superblock* info, BlockDevice* device, bool repair);
#endif  // __Fuchsia__

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_BLOBFS_CHECKER_H_
