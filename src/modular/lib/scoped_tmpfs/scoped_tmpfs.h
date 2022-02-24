// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_MODULAR_LIB_SCOPED_TMPFS_SCOPED_TMPFS_H_
#define SRC_MODULAR_LIB_SCOPED_TMPFS_SCOPED_TMPFS_H_

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>

#include <fbl/unique_fd.h>

#include "src/storage/memfs/scoped_memfs.h"

namespace scoped_tmpfs {

// Scoped temporary filesystem that will be destroyed when this class is
// deleted. The filesystem is not mounted on the process namespace and should be
// accessed via the root file descriptor.
class ScopedTmpFS {
 public:
  ScopedTmpFS();
  ScopedTmpFS(const ScopedTmpFS&) = delete;
  ScopedTmpFS& operator=(const ScopedTmpFS&) = delete;
  ~ScopedTmpFS();

  int root_fd() { return root_fd_.get(); }

 private:
  async_loop_config_t config_;
  async::Loop loop_;
  ScopedMemfs memfs_;
  fbl::unique_fd root_fd_;
};

}  // namespace scoped_tmpfs

#endif  // SRC_MODULAR_LIB_SCOPED_TMPFS_SCOPED_TMPFS_H_
