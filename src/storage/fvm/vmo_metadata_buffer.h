// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_STORAGE_FVM_VMO_METADATA_BUFFER_H_
#define SRC_STORAGE_FVM_VMO_METADATA_BUFFER_H_

#include <lib/fzl/owned-vmo-mapper.h>
#include <zircon/types.h>

#include <memory>

#include "src/storage/fvm/metadata_buffer.h"

namespace fvm {

// VmoMetadataBuffer is an instance of |MetadataBuffer| backed by a VMO.
class VmoMetadataBuffer : public MetadataBuffer {
 public:
  VmoMetadataBuffer() = default;
  explicit VmoMetadataBuffer(fzl::OwnedVmoMapper vmo);
  ~VmoMetadataBuffer() override;

  // Accesses the underlying vmo.
  const zx::vmo& vmo() const { return vmo_.vmo(); }

  // MetadataBuffer implementation
  std::unique_ptr<MetadataBuffer> Create(size_t size) const override;
  void* data() const override { return vmo_.start(); }
  size_t size() const override { return vmo_.size(); }

 private:
  fzl::OwnedVmoMapper vmo_;
};

}  // namespace fvm

#endif  // SRC_STORAGE_FVM_VMO_METADATA_BUFFER_H_
