// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LIB_PROCFS_CPP_INTERNAL_CWD_H_
#define LIB_PROCFS_CPP_INTERNAL_CWD_H_

#include <lib/vfs/cpp/internal/node.h>

#include <memory>

namespace procfs {
namespace internal {

std::unique_ptr<vfs::internal::Node> CreateCwd();

}  // namespace internal
}  // namespace procfs

#endif  // LIB_PROCFS_CPP_INTERNAL_CWD_H_
