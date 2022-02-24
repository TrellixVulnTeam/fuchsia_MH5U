// Copyright 2019 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/lib/digest/merkle-tree.h"

#include <stddef.h>
#include <stdint.h>
#include <zircon/assert.h>
#include <zircon/errors.h>
#include <zircon/types.h>

#include <memory>

#include <fbl/algorithm.h>
#include <fbl/alloc_checker.h>

#include "src/lib/digest/digest.h"
#include "src/lib/digest/hash-list.h"

namespace digest {
namespace internal {

// MerkleTree

template <typename T, typename VP, class MT, class HL>
zx_status_t MerkleTree<T, VP, MT, HL>::SetDataLength(size_t data_len) {
  zx_status_t rc = hash_list_.SetDataLength(data_len);
  if (rc != ZX_OK) {
    return rc;
  }
  size_t list_len = hash_list_.GetListLength();
  if (list_len == hash_list_.GetDigestSize()) {
    return ZX_OK;
  }
  fbl::AllocChecker ac;
  next_.reset(new (&ac) MT());
  if (!ac.check()) {
    return ZX_ERR_NO_MEMORY;
  }
  next_->hash_list_.SetNodeId(hash_list_.GetNodeId() + 1);
  next_->SetNodeSize(GetNodeSize());
  next_->SetUseCompactFormat(use_compact_format_);
  size_t next_len = use_compact_format_ ? list_len : fbl::round_up(list_len, GetNodeSize());
  return next_->SetDataLength(next_len);
}

template <typename T, typename VP, class MT, class HL>
size_t MerkleTree<T, VP, MT, HL>::GetTreeLength() const {
  if (next_.get() == nullptr) {
    return 0;
  }
  size_t list_len = next_->hash_list_.data_len();
  return list_len + next_->GetTreeLength();
}

template <typename T, typename VP, class MT, class HL>
zx_status_t MerkleTree<T, VP, MT, HL>::SetTree(VP tree, size_t tree_len, VP root, size_t root_len) {
  if (next_ == nullptr) {
    return hash_list_.SetList(static_cast<T *>(root), root_len);
  }
  size_t list_len = next_->hash_list_.data_len();
  if (tree_len < list_len) {
    return ZX_ERR_BUFFER_TOO_SMALL;
  }
  T *list = static_cast<T *>(tree);
  zx_status_t rc = hash_list_.SetList(list, list_len);
  if (rc != ZX_OK) {
    return rc;
  }
  return next_->SetTree(list + list_len, tree_len - list_len, root, root_len);
}

template <typename T, typename VP, class MT, class HL>
void MerkleTree<T, VP, MT, HL>::SetUseCompactFormat(bool use_compact_format) {
  use_compact_format_ = use_compact_format;
  // The data is only padded to the node size when creating a hash list of a hash list and not
  // when creating a hash list of the leaf data.
  hash_list_.SetPadDataToNodeSize(use_compact_format_ && hash_list_.GetNodeId());
}

}  // namespace internal

// MerkleTreeCreator

// Forward declarations for templates
template zx_status_t internal::MerkleTree<uint8_t, void *, MerkleTreeCreator,
                                          HashListCreator>::SetDataLength(size_t data_len);

template size_t
internal::MerkleTree<uint8_t, void *, MerkleTreeCreator, HashListCreator>::GetTreeLength() const;

template zx_status_t internal::MerkleTree<uint8_t, void *, MerkleTreeCreator,
                                          HashListCreator>::SetTree(void *tree, size_t tree_len,
                                                                    void *root, size_t root_len);

template void internal::MerkleTree<uint8_t, void *, MerkleTreeCreator,
                                   HashListCreator>::SetUseCompactFormat(bool use_compact_format);

// static
zx_status_t MerkleTreeCreator::Create(const void *data, size_t data_len,
                                      std::unique_ptr<uint8_t[]> *out_tree, size_t *out_tree_len,
                                      Digest *out_root) {
  if (out_tree == nullptr || out_tree_len == nullptr || out_root == nullptr) {
    return ZX_ERR_INVALID_ARGS;
  }
  uint8_t root[kSha256Length];
  MerkleTreeCreator creator;
  zx_status_t rc = creator.SetDataLength(data_len);
  if (rc != ZX_OK) {
    return rc;
  }
  size_t tree_len = creator.GetTreeLength();
  std::unique_ptr<uint8_t[]> tree(nullptr);
  if (tree_len > 0) {
    fbl::AllocChecker ac;
    tree.reset(new (&ac) uint8_t[tree_len]);
    if (!ac.check()) {
      return ZX_ERR_NO_MEMORY;
    }
  }
  if ((rc = creator.SetTree(tree.get(), tree_len, root, sizeof(root))) != ZX_OK ||
      (rc = creator.Append(data, data_len)) != ZX_OK) {
    return rc;
  }
  *out_tree = std::move(tree);
  *out_tree_len = tree_len;
  *out_root = root;

  return ZX_OK;
}

zx_status_t MerkleTreeCreator::Append(const void *buf, size_t buf_len) {
  if (buf_len == 0) {
    return ZX_OK;
  }
  size_t list_off = hash_list_.list_off();
  zx_status_t rc = hash_list_.Append(buf, buf_len);
  if (rc != ZX_OK) {
    return rc;
  }
  if (next_.get() == nullptr) {
    return ZX_OK;
  }
  auto list = hash_list_.list() + list_off;
  auto list_len = hash_list_.list_off() - list_off;
  if ((rc = next_->Append(list, list_len)) != ZX_OK) {
    return rc;
  }
  if (hash_list_.data_off() != hash_list_.data_len()) {
    return ZX_OK;
  }
  if (!GetUseCompactFormat()) {
    list = hash_list_.list() + hash_list_.list_off();
    list_len = hash_list_.list_len() - hash_list_.list_off();
    memset(list, 0, list_len);
    return next_->Append(list, list_len);
  }
  return ZX_OK;
}

// MerkleTreeVerifier

// Forward declarations for templates
template zx_status_t internal::MerkleTree<const uint8_t, const void *, MerkleTreeVerifier,
                                          HashListVerifier>::SetDataLength(size_t data_len);

template size_t internal::MerkleTree<const uint8_t, const void *, MerkleTreeVerifier,
                                     HashListVerifier>::GetTreeLength() const;

template zx_status_t
internal::MerkleTree<const uint8_t, const void *, MerkleTreeVerifier, HashListVerifier>::SetTree(
    const void *tree, size_t tree_len, const void *root, size_t root_len);

template void internal::MerkleTree<const uint8_t, const void *, MerkleTreeVerifier,
                                   HashListVerifier>::SetUseCompactFormat(bool use_compact_format);

// static
zx_status_t MerkleTreeVerifier::Verify(const void *buf, size_t buf_len, size_t data_off,
                                       size_t data_len, const void *tree, size_t tree_len,
                                       const Digest &root) {
  zx_status_t rc;
  MerkleTreeVerifier verifier;
  if ((rc = verifier.SetDataLength(data_len)) != ZX_OK ||
      (rc = verifier.SetTree(tree, tree_len, root.get(), root.len())) != ZX_OK ||
      (rc = verifier.Verify(buf, buf_len, data_off)) != ZX_OK) {
    return rc;
  }
  return ZX_OK;
}

zx_status_t MerkleTreeVerifier::Verify(const void *buf, size_t buf_len, size_t data_off) {
  zx_status_t rc = hash_list_.Verify(buf, buf_len, data_off);
  if (rc != ZX_OK) {
    return rc;
  }
  if (next_.get() == nullptr) {
    return ZX_OK;
  }
  data_off = hash_list_.GetListOffset(data_off);
  buf_len = hash_list_.list_off() - data_off;
  if ((rc = next_->hash_list_.Align(&data_off, &buf_len)) != ZX_OK) {
    return rc;
  }
  buf = hash_list_.list() + data_off;
  return next_->Verify(buf, buf_len, data_off);
}

size_t CalculateMerkleTreeSize(size_t data_size, size_t node_size, bool use_compact_format) {
  ZX_ASSERT_MSG(NodeDigest::IsValidNodeSize(node_size), "node_size=%lu", node_size);
  size_t merkle_tree_size = 0;
  while (data_size > node_size) {
    size_t list_size = CalculateHashListSize(data_size, node_size);
    // The non compact format pads the hash list to be a multiple of the node size.
    data_size = use_compact_format ? list_size : fbl::round_up(list_size, node_size);
    merkle_tree_size += data_size;
  }
  return merkle_tree_size;
}

}  // namespace digest
