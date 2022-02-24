// Copyright 2020 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <lib/zbitl/vmo.h>

#include <cstddef>
#include <memory>

#include <fbl/alloc_checker.h>

namespace zbitl {

fitx::result<zx_status_t, uint32_t> StorageTraits<zx::vmo>::Capacity(const zx::vmo& vmo) {
  uint64_t size;
  zx_status_t status = vmo.get_size(&size);
  if (status == ZX_OK) {
    uint64_t content_size;
    status = vmo.get_property(ZX_PROP_VMO_CONTENT_SIZE, &content_size, sizeof(content_size));
    if (status == ZX_OK && content_size != 0) {
      size = content_size;
    }
  }
  if (status != ZX_OK) {
    return fitx::error{status};
  }
  return fitx::ok(static_cast<uint32_t>(
      std::min(static_cast<uint64_t>(std::numeric_limits<uint32_t>::max()), size)));
}

fitx::result<zx_status_t> StorageTraits<zx::vmo>::EnsureCapacity(const zx::vmo& vmo,
                                                                 uint32_t capacity_bytes) {
  auto current = Capacity(vmo);
  if (current.is_error()) {
    return current.take_error();
  } else if (current.value() >= capacity_bytes) {
    return fitx::ok();  // Current capacity is sufficient.
  }

  uint64_t cap = static_cast<uint64_t>(capacity_bytes);
  if (auto status = vmo.set_size(cap); status != ZX_OK) {
    return fitx::error{status};
  }
  return fitx::ok();
}

fitx::result<zx_status_t> StorageTraits<zx::vmo>::Read(const zx::vmo& vmo, payload_type payload,
                                                       void* buffer, uint32_t length) {
  zx_status_t status = vmo.read(buffer, payload, length);
  if (status != ZX_OK) {
    return fitx::error{status};
  }
  return fitx::ok();
}

fitx::result<zx_status_t> StorageTraits<zx::vmo>::Write(const zx::vmo& vmo, uint32_t offset,
                                                        ByteView data) {
  zx_status_t status = vmo.write(data.data(), offset, data.size());
  if (status != ZX_OK) {
    return fitx::error{status};
  }
  return fitx::ok();
}

fitx::result<zx_status_t, zx::vmo> StorageTraits<zx::vmo>::Create(const zx::vmo&, uint32_t size,
                                                                  uint32_t initial_zero_size) {
  // While `initial_zero_size` is a required parameter for the creation trait,
  // it is unnecessary in the case of VMOs, as newly-created instances are
  // always zero-filled.
  zx::vmo vmo;
  if (zx_status_t status = zx::vmo::create(size, ZX_VMO_RESIZABLE, &vmo); status != ZX_OK) {
    return fitx::error{status};
  }
  return fitx::ok(std::move(vmo));
}

fitx::result<zx_status_t, std::optional<std::pair<zx::vmo, uint32_t>>>
StorageTraits<zx::vmo>::DoClone(const zx::vmo& original, uint32_t offset, uint32_t length) {
  const uint32_t slop = offset % uint32_t{ZX_PAGE_SIZE};
  const uint32_t clone_start = offset & -uint32_t{ZX_PAGE_SIZE};
  const uint32_t clone_size = slop + length;

  zx::vmo clone;
  zx_status_t status = original.create_child(ZX_VMO_CHILD_SNAPSHOT | ZX_VMO_CHILD_RESIZABLE,
                                             clone_start, clone_size, &clone);
  if (status == ZX_OK && slop > 0) {
    // Explicitly zero the partial page before the range so it remains unseen.
    status = clone.op_range(ZX_VMO_OP_ZERO, 0, slop, nullptr, 0);
  }
  if (status == ZX_OK && clone_size % ZX_PAGE_SIZE != 0) {
    // Explicitly zero the partial page after the range so it remains unseen.
    status = clone.op_range(ZX_VMO_OP_ZERO, clone_size, ZX_PAGE_SIZE - (clone_size % ZX_PAGE_SIZE),
                            nullptr, 0);
  }
  if (status != ZX_OK) {
    return fitx::error{status};
  }

  return fitx::ok(std::make_pair(std::move(clone), slop));
}

}  // namespace zbitl
