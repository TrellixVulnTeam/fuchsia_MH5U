// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/playback/mediaplayer/graph/payloads/vmo_payload_allocator.h"

#include <lib/syslog/cpp/macros.h>
#include <lib/zx/vmar.h>

#include "src/media/playback/mediaplayer/graph/formatting.h"

namespace media_player {

// static
fbl::RefPtr<VmoPayloadAllocator> VmoPayloadAllocator::Create() {
  return fbl::MakeRefCounted<VmoPayloadAllocator>();
}

void VmoPayloadAllocator::Dump(std::ostream& os) const {
  std::lock_guard<std::mutex> locker(mutex_);

  os << fostr::Indent;
  os << fostr::NewLine << "vmo allocation: " << vmo_allocation_;
  os << fostr::NewLine << "payload vmos:   " << payload_vmos_;
  os << fostr::Outdent;
}

void VmoPayloadAllocator::SetVmoAllocation(VmoAllocation vmo_allocation) {
  FX_DCHECK(vmo_allocation != VmoAllocation::kNotApplicable);
  std::lock_guard<std::mutex> locker(mutex_);
  FX_DCHECK(!vmo_allocation_set_) << "SetVmoAllocation may only be called once.";
  vmo_allocation_set_ = true;
  vmo_allocation_ = vmo_allocation;
}

fbl::RefPtr<PayloadBuffer> VmoPayloadAllocator::AllocatePayloadBuffer(uint64_t size) {
  std::lock_guard<std::mutex> locker(mutex_);
  FX_DCHECK(vmo_allocation_set_);
  FX_DCHECK(vmo_allocation_ != VmoAllocation::kNotApplicable);
  FX_DCHECK(!payload_vmos_.empty());
  FX_DCHECK((vmo_allocation_ != VmoAllocation::kSingleVmo) || (payload_vmos_.size() == 1));

  fbl::RefPtr<PayloadBuffer> payload_buffer;
  size_t vmo_index = suggested_allocation_vmo_;

  while (true) {
    payload_buffer = TryAllocateFromVmo(payload_vmos_[vmo_index], size);
    if (payload_buffer) {
      break;
    }

    vmo_index = (vmo_index + 1) % payload_vmos_.size();
    if (vmo_index == suggested_allocation_vmo_) {
      // Payload memory exhausted.
      return nullptr;
    }
  }

  if (vmo_allocation_ == VmoAllocation::kVmoPerBuffer) {
    // We've used the entire VMO, so it's not a good suggestion.
    suggested_allocation_vmo_ = (vmo_index + 1) % payload_vmos_.size();
  }

  return payload_buffer;
}

std::vector<fbl::RefPtr<PayloadVmo>> VmoPayloadAllocator::GetVmos() const {
  std::lock_guard<std::mutex> locker(mutex_);
  return payload_vmos_;
}

void VmoPayloadAllocator::AddVmo(fbl::RefPtr<PayloadVmo> payload_vmo) {
  FX_DCHECK(payload_vmo);

  std::lock_guard<std::mutex> locker(mutex_);
  FX_DCHECK(payload_vmos_.empty() || vmo_allocation_ != VmoAllocation::kSingleVmo)
      << "Attempt to add more than one VMO to single-vmo allocator.";

  payload_vmo->SetIndex(payload_vmos_.size());

  payload_vmos_.push_back(payload_vmo);
  if (vmo_allocation_ == VmoAllocation::kSingleVmo ||
      vmo_allocation_ == VmoAllocation::kUnrestricted) {
    payload_vmo->allocator_ = std::make_unique<FifoAllocator>(payload_vmo->size());
  }
}

void VmoPayloadAllocator::RemoveVmo(fbl::RefPtr<PayloadVmo> payload_vmo) {
  std::lock_guard<std::mutex> locker(mutex_);

  for (auto iter = payload_vmos_.begin(); iter != payload_vmos_.end(); ++iter) {
    if (*iter == payload_vmo) {
      payload_vmos_.erase(iter);

      if (suggested_allocation_vmo_ >= payload_vmos_.size()) {
        suggested_allocation_vmo_ = 0;
      }
      return;
    }
  }

  FX_LOGS(WARNING) << "VMO not found in RemoveVmo.";
}

fbl::RefPtr<PayloadBuffer> VmoPayloadAllocator::TryAllocateFromVmo(
    fbl::RefPtr<PayloadVmo> payload_vmo, uint64_t size) {
  FX_DCHECK(payload_vmo);

  if (vmo_allocation_ == VmoAllocation::kVmoPerBuffer) {
    // Try to allocate the entire VMO.

    if (payload_vmo->allocated_) {
      // It's already allocated.
      return nullptr;
    }

    if (payload_vmo->size() < size) {
      // Buffers are smaller than the requested size.
      return nullptr;
    }

    payload_vmo->allocated_ = true;

    // Success!
    return PayloadBuffer::Create(
        size, payload_vmo->start(), payload_vmo, 0,
        [this, this_refptr = fbl::RefPtr(this)](PayloadBuffer* payload_buffer) {
          FX_DCHECK(payload_buffer->vmo());
          // Take the |VmoPayloadAllocator|'s mutex to
          // serialize access to the |allocated_| field
          // of the |PayloadVmo|.
          std::lock_guard<std::mutex> locker(mutex_);
          payload_buffer->vmo()->allocated_ = false;
        });
  }

  // Try to allocate a region from the VMO.
  FX_DCHECK(payload_vmo->allocator_);

  // The region allocated at the top of the VMO will be aligned to 4096 bytes.
  // We ensure that subsequent allocations will be |kByteAlignment|-aligned by
  // aligning-up all the sizes.
  uint64_t offset = payload_vmo->allocator_->AllocateRegion(PayloadBuffer::AlignUp(size));
  if (offset == FifoAllocator::kNullOffset) {
    // Can't allocate.
    return nullptr;
  }

  // Success!
  return PayloadBuffer::Create(
      size, reinterpret_cast<uint8_t*>(payload_vmo->start()) + offset, payload_vmo, offset,
      [this, this_refptr = fbl::RefPtr(this), offset](PayloadBuffer* payload_buffer) {
        FX_DCHECK(payload_buffer->vmo());
        FX_DCHECK(payload_buffer->vmo()->allocator_);
        // Take the |VmoPayloadAllocator|'s mutex to serialize access to the
        // |FifoAllocator|.
        std::lock_guard<std::mutex> locker(mutex_);
        payload_buffer->vmo()->allocator_->ReleaseRegion(offset);
      });
}

}  // namespace media_player
