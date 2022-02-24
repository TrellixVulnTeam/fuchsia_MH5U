// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/storage/f2fs/f2fs.h"

namespace f2fs {

uint32_t SegmentManager::GetGcCost(uint32_t segno, VictimSelPolicy *p) {
  if (p->alloc_mode == AllocMode::kSSR)
    return SegmentManager::GetSegmentEntry(segno).ckpt_valid_blocks;

  ZX_ASSERT(0);
#if 0  // porting needed
  /* alloc_mode == kLFS */
  if (p->gc_mode == kGcGreedy)
    return get_valid_blocks(superblock_info_, segno, superblock_info_->GetSegsPerSec());
  else
    return get_cb_cost(superblock_info_, segno);
#endif
}

void SegmentManager::SelectPolicy(GcType gc_type, CursegType type, VictimSelPolicy *p) {
  if (p->alloc_mode == AllocMode::kSSR) {
    p->gc_mode = GcMode::kGcGreedy;
    p->dirty_segmap = dirty_info_->dirty_segmap[static_cast<int>(type)].get();
    p->ofs_unit = 1;
  } else {
    ZX_ASSERT(0);
#if 0  // porting needed
  p->gc_mode = select_gc_type(gc_type);
  p->dirty_segmap = dirty_i->dirty_segmap[DIRTY].get();
  p->ofs_unit = superblock_info_->GetSegsPerSec();
#endif
  }

  p->offset = superblock_info_->GetLastVictim(static_cast<int>(p->gc_mode));
}

uint32_t SegmentManager::GetMaxCost(VictimSelPolicy *p) {
  if (p->gc_mode == GcMode::kGcGreedy)
    return (1 << superblock_info_->GetLogBlocksPerSeg()) * p->ofs_unit;
  else if (p->gc_mode == GcMode::kGcCb)
    return kUint32Max;
  return 0;
}

bool SegmentManager::GetVictimByDefault(GcType gc_type, CursegType type, AllocMode alloc_mode,
                                        uint32_t *out) {
  VictimSelPolicy p;

  p.alloc_mode = alloc_mode;
  SelectPolicy(gc_type, type, &p);

  p.min_segno = kNullSegNo;
  p.min_cost = GetMaxCost(&p);

  std::lock_guard lock(dirty_info_->seglist_lock);

#if 0  // porting needed
	if (p.alloc_mode == AllocMode::kLFS && gc_type == GcType::kFgGC) {
		p.min_segno = check_bg_victims(superblock_info_;
		if (p.min_segno != kNullSegNo)
			goto got_it;
	}
#endif

  uint32_t nsearched = 0;

  while (1) {
    uint32_t segno = FindNextBit(p.dirty_segmap, TotalSegs(), p.offset);
    if (segno >= TotalSegs()) {
      if (superblock_info_->GetLastVictim(static_cast<int>(p.gc_mode))) {
        superblock_info_->SetLastVictim(static_cast<int>(p.gc_mode), 0);
        p.offset = 0;
        continue;
      }
      break;
    }
    p.offset = ((segno / p.ofs_unit) * p.ofs_unit) + p.ofs_unit;

    if (TestBit(segno, dirty_info_->victim_segmap[static_cast<int>(GcType::kFgGc)].get()))
      continue;
    if (gc_type == GcType::kBgGc &&
        TestBit(segno, dirty_info_->victim_segmap[static_cast<int>(GcType::kBgGc)].get()))
      continue;
    if (IsCurSec(GetSecNo(segno)))
      continue;

    uint32_t cost = GetGcCost(segno, &p);

    if (p.min_cost > cost) {
      p.min_segno = segno;
      p.min_cost = cost;
    }

    if (cost == GetMaxCost(&p))
      continue;

    if (nsearched++ >= kMaxSearchLimit) {
      superblock_info_->SetLastVictim(static_cast<int>(p.gc_mode), segno);
      break;
    }
  }
#if 0  // porting needed
got_it:
#endif
  if (p.min_segno != kNullSegNo) {
    *out = static_cast<uint32_t>((p.min_segno / p.ofs_unit) * p.ofs_unit);
    if (p.alloc_mode == AllocMode::kLFS) {
      for (uint32_t i = 0; i < p.ofs_unit; ++i)
        SetBit(*out + i, dirty_info_->victim_segmap[static_cast<int>(gc_type)].get());
    }
  }

  return (p.min_segno == kNullSegNo) ? false : true;
}

}  // namespace f2fs
