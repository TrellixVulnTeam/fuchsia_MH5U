// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/syslog/cpp/macros.h>

#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "src/lib/storage/block_client/cpp/fake_block_device.h"
#include "src/storage/f2fs/f2fs.h"
#include "unit_lib.h"

namespace f2fs {
namespace {

constexpr uint32_t kMaxNodeCnt = 10;

using NodeManagerTest = F2fsFakeDevTestFixture;

void FaultInjectToDnodeAndTruncate(NodeManager &node_manager, fbl::RefPtr<VnodeF2fs> &vnode,
                                   pgoff_t page_index, block_t fault_address,
                                   zx_status_t exception_type) {
  DnodeOfData dn;
  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);

  ASSERT_EQ(node_manager.GetDnodeOfData(dn, page_index, 0), ZX_OK);
  ino_t node_id = dn.nid;
  F2fsPutDnode(&dn);
  block_t temp_block_address;

  // Write out dirty nodes to allocate lba
  WritebackOperation op = {.bSync = true};
  vnode->Vfs()->GetNodeVnode().Writeback(op);
  MapTester::GetCachedNatEntryBlockAddress(node_manager, node_id, temp_block_address);
  vnode->Vfs()->GetNodeVnode().InvalidatePages();

  // Set fault_address to the NAT entry
  MapTester::SetCachedNatEntryBlockAddress(node_manager, node_id, fault_address);

  ASSERT_EQ(node_manager.TruncateInodeBlocks(*vnode, page_index), exception_type);

  // Restore the NAT entry
  MapTester::SetCachedNatEntryBlockAddress(node_manager, node_id, temp_block_address);

  // Retry truncate
  vnode->Vfs()->GetNodeVnode().InvalidatePages();
  ASSERT_EQ(node_manager.TruncateInodeBlocks(*vnode, page_index), ZX_OK);
}

TEST_F(NodeManagerTest, NatCache) {
  NodeManager &node_manager = fs_->GetNodeManager();

  size_t num_tree = 0, num_clean = 0, num_dirty = 0;

  // 1. Check NAT cache is empty
  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, 1UL);
  ASSERT_EQ(num_clean, 1UL);  // root inode
  ASSERT_EQ(num_dirty, 0UL);

  // 2. Check NAT entry is cached in dirty NAT entries list
  std::vector<fbl::RefPtr<VnodeF2fs>> vnodes;
  std::vector<uint32_t> inos;

  // Fill NAT cache
  FileTester::CreateChildren(fs_.get(), vnodes, inos, root_dir_, "NATCache_", kMaxNodeCnt);
  ASSERT_EQ(vnodes.size(), kMaxNodeCnt);
  ASSERT_EQ(inos.size(), kMaxNodeCnt);

  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(kMaxNodeCnt + 1));
  ASSERT_EQ(num_clean, static_cast<size_t>(1));
  ASSERT_EQ(num_dirty, static_cast<size_t>(kMaxNodeCnt));
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<nid_t>(kMaxNodeCnt + 1));

  // Lookup NAT cache
  for (auto ino : inos) {
    NodeInfo ni;
    ASSERT_TRUE(MapTester::IsCachedNat(node_manager, ino));
    fs_->GetNodeManager().GetNodeInfo(ino, ni);
    ASSERT_EQ(ni.nid, ino);
  }

  // Move dirty entries to clean entries
  fs_->WriteCheckpoint(false, false);

  // 3. Check NAT entry is cached in clean NAT entries list
  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(kMaxNodeCnt + 1));
  ASSERT_EQ(num_clean, static_cast<size_t>(kMaxNodeCnt + 1));
  ASSERT_EQ(num_dirty, static_cast<size_t>(0));
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<nid_t>(kMaxNodeCnt + 1));

  // Lookup NAT cache
  for (auto ino : inos) {
    NodeInfo ni;
    ASSERT_TRUE(MapTester::IsCachedNat(node_manager, ino));
    fs_->GetNodeManager().GetNodeInfo(ino, ni);
    ASSERT_EQ(ni.nid, ino);
  }

  // 4. Flush all NAT cache entries
  MapTester::RemoveAllNatEntries(node_manager);
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<uint32_t>(0));

  CursegInfo *curseg =
      fs_->GetSegmentManager().CURSEG_I(CursegType::kCursegHotData);  // NAT Journal
  SummaryBlock *sum = curseg->sum_blk;
  ASSERT_EQ(GetSumType(&sum->footer), kSumTypeData);

  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(0));
  ASSERT_EQ(num_clean, static_cast<size_t>(0));
  ASSERT_EQ(num_dirty, static_cast<size_t>(0));
  ASSERT_EQ(NatsInCursum(sum), static_cast<int>(kMaxNodeCnt + 1));

  // Lookup NAT journal
  for (auto ino : inos) {
    NodeInfo ni;
    ASSERT_FALSE(MapTester::IsCachedNat(node_manager, ino));
    fs_->GetNodeManager().GetNodeInfo(ino, ni);
    ASSERT_EQ(ni.nid, ino);
  }

  // 5. Check NAT cache miss and journal miss
  std::vector<uint32_t> journal_inos;

  // Fill NAT cache with journal size -2
  // Root inode NAT(nid=4) is duplicated in cache and journal, so we need to keep two empty NAT
  // entries
  FileTester::CreateChildren(fs_.get(), vnodes, journal_inos, root_dir_, "NATJournal_",
                             kNatJournalEntries - kMaxNodeCnt - 2);
  ASSERT_EQ(vnodes.size(), kNatJournalEntries - 2);
  ASSERT_EQ(inos.size() + journal_inos.size(), kNatJournalEntries - 2);

  // Fill NAT journal
  fs_->WriteCheckpoint(false, false);
  ASSERT_EQ(NatsInCursum(sum), static_cast<int>(kNatJournalEntries - 1));

  // Fill NAT cache over journal size
  FileTester::CreateChildren(fs_.get(), vnodes, journal_inos, root_dir_, "NATJournalFlush_", 2);
  ASSERT_EQ(vnodes.size(), kNatJournalEntries);
  ASSERT_EQ(inos.size() + journal_inos.size(), kNatJournalEntries);

  // Flush NAT journal
  fs_->WriteCheckpoint(false, false);
  ASSERT_EQ(NatsInCursum(sum), static_cast<int>(0));

  // Flush NAT cache
  MapTester::RemoveAllNatEntries(node_manager);
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<uint32_t>(0));

  // Check NAT cache empty
  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(0));
  ASSERT_EQ(num_clean, static_cast<size_t>(0));
  ASSERT_EQ(num_dirty, static_cast<size_t>(0));
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<nid_t>(0));

  // Read NAT block
  for (auto ino : inos) {
    NodeInfo ni;
    ASSERT_FALSE(MapTester::IsCachedNat(node_manager, ino));
    fs_->GetNodeManager().GetNodeInfo(ino, ni);
    ASSERT_EQ(ni.nid, ino);
  }

  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(10));
  ASSERT_EQ(num_clean, static_cast<size_t>(10));
  ASSERT_EQ(num_dirty, static_cast<size_t>(0));
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<nid_t>(10));

  // Shrink nat cache to reduce memory usage (test TryToFreeNats())
  MapTester::SetNatCount(node_manager, node_manager.GetNatCount() + kNmWoutThreshold * 3);
  fs_->WriteCheckpoint(false, false);

  MapTester::GetNatCacheEntryCount(node_manager, num_tree, num_clean, num_dirty);
  ASSERT_EQ(num_tree, static_cast<size_t>(0));
  ASSERT_EQ(num_clean, static_cast<size_t>(0));
  ASSERT_EQ(node_manager.GetNatCount(), static_cast<uint32_t>(kNmWoutThreshold * 3));
  MapTester::SetNatCount(node_manager, 0);

  for (auto &vnode_refptr : vnodes) {
    ASSERT_EQ(vnode_refptr->Close(), ZX_OK);
    vnode_refptr.reset();
  }
}

TEST_F(NodeManagerTest, FreeNid) {
  NodeManager &node_manager = fs_->GetNodeManager();

  ASSERT_EQ(node_manager.GetFirstScanNid(), static_cast<nid_t>(4));

  nid_t nid = node_manager.GetFirstScanNid();
  nid_t init_fcnt = node_manager.GetFreeNidCount();

  nid = MapTester::ScanFreeNidList(node_manager, nid);
  ASSERT_EQ(nid, node_manager.GetNextScanNid());

  // Alloc Done
  fs_->GetNodeManager().AllocNid(nid);
  ASSERT_EQ(nid, static_cast<nid_t>(4));
  ASSERT_EQ(node_manager.GetFreeNidCount(), init_fcnt - 1);

  FreeNid *fi = MapTester::GetNextFreeNidInList(node_manager);
  ASSERT_EQ(fi->nid, static_cast<nid_t>(4));
  ASSERT_EQ(fi->state, static_cast<int>(NidState::kNidAlloc));

  fs_->GetNodeManager().AllocNidDone(nid);
  fi = MapTester::GetNextFreeNidInList(node_manager);
  ASSERT_EQ(fi->nid, static_cast<nid_t>(5));
  ASSERT_EQ(fi->state, static_cast<int>(NidState::kNidNew));

  // Alloc Failed
  fs_->GetNodeManager().AllocNid(nid);
  ASSERT_EQ(nid, static_cast<nid_t>(5));
  ASSERT_EQ(node_manager.GetFreeNidCount(), init_fcnt - 2);

  fi = MapTester::GetNextFreeNidInList(node_manager);
  ASSERT_EQ(fi->nid, static_cast<nid_t>(5));
  ASSERT_EQ(fi->state, static_cast<int>(NidState::kNidAlloc));

  fs_->GetNodeManager().AllocNidFailed(nid);
  fi = MapTester::GetTailFreeNidInList(node_manager);
  ASSERT_EQ(fi->nid, static_cast<nid_t>(5));
  ASSERT_EQ(fi->state, static_cast<int>(NidState::kNidNew));
}

TEST_F(NodeManagerTest, NodePage) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);
  nid_t inode_nid = vnode->Ino();

  DnodeOfData dn;
  NodeManager &node_manager = fs_->GetNodeManager();
  uint64_t free_node_cnt = node_manager.GetFreeNidCount();

  // Inode block
  //   |- direct node
  //   |- direct node
  //   |- indirect node
  //   |            `- direct node
  //   |- indirect node
  //   |            `- direct node
  //   `- double indirect node
  //                `- indirect node
  //                      `- direct node

  // Check inode (level 0)
  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);
  const pgoff_t direct_index = 1;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, direct_index, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, direct_index, true);
  F2fsPutDnode(&dn);

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, direct_index, kRdOnlyNode), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, direct_index, true);
  F2fsPutDnode(&dn);
  ASSERT_EQ(node_manager.GetFreeNidCount(), free_node_cnt);
  inode_nid += 1;

  // Check direct node (level 1)
  pgoff_t indirect_index_lv1 = direct_index + kAddrsPerInode;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv1, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv1, false);
  F2fsPutDnode(&dn);

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv1, kRdOnlyNode), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv1, false);
  F2fsPutDnode(&dn);
  ASSERT_EQ(node_manager.GetFreeNidCount(), free_node_cnt -= 1);
  inode_nid += 2;

  // Check indirect node (level 2)
  const pgoff_t direct_blks = kAddrsPerBlock;
  pgoff_t indirect_index_lv2 = indirect_index_lv1 + direct_blks * 2;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv2, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv2, false);
  F2fsPutDnode(&dn);

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv2, kRdOnlyNode), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv2, false);
  F2fsPutDnode(&dn);
  ASSERT_EQ(node_manager.GetFreeNidCount(), free_node_cnt -= 2);
  inode_nid += 2;

  // Check second indirect node (level 2)
  const pgoff_t indirect_blks = kAddrsPerBlock * kNidsPerBlock;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv2 + indirect_blks, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv2 + indirect_blks, false);
  F2fsPutDnode(&dn);

  ASSERT_EQ(
      fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv2 + indirect_blks, kRdOnlyNode),
      ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv2 + indirect_blks, false);
  F2fsPutDnode(&dn);
  ASSERT_EQ(node_manager.GetFreeNidCount(), free_node_cnt -= 2);
  inode_nid += 3;

  // Check double indirect node (level 3)
  pgoff_t indirect_index_lv3 = indirect_index_lv2 + indirect_blks * 2;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv3, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv3, false);
  F2fsPutDnode(&dn);

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv3, kRdOnlyNode), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, indirect_index_lv3, false);
  F2fsPutDnode(&dn);
  ASSERT_EQ(node_manager.GetFreeNidCount(), free_node_cnt -= 3);

  vnode->SetBlocks(1);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

TEST_F(NodeManagerTest, NodePageExceptionCase) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);

  DnodeOfData dn;
  NodeManager &node_manager = fs_->GetNodeManager();
  SuperblockInfo &superblock_info = fs_->GetSuperblockInfo();

  // Inode block
  //   |- direct node
  //   |- direct node
  //   |- indirect node
  //   |            `- direct node
  //   |- indirect node
  //   |            `- direct node
  //   `- double indirect node
  //                `- indirect node
  //                      `- direct node

  // Check inode (level 0)
  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);

  const pgoff_t direct_index = 1;
  const pgoff_t direct_blks = kAddrsPerBlock;
  const pgoff_t indirect_blks = kAddrsPerBlock * kNidsPerBlock;
  const pgoff_t indirect_index_lv1 = direct_index + kAddrsPerInode;
  const pgoff_t indirect_index_lv2 = indirect_index_lv1 + direct_blks * 2;
  const pgoff_t indirect_index_lv3 = indirect_index_lv2 + indirect_blks * 2;

  // Check invalid page offset exception case
  pgoff_t indirect_index_invalid_lv4 = indirect_index_lv3 + indirect_blks * kNidsPerBlock;
  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_invalid_lv4, 0),
            ZX_ERR_NOT_FOUND);

  // Check invalid address
  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv3 + 1, 0), ZX_OK);
  F2fsPutDnode(&dn);

  // fault injection for ReadNodePage()
  fs_->WriteCheckpoint(false, false);
  MapTester::SetCachedNatEntryBlockAddress(node_manager, dn.nid, kNullAddr);

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv3 + 1, 0), ZX_ERR_NOT_FOUND);

  // Check IncValidNodeCount() exception case
  block_t tmp_total_valid_block_count = superblock_info.GetTotalValidBlockCount();
  superblock_info.SetTotalValidBlockCount(superblock_info.GetUserBlockCount());
  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv1 + direct_blks, 0),
            ZX_ERR_NO_SPACE);
  superblock_info.SetTotalValidBlockCount(tmp_total_valid_block_count);

  block_t tmp_total_valid_node_count = superblock_info.GetTotalValidNodeCount();
  superblock_info.SetTotalValidNodeCount(superblock_info.GetTotalNodeCount());
  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, indirect_index_lv1 + direct_blks, 0),
            ZX_ERR_NO_SPACE);
  superblock_info.SetTotalValidNodeCount(tmp_total_valid_node_count);

  // Check NewNodePage() exception case
  fbl::RefPtr<VnodeF2fs> test_vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, test_vnode);

  test_vnode->SetFlag(InodeInfoFlag::kNoAlloc);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), test_vnode.get()),
            ZX_ERR_ACCESS_DENIED);
  test_vnode->ClearFlag(InodeInfoFlag::kNoAlloc);

  tmp_total_valid_block_count = superblock_info.GetTotalValidBlockCount();
  superblock_info.SetTotalValidBlockCount(superblock_info.GetUserBlockCount());
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), test_vnode.get()), ZX_ERR_NO_SPACE);
  ASSERT_EQ(test_vnode->Close(), ZX_OK);
  test_vnode.reset();
  superblock_info.SetTotalValidBlockCount(tmp_total_valid_block_count);

  vnode->SetBlocks(1);

  // Check MaxNid
  const Superblock &sb_raw = superblock_info.GetRawSuperblock();
  uint32_t nat_segs = LeToCpu(sb_raw.segment_count_nat) >> 1;
  uint32_t nat_blocks = nat_segs << LeToCpu(sb_raw.log_blocks_per_seg);
  ASSERT_EQ(fs_->GetNodeManager().GetMaxNid(), kNatEntryPerBlock * nat_blocks);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

TEST_F(NodeManagerTest, TruncateDoubleIndirect) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);

  DnodeOfData dn;
  SuperblockInfo &superblock_info = fs_->GetSuperblockInfo();

  // Inode block
  //   |- direct node
  //   |- direct node
  //   |- indirect node
  //   |            `- direct node
  //   |- indirect node
  //   |            `- direct node
  //   `- double indirect node
  //                `- indirect node
  //                      `- direct node

  // Alloc a double indirect node (level 3)
  const pgoff_t direct_blks = kAddrsPerBlock;
  const pgoff_t indirect_blks = kAddrsPerBlock * kNidsPerBlock;
  const pgoff_t direct_index = kAddrsPerInode + 1;
  const pgoff_t indirect_index = direct_index + direct_blks * 2;
  const pgoff_t double_indirect_index = indirect_index + indirect_blks * 2;
  const uint32_t inode_cnt = 2;

  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), inode_cnt);

  std::vector<nid_t> nids;
  NodeManager &node_manager = fs_->GetNodeManager();
  uint64_t initial_free_nid_cnt = node_manager.GetFreeNidCount();

  // Alloc a direct node at double_indirect_index
  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);
  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, double_indirect_index, 0), ZX_OK);
  nids.push_back(dn.nid);
  F2fsPutDnode(&dn);

  // # of alloc nodes = 1 double indirect + 1 indirect + 1 direct
  uint32_t alloc_node_cnt = 3;
  uint32_t node_cnt = inode_cnt + alloc_node_cnt;

  // alloc_dnode cnt should be one
  ASSERT_EQ(nids.size(), 1UL);
  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  // Truncate double the indirect node
  ASSERT_EQ(fs_->GetNodeManager().TruncateInodeBlocks(*vnode, double_indirect_index), ZX_OK);
  node_cnt = inode_cnt;
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  MapTester::RemoveTruncatedNode(node_manager, nids);
  ASSERT_EQ(nids.size(), 0UL);

  ASSERT_EQ(node_manager.GetFreeNidCount(), initial_free_nid_cnt - alloc_node_cnt);
  fs_->WriteCheckpoint(false, false);
  // After checkpoint, we can reuse the removed nodes
  ASSERT_EQ(node_manager.GetFreeNidCount(), initial_free_nid_cnt);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

TEST_F(NodeManagerTest, TruncateIndirect) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);

  DnodeOfData dn;
  SuperblockInfo &superblock_info = fs_->GetSuperblockInfo();

  // Inode block
  //   |- direct node
  //   |- direct node
  //   |- indirect node
  //   |            `- direct node
  // Fill indirect node (level 2)
  const pgoff_t direct_blks = kAddrsPerBlock;
  const pgoff_t direct_index = kAddrsPerInode + 1;
  const pgoff_t indirect_index = direct_index + direct_blks * 2;
  const uint32_t inode_cnt = 2;
  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), inode_cnt);

  std::vector<nid_t> nids;
  NodeManager &node_manager = fs_->GetNodeManager();
  uint64_t initial_free_nid_cnt = node_manager.GetFreeNidCount();

  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);
  // Start from kAddrsPerInode to alloc new dnodes
  for (pgoff_t i = kAddrsPerInode; i <= indirect_index; i += kAddrsPerBlock) {
    ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, i, 0), ZX_OK);
    nids.push_back(dn.nid);
    F2fsPutDnode(&dn);
  }

  uint32_t indirect_node_cnt = 1;
  uint32_t direct_node_cnt = 3;
  uint32_t node_cnt = inode_cnt + direct_node_cnt + indirect_node_cnt;
  uint32_t alloc_node_cnt = indirect_node_cnt + direct_node_cnt;

  ASSERT_EQ(nids.size(), direct_node_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  // Truncate indirect nodes
  ASSERT_EQ(fs_->GetNodeManager().TruncateInodeBlocks(*vnode, indirect_index), ZX_OK);
  --indirect_node_cnt;
  --direct_node_cnt;
  node_cnt = inode_cnt + direct_node_cnt + indirect_node_cnt;
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  MapTester::RemoveTruncatedNode(node_manager, nids);

  ASSERT_EQ(nids.size(), direct_node_cnt);

  // Truncate direct nodes
  ASSERT_EQ(fs_->GetNodeManager().TruncateInodeBlocks(*vnode, direct_index), ZX_OK);
  direct_node_cnt -= 2;
  node_cnt = inode_cnt + direct_node_cnt + indirect_node_cnt;
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  MapTester::RemoveTruncatedNode(node_manager, nids);
  ASSERT_EQ(nids.size(), direct_node_cnt);

  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);

  ASSERT_EQ(node_manager.GetFreeNidCount(), initial_free_nid_cnt - alloc_node_cnt);
  fs_->WriteCheckpoint(false, false);
  // After checkpoint, we can reuse the removed nodes
  ASSERT_EQ(node_manager.GetFreeNidCount(), initial_free_nid_cnt);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

TEST_F(NodeManagerTest, TruncateExceptionCase) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);

  DnodeOfData dn;
  SuperblockInfo &superblock_info = fs_->GetSuperblockInfo();

  // Inode block
  //   |- direct node
  //   |- direct node

  // Fill direct node (level 1)
  const uint32_t inode_cnt = 2;

  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), inode_cnt);

  const pgoff_t direct_index = 1;
  const pgoff_t direct_blks = kAddrsPerBlock;
  const pgoff_t indirect_blks = kAddrsPerBlock * kNidsPerBlock;
  const pgoff_t indirect_index_lv1 = direct_index + kAddrsPerInode;
  const pgoff_t indirect_index_lv1_2nd = indirect_index_lv1 + direct_blks;
  const pgoff_t indirect_index_lv2 = indirect_index_lv1 + direct_blks * 2;
  const pgoff_t indirect_index_lv3 = indirect_index_lv2 + indirect_blks * 2;

  // Check invalid page offset exception case
  pgoff_t indirect_index_invalid_lv4 = indirect_index_lv3 + indirect_blks * kNidsPerBlock;

  std::vector<nid_t> nids;
  NodeManager &node_manager = fs_->GetNodeManager();
  uint64_t initial_free_nid_cnt = node_manager.GetFreeNidCount();

  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);
  // Start from kAddrsPerInode to alloc new dnodes
  for (pgoff_t i = kAddrsPerInode; i <= indirect_index_lv3 + kNidsPerBlock; i += kAddrsPerBlock) {
    ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, i, 0), ZX_OK);
    nids.push_back(dn.nid);
    F2fsPutDnode(&dn);
  }

  uint32_t direct_node_cnt = 4 + kNidsPerBlock * 2;
  uint32_t indirect_node_cnt = 4;  // 1 double indirect + 3 indirect
  uint32_t node_cnt = inode_cnt + direct_node_cnt + indirect_node_cnt;

  ASSERT_EQ(nids.size(), direct_node_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  // 1. Truncate invalid node
  ASSERT_EQ(fs_->GetNodeManager().TruncateInodeBlocks(*vnode, indirect_index_invalid_lv4),
            ZX_ERR_NOT_FOUND);

  // 2. Check exception case of TruncatePartialNodes()
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv3 + kNidsPerBlock, kNewAddr,
                                ZX_ERR_OUT_OF_RANGE);
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv2 + kNidsPerBlock, kNewAddr,
                                ZX_ERR_OUT_OF_RANGE);
  --indirect_node_cnt;

  // 3. Check exception case of TruncateNodes()
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv3, kNewAddr,
                                ZX_ERR_OUT_OF_RANGE);
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv2, kNewAddr,
                                ZX_ERR_OUT_OF_RANGE);
  --indirect_node_cnt;

  // 4. Check exception case of TruncateDnode()
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv1_2nd, kNewAddr,
                                ZX_ERR_OUT_OF_RANGE);
  --indirect_node_cnt;

  // 5. Check exception case truncation of invalid address
  FaultInjectToDnodeAndTruncate(node_manager, vnode, indirect_index_lv1, kNullAddr, ZX_OK);
  --indirect_node_cnt;
  node_cnt = inode_cnt + indirect_node_cnt;
  ASSERT_EQ(superblock_info.GetTotalValidNodeCount(), node_cnt);

  // 6. Wrap up
  MapTester::RemoveTruncatedNode(node_manager, nids);
  ASSERT_EQ(nids.size(), 0UL);

  ASSERT_EQ(superblock_info.GetTotalValidInodeCount(), inode_cnt);

  fs_->WriteCheckpoint(false, false);

  // After checkpoint, we can reuse the removed nodes
  ASSERT_EQ(node_manager.GetFreeNidCount(), initial_free_nid_cnt);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

TEST_F(NodeManagerTest, NodeFooter) {
  // Alloc Inode
  fbl::RefPtr<VnodeF2fs> vnode;
  FileTester::VnodeWithoutParent(fs_.get(), S_IFREG, vnode);
  ASSERT_EQ(fs_->GetNodeManager().NewInodePage(root_dir_.get(), vnode.get()), ZX_OK);
  nid_t inode_nid = vnode->Ino();

  DnodeOfData dn;
  NodeManager::SetNewDnode(dn, vnode.get(), nullptr, nullptr, 0);
  const pgoff_t direct_index = 1;

  ASSERT_EQ(fs_->GetNodeManager().GetDnodeOfData(dn, direct_index, 0), ZX_OK);
  MapTester::CheckDnodeOfData(&dn, inode_nid, direct_index, true);

  fbl::RefPtr<Page> page = nullptr;
  fs_->GetMetaVnode().GrabCachePage(direct_index, &page);

  // Check CopyNodeFooter()
  NodeManager::CopyNodeFooter(*page, *dn.node_page);

  ASSERT_EQ(NodeManager::InoOfNode(*page), vnode->Ino());
  ASSERT_EQ(NodeManager::InoOfNode(*page), NodeManager::InoOfNode(*dn.node_page));
  ASSERT_EQ(NodeManager::NidOfNode(*page), NodeManager::NidOfNode(*dn.node_page));
  ASSERT_EQ(NodeManager::OfsOfNode(*page), NodeManager::OfsOfNode(*dn.node_page));
  ASSERT_EQ(NodeManager::CpverOfNode(*page), NodeManager::CpverOfNode(*dn.node_page));
  ASSERT_EQ(NodeManager::NextBlkaddrOfNode(*page), NodeManager::NextBlkaddrOfNode(*dn.node_page));

  // Check footer.flag
  ASSERT_EQ(NodeManager::IsFsyncDnode(*page), NodeManager::IsFsyncDnode(*dn.node_page));
  ASSERT_EQ(NodeManager::IsFsyncDnode(*page), 0);
  NodeManager::SetFsyncMark(*page, 1);
  ASSERT_EQ(NodeManager::IsFsyncDnode(*page), 0x1 << static_cast<int>(BitShift::kFsyncBitShift));
  NodeManager::SetFsyncMark(*page, 0);
  ASSERT_EQ(NodeManager::IsFsyncDnode(*page), 0);

  ASSERT_EQ(NodeManager::IsDentDnode(*page), NodeManager::IsDentDnode(*dn.node_page));
  ASSERT_EQ(NodeManager::IsDentDnode(*page), 0);
  NodeManager::SetDentryMark(*page, 0);
  ASSERT_EQ(NodeManager::IsDentDnode(*page), 0);
  NodeManager::SetDentryMark(*page, 1);
  ASSERT_EQ(NodeManager::IsDentDnode(*page), 0x1 << static_cast<int>(BitShift::kDentBitShift));
  int mark = !fs_->GetNodeManager().IsCheckpointedNode(NodeManager::InoOfNode(*page));
  NodeManager::SetDentryMark(*page, mark);
  ASSERT_EQ(NodeManager::IsDentDnode(*page), 0x1 << static_cast<int>(BitShift::kDentBitShift));

  MapTester::SetCachedNatEntryCheckpointed(fs_->GetNodeManager(), dn.nid);
  mark = !fs_->GetNodeManager().IsCheckpointedNode(NodeManager::InoOfNode(*page));
  NodeManager::SetDentryMark(*page, mark);
  ASSERT_EQ(NodeManager::IsDentDnode(*page), 0x0 << static_cast<int>(BitShift::kDentBitShift));

  Page::PutPage(std::move(page), true);
  F2fsPutDnode(&dn);

  ASSERT_EQ(vnode->Close(), ZX_OK);
  vnode.reset();
}

}  // namespace
}  // namespace f2fs
