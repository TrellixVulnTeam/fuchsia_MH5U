// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_VOLUME_IMAGE_FTL_FTL_TEST_HELPER_H_
#define SRC_STORAGE_VOLUME_IMAGE_FTL_FTL_TEST_HELPER_H_

#include <lib/ftl/ndm-driver.h>
#include <lib/ftl/volume.h>

#include <cstdint>
#include <map>
#include <vector>

#include "src/storage/volume_image/ftl/options.h"

namespace storage::volume_image {

struct InMemoryRawNand {
  RawNandOptions options;
  std::map<uint32_t, std::vector<uint8_t>> page_data;
  std::map<uint32_t, std::vector<uint8_t>> page_oob;
};

// Provides a in memory NDM driver usable for unit testing.
class InMemoryNdm final : public ftl::NdmBaseDriver {
 public:
  // The options in raw_nand should be prepopulated as desired, and optionally
  // page_data and page_oob. page_size and oob_size should have the
  // corresponding sizes to match with the sizes used in raw_nand, where
  // page_size is the size of volume pages and oob_size is the size of the
  // corresponding out of band data used by the FTL. max_bad_blocks is the
  // maximum number of bad blocks to support in the ndm, more means a smaller
  // overall volume as spare blocks at the end of the volume are set aside to
  // account for this.
  explicit InMemoryNdm(InMemoryRawNand* raw_nand, uint64_t page_size, uint64_t oob_size,
                       uint32_t max_bad_blocks = 0)
      : NdmBaseDriver(ftl::DefaultLogger()),
        raw_nand_(raw_nand),
        page_size_(page_size),
        oob_size_(oob_size),
        max_bad_blocks_(max_bad_blocks) {}

  // Performs driver initialization. Returns an error string, or nullptr on
  // success.
  const char* Init() final { return nullptr; }

  // Creates a new volume. Note that multiple volumes are not supported.
  // |ftl_volume| (if provided) will be notified with the volume details.
  // Returns an error string, or nullptr on success.
  const char* Attach(const ftl::Volume* ftl_volume) final;

  // Destroy the volume created with Attach(). Returns true on success.
  bool Detach() final { return true; }

  // Reads |page_count| pages starting at |start_page|, placing the results on
  // |page_buffer| and |oob_buffer|. Either pointer can be nullptr if that
  // part is not desired.
  // Returns kNdmOk, kNdmUncorrectableEcc, kNdmFatalError or kNdmUnsafeEcc.
  int NandRead(uint32_t start_page, uint32_t page_count, void* page_buffer, void* oob_buffer) final;

  // Writes |page_count| pages starting at |start_page|, using the data from
  // |page_buffer| and |oob_buffer|.
  // Returns kNdmOk, kNdmError or kNdmFatalError. kNdmError triggers marking
  // the block as bad.
  int NandWrite(uint32_t start_page, uint32_t page_count, const void* page_buffer,
                const void* oob_buffer) final;

  // Erases the block containing |page_num|.
  // Returns kNdmOk or kNdmError. kNdmError triggers marking the block as bad.
  int NandErase(uint32_t page_num) final;

  // Returns whether the block containing |page_num| was factory-marked as bad.
  // Returns kTrue, kFalse or kNdmError.
  int IsBadBlock(uint32_t page_num) final { return ftl::kFalse; }

  // Returns whether a given page is empty or not. |data| and |spare| store
  // the contents of the page.
  bool IsEmptyPage(uint32_t page_num, const uint8_t* data, const uint8_t* spare) final;

  uint32_t PageSize() final { return static_cast<uint32_t>(page_size_); }
  uint8_t SpareSize() final { return static_cast<uint8_t>(oob_size_); }

 private:
  InMemoryRawNand* raw_nand_ = nullptr;
  uint64_t page_size_ = 0;
  uint64_t oob_size_ = 0;
  uint32_t max_bad_blocks_ = 0;
};

}  // namespace storage::volume_image

#endif  // SRC_STORAGE_VOLUME_IMAGE_FTL_FTL_TEST_HELPER_H_
