// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/shell/mirror/test_shared.h"

#include <fstream>

#include <gtest/gtest.h>

#include "src/developer/shell/mirror/wire_format.h"

namespace shell::mirror {

void FileRepo::InitMemRepo(std::string path) {
  path_ = path;
  ASSERT_EQ(loop_.StartThread(), ZX_OK);
  ASSERT_EQ(ZX_OK, memfs_install_at(loop_.dispatcher(), path.c_str(), &fs_));
}

FileRepo::~FileRepo() {
  sync_completion_t unmounted;
  memfs_free_filesystem(fs_, &unmounted);
  sync_completion_wait(&unmounted, zx::duration::infinite().get());

  loop_.Shutdown();
}

void FileRepo::WriteFiles(const std::vector<std::pair<std::string, std::string>>& golden) {
  for (const auto& gold : golden) {
    std::ofstream fout(gold.first, std::ios::out | std::ios::binary);
    fout << gold.second;
    fout.close();
  }
}

}  // namespace shell::mirror
