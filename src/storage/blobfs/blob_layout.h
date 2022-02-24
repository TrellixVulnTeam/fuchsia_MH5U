// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be found in the LICENSE file.

#ifndef SRC_STORAGE_BLOBFS_BLOB_LAYOUT_H_
#define SRC_STORAGE_BLOBFS_BLOB_LAYOUT_H_

#include <lib/zx/status.h>
#include <zircon/types.h>

#include <type_traits>

#include <safemath/safe_conversions.h>

#include "src/storage/blobfs/format.h"

namespace blobfs {

// Possible formats for how a blob can be layed out in storage.
// This enum is serialized and stored in blobfs's superblock which prevents the enum values from
// being changed.
enum class BlobLayoutFormat : uint8_t {
  // THIS VALUE IS DEPRECATED. It is used only on Astro devices. It uses more space because of the
  // additional padding to block-align the beginning blob data. Do not add new uses, it will be
  // removed once Astro support is no longer needed.
  //
  // The "Padded Merkle Tree at Start" layout stores the Merkle tree in the padded format at the
  // start of the blob.  The data is stored at the start of the block following the Merkle tree.
  // | block 001 | block 002 | block 003 | block 004 | block 005 | ... | block 579 | block 580 |
  // |<-       Padded Merkle Tree      ->|<-                  Data                 ->|
  // This is the layout format that was in use prior to the layout format being added to the
  // superblock.  The new field was added to a section of the superpblock that was already zero and
  // to maintain backwards compatibility this enum value has the value zero.
  kDeprecatedPaddedMerkleTreeAtStart = 0,

  // The "Compact Merkle Tree at End" layout stores the data at the start of the blob.  The Merkle
  // tree is stored in the compact format after the data and aligned so it ends at the end of the
  // blob.  The Merkle tree and the data may share a block.
  // | block 001 | block 002 | ... | block 576 | block 577 | block 578 | block 579 |
  // |<-                  Data                 ->|      |<- Compact Merkle Tree  ->|
  kCompactMerkleTreeAtEnd = 1,
};

// Returns enum name for |format|.
const char* BlobLayoutFormatToString(BlobLayoutFormat format);

// Returns whether |format| uses the compact Merkle tree format or not.
bool ShouldUseCompactMerkleTreeFormat(BlobLayoutFormat format);

// Layout information for where the data and Merkle tree are positioned in a blob.
class BlobLayout {
 public:
  // The type used to represent a number of bytes in blobfs.  Must be large enough to hold blobfs's
  // maximum file size.
  using ByteCountType = uint64_t;
  // The type used to represent a number of blocks in blobfs.
  using BlockCountType = uint32_t;
  // The type used to represent the block size in blobfs.
  using BlockSizeType = uint64_t;

  // Ensure that the types used in this class match the types used in the |Inode|.  This class
  // depends on the exact sizes of these types and should fail to compile if the types used in the
  // |Inode| change.
  static_assert(std::is_same<ByteCountType, decltype(Inode::blob_size)>::value);
  static_assert(std::is_same<BlockCountType, decltype(Inode::block_count)>::value);
  static_assert(
      std::is_same<BlockSizeType, std::remove_const<decltype(kBlobfsBlockSize)>::type>::value);

  BlockSizeType blobfs_block_size() const { return blobfs_block_size_; }

  // The uncompressed size of the file.
  ByteCountType FileSize() const;
  // The uncompressed size of the file rounded up to the next multiple of the block size.
  ByteCountType FileBlockAlignedSize() const;

  // The number of bytes used to store the blob's data.
  // When reading a compressed blob this may not be the exact size but a safe upper bound.  All
  // bytes between the actual compressed size and |DataSizeUpperBound| will be zeros.  This is
  // because the size of the compressed file is not stored.  See fxbug.dev/44547.
  ByteCountType DataSizeUpperBound() const;
  // The size of buffer required to hold |DataBlockCount| blocks.
  ByteCountType DataBlockAlignedSize() const;
  // The number of blocks that the data spans.
  BlockCountType DataBlockCount() const;

  // The data may not start at the beginning of the blob contents but it will always be on a
  // block-aligned offset. The rest of the blob's data will be in the following |DataBlockCount| - 1
  // blocks.
  virtual BlockCountType DataBlockOffset() const = 0;
  ByteCountType DataOffset() const {
    return safemath::checked_cast<ByteCountType>(DataBlockOffset()) * blobfs_block_size_;
  }

  // The number of bytes required to store the Merkle tree.
  ByteCountType MerkleTreeSize() const;
  // The size of buffer required to hold |MerkleTreeBlockCount| blocks.
  ByteCountType MerkleTreeBlockAlignedSize() const;
  // The number of blocks that the Merkle tree spans.
  BlockCountType MerkleTreeBlockCount() const;

  // Returns the offset within the file for the merkle tree.
  virtual ByteCountType MerkleTreeOffset() const = 0;

  // The first block of the blob containing part of the Merkle tree.  The rest of the Merkle tree
  // will be in the following |MerkleTreeBlockCount| - 1 blocks.
  BlockCountType MerkleTreeBlockOffset() const {
    return static_cast<BlockCountType>(MerkleTreeOffset() / blobfs_block_size_);
  }

  // The offset within |MerkleTreeBlockOffset| that the Merkle tree starts at.
  ByteCountType MerkleTreeOffsetWithinBlockOffset() const {
    return MerkleTreeOffset() % blobfs_block_size_;
  }

  // The total number of blocks that the blob spans.
  virtual BlockCountType TotalBlockCount() const = 0;

  // True if the data and Merkle tree share a block.
  virtual bool HasMerkleTreeAndDataSharedBlock() const = 0;

  // The format that this layout is in.
  virtual BlobLayoutFormat Format() const = 0;

  // Initializes a |BlobLayout| from a blob's inode.
  static zx::status<std::unique_ptr<BlobLayout>> CreateFromInode(BlobLayoutFormat format,
                                                                 const Inode& inode,
                                                                 BlockSizeType blobfs_block_size);

  // Initializes a |BlobLayout| from a blob's file size and data size.
  // For uncompressed blobs |data_size| is the same as |file_size|.
  // For compressed blobs |data_size| is the compressed size of the file.
  static zx::status<std::unique_ptr<BlobLayout>> CreateFromSizes(BlobLayoutFormat format,
                                                                 ByteCountType file_size,
                                                                 ByteCountType data_size,
                                                                 BlockSizeType blobfs_block_size);

  virtual ~BlobLayout() = default;

 protected:
  BlobLayout(ByteCountType file_size, ByteCountType data_size, ByteCountType merkle_tree_size,
             BlockSizeType blobfs_block_size);

 private:
  // The uncompressed size of the file.
  ByteCountType file_size_;

  // The number of bytes required to store the blob's data.
  ByteCountType data_size_;

  // The number of bytes required to store the Merkle tree.
  // This field can be derived from |file_size_| but is cached because it's not a constant time
  // calculation and is required in many of the other calculations.
  ByteCountType merkle_tree_size_;

  // The size of a block in blobfs.
  BlockSizeType blobfs_block_size_;
};

}  // namespace blobfs

#endif  // SRC_STORAGE_BLOBFS_BLOB_LAYOUT_H_
