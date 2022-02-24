// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/macros.h>

#include <cstddef>
#include <vector>

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/f2fs/f2fs.h"
#include "unit_lib.h"

namespace f2fs {
namespace {

constexpr uint32_t kOrphanCnt = 10;

TEST(OrphanInode, RecoverOrphanInode) {
  std::unique_ptr<Bcache> bc;
  FileTester::MkfsOnFakeDev(&bc);

  std::unique_ptr<F2fs> fs;
  MountOptions options{};
  async::Loop loop(&kAsyncLoopConfigAttachToCurrentThread);
  FileTester::MountWithOptions(loop.dispatcher(), options, &bc, &fs);

  fbl::RefPtr<VnodeF2fs> root;
  FileTester::CreateRoot(fs.get(), &root);
  fbl::RefPtr<Dir> root_dir = fbl::RefPtr<Dir>::Downcast(std::move(root));

  ASSERT_FALSE(fs->GetSuperblockInfo().GetCheckpoint().ckpt_flags & kCpOrphanPresentFlag);

  // 1. Create files
  std::vector<fbl::RefPtr<VnodeF2fs>> vnodes;
  std::vector<uint32_t> inos;

  ASSERT_EQ(fs->ValidInodeCount(), static_cast<uint64_t>(1));
  ASSERT_EQ(fs->ValidNodeCount(), static_cast<uint64_t>(1));
  ASSERT_EQ(fs->ValidUserBlocks(), static_cast<uint64_t>(2));

  FileTester::CreateChildren(fs.get(), vnodes, inos, root_dir, "orphan_", kOrphanCnt);
  ASSERT_EQ(vnodes.size(), kOrphanCnt);
  ASSERT_EQ(inos.size(), kOrphanCnt);

  ASSERT_EQ(fs->ValidInodeCount(), static_cast<uint64_t>(kOrphanCnt + 1));
  ASSERT_EQ(fs->ValidNodeCount(), static_cast<uint64_t>(kOrphanCnt + 1));
  ASSERT_EQ(fs->ValidUserBlocks(), static_cast<uint64_t>(kOrphanCnt + 2));

  for (const auto &iter : vnodes) {
    ASSERT_EQ(iter->GetNlink(), static_cast<uint32_t>(1));
  }

  // 2. Make orphan inodes
  ASSERT_EQ(fs->GetSuperblockInfo().GetOrphanCount(), static_cast<uint64_t>(0));
  FileTester::DeleteChildren(vnodes, root_dir, kOrphanCnt);
  ASSERT_EQ(fs->GetSuperblockInfo().GetOrphanCount(), kOrphanCnt);

  for (const auto &iter : vnodes) {
    ASSERT_EQ(iter->GetNlink(), (uint32_t)0);
  }

  fs->WriteCheckpoint(false, true);

  // 3. Sudden power off
  for (const auto &iter : vnodes) {
    iter->Close();
  }

  vnodes.clear();
  vnodes.shrink_to_fit();
  ASSERT_EQ(root_dir->Close(), ZX_OK);
  root_dir.reset();

  FileTester::SuddenPowerOff(std::move(fs), &bc);

  // 4. Remount and recover orphan inodes
  FileTester::MountWithOptions(loop.dispatcher(), options, &bc, &fs);

  ASSERT_EQ(fs->GetSuperblockInfo().GetOrphanCount(), static_cast<uint64_t>(0));

  ASSERT_EQ(fs->ValidInodeCount(), static_cast<uint64_t>(1));
  ASSERT_EQ(fs->ValidNodeCount(), static_cast<uint64_t>(1));
  ASSERT_EQ(fs->ValidUserBlocks(), static_cast<uint64_t>(2));

  // Check Orphan nids has been freed
  for (const auto &iter : inos) {
    NodeInfo ni;
    fs->GetNodeManager().GetNodeInfo(iter, ni);
    ASSERT_EQ(ni.blk_addr, kNullAddr);
  }

  FileTester::Unmount(std::move(fs), &bc);
}

}  // namespace
}  // namespace f2fs
