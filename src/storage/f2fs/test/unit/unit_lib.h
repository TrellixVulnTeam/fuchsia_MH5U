// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_F2FS_TEST_UNIT_UNIT_LIB_H_
#define SRC_STORAGE_F2FS_TEST_UNIT_UNIT_LIB_H_

#include <unordered_set>

#include <gtest/gtest.h>

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

struct TestOptions {
  uint64_t block_count = 819200;
  uint64_t block_size = kDefaultSectorSize;
  std::vector<std::pair<uint32_t, uint32_t>> mount_options;
};

class F2fsFakeDevTestFixture : public testing::Test {
 public:
  F2fsFakeDevTestFixture(TestOptions options = TestOptions());
  ~F2fsFakeDevTestFixture();

 protected:
  uint64_t block_count_;
  uint64_t block_size_;
  MountOptions options_{};
  std::unique_ptr<f2fs::Bcache> bc_;
  std::unique_ptr<F2fs> fs_;
  fbl::RefPtr<Dir> root_dir_;
  async::Loop loop_ = async::Loop(&kAsyncLoopConfigAttachToCurrentThread);
};

class FileTester {
 public:
  static void MkfsOnFakeDev(std::unique_ptr<Bcache> *bc, uint64_t block_count = 819200,
                            uint32_t block_size = kDefaultSectorSize, bool btrim = true);
  static void MkfsOnFakeDevWithOptions(std::unique_ptr<Bcache> *bc, const MkfsOptions &options,
                                       uint64_t block_count = 819200,
                                       uint32_t block_size = kDefaultSectorSize, bool btrim = true);
  static void MountWithOptions(async_dispatcher_t *dispatcher, const MountOptions &options,
                               std::unique_ptr<Bcache> *bc, std::unique_ptr<F2fs> *fs);
  static void Unmount(std::unique_ptr<F2fs> fs, std::unique_ptr<Bcache> *bc);
  static void SuddenPowerOff(std::unique_ptr<F2fs> fs, std::unique_ptr<Bcache> *bc);

  static void CreateRoot(F2fs *fs, fbl::RefPtr<VnodeF2fs> *out);
  static void Lookup(VnodeF2fs *parent, std::string_view name, fbl::RefPtr<fs::Vnode> *out);

  static void CreateChild(Dir *vn, uint32_t mode, std::string_view name);
  static void DeleteChild(Dir *vn, std::string_view name);
  static void CreateChildren(F2fs *fs, std::vector<fbl::RefPtr<VnodeF2fs>> &vnodes,
                             std::vector<uint32_t> &inos, fbl::RefPtr<Dir> &parent,
                             std::string name, uint32_t inode_cnt);
  static void DeleteChildren(std::vector<fbl::RefPtr<VnodeF2fs>> &vnodes, fbl::RefPtr<Dir> &parent,
                             uint32_t inode_cnt);

  static void VnodeWithoutParent(F2fs *fs, uint32_t mode, fbl::RefPtr<VnodeF2fs> &vnode);

  static void CheckInlineDir(VnodeF2fs *vn);
  static void CheckNonInlineDir(VnodeF2fs *vn);

  static void CheckChildrenFromReaddir(Dir *dir, std::unordered_set<std::string> childs);
  static void CheckChildrenInBlock(Dir *vn, uint64_t bidx, std::unordered_set<std::string> childs);

  static std::string GetRandomName(unsigned int len);

  static void AppendToFile(File *file, const void *data, size_t len);
};

class MapTester {
 public:
  static void CheckNodeLevel(F2fs *fs, VnodeF2fs *vn, int level);
  static void CheckNidsFree(F2fs *fs, std::unordered_set<nid_t> &nids);
  static void CheckNidsInuse(F2fs *fs, std::unordered_set<nid_t> &nids);
  static void CheckBlkaddrsFree(F2fs *fs, std::unordered_set<block_t> &blkaddrs);
  static void CheckBlkaddrsInuse(F2fs *fs, std::unordered_set<block_t> &blkaddrs);
  static void CheckDnodeOfData(DnodeOfData *dn, nid_t exp_nid, pgoff_t exp_index, bool is_inode);
  static void DoWriteNat(F2fs *fs, nid_t nid, block_t blkaddr, uint8_t version);
  static void RemoveTruncatedNode(NodeManager &nm_i, std::vector<nid_t> &nids)
      __TA_EXCLUDES(nm_i.nat_tree_lock_);
  static bool IsCachedNat(NodeManager &nm_i, nid_t n) __TA_EXCLUDES(nm_i.nat_tree_lock_);
  static void ClearAllDirtyNatEntries(NodeManager &manager) __TA_EXCLUDES(manager.nat_tree_lock_);
  static void RemoveAllNatEntries(NodeManager &manager) __TA_EXCLUDES(manager.nat_tree_lock_);
  static nid_t ScanFreeNidList(NodeManager &manager, nid_t start)
      __TA_EXCLUDES(manager.free_nid_list_lock_);
  static void GetCachedNatEntryBlockAddress(NodeManager &manager, nid_t nid, block_t &out)
      __TA_EXCLUDES(manager.free_nid_list_lock_);
  static void SetCachedNatEntryBlockAddress(NodeManager &manager, nid_t nid, block_t address)
      __TA_EXCLUDES(manager.free_nid_list_lock_);
  static void SetCachedNatEntryCheckpointed(NodeManager &manager, nid_t nid)
      __TA_EXCLUDES(manager.free_nid_list_lock_);
  static FreeNid *GetNextFreeNidInList(NodeManager &manager)
      __TA_EXCLUDES(manager.free_nid_list_lock_) {
    std::lock_guard nat_lock(manager.free_nid_list_lock_);
    return containerof(manager.free_nid_list_.next, FreeNid, list);
  }
  static FreeNid *GetTailFreeNidInList(NodeManager &manager)
      __TA_EXCLUDES(manager.free_nid_list_lock_) {
    std::lock_guard nat_lock(manager.free_nid_list_lock_);
    return containerof(list_peek_tail(&manager.free_nid_list_), FreeNid, list);
  }
  static void GetNatCacheEntryCount(NodeManager &manager, size_t &num_tree, size_t &num_clean,
                                    size_t &num_dirty) __TA_EXCLUDES(manager.nat_tree_lock_) {
    std::lock_guard nat_lock(manager.nat_tree_lock_);
    num_tree = manager.nat_cache_.size();
    num_clean = manager.clean_nat_list_.size_slow();
    num_dirty = manager.dirty_nat_list_.size_slow();
  }
  static void SetNatCount(NodeManager &manager, uint32_t count) {
    manager.nat_entries_count_ = count;
  }
};

class MkfsTester {
 public:
  static GlobalParameters &GetGlobalParameters(MkfsWorker &mkfs) { return mkfs.params_; }

  static zx_status_t InitAndGetDeviceInfo(MkfsWorker &mkfs);
  static zx::status<std::unique_ptr<Bcache>> FormatDevice(MkfsWorker &mkfs);
};

}  // namespace f2fs

#endif  // SRC_STORAGE_F2FS_TEST_UNIT_UNIT_LIB_H_
